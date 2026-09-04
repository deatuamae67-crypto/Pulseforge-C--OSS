#include "pulseforge/streaming_chart_importer.hpp"
#include "pulseforge/ascii_number.hpp"
#include "pulseforge/musical_chart.hpp"
#include "pulseforge/visual_density_index.hpp"

#include "pulseforge/chart_loader.hpp"
#include "pulseforge/packed_chart_stream.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulseforge {
namespace {

class ImportError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result
) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

class BufferedInput final {
public:
    BufferedInput(
        const std::filesystem::path& path,
        const std::size_t buffer_bytes
    ) : input_(path, std::ios::binary), buffer_(buffer_bytes) {
        if (!input_) {
            throw ImportError("cannot open source chart JSON");
        }
        if (buffer_.empty()) {
            throw ImportError("JSON input buffer cannot be empty");
        }
    }

    [[nodiscard]] int peek() {
        if (position_ == available_ && !refill()) {
            return -1;
        }
        return static_cast<unsigned char>(buffer_[position_]);
    }

    [[nodiscard]] int get() {
        const auto value = peek();
        if (value < 0) {
            return -1;
        }
        ++position_;
        ++offset_;
        hash_ ^= static_cast<std::uint8_t>(value);
        hash_ *= 1'099'511'628'211ULL;
        return value;
    }

    [[nodiscard]] std::uint64_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }

    void consume_optional_utf8_bom() {
        if (peek() != 0xEF) {
            return;
        }
        (void)get();
        if (get() != 0xBB || get() != 0xBF) {
            throw ImportError("source chart begins with an invalid UTF-8 BOM");
        }
    }

private:
    [[nodiscard]] bool refill() {
        if (at_end_) {
            return false;
        }
        input_.read(
            buffer_.data(),
            static_cast<std::streamsize>(buffer_.size())
        );
        const auto count = input_.gcount();
        if (count < 0 || input_.bad()) {
            throw ImportError("I/O error while reading source chart JSON");
        }
        position_ = 0U;
        available_ = static_cast<std::size_t>(count);
        if (available_ == 0U) {
            at_end_ = true;
            return false;
        }
        return true;
    }

    std::ifstream input_;
    std::vector<char> buffer_;
    std::size_t position_{};
    std::size_t available_{};
    std::uint64_t offset_{};
    std::uint64_t hash_{1'469'598'103'934'665'603ULL};
    bool at_end_{};
};

enum class TokenKind : std::uint8_t {
    left_brace,
    right_brace,
    left_bracket,
    right_bracket,
    colon,
    comma,
    string,
    number,
    true_value,
    false_value,
    null_value,
    end,
};

struct Token final {
    TokenKind kind{TokenKind::end};
    std::string text;
    std::uint64_t offset{};
};

[[nodiscard]] bool valid_utf8(const std::string_view text) noexcept {
    std::size_t index = 0U;
    while (index < text.size()) {
        const auto first = static_cast<std::uint8_t>(text[index]);
        std::size_t remaining{};
        std::uint32_t codepoint{};
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        if (first >= 0xC2U && first <= 0xDFU) {
            remaining = 1U;
            codepoint = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            remaining = 2U;
            codepoint = first & 0x0FU;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            remaining = 3U;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (remaining > text.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1U; offset <= remaining; ++offset) {
            const auto continuation = static_cast<std::uint8_t>(
                text[index + offset]
            );
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (continuation & 0x3FU);
        }
        if ((remaining == 2U && codepoint < 0x800U)
            || (remaining == 3U && codepoint < 0x10000U)
            || codepoint > 0x10FFFFU
            || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
            return false;
        }
        index += remaining + 1U;
    }
    return true;
}

class JsonLexer final {
public:
    JsonLexer(
        const std::filesystem::path& path,
        const StreamingChartImportOptions& options
    ) : input_(path, options.input_buffer_bytes),
        max_string_source_bytes_(options.max_string_source_bytes),
        max_decoded_string_bytes_(options.max_decoded_string_bytes) {
        input_.consume_optional_utf8_bom();
    }

    [[nodiscard]] Token next() {
        skip_whitespace();
        Token token;
        token.offset = input_.offset();
        const auto current = input_.get();
        switch (current) {
        case -1: token.kind = TokenKind::end; return token;
        case '{': token.kind = TokenKind::left_brace; return token;
        case '}': token.kind = TokenKind::right_brace; return token;
        case '[': token.kind = TokenKind::left_bracket; return token;
        case ']': token.kind = TokenKind::right_bracket; return token;
        case ':': token.kind = TokenKind::colon; return token;
        case ',': token.kind = TokenKind::comma; return token;
        case '"':
            token.kind = TokenKind::string;
            token.text = read_string(token.offset);
            return token;
        case 't': read_literal("rue", token.offset); token.kind = TokenKind::true_value; return token;
        case 'f': read_literal("alse", token.offset); token.kind = TokenKind::false_value; return token;
        case 'n': read_literal("ull", token.offset); token.kind = TokenKind::null_value; return token;
        default:
            if (current == '-' || (current >= '0' && current <= '9')) {
                token.kind = TokenKind::number;
                token.text = read_number(current, token.offset);
                return token;
            }
            throw_at("invalid JSON token", token.offset);
        }
    }

    [[nodiscard]] std::uint64_t fingerprint() const noexcept {
        return input_.hash();
    }

private:
    [[noreturn]] static void throw_at(
        const std::string_view message,
        const std::uint64_t offset
    ) {
        throw ImportError(
            std::string(message) + " at byte " + std::to_string(offset)
        );
    }

    void skip_whitespace() {
        while (true) {
            const auto value = input_.peek();
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                return;
            }
            (void)input_.get();
        }
    }

    void read_literal(
        const std::string_view suffix,
        const std::uint64_t offset
    ) {
        for (const auto expected : suffix) {
            if (input_.get() != expected) {
                throw_at("invalid JSON literal", offset);
            }
        }
    }

    [[nodiscard]] static int hex_value(const int value) noexcept {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    }

    [[nodiscard]] std::uint32_t read_hex4(const std::uint64_t offset) {
        std::uint32_t result{};
        for (std::size_t index = 0U; index < 4U; ++index) {
            const auto digit = hex_value(input_.get());
            if (digit < 0) {
                throw_at("invalid JSON Unicode escape", offset);
            }
            result = (result << 4U) | static_cast<std::uint32_t>(digit);
        }
        return result;
    }

    void append_utf8(
        std::string& output,
        const std::uint32_t codepoint,
        const std::uint64_t offset
    ) const {
        if (codepoint <= 0x7FU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint <= 0x10FFFFU) {
            output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            throw_at("invalid JSON Unicode code point", offset);
        }
        if (output.size() > max_decoded_string_bytes_) {
            throw_at("decoded JSON string exceeds its limit", offset);
        }
    }

    [[nodiscard]] std::string read_string(const std::uint64_t offset) {
        std::string output;
        std::size_t source_bytes{};
        while (true) {
            const auto value = input_.get();
            if (value < 0) {
                throw_at("unterminated JSON string", offset);
            }
            if (++source_bytes > max_string_source_bytes_) {
                throw_at("JSON string source exceeds its limit", offset);
            }
            if (value == '"') {
                if (!valid_utf8(output)) {
                    throw_at("JSON string is not valid UTF-8", offset);
                }
                return output;
            }
            if (value >= 0 && value < 0x20) {
                throw_at("unescaped control byte in JSON string", offset);
            }
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
                if (output.size() > max_decoded_string_bytes_) {
                    throw_at("decoded JSON string exceeds its limit", offset);
                }
                continue;
            }

            const auto escaped = input_.get();
            if (escaped < 0 || ++source_bytes > max_string_source_bytes_) {
                throw_at("unterminated JSON escape", offset);
            }
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                source_bytes += 4U;
                if (source_bytes > max_string_source_bytes_) {
                    throw_at("JSON string source exceeds its limit", offset);
                }
                auto codepoint = read_hex4(offset);
                if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                    if (input_.get() != '\\' || input_.get() != 'u') {
                        throw_at("JSON high surrogate lacks a low surrogate", offset);
                    }
                    source_bytes += 6U;
                    const auto low = read_hex4(offset);
                    if (low < 0xDC00U || low > 0xDFFFU) {
                        throw_at("invalid JSON low surrogate", offset);
                    }
                    codepoint = 0x10000U
                        + ((codepoint - 0xD800U) << 10U)
                        + (low - 0xDC00U);
                } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
                    throw_at("unpaired JSON low surrogate", offset);
                }
                append_utf8(output, codepoint, offset);
                break;
            }
            default:
                throw_at("invalid JSON string escape", offset);
            }
            if (output.size() > max_decoded_string_bytes_) {
                throw_at("decoded JSON string exceeds its limit", offset);
            }
        }
    }

    [[nodiscard]] std::string read_number(
        const int first,
        const std::uint64_t offset
    ) {
        std::string output(1U, static_cast<char>(first));
        auto current = first;
        if (current == '-') {
            current = input_.peek();
            if (current < '0' || current > '9') {
                throw_at("invalid JSON number", offset);
            }
            output.push_back(static_cast<char>(input_.get()));
        }
        if (output.back() == '0') {
            const auto next = input_.peek();
            if (next >= '0' && next <= '9') {
                throw_at("JSON number has a leading zero", offset);
            }
        } else {
            while (input_.peek() >= '0' && input_.peek() <= '9') {
                output.push_back(static_cast<char>(input_.get()));
            }
        }
        if (input_.peek() == '.') {
            output.push_back(static_cast<char>(input_.get()));
            if (input_.peek() < '0' || input_.peek() > '9') {
                throw_at("JSON fraction has no digits", offset);
            }
            while (input_.peek() >= '0' && input_.peek() <= '9') {
                output.push_back(static_cast<char>(input_.get()));
            }
        }
        if (input_.peek() == 'e' || input_.peek() == 'E') {
            output.push_back(static_cast<char>(input_.get()));
            if (input_.peek() == '+' || input_.peek() == '-') {
                output.push_back(static_cast<char>(input_.get()));
            }
            if (input_.peek() < '0' || input_.peek() > '9') {
                throw_at("JSON exponent has no digits", offset);
            }
            while (input_.peek() >= '0' && input_.peek() <= '9') {
                output.push_back(static_cast<char>(input_.get()));
            }
        }
        if (output.size() > 128U) {
            throw_at("JSON number token exceeds its limit", offset);
        }
        return output;
    }

    BufferedInput input_;
    std::size_t max_string_source_bytes_{};
    std::size_t max_decoded_string_bytes_{};
};

[[nodiscard]] std::optional<double> token_double(const Token& token) {
    if (token.kind != TokenKind::number) {
        return std::nullopt;
    }
    double value{};
    if (!parse_ascii_floating(token.text, value)) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<long double> token_long_double(const Token& token) {
    if (token.kind != TokenKind::number) {
        return std::nullopt;
    }
    long double value{};
    if (!parse_ascii_floating(token.text, value)) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::int64_t> token_integer(const Token& token) {
    if (token.kind != TokenKind::number
        || token.text.find_first_of(".eE") != std::string::npos) {
        return std::nullopt;
    }
    std::int64_t value{};
    const auto parsed = std::from_chars(
        token.text.data(),
        token.text.data() + token.text.size(),
        value
    );
    if (parsed.ec != std::errc{}
        || parsed.ptr != token.text.data() + token.text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::int64_t> milliseconds_to_microseconds(
    const double milliseconds
) noexcept {
    if (!std::isfinite(milliseconds)) {
        return std::nullopt;
    }
    const auto scaled = static_cast<long double>(milliseconds) * 1'000.0L;
    if (scaled < static_cast<long double>(
            std::numeric_limits<std::int64_t>::min())
        || scaled > static_cast<long double>(
            std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(std::round(scaled));
}

[[nodiscard]] std::optional<std::uint64_t> duration_to_microseconds(
    const long double milliseconds
) noexcept {
    if (!std::isfinite(milliseconds) || milliseconds < 0.0L) {
        return std::nullopt;
    }
    const auto scaled = milliseconds * 1'000.0L;
    if (scaled > static_cast<long double>(
            std::numeric_limits<std::uint64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(std::round(scaled));
}

enum class NoteShape : std::uint8_t { unknown, native, psych };

struct PendingTempoChange final {
    double start_step{};
    double bpm{120.0};
};

struct ImportMetadata final {
    std::optional<std::string> title;
    std::optional<std::string> name;
    std::optional<std::string> song;
    std::optional<std::string> header_song;
    std::optional<std::string> artist;
    std::optional<std::string> header_artist;
    std::optional<std::string> charter;
    std::optional<std::string> header_charter;
    std::optional<std::string> difficulty;
    std::optional<std::string> stage;
    std::optional<std::string> player;
    std::optional<std::string> opponent;
    // PULSEFORGE_P1_4_0_STREAMING_PLAYER4_METADATA_V1
    std::optional<std::string> secondary_opponent;
    std::optional<bool> secondary_opponent_enabled;
    std::optional<std::string> girlfriend;
    std::optional<std::string> note_style;
    std::optional<double> song_bpm;
    std::optional<double> header_bpm;
    std::optional<double> song_speed;
    std::optional<double> option_speed;
    std::optional<bool> needs_voices;
    std::filesystem::path instrumental;
    std::vector<std::filesystem::path> vocals;
    std::vector<PendingTempoChange> psych_tempo_changes;
    double psych_section_step{};
    bool saw_header_object{};
    bool saw_options_object{};
    bool audio_vocals_explicit{};
};

enum class RawOwnerMode : std::uint8_t {
    native_player,
    native_opponent,
    psych_must_hit_false,
    psych_must_hit_true,
};

struct CoincidentPatternCandidate final {
    std::uint64_t first_sequence{};
    std::uint64_t count{};
    std::int64_t time_us{};
    std::uint64_t duration_us{};
    std::uint64_t raw_lane{};
    std::uint32_t kind_id{};
    RawOwnerMode owner_mode{RawOwnerMode::native_player};
};

// C15: arithmetic PatternRun candidate. The lane sequence is reduced to its
// repeating period while the first pass is still streaming, so a run with
// billions of logical notes needs only constant/bounded metadata.
struct ArithmeticPatternCandidate final {
    std::uint64_t first_sequence{};
    std::uint64_t count{};
    std::int64_t start_us{};
    std::uint64_t interval_us{};
    std::uint64_t duration_us{};
    std::uint32_t kind_id{};
    RawOwnerMode owner_mode{RawOwnerMode::native_player};
    std::vector<std::uint64_t> raw_lane_pattern;
};

struct PendingArithmeticPattern final {
    ArithmeticPatternCandidate candidate;
    std::int64_t last_time_us{};
    // Before a period is proven, keep only a small prefix. Once two identical
    // halves are observed, lane_period_locked becomes true and the prefix is
    // reduced to the exact repeating lane pattern.
    bool interval_known{};
    bool lane_period_locked{};
};

// Only sufficiently large runs are promoted so ordinary charts do not inflate
// the pattern table. The lane probe is deliberately bounded.
constexpr std::uint64_t minimum_coincident_pattern_notes = 32U;
constexpr std::uint64_t minimum_arithmetic_pattern_notes = 64U;
constexpr std::size_t maximum_arithmetic_lane_probe = 128U;

struct ImportSummary final {
    NoteShape shape{NoteShape::unknown};
    ChartFormat format{ChartFormat::native};
    std::optional<std::int64_t> explicit_key_count;
    // Psych-family forks disagree on this field: song.mania is commonly a
    // lane count, while options.mania follows the Kade/JS index convention
    // (0 => 1K, 3 => 4K, 8 => 9K). Keep their scopes separate.
    std::optional<std::int64_t> song_mania;
    std::optional<std::int64_t> option_mania;
    std::uint16_t key_count{4U};
    std::uint64_t note_count{};
    std::uint64_t skipped{};
    std::uint64_t max_raw_lane{};
    std::uint64_t source_fingerprint{};
    std::uint64_t content_end_us{};
    bool saw_lane{};
    bool sorted{true};
    std::optional<std::int64_t> last_time;
    std::vector<std::string> kinds;
    std::unordered_map<std::string, std::uint32_t> kind_ids;
    std::vector<bool> section_must_hit;
    std::optional<CoincidentPatternCandidate> pending_coincident;
    std::vector<CoincidentPatternCandidate> coincident_patterns;
    std::optional<PendingArithmeticPattern> pending_arithmetic;
    std::vector<ArithmeticPatternCandidate> arithmetic_patterns;
    std::uint64_t compressed_note_count{};
    std::vector<ChartEvent> events;
    ImportMetadata metadata;
};

enum class ParsePass : std::uint8_t { gather, emit };

struct NativeCandidate final {
    std::optional<double> time;
    std::optional<double> time_ms;
    std::optional<long double> duration;
    std::optional<long double> duration_ms;
    std::optional<std::int64_t> lane;
    std::optional<std::string> owner;
    std::optional<std::string> kind;
    std::optional<std::string> type;
    bool saw_field{};
};

struct PsychCandidate final {
    std::optional<double> time;
    std::optional<std::int64_t> raw_lane;
    std::optional<long double> duration;
    std::string kind{"normal"};
    std::string event_name;
    std::string event_value1;
    std::string event_value2;
};

class StreamingParser final {
public:
    using NoteConsumer = std::function<void(
        const PackedNote&,
        std::uint64_t
    )>;

    StreamingParser(
        const std::filesystem::path& path,
        const StreamingChartImportOptions& options,
        ImportSummary& summary,
        const ParsePass pass,
        NoteConsumer consumer = {}
    ) : lexer_(path, options),
        options_(options),
        summary_(summary),
        pass_(pass),
        consumer_(std::move(consumer)) {}

    void parse() {
        if (peek().kind != TokenKind::left_brace) {
            throw_here("chart JSON root must be an object");
        }
        parse_container_object(false, 0U);
        expect(TokenKind::end, "trailing data after chart JSON root");
        if (notes_containers_ > 1U) {
            throw ImportError("chart JSON has multiple note containers");
        }
        if (pass_ == ParsePass::gather
            && saw_note_array_item_
            && summary_.shape == NoteShape::unknown) {
            throw ImportError("unsupported chart note shape; expected native objects or Psych sections");
        }
        if (pass_ == ParsePass::gather) {
            flush_pending_coincident();
            flush_pending_arithmetic();
            std::stable_sort(
                summary_.events.begin(),
                summary_.events.end(),
                [](const ChartEvent& left, const ChartEvent& right) {
                    return left.time_ms < right.time_ms;
                }
            );
        }
        if (pass_ == ParsePass::emit
            && section_index_ != summary_.section_must_hit.size()) {
            throw ImportError("chart sections changed between streaming passes");
        }
    }

    [[nodiscard]] std::uint64_t fingerprint() const noexcept {
        return lexer_.fingerprint();
    }

private:
    [[nodiscard]] const Token& peek() {
        if (!lookahead_.has_value()) {
            lookahead_ = lexer_.next();
        }
        return *lookahead_;
    }

    [[nodiscard]] Token take() {
        const auto token = peek();
        lookahead_.reset();
        return token;
    }

    void expect(const TokenKind kind, const std::string_view message) {
        if (peek().kind != kind) {
            throw_here(message);
        }
        (void)take();
    }

    [[noreturn]] void throw_here(const std::string_view message) {
        throw ImportError(
            std::string(message) + " at byte "
                + std::to_string(peek().offset)
        );
    }

    void skip_value(const std::size_t depth) {
        if (depth > options_.max_json_depth) {
            throw_here("JSON nesting exceeds configured depth");
        }
        switch (peek().kind) {
        case TokenKind::left_brace: {
            (void)take();
            if (peek().kind == TokenKind::right_brace) {
                (void)take();
                return;
            }
            while (true) {
                expect(TokenKind::string, "JSON object key must be a string");
                expect(TokenKind::colon, "JSON object key lacks a colon");
                skip_value(depth + 1U);
                if (peek().kind == TokenKind::right_brace) {
                    (void)take();
                    return;
                }
                expect(TokenKind::comma, "JSON object entries require a comma");
            }
        }
        case TokenKind::left_bracket: {
            (void)take();
            if (peek().kind == TokenKind::right_bracket) {
                (void)take();
                return;
            }
            while (true) {
                skip_value(depth + 1U);
                if (peek().kind == TokenKind::right_bracket) {
                    (void)take();
                    return;
                }
                expect(TokenKind::comma, "JSON array entries require a comma");
            }
        }
        case TokenKind::string:
        case TokenKind::number:
        case TokenKind::true_value:
        case TokenKind::false_value:
        case TokenKind::null_value:
            (void)take();
            return;
        default:
            throw_here("expected a JSON value");
        }
    }

    [[nodiscard]] std::optional<double> read_number() {
        if (peek().kind != TokenKind::number) {
            skip_value(0U);
            return std::nullopt;
        }
        return token_double(take());
    }

    [[nodiscard]] std::optional<long double> read_duration_number() {
        if (peek().kind != TokenKind::number) {
            skip_value(0U);
            return std::nullopt;
        }
        return token_long_double(take());
    }

    [[nodiscard]] std::optional<std::int64_t> read_integer() {
        if (peek().kind != TokenKind::number) {
            skip_value(0U);
            return std::nullopt;
        }
        return token_integer(take());
    }

    [[nodiscard]] std::optional<bool> read_boolean() {
        if (peek().kind == TokenKind::true_value) {
            (void)take();
            return true;
        }
        if (peek().kind == TokenKind::false_value) {
            (void)take();
            return false;
        }
        skip_value(0U);
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> read_string() {
        if (peek().kind != TokenKind::string) {
            skip_value(0U);
            return std::nullopt;
        }
        return take().text;
    }

    [[nodiscard]] std::string read_scalar_kind() {
        switch (peek().kind) {
        case TokenKind::string:
        case TokenKind::number: {
            auto result = take().text;
            return result.empty() || result == "0" ? "normal" : result;
        }
        case TokenKind::true_value:
            (void)take();
            return "true";
        case TokenKind::false_value:
            (void)take();
            return "false";
        case TokenKind::null_value:
            (void)take();
            return "normal";
        default:
            skip_value(0U);
            return "normal";
        }
    }

    [[nodiscard]] std::string read_scalar_event_value() {
        switch (peek().kind) {
        case TokenKind::string:
        case TokenKind::number:
            return take().text;
        case TokenKind::true_value:
            (void)take();
            return "true";
        case TokenKind::false_value:
            (void)take();
            return "false";
        case TokenKind::null_value:
            (void)take();
            return {};
        default:
            skip_value(0U);
            return {};
        }
    }

    void append_event(ChartEvent event) {
        if (pass_ != ParsePass::gather) {
            return;
        }
        if (!std::isfinite(event.time_ms)) {
            ++summary_.skipped;
            return;
        }
        if (summary_.events.size() >= maximum_chart_events) {
            throw ImportError("chart event count exceeds the engine limit");
        }
        if (event.name.size() > maximum_chart_event_name_bytes
            || event.value1.size() > maximum_chart_event_value_bytes
            || event.value2.size() > maximum_chart_event_value_bytes
            || event.payload_json.size() > maximum_chart_event_value_bytes) {
            throw ImportError("chart event string exceeds the engine limit");
        }
        summary_.events.push_back(std::move(event));
        if (summary_.events.back().time_ms >= 0.0) {
            const auto time_us = milliseconds_to_microseconds(
                summary_.events.back().time_ms
            );
            if (time_us.has_value() && *time_us >= 0) {
                summary_.content_end_us = std::max(
                    summary_.content_end_us,
                    static_cast<std::uint64_t>(*time_us)
                );
            }
        }
    }

    void parse_event_values(ChartEvent& event, const std::size_t depth) {
        if (peek().kind != TokenKind::left_bracket) {
            event.value1 = read_scalar_event_value();
            return;
        }
        expect(TokenKind::left_bracket, "event value payload must be an array");
        std::size_t index = 0U;
        if (peek().kind != TokenKind::right_bracket) {
            while (true) {
                if (index == 0U) event.value1 = read_scalar_event_value();
                else if (index == 1U) event.value2 = read_scalar_event_value();
                else skip_value(depth + 1U);
                ++index;
                if (peek().kind == TokenKind::right_bracket) break;
                expect(TokenKind::comma, "event value payload requires a comma");
            }
        }
        expect(TokenKind::right_bracket, "event value payload is unterminated");
    }

    void parse_event_object(const std::size_t depth) {
        ChartEvent event;
        std::optional<double> time;
        std::optional<double> time_ms;
        expect(TokenKind::left_brace, "expected native chart event object");
        if (peek().kind != TokenKind::right_brace) {
            while (true) {
                if (peek().kind != TokenKind::string) {
                    throw_here("chart event key must be a string");
                }
                const auto key = take().text;
                expect(TokenKind::colon, "chart event key lacks a colon");
                if (key == "time" || key == "t") {
                    time = read_number();
                } else if (key == "timeMs") {
                    time_ms = read_number();
                } else if (key == "name" || key == "event" || key == "e") {
                    event.name = read_scalar_event_value();
                } else if (key == "value1") {
                    event.value1 = read_scalar_event_value();
                } else if (key == "value2") {
                    event.value2 = read_scalar_event_value();
                } else if (key == "v") {
                    // V-Slice stores event arguments in `v`. Lua/Psych expose
                    // the first two scalar values through value1/value2.
                    parse_event_values(event, depth + 1U);
                } else {
                    skip_value(depth + 1U);
                }
                if (peek().kind == TokenKind::right_brace) break;
                expect(TokenKind::comma, "chart event entries require a comma");
            }
        }
        expect(TokenKind::right_brace, "chart event object is unterminated");
        event.time_ms = time.value_or(time_ms.value_or(0.0));
        append_event(std::move(event));
    }

    void parse_psych_event_entry(const double time_ms, const std::size_t depth) {
        ChartEvent event;
        event.time_ms = time_ms;
        expect(TokenKind::left_bracket, "expected Psych event entry");
        std::size_t index = 0U;
        if (peek().kind != TokenKind::right_bracket) {
            while (true) {
                if (index == 0U) event.name = read_scalar_event_value();
                else if (index == 1U) event.value1 = read_scalar_event_value();
                else if (index == 2U) event.value2 = read_scalar_event_value();
                else skip_value(depth + 1U);
                ++index;
                if (peek().kind == TokenKind::right_bracket) break;
                expect(TokenKind::comma, "Psych event entry requires a comma");
            }
        }
        expect(TokenKind::right_bracket, "Psych event entry is unterminated");
        append_event(std::move(event));
    }

    void parse_psych_event_group(const std::size_t depth) {
        expect(TokenKind::left_bracket, "expected Psych event group");
        const auto time = read_number().value_or(0.0);
        if (peek().kind != TokenKind::right_bracket) {
            expect(TokenKind::comma, "Psych event group requires entries");
            if (peek().kind == TokenKind::left_bracket) {
                (void)take();
                if (peek().kind != TokenKind::right_bracket) {
                    while (true) {
                        if (peek().kind == TokenKind::left_bracket) {
                            parse_psych_event_entry(time, depth + 1U);
                        } else {
                            skip_value(depth + 1U);
                        }
                        if (peek().kind == TokenKind::right_bracket) break;
                        expect(TokenKind::comma, "Psych event list requires a comma");
                    }
                }
                expect(TokenKind::right_bracket, "Psych event list is unterminated");
            } else {
                skip_value(depth + 1U);
            }
            while (peek().kind != TokenKind::right_bracket) {
                expect(TokenKind::comma, "Psych event group requires a comma");
                skip_value(depth + 1U);
            }
        }
        expect(TokenKind::right_bracket, "Psych event group is unterminated");
    }

    void parse_events_array(const std::size_t depth) {
        expect(TokenKind::left_bracket, "chart events field must be an array");
        if (peek().kind == TokenKind::right_bracket) {
            (void)take();
            return;
        }
        while (true) {
            if (peek().kind == TokenKind::left_brace) {
                parse_event_object(depth + 1U);
            } else if (peek().kind == TokenKind::left_bracket) {
                parse_psych_event_group(depth + 1U);
            } else {
                skip_value(depth + 1U);
            }
            if (peek().kind == TokenKind::right_bracket) {
                (void)take();
                return;
            }
            expect(TokenKind::comma, "chart event groups require a comma");
        }
    }

    void gather_string(std::optional<std::string>& destination) {
        const auto value = read_string();
        if (pass_ == ParsePass::gather && value.has_value()) {
            destination = *value;
        }
    }

    void gather_number(std::optional<double>& destination) {
        const auto value = read_number();
        if (pass_ == ParsePass::gather && value.has_value()) {
            destination = *value;
        }
    }

    void parse_header_object(const std::size_t depth) {
        expect(TokenKind::left_brace, "expected header object");
        if (pass_ == ParsePass::gather) {
            summary_.metadata.saw_header_object = true;
        }
        if (peek().kind == TokenKind::right_brace) {
            (void)take();
            return;
        }
        while (true) {
            if (peek().kind != TokenKind::string) {
                throw_here("header key must be a string");
            }
            const auto key = take().text;
            expect(TokenKind::colon, "header key lacks a colon");
            if (key == "song") {
                gather_string(summary_.metadata.header_song);
            } else if (key == "artist") {
                gather_string(summary_.metadata.header_artist);
            } else if (key == "charter") {
                gather_string(summary_.metadata.header_charter);
            } else if (key == "bpm") {
                gather_number(summary_.metadata.header_bpm);
            } else if (key == "needsVoices") {
                const auto value = read_boolean();
                if (pass_ == ParsePass::gather && value.has_value()) {
                    summary_.metadata.needs_voices = *value;
                }
            } else {
                skip_value(depth + 1U);
            }
            if (peek().kind == TokenKind::right_brace) {
                (void)take();
                return;
            }
            expect(TokenKind::comma, "header entries require a comma");
        }
    }

    void parse_audio_object(const std::size_t depth) {
        expect(TokenKind::left_brace, "expected audio object");
        if (peek().kind == TokenKind::right_brace) {
            (void)take();
            return;
        }
        while (true) {
            if (peek().kind != TokenKind::string) {
                throw_here("audio key must be a string");
            }
            const auto key = take().text;
            expect(TokenKind::colon, "audio key lacks a colon");
            if (key == "instrumental") {
                const auto value = read_string();
                if (pass_ == ParsePass::gather && value.has_value()) {
                    summary_.metadata.instrumental =
                        std::filesystem::path(*value);
                }
            } else if (key == "vocals") {
                if (pass_ == ParsePass::gather) {
                    summary_.metadata.audio_vocals_explicit = true;
                }
                if (peek().kind == TokenKind::string) {
                    const auto value = read_string();
                    if (pass_ == ParsePass::gather && value.has_value()) {
                        summary_.metadata.vocals.emplace_back(*value);
                    }
                } else if (peek().kind == TokenKind::left_bracket) {
                    (void)take();
                    if (peek().kind != TokenKind::right_bracket) {
                        while (true) {
                            const auto value = read_string();
                            if (pass_ == ParsePass::gather
                                && value.has_value()) {
                                if (summary_.metadata.vocals.size() >= 8U) {
                                    throw ImportError(
                                        "audio vocal stem count exceeds 8"
                                    );
                                }
                                summary_.metadata.vocals.emplace_back(*value);
                            }
                            if (peek().kind == TokenKind::right_bracket) {
                                break;
                            }
                            expect(
                                TokenKind::comma,
                                "audio vocal entries require a comma"
                            );
                        }
                    }
                    expect(
                        TokenKind::right_bracket,
                        "audio vocals array is unterminated"
                    );
                } else {
                    skip_value(depth + 1U);
                }
            } else {
                skip_value(depth + 1U);
            }
            if (peek().kind == TokenKind::right_brace) {
                (void)take();
                return;
            }
            expect(TokenKind::comma, "audio entries require a comma");
        }
    }

    void parse_assets_object(const std::size_t depth) {
        expect(TokenKind::left_brace, "expected assets object");
        if (peek().kind == TokenKind::right_brace) {
            (void)take();
            return;
        }
        while (true) {
            if (peek().kind != TokenKind::string) {
                throw_here("assets key must be a string");
            }
            const auto key = take().text;
            expect(TokenKind::colon, "assets key lacks a colon");
            if (key == "player1") {
                gather_string(summary_.metadata.player);
            } else if (key == "player2") {
                gather_string(summary_.metadata.opponent);
            } else if (key == "player4") {
                gather_string(summary_.metadata.secondary_opponent);
            } else if (key == "enablePlayer4") {
                const auto value = read_boolean();
                if (pass_ == ParsePass::gather && value.has_value()) {
                    summary_.metadata.secondary_opponent_enabled = *value;
                }
            } else if (key == "gfVersion" || key == "player3") {
                gather_string(summary_.metadata.girlfriend);
            } else if (key == "arrowSkin" || key == "noteStyle") {
                gather_string(summary_.metadata.note_style);
            } else if (key == "stage") {
                gather_string(summary_.metadata.stage);
            } else {
                skip_value(depth + 1U);
            }
            if (peek().kind == TokenKind::right_brace) {
                (void)take();
                return;
            }
            expect(TokenKind::comma, "assets entries require a comma");
        }
    }

    void parse_container_object(const bool nested_song, const std::size_t depth) {
        if (depth > options_.max_json_depth) {
            throw_here("JSON nesting exceeds configured depth");
        }
        expect(TokenKind::left_brace, "expected chart object");
        if (peek().kind == TokenKind::right_brace) {
            (void)take();
            return;
        }
        while (true) {
            if (peek().kind != TokenKind::string) {
                throw_here("chart object key must be a string");
            }
            const auto key = take().text;
            expect(TokenKind::colon, "chart object key lacks a colon");
            if (!nested_song && key == "song"
                && peek().kind == TokenKind::left_brace) {
                parse_container_object(true, depth + 1U);
            } else if (key == "notes") {
                parse_notes_array(depth + 1U, nested_song);
            } else if (key == "events" && peek().kind == TokenKind::left_bracket) {
                if (pass_ == ParsePass::gather) {
                    parse_events_array(depth + 1U);
                } else {
                    skip_value(depth + 1U);
                }
            } else if (key == "header"
                && peek().kind == TokenKind::left_brace) {
                parse_header_object(depth + 1U);
            } else if (key == "keyCount") {
                const auto value = read_integer();
                if (pass_ == ParsePass::gather && value.has_value()) {
                    summary_.explicit_key_count = value;
                }
            } else if (key == "mania") {
                const auto value = read_integer();
                if (pass_ == ParsePass::gather && value.has_value()) {
                    summary_.song_mania = value;
                }
            } else if (key == "options" && peek().kind == TokenKind::left_brace) {
                parse_options_object(depth + 1U);
            } else if (key == "audio"
                && peek().kind == TokenKind::left_brace) {
                parse_audio_object(depth + 1U);
            } else if (key == "assets"
                && peek().kind == TokenKind::left_brace) {
                parse_assets_object(depth + 1U);
            } else if (key == "title") {
                gather_string(summary_.metadata.title);
            } else if (key == "name") {
                gather_string(summary_.metadata.name);
            } else if (key == "song") {
                gather_string(summary_.metadata.song);
            } else if (key == "artist") {
                gather_string(summary_.metadata.artist);
            } else if (key == "charter") {
                gather_string(summary_.metadata.charter);
            } else if (key == "difficulty") {
                gather_string(summary_.metadata.difficulty);
            } else if (key == "stage") {
                gather_string(summary_.metadata.stage);
            } else if (key == "player1") {
                gather_string(summary_.metadata.player);
            } else if (key == "player2") {
                gather_string(summary_.metadata.opponent);
            } else if (key == "gfVersion" || key == "girlfriend") {
                gather_string(summary_.metadata.girlfriend);
            } else if (key == "noteStyle" || key == "arrowSkin") {
                gather_string(summary_.metadata.note_style);
            } else if (key == "bpm") {
                gather_number(summary_.metadata.song_bpm);
            } else if (key == "speed" || key == "scrollSpeed") {
                gather_number(summary_.metadata.song_speed);
            } else if (key == "needsVoices") {
                const auto value = read_boolean();
                if (pass_ == ParsePass::gather && value.has_value()) {
                    summary_.metadata.needs_voices = *value;
                }
            } else {
                skip_value(depth + 1U);
            }
            if (peek().kind == TokenKind::right_brace) {
                (void)take();
                return;
            }
            expect(TokenKind::comma, "chart object entries require a comma");
        }
    }

    void parse_options_object(const std::size_t depth) {
        expect(TokenKind::left_brace, "expected options object");
        if (pass_ == ParsePass::gather) {
            summary_.metadata.saw_options_object = true;
        }
        if (peek().kind == TokenKind::right_brace) {
            (void)take();
            return;
        }
        while (true) {
            if (peek().kind != TokenKind::string) {
                throw_here("options key must be a string");
            }
            const auto key = take().text;
            expect(TokenKind::colon, "options key lacks a colon");
            if (key == "mania") {
                const auto value = read_integer();
                if (pass_ == ParsePass::gather && value.has_value()) {
                    summary_.option_mania = value;
                }
            } else if (key == "speed") {
                gather_number(summary_.metadata.option_speed);
            } else {
                skip_value(depth + 1U);
            }
            if (peek().kind == TokenKind::right_brace) {
                (void)take();
                return;
            }
            expect(TokenKind::comma, "options entries require a comma");
        }
    }

    void ensure_shape(const NoteShape shape) {
        if (pass_ == ParsePass::emit) {
            if (summary_.shape != shape) {
                throw ImportError("chart note shape changed between streaming passes");
            }
            return;
        }
        if (summary_.shape == NoteShape::unknown) {
            summary_.shape = shape;
            summary_.format = shape == NoteShape::psych
                ? ChartFormat::psych
                : ChartFormat::native;
        } else if (summary_.shape != shape) {
            throw ImportError("chart mixes native notes with Psych sections");
        }
    }

    void parse_notes_array(
        const std::size_t depth,
        const bool nested_song
    ) {
        ++notes_containers_;
        expect(TokenKind::left_bracket, "chart notes field must be an array");
        if (peek().kind == TokenKind::right_bracket) {
            (void)take();
            if (nested_song && pass_ == ParsePass::gather) {
                // In the supported schemas native notes live at $.notes,
                // whereas Psych section arrays live at $.song.notes.
                ensure_shape(NoteShape::psych);
            }
            return;
        }
        while (true) {
            saw_note_array_item_ = true;
            if (peek().kind == TokenKind::left_brace) {
                parse_note_or_section(depth + 1U);
            } else {
                if (pass_ == ParsePass::gather) {
                    ++summary_.skipped;
                }
                skip_value(depth + 1U);
            }
            if (peek().kind == TokenKind::right_bracket) {
                (void)take();
                return;
            }
            expect(TokenKind::comma, "chart note entries require a comma");
        }
    }

    void parse_note_or_section(const std::size_t depth) {
        NativeCandidate native;
        bool saw_section_notes = false;
        bool must_hit = false;
        bool saw_must_hit = false;
        bool change_bpm = false;
        std::optional<double> section_bpm;
        double length_in_steps = 16.0;
        const auto known_must_hit = pass_ == ParsePass::emit
            && section_index_ < summary_.section_must_hit.size()
            ? summary_.section_must_hit[section_index_]
            : false;

        expect(TokenKind::left_brace, "expected note/section object");
        if (peek().kind == TokenKind::right_brace) {
            (void)take();
            if (pass_ == ParsePass::gather) {
                ++summary_.skipped;
            }
            return;
        }
        while (true) {
            if (peek().kind != TokenKind::string) {
                throw_here("note/section key must be a string");
            }
            const auto key = take().text;
            expect(TokenKind::colon, "note/section key lacks a colon");
            if (key == "sectionNotes") {
                ensure_shape(NoteShape::psych);
                saw_section_notes = true;
                parse_psych_note_array(known_must_hit, depth + 1U);
            } else if (key == "mustHitSection") {
                const auto parsed = read_boolean();
                saw_must_hit = parsed.has_value();
                must_hit = parsed.value_or(false);
            } else if (key == "changeBPM") {
                change_bpm = read_boolean().value_or(false);
            } else if (key == "bpm") {
                section_bpm = read_number();
            } else if (key == "lengthInSteps") {
                length_in_steps = read_number().value_or(16.0);
            } else if (key == "time") {
                native.time = read_number();
                native.saw_field = true;
            } else if (key == "timeMs") {
                native.time_ms = read_number();
                native.saw_field = true;
            } else if (key == "duration") {
                native.duration = read_duration_number();
                native.saw_field = true;
            } else if (key == "durationMs") {
                native.duration_ms = read_duration_number();
                native.saw_field = true;
            } else if (key == "lane") {
                native.lane = read_integer();
                native.saw_field = true;
            } else if (key == "owner") {
                native.owner = read_string();
                native.saw_field = true;
            } else if (key == "kind") {
                native.kind = read_string();
                native.saw_field = true;
            } else if (key == "type") {
                native.type = read_string();
                native.saw_field = true;
            } else {
                skip_value(depth + 1U);
            }
            if (peek().kind == TokenKind::right_brace) {
                (void)take();
                break;
            }
            expect(TokenKind::comma, "note/section entries require a comma");
        }

        if (saw_section_notes) {
            if (pass_ == ParsePass::gather) {
                if (summary_.section_must_hit.size() >= options_.max_sections) {
                    throw ImportError("Psych section count exceeds configured limit");
                }
                summary_.section_must_hit.push_back(
                    saw_must_hit ? must_hit : false
                );
                if (!std::isfinite(length_in_steps)
                    || length_in_steps < 0.0) {
                    throw ImportError(
                        "Psych section lengthInSteps is invalid"
                    );
                }
                if (change_bpm && section_bpm.has_value()) {
                    if (summary_.metadata.psych_tempo_changes.size()
                        >= maximum_chart_tempo_changes) {
                        throw ImportError(
                            "Psych tempo change count exceeds its limit"
                        );
                    }
                    summary_.metadata.psych_tempo_changes.push_back({
                        summary_.metadata.psych_section_step,
                        *section_bpm,
                    });
                }
                summary_.metadata.psych_section_step += length_in_steps;
                if (!std::isfinite(
                        summary_.metadata.psych_section_step
                    )) {
                    throw ImportError("Psych section timeline overflowed");
                }
            } else {
                if (section_index_ >= summary_.section_must_hit.size()
                    || summary_.section_must_hit[section_index_]
                        != (saw_must_hit ? must_hit : false)) {
                    throw ImportError("Psych section ownership changed between streaming passes");
                }
            }
            ++section_index_;
            return;
        }
        if (native.saw_field) {
            ensure_shape(NoteShape::native);
            process_native(native);
        } else if (pass_ == ParsePass::gather) {
            ++summary_.skipped;
        }
    }

    void parse_psych_note_array(
        const bool must_hit,
        const std::size_t depth
    ) {
        if (peek().kind != TokenKind::left_bracket) {
            if (pass_ == ParsePass::gather) {
                ++summary_.skipped;
            }
            skip_value(depth);
            return;
        }
        (void)take();
        if (peek().kind == TokenKind::right_bracket) {
            (void)take();
            return;
        }
        while (true) {
            if (peek().kind == TokenKind::left_bracket) {
                parse_psych_note(must_hit, depth + 1U);
            } else {
                if (pass_ == ParsePass::gather) {
                    ++summary_.skipped;
                }
                skip_value(depth + 1U);
            }
            if (peek().kind == TokenKind::right_bracket) {
                (void)take();
                return;
            }
            expect(TokenKind::comma, "Psych section note entries require a comma");
        }
    }

    void parse_psych_note(const bool must_hit, const std::size_t depth) {
        PsychCandidate candidate;
        expect(TokenKind::left_bracket, "expected Psych note tuple");
        std::size_t index = 0U;
        if (peek().kind != TokenKind::right_bracket) {
            while (true) {
                switch (index) {
                case 0U: candidate.time = read_number(); break;
                case 1U: candidate.raw_lane = read_integer(); break;
                case 2U:
                    if (candidate.raw_lane.has_value() && *candidate.raw_lane < 0)
                        candidate.event_name = read_scalar_event_value();
                    else
                        candidate.duration = read_duration_number();
                    break;
                case 3U:
                    if (candidate.raw_lane.has_value() && *candidate.raw_lane < 0)
                        candidate.event_value1 = read_scalar_event_value();
                    else
                        candidate.kind = read_scalar_kind();
                    break;
                case 4U:
                    if (candidate.raw_lane.has_value() && *candidate.raw_lane < 0)
                        candidate.event_value2 = read_scalar_event_value();
                    else
                        skip_value(depth + 1U);
                    break;
                default: skip_value(depth + 1U); break;
                }
                ++index;
                if (peek().kind == TokenKind::right_bracket) {
                    break;
                }
                expect(TokenKind::comma, "Psych note tuple entries require a comma");
            }
        }
        expect(TokenKind::right_bracket, "Psych note tuple is unterminated");
        if (candidate.raw_lane.has_value() && *candidate.raw_lane < 0) {
            if (candidate.time.has_value() && !candidate.event_name.empty()) {
                append_event({
                    *candidate.time,
                    std::move(candidate.event_name),
                    std::move(candidate.event_value1),
                    std::move(candidate.event_value2),
                    {},
                });
            }
            return;
        }
        process_psych(candidate, must_hit);
    }

    void register_kind(const std::string& kind) {
        if (summary_.kind_ids.contains(kind)) {
            return;
        }
        if (kind.empty()
            || kind.size() > options_.packed_limits.max_kind_bytes
            || summary_.kinds.size() >= options_.packed_limits.max_kinds
            || summary_.kinds.size()
                >= static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())) {
            throw ImportError("note kind dictionary exceeds PFC1 limits");
        }
        const auto id = static_cast<std::uint32_t>(summary_.kinds.size());
        summary_.kinds.push_back(kind);
        summary_.kind_ids.emplace(summary_.kinds.back(), id);
    }

    void flush_pending_coincident() {
        if (!summary_.pending_coincident.has_value()) {
            return;
        }

        const auto candidate = *summary_.pending_coincident;
        summary_.pending_coincident.reset();

        // Phase B deliberately compresses only exact tap stacks. Sustains retain
        // their explicit gameplay/tick semantics until a dedicated run path is
        // implemented and tested.
        if (candidate.duration_us != 0U
            || candidate.count < minimum_coincident_pattern_notes
            || summary_.coincident_patterns.size()
                >= options_.packed_limits.max_patterns) {
            return;
        }

        summary_.coincident_patterns.push_back(candidate);
        if (!checked_add(
                summary_.compressed_note_count,
                candidate.count,
                summary_.compressed_note_count
            )) {
            throw ImportError("compressed logical note count overflows");
        }
    }

    void flush_pending_arithmetic() {
        if (!summary_.pending_arithmetic.has_value()) {
            return;
        }

        auto pending = std::move(*summary_.pending_arithmetic);
        summary_.pending_arithmetic.reset();
        const auto& candidate = pending.candidate;
        if (!pending.interval_known
            || !pending.lane_period_locked
            || candidate.interval_us == 0U
            || candidate.duration_us != 0U
            || candidate.count < minimum_arithmetic_pattern_notes
            || candidate.raw_lane_pattern.empty()
            || summary_.coincident_patterns.size()
                    + summary_.arithmetic_patterns.size()
                >= options_.packed_limits.max_patterns) {
            return;
        }

        const auto compressed_count = candidate.count;
        summary_.arithmetic_patterns.push_back(std::move(pending.candidate));
        if (!checked_add(
                summary_.compressed_note_count,
                compressed_count,
                summary_.compressed_note_count
            )) {
            throw ImportError("compressed arithmetic note count overflows");
        }
    }

    void start_arithmetic_candidate(
        const CoincidentPatternCandidate& current
    ) {
        PendingArithmeticPattern pending;
        pending.candidate.first_sequence = current.first_sequence;
        pending.candidate.count = 1U;
        pending.candidate.start_us = current.time_us;
        pending.candidate.duration_us = current.duration_us;
        pending.candidate.kind_id = current.kind_id;
        pending.candidate.owner_mode = current.owner_mode;
        pending.candidate.raw_lane_pattern.push_back(current.raw_lane);
        pending.last_time_us = current.time_us;
        summary_.pending_arithmetic = std::move(pending);
    }

    void observe_arithmetic_candidate(
        const CoincidentPatternCandidate& current
    ) {
        // C15 deliberately targets tap streams. Sustains remain explicit so
        // their hold/tick semantics stay exact. Coincident stacks are handled
        // independently by Phase B (interval == 0).
        if (current.duration_us != 0U) {
            flush_pending_arithmetic();
            return;
        }
        if (!summary_.pending_arithmetic.has_value()) {
            start_arithmetic_candidate(current);
            return;
        }

        auto& pending = *summary_.pending_arithmetic;
        auto& candidate = pending.candidate;
        const bool compatible = candidate.duration_us == 0U
            && candidate.kind_id == current.kind_id
            && candidate.owner_mode == current.owner_mode
            && current.time_us > pending.last_time_us;
        if (!compatible) {
            flush_pending_arithmetic();
            start_arithmetic_candidate(current);
            return;
        }

        const auto signed_delta = current.time_us - pending.last_time_us;
        if (signed_delta <= 0) {
            flush_pending_arithmetic();
            start_arithmetic_candidate(current);
            return;
        }
        const auto delta = static_cast<std::uint64_t>(signed_delta);
        if (!pending.interval_known) {
            candidate.interval_us = delta;
            pending.interval_known = true;
        } else if (candidate.interval_us != delta) {
            flush_pending_arithmetic();
            start_arithmetic_candidate(current);
            return;
        }

        if (pending.lane_period_locked) {
            const auto period = candidate.raw_lane_pattern.size();
            if (period == 0U
                || current.raw_lane
                    != candidate.raw_lane_pattern[candidate.count % period]) {
                flush_pending_arithmetic();
                start_arithmetic_candidate(current);
                return;
            }
        } else {
            if (candidate.raw_lane_pattern.size()
                >= maximum_arithmetic_lane_probe) {
                flush_pending_arithmetic();
                start_arithmetic_candidate(current);
                return;
            }
            candidate.raw_lane_pattern.push_back(current.raw_lane);
            const auto observed = candidate.raw_lane_pattern.size();
            if ((observed % 2U) == 0U) {
                const auto half = observed / 2U;
                if (std::equal(
                        candidate.raw_lane_pattern.begin(),
                        candidate.raw_lane_pattern.begin()
                            + static_cast<std::ptrdiff_t>(half),
                        candidate.raw_lane_pattern.begin()
                            + static_cast<std::ptrdiff_t>(half)
                    )) {
                    candidate.raw_lane_pattern.resize(half);
                    pending.lane_period_locked = true;
                }
            }
        }

        ++candidate.count;
        pending.last_time_us = current.time_us;
    }

    void gather_note(
        const std::int64_t time_us,
        const std::uint64_t duration_us,
        const std::uint64_t raw_lane,
        const std::string& kind,
        const RawOwnerMode owner_mode
    ) {
        if (summary_.note_count == std::numeric_limits<std::uint64_t>::max()) {
            throw ImportError("logical note counter exhausted its uint64 representation");
        }
        if (summary_.note_count >= options_.max_notes) {
            throw ImportError("logical note count exceeds the configured import policy");
        }
        if (summary_.note_count
            >= options_.packed_limits.max_logical_notes) {
            throw ImportError("logical note count exceeds the configured PFC1 policy");
        }
        if (summary_.last_time.has_value() && *summary_.last_time > time_us) {
            summary_.sorted = false;
        }
        summary_.last_time = time_us;

        register_kind(kind);
        const auto kind_iterator = summary_.kind_ids.find(kind);
        if (kind_iterator == summary_.kind_ids.end()) {
            throw ImportError("internal note kind registration failed");
        }

        const CoincidentPatternCandidate current{
            summary_.note_count,
            1U,
            time_us,
            duration_us,
            raw_lane,
            kind_iterator->second,
            owner_mode,
        };
        observe_arithmetic_candidate(current);

        const auto same = [](
            const CoincidentPatternCandidate& left,
            const CoincidentPatternCandidate& right
        ) noexcept {
            return left.time_us == right.time_us
                && left.duration_us == right.duration_us
                && left.raw_lane == right.raw_lane
                && left.kind_id == right.kind_id
                && left.owner_mode == right.owner_mode;
        };

        if (summary_.pending_coincident.has_value()
            && same(*summary_.pending_coincident, current)) {
            if (summary_.pending_coincident->count
                == std::numeric_limits<std::uint64_t>::max()) {
                throw ImportError("coincident note count overflows");
            }
            ++summary_.pending_coincident->count;
        } else {
            flush_pending_coincident();
            summary_.pending_coincident = current;
        }

        ++summary_.note_count;
        summary_.saw_lane = true;
        summary_.max_raw_lane = std::max(summary_.max_raw_lane, raw_lane);
        if (time_us >= 0) {
            const auto start = static_cast<std::uint64_t>(time_us);
            const auto end = duration_us
                    > std::numeric_limits<std::uint64_t>::max() - start
                ? std::numeric_limits<std::uint64_t>::max()
                : start + duration_us;
            summary_.content_end_us = std::max(
                summary_.content_end_us,
                end
            );
        }
    }

    void process_native(const NativeCandidate& candidate) {
        const auto time = candidate.time.has_value()
            ? candidate.time
            : candidate.time_ms;
        const auto duration = candidate.duration.has_value()
            ? candidate.duration
            : candidate.duration_ms;
        const auto time_us = time.has_value()
            ? milliseconds_to_microseconds(*time)
            : std::nullopt;
        const auto duration_us = duration_to_microseconds(
            duration.value_or(0.0)
        );
        if (!time_us.has_value()
            || !duration_us.has_value()
            || !candidate.lane.has_value()
            || *candidate.lane < 0) {
            if (pass_ == ParsePass::gather) {
                ++summary_.skipped;
            }
            return;
        }
        auto kind = candidate.kind.has_value()
            ? *candidate.kind
            : candidate.type.value_or("normal");
        if (kind.empty() || kind == "0") {
            kind = "normal";
        }
        const auto raw_lane = static_cast<std::uint64_t>(*candidate.lane);
        const auto owner = candidate.owner.value_or("player");
        const auto owner_mode = owner == "opponent" || owner == "enemy"
            ? RawOwnerMode::native_opponent
            : RawOwnerMode::native_player;
        if (pass_ == ParsePass::gather) {
            gather_note(
                *time_us,
                *duration_us,
                raw_lane,
                kind,
                owner_mode
            );
            return;
        }
        if (raw_lane >= summary_.key_count) {
            throw ImportError("native note lane exceeds chart key count");
        }
        const auto kind_iterator = summary_.kind_ids.find(kind);
        if (kind_iterator == summary_.kind_ids.end()) {
            throw ImportError("note kind changed between streaming passes");
        }
        emit(PackedNote{
            *time_us,
            *duration_us,
            static_cast<std::uint16_t>(raw_lane),
            owner == "opponent" || owner == "enemy"
                ? PackedNoteOwner::opponent
                : PackedNoteOwner::player,
            0U,
            kind_iterator->second,
        });
    }

    void process_psych(
        const PsychCandidate& candidate,
        const bool must_hit
    ) {
        const auto time_us = candidate.time.has_value()
            ? milliseconds_to_microseconds(*candidate.time)
            : std::nullopt;
        const long double raw_duration = candidate.duration.value_or(0.0L);
        const long double psych_duration = raw_duration < 0.0L
            ? 0.0L
            : raw_duration;
        const auto duration_us = duration_to_microseconds(
            psych_duration
        );
        if (!time_us.has_value()
            || !duration_us.has_value()
            || !candidate.raw_lane.has_value()
            || *candidate.raw_lane < 0) {
            if (pass_ == ParsePass::gather) {
                ++summary_.skipped;
            }
            return;
        }
        const auto raw_lane = static_cast<std::uint64_t>(*candidate.raw_lane);
        if (pass_ == ParsePass::gather) {
            gather_note(
                *time_us,
                *duration_us,
                raw_lane,
                candidate.kind,
                must_hit
                    ? RawOwnerMode::psych_must_hit_true
                    : RawOwnerMode::psych_must_hit_false
            );
            return;
        }
        const auto lane_domain = static_cast<std::uint64_t>(summary_.key_count) * 2U;
        if (raw_lane >= lane_domain) {
            throw ImportError("Psych note lane exceeds both chart strumlines");
        }
        const auto other_side = raw_lane >= summary_.key_count;
        const auto player = other_side ? !must_hit : must_hit;
        const auto kind_iterator = summary_.kind_ids.find(candidate.kind);
        if (kind_iterator == summary_.kind_ids.end()) {
            throw ImportError("note kind changed between streaming passes");
        }
        // PULSEFORGE_P1_4_0_STREAMING_THIRD_STRUM_IDENTITY_V1
        // PULSEFORGE_P1_4_0D_STREAMING_THIRD_STRUM_IMPORT_PARITY_V1
        // PFC1 v1 deliberately keeps its historical two-value physical owner
        // field. Third Strum remains losslessly identifiable through the kind
        // dictionary and is projected back to secondary_opponent at runtime.
        // Treat the canonical note type identically for Psych and DenpaEx.
        const auto packed_owner = candidate.kind == "Third Strum"
            ? PackedNoteOwner::opponent
            : player ? PackedNoteOwner::player : PackedNoteOwner::opponent;
        emit(PackedNote{
            *time_us,
            *duration_us,
            static_cast<std::uint16_t>(raw_lane % summary_.key_count),
            packed_owner,
            0U,
            kind_iterator->second,
        });
    }

    void emit(const PackedNote& note) {
        if (!consumer_) {
            throw ImportError("streaming note consumer is unavailable");
        }
        consumer_(note, emit_sequence_++);
    }

    JsonLexer lexer_;
    const StreamingChartImportOptions& options_;
    ImportSummary& summary_;
    ParsePass pass_;
    NoteConsumer consumer_;
    std::optional<Token> lookahead_;
    std::size_t notes_containers_{};
    std::size_t section_index_{};
    std::uint64_t emit_sequence_{};
    bool saw_note_array_item_{};
};

void finalize_summary(
    ImportSummary& summary,
    const StreamingChartImportOptions& options
) {
    if (summary.shape == NoteShape::psych) {
        if (summary.metadata.saw_header_object
            && summary.metadata.saw_options_object) {
            summary.format = ChartFormat::denpa;
        }
        std::optional<std::int64_t> declared_key_count;
        const auto absorb_lane_count = [&](
            const std::optional<std::int64_t> value
        ) {
            if (!value.has_value() || *value <= 0) {
                return;
            }
            if (*value > maximum_supported_key_count) {
                throw ImportError(
                    "Psych key count is outside the supported 1..18 range"
                );
            }
            declared_key_count = declared_key_count.has_value()
                ? std::max(*declared_key_count, *value)
                : *value;
        };
        absorb_lane_count(summary.explicit_key_count);
        absorb_lane_count(summary.song_mania);
        if (summary.option_mania.has_value()) {
            const auto mania_index = *summary.option_mania;
            if (mania_index < 0
                || mania_index >= maximum_supported_key_count) {
                throw ImportError("Psych options.mania index is outside 0..17");
            }
            const auto option_key_count = mania_index + 1;
            declared_key_count = declared_key_count.has_value()
                ? std::max(*declared_key_count, option_key_count)
                : option_key_count;
        }
        auto key_count = declared_key_count.value_or(4);
        if (summary.saw_lane) {
            const auto required = summary.max_raw_lane / 2U + 1U;
            if (required > maximum_supported_key_count) {
                throw ImportError(
                    "Psych note lane exceeds the supported 18K domain"
                );
            }
            key_count = std::max<std::int64_t>(
                key_count,
                static_cast<std::int64_t>(required)
            );
        }
        summary.key_count = static_cast<std::uint16_t>(key_count);
    } else {
        auto key_count = summary.explicit_key_count.value_or(4);
        if (key_count < 1
            || key_count > maximum_supported_key_count) {
            throw ImportError("native key count is outside the supported 1..18 range");
        }
        if (summary.saw_lane) {
            if (summary.max_raw_lane >= maximum_supported_key_count) {
                throw ImportError(
                    "native note lane exceeds the supported 18K domain"
                );
            }
            key_count = std::max<std::int64_t>(
                key_count,
                static_cast<std::int64_t>(summary.max_raw_lane + 1U)
            );
        }
        summary.key_count = static_cast<std::uint16_t>(key_count);
    }
    if (!summary.sorted) {
        summary.coincident_patterns.clear();
        summary.arithmetic_patterns.clear();
        summary.compressed_note_count = 0U;
    } else {
        // Psych PatternRun has one owner for the complete run. Discard any
        // arithmetic candidate whose raw lane period crosses the two Psych
        // strumlines after the final key count becomes known. Those notes stay
        // explicit and therefore preserve exact ownership semantics.
        auto write = summary.arithmetic_patterns.begin();
        for (auto read = summary.arithmetic_patterns.begin();
             read != summary.arithmetic_patterns.end(); ++read) {
            bool valid = !read->raw_lane_pattern.empty();
            if (valid && summary.shape == NoteShape::psych) {
                const auto lane_domain = static_cast<std::uint64_t>(
                    summary.key_count
                ) * 2U;
                std::optional<bool> side;
                for (const auto raw_lane : read->raw_lane_pattern) {
                    if (raw_lane >= lane_domain) {
                        valid = false;
                        break;
                    }
                    const bool current_side = raw_lane >= summary.key_count;
                    if (side.has_value() && *side != current_side) {
                        valid = false;
                        break;
                    }
                    side = current_side;
                }
            } else if (valid) {
                valid = std::all_of(
                    read->raw_lane_pattern.begin(),
                    read->raw_lane_pattern.end(),
                    [&summary](const std::uint64_t lane) {
                        return lane < summary.key_count;
                    }
                );
            }
            if (!valid) {
                if (read->count > summary.compressed_note_count) {
                    throw ImportError("compressed arithmetic accounting underflows");
                }
                summary.compressed_note_count -= read->count;
                continue;
            }
            if (write != read) {
                *write = std::move(*read);
            }
            ++write;
        }
        summary.arithmetic_patterns.erase(write, summary.arithmetic_patterns.end());
    }
    if (summary.compressed_note_count > summary.note_count) {
        throw ImportError("compressed note accounting is invalid");
    }
    const auto explicit_note_count =
        summary.note_count - summary.compressed_note_count;
    if (summary.note_count > options.max_notes) {
        throw ImportError("chart note count exceeds the configured import policy");
    }
    if (summary.note_count > options.packed_limits.max_logical_notes) {
        throw ImportError("chart logical count exceeds the configured PFC1 policy");
    }
    if (explicit_note_count > options.packed_limits.max_explicit_notes) {
        throw ImportError("chart explicit count exceeds the configured PFC1 policy");
    }
}

[[nodiscard]] std::string optional_or(
    const std::optional<std::string>& primary,
    const std::optional<std::string>& fallback,
    const std::string_view default_value
) {
    if (primary.has_value() && !primary->empty()) {
        return *primary;
    }
    if (fallback.has_value() && !fallback->empty()) {
        return *fallback;
    }
    return std::string(default_value);
}

[[nodiscard]] Chart make_chart_metadata(const ImportSummary& summary) {
    const auto& source = summary.metadata;
    Chart chart;
    chart.source_format = summary.format;
    chart.key_count = summary.key_count;

    const auto scalar_song = source.song.has_value()
        ? source.song
        : source.name;
    chart.title = optional_or(
        source.header_song,
        source.title.has_value() ? source.title : scalar_song,
        "Untitled"
    );
    chart.artist = optional_or(
        source.header_artist,
        source.artist,
        "Unknown"
    );
    chart.charter = optional_or(
        source.header_charter,
        source.charter,
        "Unknown"
    );
    chart.difficulty = source.difficulty.value_or("normal");
    chart.stage_id = source.stage.value_or(std::string{});
    chart.player_character = source.player.value_or(std::string{});
    chart.opponent_character = source.opponent.value_or(std::string{});
    chart.secondary_opponent_character = source.secondary_opponent.value_or(
        std::string{}
    );
    chart.secondary_opponent_enabled = source.secondary_opponent_enabled.value_or(
        false
    );
    if (summary.format == ChartFormat::denpa
        && std::find(summary.kinds.begin(), summary.kinds.end(), "Third Strum")
            != summary.kinds.end()) {
        chart.secondary_opponent_enabled = true;
    }
    if (chart.secondary_opponent_enabled
        && chart.secondary_opponent_character.empty()) {
        chart.secondary_opponent_character = chart.opponent_character;
    }
    chart.girlfriend_character = source.girlfriend.value_or(std::string{});
    chart.note_style = source.note_style.value_or(std::string{});
    chart.chart_scroll_speed = source.option_speed.value_or(
        source.song_speed.value_or(1.0)
    );
    chart.audio.instrumental = source.instrumental;
    chart.audio.vocals = source.vocals;
    chart.events = summary.events;

    double current_bpm = source.header_bpm.value_or(
        source.song_bpm.value_or(120.0)
    );
    if (!std::isfinite(current_bpm)
    || current_bpm <= 0.0) {
    throw ImportError("chart BPM must be finite and positive");
    }
    chart.tempos.push_back({0.0, current_bpm, 4U, 4U});
    double previous_step = 0.0;
    double time_ms = 0.0;
for (const auto& change : source.psych_tempo_changes) {
    if (!std::isfinite(change.start_step)) {
        throw ImportError(
            "Psych tempo start_step is non-finite: "
            + std::to_string(change.start_step)
        );
    }

    if (change.start_step < previous_step) {
        throw ImportError(
            "Psych tempo timeline goes backwards: start_step="
            + std::to_string(change.start_step)
            + ", previous_step="
            + std::to_string(previous_step)
        );
    }

    if (!std::isfinite(change.bpm)) {
        throw ImportError(
            "Psych BPM is non-finite"
        );
    }

    if (change.bpm <= 0.0) {
    throw ImportError(
        "Psych BPM must be positive: "
        + std::to_string(change.bpm));
    }
        time_ms += (change.start_step - previous_step)
            * (60'000.0 / current_bpm) / 4.0;
        if (!std::isfinite(time_ms)) {
            throw ImportError("Psych tempo timeline overflowed");
        }
        if (std::abs(change.bpm - current_bpm) > 0.0001) {
            current_bpm = change.bpm;
            chart.tempos.push_back({time_ms, current_bpm, 4U, 4U});
        }
        previous_step = change.start_step;
    }
    chart.normalize();
    for (const auto& issue : validate_chart(chart)) {
        if (issue.severity == ValidationSeverity::error) {
            throw ImportError(
                "streaming chart metadata is invalid: " + issue.message
            );
        }
    }
    return chart;
}

[[nodiscard]] std::string requested_song_id(const ImportSummary& summary) {
    const auto& source = summary.metadata;
    if (source.header_song.has_value() && !source.header_song->empty()) {
        return *source.header_song;
    }
    if (source.song.has_value() && !source.song->empty()) {
        return *source.song;
    }
    if (source.title.has_value() && !source.title->empty()) {
        return *source.title;
    }
    if (source.name.has_value() && !source.name->empty()) {
        return *source.name;
    }
    return {};
}

struct SequencedNote final {
    PackedNote note;
    std::uint64_t sequence{};
};

[[nodiscard]] bool ordered_before(
    const SequencedNote& left,
    const SequencedNote& right
) noexcept {
    return left.note.time_us < right.note.time_us
        || (left.note.time_us == right.note.time_us
            && left.sequence < right.sequence);
}

void radix_sort_by_time(
    std::vector<SequencedNote>& notes
) {
    if (notes.size() < 2U) {
        return;
    }

    std::vector<SequencedNote> scratch(notes.size());

    // Stable LSD radix sort over the signed 64-bit timestamp. Since input
    // sequence order is stable, equal timestamps retain their sequence order.
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        std::array<std::size_t, 256U> counts{};

        for (const auto& item : notes) {
            const auto key = std::bit_cast<std::uint64_t>(item.note.time_us)
                ^ 0x8000000000000000ULL;
            const auto bucket = static_cast<std::uint8_t>(key >> shift);
            ++counts[bucket];
        }

        std::array<std::size_t, 256U> offsets{};
        std::size_t position = 0U;
        for (std::size_t bucket = 0U; bucket < counts.size(); ++bucket) {
            offsets[bucket] = position;
            position += counts[bucket];
        }

        for (auto& item : notes) {
            const auto key = std::bit_cast<std::uint64_t>(item.note.time_us)
                ^ 0x8000000000000000ULL;
            const auto bucket = static_cast<std::uint8_t>(key >> shift);
            scratch[offsets[bucket]++] = std::move(item);
        }

        notes.swap(scratch);
    }
}

template <typename Unsigned>
void encode_le(char*& output, const Unsigned value) noexcept {
    static_assert(std::is_unsigned_v<Unsigned>);
    for (std::size_t index = 0U; index < sizeof(Unsigned); ++index) {
        *output++ = static_cast<char>(static_cast<std::uint8_t>(
            (value >> static_cast<unsigned int>(index * 8U))
                & static_cast<Unsigned>(0xFFU)
        ));
    }
}

template <typename Unsigned>
[[nodiscard]] Unsigned decode_le(const char*& input) noexcept {
    static_assert(std::is_unsigned_v<Unsigned>);
    Unsigned value{};
    for (std::size_t index = 0U; index < sizeof(Unsigned); ++index) {
        value |= static_cast<Unsigned>(static_cast<std::uint8_t>(*input++))
            << static_cast<unsigned int>(index * 8U);
    }
    return value;
}

constexpr std::size_t sort_record_bytes = 33U;

[[nodiscard]] std::array<char, sort_record_bytes> encode_sort_record(
    const SequencedNote& value
) noexcept {
    std::array<char, sort_record_bytes> bytes{};
    auto* output = bytes.data();
    encode_le(output, std::bit_cast<std::uint64_t>(value.note.time_us));
    encode_le(output, value.note.duration_us);
    encode_le(output, value.note.lane);
    encode_le(output, static_cast<std::uint8_t>(value.note.owner));
    encode_le(output, value.note.flags);
    encode_le(output, value.note.kind_id);
    encode_le(output, value.sequence);
    return bytes;
}

[[nodiscard]] SequencedNote decode_sort_record(
    const std::array<char, sort_record_bytes>& bytes
) noexcept {
    const auto* input = bytes.data();
    SequencedNote result;
    result.note.time_us = std::bit_cast<std::int64_t>(
        decode_le<std::uint64_t>(input)
    );
    result.note.duration_us = decode_le<std::uint64_t>(input);
    result.note.lane = decode_le<std::uint16_t>(input);
    result.note.owner = static_cast<PackedNoteOwner>(
        decode_le<std::uint8_t>(input)
    );
    result.note.flags = decode_le<std::uint16_t>(input);
    result.note.kind_id = decode_le<std::uint32_t>(input);
    result.sequence = decode_le<std::uint64_t>(input);
    return result;
}

class RunReader final {
public:
    explicit RunReader(const std::filesystem::path& path)
        : input_(path, std::ios::binary) {
        if (!input_) {
            throw ImportError("cannot open external-sort run");
        }
        advance();
    }

    void advance() {
        std::array<char, sort_record_bytes> bytes{};
        input_.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        const auto count = input_.gcount();
        if (count == 0 && input_.eof()) {
            current_.reset();
            return;
        }
        if (count != static_cast<std::streamsize>(bytes.size()) || input_.bad()) {
            throw ImportError("external-sort run is truncated");
        }
        current_ = decode_sort_record(bytes);
    }

    [[nodiscard]] const std::optional<SequencedNote>& current() const noexcept {
        return current_;
    }

private:
    std::ifstream input_;
    std::optional<SequencedNote> current_;
};

class ExternalSorter final {
public:
    ExternalSorter(
        const std::filesystem::path& destination,
        const StreamingChartImportOptions& options
    ) : options_(options) {
        std::error_code error;
        const auto parent = std::filesystem::absolute(destination, error)
            .lexically_normal().parent_path();
        if (error || !std::filesystem::is_directory(parent)) {
            throw ImportError("cannot resolve external-sort directory");
        }
        const auto nonce = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
        for (std::uint32_t attempt = 0U; attempt < 1'024U; ++attempt) {
            const auto candidate = parent / (
                ".pulseforge-json-sort-" + std::to_string(nonce)
                + "-" + std::to_string(attempt)
            );
            error.clear();
            if (std::filesystem::create_directory(candidate, error)) {
                directory_ = candidate;
                owns_directory_ = true;
                break;
            }
            if (error) {
                throw ImportError("cannot create external-sort directory");
            }
        }
        if (!owns_directory_) {
            throw ImportError("cannot reserve external-sort directory");
        }
        buffer_.reserve(options_.max_sort_notes_in_memory);
    }

    ~ExternalSorter() {
        std::error_code ignored;
        for (const auto& path : owned_files_) {
            std::filesystem::remove(path, ignored);
            ignored.clear();
        }
        if (owns_directory_) {
            std::filesystem::remove(directory_, ignored);
        }
    }

    void add(const PackedNote& note) {
        buffer_.push_back({note, next_sequence_++});
        peak_buffered_ = std::max<std::uint64_t>(
            peak_buffered_,
            buffer_.size()
        );
        if (buffer_.size() == options_.max_sort_notes_in_memory) {
            flush_run();
        }
    }

    void finish_input() {
        flush_run();
        runs_.clear();
        for (auto& level : levels_) {
            runs_.insert(
                runs_.end(),
                std::make_move_iterator(level.begin()),
                std::make_move_iterator(level.end())
            );
            level.clear();
        }
    }

    void emit_sorted(
        PackedChartStreamWriter& writer,
        std::unique_ptr<VisualDensityIndexBuilder>& visual_density,
        bool& visual_density_enabled,
        std::string& visual_density_error
    ) {
        while (runs_.size() > options_.max_merge_fan_in) {
            std::vector<std::filesystem::path> next_generation;
            for (std::size_t first = 0U; first < runs_.size();) {
                const auto count = std::min(
                    options_.max_merge_fan_in,
                    runs_.size() - first
                );
                std::vector<std::filesystem::path> group(
                    runs_.begin() + static_cast<std::ptrdiff_t>(first),
                    runs_.begin() + static_cast<std::ptrdiff_t>(first + count)
                );
                const auto output = new_run_path();
                std::ofstream merged(output, std::ios::binary | std::ios::trunc);
                if (!merged) {
                    throw ImportError("cannot create merged external-sort run");
                }
                merge_group(group, [&merged](const SequencedNote& note) {
                    const auto bytes = encode_sort_record(note);
                    merged.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                    if (!merged) {
                        throw ImportError("cannot write merged external-sort run");
                    }
                });
                merged.flush();
                if (!merged) {
                    throw ImportError("cannot flush merged external-sort run");
                }
                merged.close();
                next_generation.push_back(output);
                for (const auto& old : group) {
                    std::error_code ignored;
                    std::filesystem::remove(old, ignored);
                }
                first += count;
            }
            runs_ = std::move(next_generation);
        }

        std::string writer_error;
        merge_group(runs_, [
            &writer,
            &writer_error,
            &visual_density,
            &visual_density_enabled,
            &visual_density_error
        ](const SequencedNote& note) {
            if (!writer.append(note.note, &writer_error)) {
                throw ImportError(
                    "cannot append sorted note to PFC1: " + writer_error
                );
            }
            if (visual_density != nullptr && visual_density_enabled
                && !visual_density->add(
                    note.note,
                    &visual_density_error
                )) {
                visual_density_enabled = false;
                visual_density.reset();
            }
        });
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return next_sequence_; }
    [[nodiscard]] std::uint64_t peak_buffered() const noexcept {
        return peak_buffered_;
    }

private:
    [[nodiscard]] std::filesystem::path new_run_path() {
        const auto path = directory_ / (
            "run-" + std::to_string(next_run_id_++) + ".bin"
        );
        owned_files_.push_back(path);
        return path;
    }

    void flush_run() {
        if (buffer_.empty()) {
            return;
        }
        radix_sort_by_time(buffer_);
        const auto path = new_run_path();
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ImportError("cannot create external-sort run");
        }
        for (const auto& note : buffer_) {
            const auto bytes = encode_sort_record(note);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!output) {
                throw ImportError("cannot write external-sort run");
            }
        }
        output.flush();
        if (!output) {
            throw ImportError("cannot flush external-sort run");
        }
        output.close();
        buffer_.clear();
        add_run(path, 0U);
    }

    void add_run(
        const std::filesystem::path& path,
        const std::size_t level
    ) {
        if (levels_.size() <= level) {
            levels_.resize(level + 1U);
        }
        levels_[level].push_back(path);
        if (levels_[level].size() < options_.max_merge_fan_in) {
            return;
        }

        auto group = std::move(levels_[level]);
        levels_[level].clear();
        const auto merged_path = new_run_path();
        std::ofstream merged(
            merged_path,
            std::ios::binary | std::ios::trunc
        );
        if (!merged) {
            throw ImportError("cannot create compacted external-sort run");
        }
        merge_group(group, [&merged](const SequencedNote& note) {
            const auto bytes = encode_sort_record(note);
            merged.write(
                bytes.data(),
                static_cast<std::streamsize>(bytes.size())
            );
            if (!merged) {
                throw ImportError("cannot write compacted external-sort run");
            }
        });
        merged.flush();
        if (!merged) {
            throw ImportError("cannot flush compacted external-sort run");
        }
        merged.close();
        for (const auto& old : group) {
            std::error_code ignored;
            std::filesystem::remove(old, ignored);
        }
        add_run(merged_path, level + 1U);
    }

    template <typename Consumer>
    static void merge_group(
        const std::vector<std::filesystem::path>& paths,
        Consumer&& consume
    ) {
        struct Cursor final {
            SequencedNote value;
            std::size_t reader{};
        };
        struct Later final {
            [[nodiscard]] bool operator()(
                const Cursor& left,
                const Cursor& right
            ) const noexcept {
                return ordered_before(right.value, left.value);
            }
        };

        std::vector<RunReader> readers;
        readers.reserve(paths.size());
        for (const auto& path : paths) {
            readers.emplace_back(path);
        }
        std::priority_queue<Cursor, std::vector<Cursor>, Later> queue;
        for (std::size_t index = 0U; index < readers.size(); ++index) {
            if (readers[index].current().has_value()) {
                queue.push({*readers[index].current(), index});
            }
        }
        while (!queue.empty()) {
            const auto cursor = queue.top();
            queue.pop();
            consume(cursor.value);
            auto& reader = readers[cursor.reader];
            reader.advance();
            if (reader.current().has_value()) {
                queue.push({*reader.current(), cursor.reader});
            }
        }
    }

    const StreamingChartImportOptions& options_;
    std::filesystem::path directory_;
    std::vector<std::filesystem::path> owned_files_;
    std::vector<std::filesystem::path> runs_;
    std::vector<std::vector<std::filesystem::path>> levels_;
    std::vector<SequencedNote> buffer_;
    std::uint64_t next_sequence_{};
    std::uint64_t next_run_id_{};
    std::uint64_t peak_buffered_{};
    bool owns_directory_{};
};

[[nodiscard]] std::uint64_t source_size(
    const std::filesystem::path& path,
    const std::uint64_t maximum
) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        throw ImportError("source chart JSON is not a regular file");
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw ImportError("cannot determine source chart JSON size");
    }
    if (size > static_cast<std::uintmax_t>(
            std::numeric_limits<std::uint64_t>::max())) {
        throw ImportError("source chart JSON size is not representable by uint64");
    }
    if (static_cast<std::uint64_t>(size) > maximum) {
        throw ImportError("source chart JSON exceeds the configured byte policy");
    }
    return static_cast<std::uint64_t>(size);
}


[[nodiscard]] std::string json_scalar_string(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_number_integer()) return std::to_string(value.get<std::int64_t>());
    if (value.is_number_unsigned()) return std::to_string(value.get<std::uint64_t>());
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        return std::isfinite(number) ? std::to_string(number) : std::string{};
    }
    return {};
}

[[nodiscard]] std::filesystem::path adjacent_psych_events_path(
    const std::filesystem::path& source
) {
    if (is_midi_chart_path(source)
        || is_pfm_chart_path(source)
        || is_pfm_source_path(source)) {
        return {};
    }

    // `events.json` is the current Psych convention. Several older engines
    // emitted the singular `event.json`, so use it only when the canonical
    // plural sidecar is absent. The selected path is passed through parsing
    // and cache fingerprinting; it is never resolved independently twice.
    constexpr std::array<std::string_view, 2U> candidates{
        "events.json",
        "event.json",
    };
    for (const auto name : candidates) {
        const auto candidate = source.parent_path() / name;
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error) || error) {
            continue;
        }
        error.clear();
        if (std::filesystem::equivalent(source, candidate, error) && !error) {
            continue;
        }
        return candidate.lexically_normal();
    }
    return {};
}

void append_checked_adjacent_event(
    ImportSummary& summary,
    ChartEvent event
) {
    if (!std::isfinite(event.time_ms)) {
        return;
    }
    if (summary.events.size() >= maximum_chart_events) {
        throw ImportError("chart event count exceeds the engine limit");
    }
    if (event.name.size() > maximum_chart_event_name_bytes
        || event.value1.size() > maximum_chart_event_value_bytes
        || event.value2.size() > maximum_chart_event_value_bytes) {
        throw ImportError(
            "adjacent chart event string exceeds the engine limit"
        );
    }
    summary.events.push_back(std::move(event));
}

[[nodiscard]] std::optional<double> finite_json_number(
    const nlohmann::json& value
) {
    if (!value.is_number()) {
        return std::nullopt;
    }
    const auto result = value.get<double>();
    return std::isfinite(result)
        ? std::optional<double>{result}
        : std::nullopt;
}

void append_modern_event_object(
    ImportSummary& summary,
    const nlohmann::json& source
) {
    if (!source.is_object()) {
        return;
    }

    ChartEvent event;
    for (const auto* key : {"time", "t", "timeMs"}) {
        const auto iterator = source.find(key);
        if (iterator != source.end()) {
            const auto time = finite_json_number(*iterator);
            if (time.has_value()) {
                event.time_ms = *time;
                break;
            }
        }
    }
    for (const auto* key : {"name", "event", "e"}) {
        const auto iterator = source.find(key);
        if (iterator != source.end()) {
            event.name = json_scalar_string(*iterator);
            break;
        }
    }
    if (const auto iterator = source.find("value1");
        iterator != source.end()) {
        event.value1 = json_scalar_string(*iterator);
    }
    if (const auto iterator = source.find("value2");
        iterator != source.end()) {
        event.value2 = json_scalar_string(*iterator);
    }
    if (const auto iterator = source.find("v"); iterator != source.end()) {
        if (iterator->is_array()) {
            if (!iterator->empty()) {
                event.value1 = json_scalar_string((*iterator)[0U]);
            }
            if (iterator->size() > 1U) {
                event.value2 = json_scalar_string((*iterator)[1U]);
            }
        } else {
            event.value1 = json_scalar_string(*iterator);
        }
    }
    append_checked_adjacent_event(summary, std::move(event));
}

void append_psych_event_entry(
    ImportSummary& summary,
    const double time_ms,
    const nlohmann::json& entry
) {
    if (!entry.is_array() || entry.empty()) {
        return;
    }
    ChartEvent event;
    event.time_ms = time_ms;
    event.name = json_scalar_string(entry[0U]);
    if (entry.size() > 1U) {
        event.value1 = json_scalar_string(entry[1U]);
    }
    if (entry.size() > 2U) {
        event.value2 = json_scalar_string(entry[2U]);
    }
    append_checked_adjacent_event(summary, std::move(event));
}

void append_modern_psych_events(
    ImportSummary& summary,
    const nlohmann::json& events
) {
    if (!events.is_array()) {
        return;
    }
    for (const auto& group : events) {
        if (group.is_object()) {
            append_modern_event_object(summary, group);
            continue;
        }
        if (!group.is_array() || group.size() < 2U) {
            continue;
        }
        const auto time = finite_json_number(group[0U]);
        if (!time.has_value() || !group[1U].is_array()) {
            continue;
        }
        const auto& entries = group[1U];
        if (!entries.empty() && !entries[0U].is_array()) {
            // Tolerate the compact [time, [name, value1, value2]] form in
            // addition to Psych's canonical nested group form.
            append_psych_event_entry(summary, *time, entries);
            continue;
        }
        for (const auto& entry : entries) {
            append_psych_event_entry(summary, *time, entry);
        }
    }
}

void append_legacy_section_events(
    ImportSummary& summary,
    const nlohmann::json& container
) {
    if (!container.is_object()) {
        return;
    }
    const auto sections = container.find("notes");
    if (sections == container.end() || !sections->is_array()) {
        return;
    }
    for (const auto& section : *sections) {
        if (!section.is_object()) {
            continue;
        }
        const auto notes = section.find("sectionNotes");
        if (notes == section.end() || !notes->is_array()) {
            continue;
        }
        for (const auto& raw : *notes) {
            if (!raw.is_array() || raw.size() < 3U
                || !raw[1U].is_number_integer()
                || raw[1U].is_number_unsigned()) {
                continue;
            }
            const auto lane = raw[1U].get<std::int64_t>();
            const auto time = finite_json_number(raw[0U]);
            if (lane >= 0 || !time.has_value()) {
                continue;
            }
            ChartEvent event;
            event.time_ms = *time;
            event.name = json_scalar_string(raw[2U]);
            if (raw.size() > 3U) {
                event.value1 = json_scalar_string(raw[3U]);
            }
            if (raw.size() > 4U) {
                event.value2 = json_scalar_string(raw[4U]);
            }
            append_checked_adjacent_event(summary, std::move(event));
        }
    }
}

void append_adjacent_psych_events(
    ImportSummary& summary,
    const std::filesystem::path& events_path,
    const std::uint64_t maximum_bytes
) {
    if (events_path.empty()) {
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(events_path, error) || error) {
        throw ImportError("selected adjacent events JSON is no longer a file");
    }
    const auto size = std::filesystem::file_size(events_path, error);
    if (error) {
        throw ImportError("cannot determine adjacent events JSON size");
    }
    if (size > static_cast<std::uintmax_t>(
            std::numeric_limits<std::uint64_t>::max())) {
        throw ImportError("adjacent events JSON size is not representable by uint64");
    }
    if (static_cast<std::uint64_t>(size) > maximum_bytes) {
        throw ImportError(
            "adjacent events JSON exceeds the configured byte policy"
        );
    }
    std::ifstream input(events_path, std::ios::binary);
    if (!input) {
        throw ImportError("cannot open adjacent events JSON");
    }
    nlohmann::json root;
    input >> root;
    if (!root.is_object()) {
        return;
    }

    const nlohmann::json* container = &root;
    if (const auto song = root.find("song");
        song != root.end() && song->is_object()) {
        container = &*song;
    }

    // Old Psych/Kade sidecars are complete song-wrapped charts and store
    // events as negative-lane section notes. New sidecars expose an `events`
    // timeline. Some transitional files contain both; preserve both exactly.
    append_legacy_section_events(summary, *container);
    if (const auto events = container->find("events");
        events != container->end()) {
        append_modern_psych_events(summary, *events);
    }
    if (container != &root) {
        if (const auto events = root.find("events"); events != root.end()) {
            append_modern_psych_events(summary, *events);
        }
    }
    std::stable_sort(
        summary.events.begin(),
        summary.events.end(),
        [](const ChartEvent& left, const ChartEvent& right) {
            return left.time_ms < right.time_ms;
        }
    );
    for (const auto& event : summary.events) {
        const auto event_us = milliseconds_to_microseconds(event.time_ms);
        if (event_us.has_value() && *event_us >= 0) {
            summary.content_end_us = std::max(
                summary.content_end_us,
                static_cast<std::uint64_t>(*event_us)
            );
        }
    }
}

void validate_options(const StreamingChartImportOptions& options) {
    if (options.input_buffer_bytes < 4'096U
        || options.input_buffer_bytes > static_cast<std::size_t>(
            std::numeric_limits<std::streamsize>::max())
        || options.max_json_depth == 0U
        || options.max_string_source_bytes == 0U
        || options.max_decoded_string_bytes == 0U
        || options.max_adjacent_event_bytes == 0U
        || options.max_sort_notes_in_memory == 0U
        || options.max_merge_fan_in < 2U
        || options.max_merge_fan_in > 1'024U
        || options.notes_per_pfc_chunk == 0U
        || options.max_notes == 0U) {
        throw ImportError("streaming chart import options are invalid");
    }
}

}  // namespace

[[nodiscard]] PatternRun make_coincident_pattern(
    const CoincidentPatternCandidate& source,
    const ImportSummary& summary
) {
    std::uint64_t lane = source.raw_lane;
    bool player = true;

    switch (source.owner_mode) {
    case RawOwnerMode::native_player:
        player = true;
        break;
    case RawOwnerMode::native_opponent:
        player = false;
        break;
    case RawOwnerMode::psych_must_hit_false:
    case RawOwnerMode::psych_must_hit_true: {
        const bool must_hit = source.owner_mode
            == RawOwnerMode::psych_must_hit_true;
        const auto lane_domain = static_cast<std::uint64_t>(summary.key_count)
            * 2U;
        if (source.raw_lane >= lane_domain) {
            throw ImportError("compressed Psych lane exceeds both strumlines");
        }
        const bool other_side = source.raw_lane >= summary.key_count;
        player = other_side ? !must_hit : must_hit;
        lane = source.raw_lane % summary.key_count;
        break;
    }
    }

    if (lane >= summary.key_count) {
        throw ImportError("compressed note lane exceeds key count");
    }
    // PULSEFORGE_P1_4_0D_COMPRESSED_THIRD_STRUM_OWNER_V1
    if (source.kind_id < summary.kinds.size()
        && summary.kinds[source.kind_id] == "Third Strum") {
        player = false;
    }

    return PatternRun{
        source.time_us,
        0U,
        source.count,
        source.duration_us,
        {static_cast<std::uint16_t>(lane)},
        player ? PackedNoteOwner::player : PackedNoteOwner::opponent,
        0U,
        source.kind_id,
    };
}

[[nodiscard]] PatternRun make_arithmetic_pattern(
    const ArithmeticPatternCandidate& source,
    const ImportSummary& summary
) {
    if (source.interval_us == 0U || source.raw_lane_pattern.empty()) {
        throw ImportError("invalid arithmetic PatternRun candidate");
    }

    bool player = true;
    std::vector<std::uint16_t> lanes;
    lanes.reserve(source.raw_lane_pattern.size());
    std::optional<bool> psych_side;
    for (const auto raw_lane : source.raw_lane_pattern) {
        std::uint64_t lane = raw_lane;
        bool note_player = true;
        switch (source.owner_mode) {
        case RawOwnerMode::native_player:
            note_player = true;
            break;
        case RawOwnerMode::native_opponent:
            note_player = false;
            break;
        case RawOwnerMode::psych_must_hit_false:
        case RawOwnerMode::psych_must_hit_true: {
            const bool must_hit = source.owner_mode
                == RawOwnerMode::psych_must_hit_true;
            const auto lane_domain = static_cast<std::uint64_t>(summary.key_count)
                * 2U;
            if (raw_lane >= lane_domain) {
                throw ImportError("arithmetic Psych lane exceeds both strumlines");
            }
            const bool other_side = raw_lane >= summary.key_count;
            if (psych_side.has_value() && *psych_side != other_side) {
                throw ImportError("arithmetic Psych run changes owner");
            }
            psych_side = other_side;
            note_player = other_side ? !must_hit : must_hit;
            lane = raw_lane % summary.key_count;
            break;
        }
        }
        if (lane >= summary.key_count) {
            throw ImportError("arithmetic note lane exceeds key count");
        }
        if (lanes.empty()) {
            player = note_player;
        } else if (player != note_player) {
            throw ImportError("arithmetic PatternRun changes owner");
        }
        lanes.push_back(static_cast<std::uint16_t>(lane));
    }

    // PULSEFORGE_P1_4_0D_COMPRESSED_THIRD_STRUM_OWNER_V1
    if (source.kind_id < summary.kinds.size()
        && summary.kinds[source.kind_id] == "Third Strum") {
        player = false;
    }

    return PatternRun{
        source.start_us,
        source.interval_us,
        source.count,
        source.duration_us,
        std::move(lanes),
        player ? PackedNoteOwner::player : PackedNoteOwner::opponent,
        0U,
        source.kind_id,
    };
}

static StreamingChartImportResult compile_streaming_json_chart_to_pfc_with_events(
    const std::filesystem::path& source_json,
    const std::filesystem::path& destination_pfc,
    const StreamingChartImportOptions& options,
    const std::filesystem::path& adjacent_events_path
) {
    StreamingChartImportResult result;
    std::filesystem::path published_pfc;
    std::filesystem::path published_pvd;
    try {
        validate_options(options);
        if (source_json.empty() || destination_pfc.empty()) {
            throw ImportError("source or destination path is empty");
        }
        std::error_code path_error;
        const auto source_absolute = std::filesystem::absolute(
            source_json,
            path_error
        ).lexically_normal();
        if (path_error) {
            throw ImportError("cannot resolve source chart JSON path");
        }
        path_error.clear();
        const auto destination_absolute = std::filesystem::absolute(
            destination_pfc,
            path_error
        ).lexically_normal();
        if (path_error || source_absolute == destination_absolute) {
            throw ImportError("source and PFC1 destination paths are invalid");
        }
        auto visual_density_absolute = destination_absolute;
        visual_density_absolute.replace_extension(".pvd");
        if (source_absolute == visual_density_absolute
            || visual_density_absolute == destination_absolute) {
            throw ImportError("source and visual-density destination paths are invalid");
        }

        result.source_bytes = source_size(
            source_absolute,
            options.max_source_bytes
        );
        const auto initial_write_time = std::filesystem::last_write_time(
            source_absolute,
            path_error
        );
        if (path_error) {
            throw ImportError("cannot inspect source chart timestamp");
        }

        ImportSummary summary;
        StreamingParser first_pass(
            source_absolute,
            options,
            summary,
            ParsePass::gather
        );
        first_pass.parse();
        summary.source_fingerprint = first_pass.fingerprint();
        append_adjacent_psych_events(
            summary,
            adjacent_events_path,
            options.max_adjacent_event_bytes
        );
        finalize_summary(summary, options);

        std::vector<PatternRun> compressed_patterns;
        compressed_patterns.reserve(
            summary.coincident_patterns.size() + summary.arithmetic_patterns.size()
        );
        for (const auto& candidate : summary.coincident_patterns) {
            compressed_patterns.push_back(
                make_coincident_pattern(candidate, summary)
            );
        }
        for (const auto& candidate : summary.arithmetic_patterns) {
            compressed_patterns.push_back(
                make_arithmetic_pattern(candidate, summary)
            );
        }
        std::sort(
            compressed_patterns.begin(),
            compressed_patterns.end(),
            [](const PatternRun& left, const PatternRun& right) {
                return left.start_us < right.start_us;
            }
        );

        struct CompressedSequenceRange final {
            std::uint64_t first_sequence{};
            std::uint64_t count{};
        };
        std::vector<CompressedSequenceRange> compressed_ranges;
        compressed_ranges.reserve(
            summary.coincident_patterns.size() + summary.arithmetic_patterns.size()
        );
        for (const auto& candidate : summary.coincident_patterns) {
            compressed_ranges.push_back({candidate.first_sequence, candidate.count});
        }
        for (const auto& candidate : summary.arithmetic_patterns) {
            compressed_ranges.push_back({candidate.first_sequence, candidate.count});
        }
        std::sort(
            compressed_ranges.begin(),
            compressed_ranges.end(),
            [](const auto& left, const auto& right) {
                return left.first_sequence < right.first_sequence;
            }
        );
        if (summary.compressed_note_count > summary.note_count) {
            throw ImportError("compressed note accounting is invalid");
        }
        const auto explicit_note_count =
            summary.note_count - summary.compressed_note_count;

        auto chart_metadata = make_chart_metadata(summary);
        auto song_id = requested_song_id(summary);
        const bool denpa_default = summary.format != ChartFormat::denpa;
        const bool discover_vocals =
            !summary.metadata.audio_vocals_explicit
            && summary.metadata.needs_voices.value_or(denpa_default);

        PackedChartStreamSpec spec;
        spec.key_count = summary.key_count;
        spec.explicit_note_count = explicit_note_count;
        spec.kinds = summary.kinds;
        spec.patterns = std::move(compressed_patterns);
        PackedChartWriteOptions write_options;
        write_options.max_notes_per_chunk = options.notes_per_pfc_chunk;
        write_options.limits = options.packed_limits;

        std::string writer_error;
        auto writer = PackedChartStreamWriter::create(
            destination_absolute,
            spec,
            write_options,
            &writer_error
        );
        if (!writer.has_value()) {
            throw ImportError("cannot create streamed PFC1: " + writer_error);
        }
        std::unique_ptr<VisualDensityIndexBuilder> visual_density;
        bool visual_density_enabled = true;
        std::string visual_density_error;
        try {
            visual_density = std::make_unique<VisualDensityIndexBuilder>(
                summary.key_count,
                summary.kinds,
                destination_absolute.parent_path()
            );
        } catch (const std::exception& exception) {
            visual_density_enabled = false;
            try {
                visual_density_error = exception.what();
            } catch (...) {
                // Diagnostic text is optional just like the derived PVD.
            }
        } catch (...) {
            visual_density_enabled = false;
            try {
                visual_density_error = "visual-density sidecar initialization failed";
            } catch (...) {
            }
        }

        std::uint64_t second_fingerprint{};
        if (summary.sorted) {
            StreamingParser second_pass(
                source_absolute,
                options,
                summary,
                ParsePass::emit,
                [
                    &writer,
                    &writer_error,
                    &visual_density,
                    &visual_density_enabled,
                    &visual_density_error,
                    &compressed_ranges,
                    compressed_range_index = std::size_t{0U}
                ](
                    const PackedNote& note,
                    const std::uint64_t sequence
                ) mutable {
                    while (compressed_range_index
                        < compressed_ranges.size()) {
                        const auto& range = compressed_ranges[compressed_range_index];
                        if (sequence < range.first_sequence) {
                            break;
                        }
                        if (sequence - range.first_sequence < range.count) {
                            return;
                        }
                        ++compressed_range_index;
                    }
                    if (!writer->append(note, &writer_error)) {
                        throw ImportError(
                            "cannot append streamed PFC1 note: " + writer_error
                        );
                    }
                    if (visual_density_enabled && visual_density != nullptr
                        && !visual_density->add(
                            note,
                            &visual_density_error
                        )) {
                        visual_density_enabled = false;
                        visual_density.reset();
                    }
                }
            );
            second_pass.parse();
            second_fingerprint = second_pass.fingerprint();
            result.peak_buffered_notes = std::min<std::uint64_t>(
                explicit_note_count,
                options.notes_per_pfc_chunk
            );
        } else {
            ExternalSorter sorter(destination_absolute, options);
            StreamingParser second_pass(
                source_absolute,
                options,
                summary,
                ParsePass::emit,
                [&sorter](
                    const PackedNote& note,
                    const std::uint64_t
                ) { sorter.add(note); }
            );
            second_pass.parse();
            second_fingerprint = second_pass.fingerprint();
            sorter.finish_input();
            if (sorter.count() != explicit_note_count) {
                throw ImportError("note count changed between streaming passes");
            }
            sorter.emit_sorted(
                *writer,
                visual_density,
                visual_density_enabled,
                visual_density_error
            );
            result.used_external_sort = true;
            result.peak_buffered_notes = sorter.peak_buffered();
        }

        path_error.clear();
        const auto final_size = source_size(
            source_absolute,
            options.max_source_bytes
        );
        const auto final_write_time = std::filesystem::last_write_time(
            source_absolute,
            path_error
        );
        if (path_error
            || final_size != result.source_bytes
            || final_write_time != initial_write_time
            || second_fingerprint != summary.source_fingerprint) {
            throw ImportError("source chart changed during streaming compilation");
        }
        if (!writer->finish(&writer_error)) {
            throw ImportError("cannot finish streamed PFC1: " + writer_error);
        }
        published_pfc = destination_absolute;
        if (visual_density_enabled && visual_density != nullptr
            && visual_density->finish(
                visual_density_absolute,
                &visual_density_error
            )) {
            published_pvd = visual_density_absolute;
            result.visual_density_path = visual_density_absolute;
        } else {
            result.visual_density_error = std::move(visual_density_error);
        }

        result.success = true;
        result.source_format = summary.format;
        result.explicit_note_count = explicit_note_count;
        result.logical_note_count = summary.note_count;
        result.skipped_entry_count = summary.skipped;
        result.section_count = static_cast<std::uint64_t>(
            summary.section_must_hit.size()
        );
        result.pfc_chunk_count = writer->chunks_written();
        result.key_count = summary.key_count;
        result.kind_count = static_cast<std::uint64_t>(summary.kinds.size());
        result.source_fingerprint = summary.source_fingerprint;
        result.content_end_us = summary.content_end_us;
        result.event_count = static_cast<std::uint64_t>(summary.events.size());
        result.chart_metadata = std::move(chart_metadata);
        result.requested_song_id = std::move(song_id);
        result.discover_vocals = discover_vocals;
        result.input_was_time_sorted = summary.sorted;
        return result;
    } catch (const std::bad_alloc&) {
        std::error_code ignored;
        if (!published_pvd.empty()) {
            std::filesystem::remove(published_pvd, ignored);
            ignored.clear();
        }
        if (!published_pfc.empty()) {
            std::filesystem::remove(published_pfc, ignored);
        }
        result.error = "streaming chart import exhausted memory while holding "
            "bounded parser/sort/metadata state";
        return result;
    } catch (const std::length_error&) {
        std::error_code ignored;
        if (!published_pvd.empty()) {
            std::filesystem::remove(published_pvd, ignored);
            ignored.clear();
        }
        if (!published_pfc.empty()) {
            std::filesystem::remove(published_pfc, ignored);
        }
        result.error = "streaming chart import metadata cannot fit in this "
            "process address space";
        return result;
    } catch (const std::exception& exception) {
        std::error_code ignored;
        if (!published_pvd.empty()) {
            std::filesystem::remove(published_pvd, ignored);
            ignored.clear();
        }
        if (!published_pfc.empty()) {
            std::filesystem::remove(published_pfc, ignored);
        }
        result.error = exception.what();
        return result;
    } catch (...) {
        std::error_code ignored;
        if (!published_pvd.empty()) {
            std::filesystem::remove(published_pvd, ignored);
            ignored.clear();
        }
        if (!published_pfc.empty()) {
            std::filesystem::remove(published_pfc, ignored);
        }
        result.error = "streaming chart import failed with an unknown error";
        return result;
    }
}

StreamingChartImportResult compile_streaming_json_chart_to_pfc(
    const std::filesystem::path& source_json,
    const std::filesystem::path& destination_pfc,
    const StreamingChartImportOptions& options
) {
    std::error_code error;
    const auto source = std::filesystem::absolute(
        source_json,
        error
    ).lexically_normal();
    return compile_streaming_json_chart_to_pfc_with_events(
        source_json,
        destination_pfc,
        options,
        error ? std::filesystem::path{}
              : adjacent_psych_events_path(source)
    );
}

namespace {

// Schema 6 adds PFE1: a bounded event timeline sidecar. PFC1 stays unchanged,
// so note caches and their high-density layout remain independent from chart
// events. Schema 4/5 manifests remain PFC-compatible; they are migrated by a
// one-pass event scan instead of destroying/recompiling the authoritative PFC.
// PULSEFORGE_P1_4_0_RUNTIME_CACHE_SCHEMA_V7
// v7 adds secondary-opponent/player4 metadata. Rebuild v6 manifests so an
// existing Denpa Third Strum cache cannot silently reuse metadata that had
// already discarded player4 identity. PFC1 payload bytes remain v1-compatible.
constexpr std::uint32_t runtime_cache_schema = 7U;
constexpr std::uint32_t runtime_cache_legacy_json_schema_4 = 4U;
constexpr std::uint32_t runtime_cache_legacy_json_schema_5 = 5U;
constexpr std::uintmax_t maximum_cache_manifest_bytes = 2U * 1024U * 1024U;

struct RuntimeCacheManifest final {
    std::uint32_t schema{runtime_cache_schema};
    std::string source_path;
    std::uint64_t source_bytes{};
    std::int64_t source_write_time{};
    std::uint64_t source_fingerprint{};
    std::string metadata_sidecar_path;
    std::uint64_t metadata_sidecar_bytes{};
    std::int64_t metadata_sidecar_write_time{};
    std::uint64_t metadata_sidecar_fingerprint{};
    std::string source_events_path;
    std::uint64_t source_events_bytes{};
    std::int64_t source_events_write_time{};
    std::uint64_t source_events_fingerprint{};
    std::uint64_t pfc_bytes{};
    std::uint64_t pvd_bytes{};
    std::uint64_t pfe_bytes{};
    std::uint64_t event_count{};
    std::uint64_t explicit_notes{};
    std::uint64_t logical_notes{};
    std::uint64_t content_end_us{};
    std::string requested_song_id;
    bool discover_vocals{true};
    Chart chart;
};

[[nodiscard]] std::string cache_path_utf8(
    const std::filesystem::path& path
) {
    const auto value = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(value.data()),
        value.size(),
    };
}

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const char character) noexcept {
            return character >= 'A' && character <= 'Z'
                ? static_cast<char>(character - 'A' + 'a')
                : character;
        }
    );
    return value;
}

[[nodiscard]] std::uint64_t fnv_bytes(
    const std::string_view bytes
) noexcept {
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    for (const auto value : bytes) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

[[nodiscard]] std::string hexadecimal_u64(std::uint64_t value) {
    constexpr std::array<char, 16> digits{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    std::array<char, 16> output{};
    for (std::size_t index = output.size(); index > 0U; --index) {
        output[index - 1U] = digits[static_cast<std::size_t>(value & 0x0FU)];
        value >>= 4U;
    }
    return {output.data(), output.size()};
}


constexpr std::array<char, 4U> pfe_magic{{'P', 'F', 'E', '1'}};
constexpr std::uint32_t pfe_version = 1U;

void write_u32_le(std::ostream& output, std::uint32_t value) {
    std::array<char, 4U> bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>((value >> (index * 8U)) & 0xFFU);
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u64_le(std::ostream& output, std::uint64_t value) {
    std::array<char, 8U> bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>((value >> (index * 8U)) & 0xFFU);
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] bool read_u32_le(std::istream& input, std::uint32_t& value) {
    std::array<unsigned char, 4U> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input) return false;
    value = 0U;
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8U);
    }
    return true;
}

[[nodiscard]] bool read_u64_le(std::istream& input, std::uint64_t& value) {
    std::array<unsigned char, 8U> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input) return false;
    value = 0U;
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    }
    return true;
}

[[nodiscard]] bool write_event_sidecar(
    const std::filesystem::path& path,
    const std::span<const ChartEvent> events,
    std::string& error
) {
    if (events.size() > maximum_chart_events) {
        error = "PFE1 event count exceeds the engine limit";
        return false;
    }
    const auto nonce = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    auto temporary = path;
    temporary += ".tmp-" + hexadecimal_u64(nonce);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "cannot create PFE1 event sidecar";
        return false;
    }
    output.write(pfe_magic.data(), static_cast<std::streamsize>(pfe_magic.size()));
    write_u32_le(output, pfe_version);
    write_u64_le(output, static_cast<std::uint64_t>(events.size()));
    for (const auto& event : events) {
        if (!std::isfinite(event.time_ms)
            || event.name.size() > maximum_chart_event_name_bytes
            || event.value1.size() > maximum_chart_event_value_bytes
            || event.value2.size() > maximum_chart_event_value_bytes
            || event.payload_json.size() > maximum_chart_event_value_bytes) {
            error = "cannot serialize invalid chart event into PFE1";
            output.close();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
        write_u64_le(output, std::bit_cast<std::uint64_t>(event.time_ms));
        write_u32_le(output, static_cast<std::uint32_t>(event.name.size()));
        write_u32_le(output, static_cast<std::uint32_t>(event.value1.size()));
        write_u32_le(output, static_cast<std::uint32_t>(event.value2.size()));
        write_u32_le(output, static_cast<std::uint32_t>(event.payload_json.size()));
        output.write(event.name.data(), static_cast<std::streamsize>(event.name.size()));
        output.write(event.value1.data(), static_cast<std::streamsize>(event.value1.size()));
        output.write(event.value2.data(), static_cast<std::streamsize>(event.value2.size()));
        output.write(event.payload_json.data(), static_cast<std::streamsize>(event.payload_json.size()));
        if (!output) {
            error = "cannot write PFE1 event sidecar";
            output.close();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    output.flush();
    if (!output) {
        error = "cannot flush PFE1 event sidecar";
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    output.close();
    std::error_code filesystem_error;
    std::filesystem::remove(path, filesystem_error);
    filesystem_error.clear();
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary, filesystem_error);
        error = "cannot publish PFE1 event sidecar";
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<std::vector<ChartEvent>> read_event_sidecar(
    const std::filesystem::path& path,
    const std::uint64_t expected_count,
    std::string& error
) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open PFE1 event sidecar";
        return std::nullopt;
    }
    std::array<char, 4U> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    std::uint32_t version{};
    std::uint64_t count{};
    if (!input || magic != pfe_magic || !read_u32_le(input, version)
        || version != pfe_version || !read_u64_le(input, count)
        || count > maximum_chart_events || count != expected_count) {
        error = "PFE1 header is invalid or does not match the cache manifest";
        return std::nullopt;
    }
    std::vector<ChartEvent> events;
    events.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0U; index < count; ++index) {
        std::uint64_t time_bits{};
        std::uint32_t name_bytes{};
        std::uint32_t value1_bytes{};
        std::uint32_t value2_bytes{};
        std::uint32_t payload_bytes{};
        if (!read_u64_le(input, time_bits)
            || !read_u32_le(input, name_bytes)
            || !read_u32_le(input, value1_bytes)
            || !read_u32_le(input, value2_bytes)
            || !read_u32_le(input, payload_bytes)
            || name_bytes > maximum_chart_event_name_bytes
            || value1_bytes > maximum_chart_event_value_bytes
            || value2_bytes > maximum_chart_event_value_bytes
            || payload_bytes > maximum_chart_event_value_bytes) {
            error = "PFE1 event record is invalid";
            return std::nullopt;
        }
        ChartEvent event;
        event.time_ms = std::bit_cast<double>(time_bits);
        if (!std::isfinite(event.time_ms)) {
            error = "PFE1 contains a non-finite event time";
            return std::nullopt;
        }
        event.name.resize(name_bytes);
        event.value1.resize(value1_bytes);
        event.value2.resize(value2_bytes);
        event.payload_json.resize(payload_bytes);
        input.read(event.name.data(), static_cast<std::streamsize>(event.name.size()));
        input.read(event.value1.data(), static_cast<std::streamsize>(event.value1.size()));
        input.read(event.value2.data(), static_cast<std::streamsize>(event.value2.size()));
        input.read(event.payload_json.data(), static_cast<std::streamsize>(event.payload_json.size()));
        if (!input) {
            error = "PFE1 event data is truncated";
            return std::nullopt;
        }
        events.push_back(std::move(event));
    }
    return events;
}

struct EventTimelineScanResult final {
    std::vector<ChartEvent> events;
    std::uint64_t source_fingerprint{};
    std::uint64_t content_end_us{};
};

[[nodiscard]] EventTimelineScanResult scan_json_event_timeline(
    const std::filesystem::path& source,
    const std::filesystem::path& adjacent_events_path,
    const StreamingChartImportOptions& options,
    const std::uint64_t expected_source_bytes,
    const std::int64_t expected_write_time
) {
    // This is intentionally only one streaming parse pass. It is used to add
    // or repair the tiny PFE1 derivative while preserving an already-verified
    // PFC1 cache from schema 4/5/6.
    const auto initial_bytes = source_size(source, options.max_source_bytes);
    std::error_code filesystem_error;
    const auto initial_time = std::filesystem::last_write_time(
        source,
        filesystem_error
    );
    if (filesystem_error
        || initial_bytes != expected_source_bytes
        || static_cast<std::int64_t>(
            initial_time.time_since_epoch().count()
        ) != expected_write_time) {
        throw ImportError("source chart changed before PFE1 event scan");
    }

    ImportSummary summary;
    StreamingParser parser(
        source,
        options,
        summary,
        ParsePass::gather
    );
    parser.parse();
    summary.source_fingerprint = parser.fingerprint();
    append_adjacent_psych_events(
        summary,
        adjacent_events_path,
        options.max_adjacent_event_bytes
    );
    std::stable_sort(
        summary.events.begin(),
        summary.events.end(),
        [](const ChartEvent& left, const ChartEvent& right) noexcept {
            return left.time_ms < right.time_ms;
        }
    );

    filesystem_error.clear();
    const auto final_bytes = source_size(source, options.max_source_bytes);
    const auto final_time = std::filesystem::last_write_time(
        source,
        filesystem_error
    );
    if (filesystem_error
        || final_bytes != expected_source_bytes
        || static_cast<std::int64_t>(
            final_time.time_since_epoch().count()
        ) != expected_write_time
        || final_bytes != initial_bytes
        || final_time != initial_time) {
        throw ImportError("source chart changed during PFE1 event scan");
    }

    EventTimelineScanResult result;
    result.events = std::move(summary.events);
    result.source_fingerprint = summary.source_fingerprint;
    result.content_end_us = summary.content_end_us;
    return result;
}

[[nodiscard]] std::optional<std::uint64_t> fingerprint_file(
    const std::filesystem::path& path,
    const std::uint64_t maximum_bytes,
    const std::size_t buffer_bytes,
    std::string& error
) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open source chart while validating its cache";
        return std::nullopt;
    }
    std::vector<char> buffer(std::max<std::size_t>(buffer_bytes, 4'096U));
    std::uint64_t hash = 1'469'598'103'934'665'603ULL;
    std::uint64_t consumed{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count < 0 || input.bad()) {
            error = "cannot read source chart while validating its cache";
            return std::nullopt;
        }
        const auto amount = static_cast<std::uint64_t>(count);
        if (amount > maximum_bytes - std::min(maximum_bytes, consumed)) {
            error = "source chart exceeds the streaming cache byte limit";
            return std::nullopt;
        }
        consumed += amount;
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<std::uint8_t>(
                buffer[static_cast<std::size_t>(index)]
            );
            hash *= 1'099'511'628'211ULL;
        }
    }
    return hash;
}

[[nodiscard]] nlohmann::json chart_metadata_json(const Chart& chart) {
    nlohmann::json tempos = nlohmann::json::array();
    for (const auto& tempo : chart.tempos) {
        tempos.push_back({
            {"time_ms", tempo.time_ms},
            {"bpm", tempo.bpm},
            {"numerator", tempo.numerator},
            {"denominator", tempo.denominator},
        });
    }
    nlohmann::json vocals = nlohmann::json::array();
    for (const auto& path : chart.audio.vocals) {
        vocals.push_back(cache_path_utf8(path));
    }
    return {
        {"title", chart.title},
        {"artist", chart.artist},
        {"charter", chart.charter},
        {"difficulty", chart.difficulty},
        {"stage", chart.stage_id},
        {"player", chart.player_character},
        {"opponent", chart.opponent_character},
        {"secondary_opponent", chart.secondary_opponent_character},
        {"secondary_opponent_enabled", chart.secondary_opponent_enabled},
        {"girlfriend", chart.girlfriend_character},
        {"note_style", chart.note_style},
        {"source_format", static_cast<std::uint8_t>(chart.source_format)},
        {"key_count", chart.key_count},
        {"scroll_speed", chart.chart_scroll_speed},
        {"instrumental", cache_path_utf8(chart.audio.instrumental)},
        {"vocals", std::move(vocals)},
        {"tempos", std::move(tempos)},
    };
}

[[nodiscard]] nlohmann::json manifest_json(
    const RuntimeCacheManifest& manifest
) {
    return {
        {"schema", runtime_cache_schema},
        {"source_path", manifest.source_path},
        {"source_bytes", manifest.source_bytes},
        {"source_write_time", manifest.source_write_time},
        {"source_fingerprint", manifest.source_fingerprint},
        {"metadata_sidecar_path", manifest.metadata_sidecar_path},
        {"metadata_sidecar_bytes", manifest.metadata_sidecar_bytes},
        {"metadata_sidecar_write_time", manifest.metadata_sidecar_write_time},
        {"metadata_sidecar_fingerprint", manifest.metadata_sidecar_fingerprint},
        {"source_events_path", manifest.source_events_path},
        {"source_events_bytes", manifest.source_events_bytes},
        {"source_events_write_time", manifest.source_events_write_time},
        {"source_events_fingerprint", manifest.source_events_fingerprint},
        {"pfc_bytes", manifest.pfc_bytes},
        {"pvd_bytes", manifest.pvd_bytes},
        {"pfe_bytes", manifest.pfe_bytes},
        {"event_count", manifest.event_count},
        {"explicit_notes", manifest.explicit_notes},
        {"logical_notes", manifest.logical_notes},
        {"content_end_us", manifest.content_end_us},
        {"requested_song_id", manifest.requested_song_id},
        {"discover_vocals", manifest.discover_vocals},
        {"chart", chart_metadata_json(manifest.chart)},
    };
}

[[nodiscard]] std::optional<Chart> chart_metadata_from_json(
    const nlohmann::json& json
) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    Chart chart;
    chart.title = json.at("title").get<std::string>();
    chart.artist = json.at("artist").get<std::string>();
    chart.charter = json.at("charter").get<std::string>();
    chart.difficulty = json.at("difficulty").get<std::string>();
    chart.stage_id = json.at("stage").get<std::string>();
    chart.player_character = json.at("player").get<std::string>();
    chart.opponent_character = json.at("opponent").get<std::string>();
    chart.secondary_opponent_character = json.value(
        "secondary_opponent", std::string{}
    );
    chart.secondary_opponent_enabled = json.value(
        "secondary_opponent_enabled", false
    );
    chart.girlfriend_character = json.at("girlfriend").get<std::string>();
    chart.note_style = json.at("note_style").get<std::string>();
    const auto format = json.at("source_format").get<std::uint32_t>();
    if (format > static_cast<std::uint32_t>(ChartFormat::pfm)) {
        return std::nullopt;
    }
    chart.source_format = static_cast<ChartFormat>(format);
    chart.key_count = json.at("key_count").get<std::uint16_t>();
    chart.chart_scroll_speed = json.at("scroll_speed").get<double>();
    chart.audio.instrumental = std::filesystem::path(
        json.at("instrumental").get<std::string>()
    );
    const auto& vocals = json.at("vocals");
    const auto& tempos = json.at("tempos");
    if (!vocals.is_array() || vocals.size() > 8U
        || !tempos.is_array()
        || tempos.size() > maximum_chart_tempo_changes) {
        return std::nullopt;
    }
    for (const auto& value : vocals) {
        chart.audio.vocals.emplace_back(value.get<std::string>());
    }
    for (const auto& value : tempos) {
        chart.tempos.push_back({
            value.at("time_ms").get<double>(),
            value.at("bpm").get<double>(),
            value.at("numerator").get<std::uint16_t>(),
            value.at("denominator").get<std::uint16_t>(),
        });
    }
    chart.normalize();
    for (const auto& issue : validate_chart(chart)) {
        if (issue.severity == ValidationSeverity::error) {
            return std::nullopt;
        }
    }
    return chart;
}

[[nodiscard]] std::optional<RuntimeCacheManifest> read_cache_manifest(
    const std::filesystem::path& path
) {
    try {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size == 0U || size > maximum_cache_manifest_bytes) {
            return std::nullopt;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return std::nullopt;
        }
        const auto json = nlohmann::json::parse(input, nullptr, true, true);
        if (!json.is_object() || !json.contains("schema")) {
            return std::nullopt;
        }
        const auto schema = json.at("schema").get<std::uint32_t>();
        if (schema != runtime_cache_schema
            && schema != runtime_cache_legacy_json_schema_4
            && schema != runtime_cache_legacy_json_schema_5) {
            return std::nullopt;
        }
        auto chart = chart_metadata_from_json(json.at("chart"));
        if (!chart.has_value()) {
            return std::nullopt;
        }
        RuntimeCacheManifest manifest;
        manifest.schema = schema;
        manifest.source_path = json.at("source_path").get<std::string>();
        manifest.source_bytes = json.at("source_bytes").get<std::uint64_t>();
        manifest.source_write_time =
            json.at("source_write_time").get<std::int64_t>();
        manifest.source_fingerprint =
            json.at("source_fingerprint").get<std::uint64_t>();
        manifest.metadata_sidecar_path =
            json.value("metadata_sidecar_path", std::string{});
        manifest.metadata_sidecar_bytes =
            json.value("metadata_sidecar_bytes", std::uint64_t{0U});
        manifest.metadata_sidecar_write_time =
            json.value("metadata_sidecar_write_time", std::int64_t{0});
        manifest.metadata_sidecar_fingerprint =
            json.value("metadata_sidecar_fingerprint", std::uint64_t{0U});
        manifest.source_events_path =
            json.value("source_events_path", std::string{});
        manifest.source_events_bytes =
            json.value("source_events_bytes", std::uint64_t{0U});
        manifest.source_events_write_time =
            json.value("source_events_write_time", std::int64_t{0});
        manifest.source_events_fingerprint =
            json.value("source_events_fingerprint", std::uint64_t{0U});
        manifest.pfc_bytes = json.at("pfc_bytes").get<std::uint64_t>();
        manifest.pvd_bytes = json.value("pvd_bytes", std::uint64_t{0U});
        manifest.pfe_bytes = json.value("pfe_bytes", std::uint64_t{0U});
        manifest.event_count = json.value("event_count", std::uint64_t{0U});
        manifest.explicit_notes =
            json.at("explicit_notes").get<std::uint64_t>();
        manifest.logical_notes =
            json.at("logical_notes").get<std::uint64_t>();
        manifest.content_end_us =
            json.at("content_end_us").get<std::uint64_t>();
        manifest.requested_song_id =
            json.at("requested_song_id").get<std::string>();
        manifest.discover_vocals =
            json.at("discover_vocals").get<bool>();
        manifest.chart = std::move(*chart);
        return manifest;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] bool write_cache_manifest(
    const std::filesystem::path& path,
    const RuntimeCacheManifest& manifest,
    std::string& error
) {
    const auto nonce = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    auto temporary = path;
    temporary += ".tmp-" + hexadecimal_u64(nonce);
    {
        std::ofstream output(
            temporary,
            std::ios::binary | std::ios::trunc
        );
        if (!output) {
            error = "cannot create streaming cache manifest";
            return false;
        }
        output << manifest_json(manifest).dump(2);
        output.flush();
        if (!output) {
            error = "cannot flush streaming cache manifest";
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    // std::filesystem::rename() does not replace an existing destination on
    // Windows. Cache hits frequently rewrite only this tiny manifest (schema
    // migration, PFE/PVD repair), so use a same-directory backup swap instead
    // of accidentally failing every update on MSVC/NTFS.
    std::error_code filesystem_error;
    auto backup = path;
    backup += ".previous-" + hexadecimal_u64(nonce);
    const bool had_existing = std::filesystem::is_regular_file(
        path,
        filesystem_error
    ) && !filesystem_error;
    filesystem_error.clear();
    if (had_existing) {
        std::filesystem::rename(path, backup, filesystem_error);
        if (filesystem_error) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            error = "cannot reserve existing streaming cache manifest";
            return false;
        }
    }

    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        if (had_existing) {
            std::filesystem::rename(backup, path, ignored);
        }
        error = "cannot atomically publish streaming cache manifest";
        return false;
    }
    if (had_existing) {
        std::error_code ignored;
        std::filesystem::remove(backup, ignored);
    }
    return true;
}

struct MusicalMetadataSignature final {
    std::string path;
    std::uint64_t bytes{};
    std::int64_t write_time{};
    std::uint64_t fingerprint{};
};

[[nodiscard]] std::filesystem::path musical_metadata_sidecar_path(
    const std::filesystem::path& source
) {
    const auto filename = cache_path_utf8(source.filename());
    const auto lower = lower_ascii(filename);
    constexpr std::string_view pfm_source_suffix{".pfm.json"};
    if (lower.ends_with(pfm_source_suffix)) {
        return source.parent_path()
            / (filename.substr(
                0U,
                filename.size() - pfm_source_suffix.size()
            ) + ".pfmeta.json");
    }
    auto sidecar = source;
    sidecar.replace_extension(".pfmeta.json");
    return sidecar;
}

[[nodiscard]] MusicalMetadataSignature musical_metadata_signature(
    const std::filesystem::path& source,
    const std::size_t input_buffer_bytes
) {
    MusicalMetadataSignature result;
    if (!is_midi_chart_path(source)
        && !is_pfm_chart_path(source)
        && !is_pfm_source_path(source)) {
        return result;
    }
    const auto sidecar = musical_metadata_sidecar_path(source);
    std::error_code error;
    if (!std::filesystem::is_regular_file(sidecar, error) || error) {
        return result;
    }
    const auto bytes = std::filesystem::file_size(sidecar, error);
    if (error) return {};
    const auto time = std::filesystem::last_write_time(sidecar, error);
    if (error) return {};
    std::string fingerprint_error;
    const auto fingerprint = fingerprint_file(
        sidecar,
        4ULL * 1024ULL * 1024ULL,
        input_buffer_bytes,
        fingerprint_error
    );
    if (!fingerprint.has_value()) {
        return {};
    }
    result.path = cache_path_utf8(sidecar);
    result.bytes = static_cast<std::uint64_t>(bytes);
    result.write_time = static_cast<std::int64_t>(
        time.time_since_epoch().count()
    );
    result.fingerprint = *fingerprint;
    return result;
}

[[nodiscard]] bool same_musical_metadata_signature(
    const MusicalMetadataSignature& left,
    const MusicalMetadataSignature& right
) noexcept {
    return left.path == right.path
        && left.bytes == right.bytes
        && left.write_time == right.write_time
        && left.fingerprint == right.fingerprint;
}


[[nodiscard]] MusicalMetadataSignature psych_events_signature(
    const std::filesystem::path& sidecar,
    const std::uint64_t maximum_bytes,
    const std::size_t input_buffer_bytes
) {
    MusicalMetadataSignature result;
    if (sidecar.empty()) {
        return result;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(sidecar, error) || error) {
        return result;
    }
    const auto bytes = std::filesystem::file_size(sidecar, error);
    if (error || bytes > maximum_bytes) return {};
    const auto time = std::filesystem::last_write_time(sidecar, error);
    if (error) return {};
    std::string fingerprint_error;
    const auto fingerprint = fingerprint_file(
        sidecar,
        maximum_bytes,
        input_buffer_bytes,
        fingerprint_error
    );
    if (!fingerprint.has_value()) return {};
    result.path = cache_path_utf8(sidecar);
    result.bytes = static_cast<std::uint64_t>(bytes);
    result.write_time = static_cast<std::int64_t>(time.time_since_epoch().count());
    result.fingerprint = *fingerprint;
    return result;
}

void resolve_metadata_audio(
    Chart& chart,
    const std::filesystem::path& source,
    const std::string_view song_id,
    const bool discover_vocals
) {
    const auto base = source.parent_path();
    if (!chart.audio.instrumental.empty()
        && chart.audio.instrumental.is_relative()) {
        chart.audio.instrumental = base / chart.audio.instrumental;
    }
    for (auto& vocal : chart.audio.vocals) {
        if (vocal.is_relative()) {
            vocal = base / vocal;
        }
    }
    ChartLoader::resolve_conventional_audio(
        chart,
        source,
        song_id,
        discover_vocals
    );
}

[[nodiscard]] bool build_visual_density_sidecar(
    const PackedChartReader& reader,
    const std::filesystem::path& destination,
    std::string& error
) {
    try {
        std::error_code filesystem_error;
        std::filesystem::remove(destination, filesystem_error);
        filesystem_error.clear();
        VisualDensityIndexBuilder builder(
            reader.key_count(),
            reader.kinds(),
            destination.parent_path()
        );
        for (std::uint64_t chunk = 0U; chunk < reader.chunk_count(); ++chunk) {
            auto decoded = reader.read_chunk(chunk);
            if (!decoded) {
                error = "cannot decode PFC1 while building PVD1: "
                    + decoded.error;
                return false;
            }
            for (const auto& note : decoded.notes) {
                if (!builder.add(note, &error)) {
                    return false;
                }
            }
        }
        if (!builder.finish(destination, &error)) {
            return false;
        }
        auto verified = VisualDensityIndexReader::open(destination, &error);
        if (!verified.has_value()
            || verified->key_count() != reader.key_count()) {
            std::filesystem::remove(destination, filesystem_error);
            error = error.empty()
                ? "PVD1 verification failed after build"
                : error;
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    } catch (...) {
        error = "visual-density sidecar build failed";
        return false;
    }
}

}  // namespace

StreamingChartCacheResult prepare_streaming_chart_cache(
    const std::filesystem::path& source_json,
    const StreamingChartCacheOptions& options
) {
    StreamingChartCacheResult result;
    try {
        if (source_json.empty()) {
            throw ImportError("streaming chart source path is empty");
        }
        validate_options(options.import);
        std::error_code filesystem_error;
        const auto source = std::filesystem::weakly_canonical(
            source_json,
            filesystem_error
        );
        if (filesystem_error
            || !std::filesystem::is_regular_file(source, filesystem_error)
            || filesystem_error) {
            throw ImportError("cannot resolve streaming chart source");
        }
        const auto source_bytes = source_size(
            source,
            options.import.max_source_bytes
        );
        const auto write_time = std::filesystem::last_write_time(
            source,
            filesystem_error
        );
        if (filesystem_error) {
            throw ImportError("cannot inspect streaming chart timestamp");
        }
        const auto write_time_value = static_cast<std::int64_t>(
            write_time.time_since_epoch().count()
        );
        const auto metadata_signature = musical_metadata_signature(
            source,
            options.import.input_buffer_bytes
        );
        const auto adjacent_events_path = adjacent_psych_events_path(source);
        const auto event_source_signature = psych_events_signature(
            adjacent_events_path,
            options.import.max_adjacent_event_bytes,
            options.import.input_buffer_bytes
        );
        const auto source_name = cache_path_utf8(source);
        auto cache_root = options.cache_root;
        if (cache_root.empty()) {
            cache_root = std::filesystem::current_path()
                / "out" / "cache" / "large-charts";
        }
        cache_root = std::filesystem::absolute(
            cache_root,
            filesystem_error
        ).lexically_normal();
        if (filesystem_error) {
            throw ImportError("cannot resolve streaming cache directory");
        }
        std::filesystem::create_directories(cache_root, filesystem_error);
        if (filesystem_error
            || !std::filesystem::is_directory(cache_root, filesystem_error)
            || filesystem_error) {
            throw ImportError("cannot create streaming cache directory");
        }
        const auto key = hexadecimal_u64(fnv_bytes(source_name));
        const auto pfc_path = cache_root / (key + ".pfc");
        const auto pvd_path = cache_root / (key + ".pvd");
        const auto pfe_path = cache_root / (key + ".pfe");
        const auto manifest_path = cache_root / (key + ".manifest.json");

        const auto cached = read_cache_manifest(manifest_path);
        const bool cache_schema_compatible = cached.has_value()
            && (cached->schema == runtime_cache_schema
                || cached->schema == runtime_cache_legacy_json_schema_4
                || cached->schema == runtime_cache_legacy_json_schema_5);

        // The authoritative PFC cache depends on the source chart and musical
        // metadata, not on the optional PFE/PVD derivatives. In particular,
        // changing events.json must never force a multi-gigabyte PFC rebuild.
        if (cache_schema_compatible
            && cached->source_path == source_name
            && cached->source_bytes == source_bytes
            && cached->source_write_time == write_time_value
            && cached->metadata_sidecar_path == metadata_signature.path
            && cached->metadata_sidecar_bytes == metadata_signature.bytes
            && cached->metadata_sidecar_write_time == metadata_signature.write_time
            && cached->metadata_sidecar_fingerprint
                == metadata_signature.fingerprint) {
            bool source_matches = true;
            std::uint64_t fingerprint = cached->source_fingerprint;

            if (options.strict_cache_validation) {
                std::string fingerprint_error;
                const auto checked = fingerprint_file(
                    source,
                    options.import.max_source_bytes,
                    options.import.input_buffer_bytes,
                    fingerprint_error
                );

                source_matches = checked.has_value()
                    && *checked == cached->source_fingerprint;

                if (checked.has_value()) {
                    fingerprint = *checked;
                }
            }

            filesystem_error.clear();
            const auto pfc_bytes = std::filesystem::file_size(
                pfc_path,
                filesystem_error
            );
            const bool pfc_size_valid = !filesystem_error
                && pfc_bytes == cached->pfc_bytes;

            if (source_matches && pfc_size_valid) {
                std::string reader_error;
                auto reader = PackedChartReader::open(
                    pfc_path,
                    &reader_error,
                    options.import.packed_limits
                );

                if (reader.has_value()
                    && reader->explicit_note_count() == cached->explicit_notes
                    && reader->logical_note_count() == cached->logical_notes
                    && reader->key_count() == cached->chart.key_count) {
                    // PFE1 is a bounded event derivative. Validate it if the
                    // schema/signature say it is current; otherwise repair it
                    // without touching the verified PFC1.
                    const bool event_signature_matches =
                        cached->schema == runtime_cache_schema
                        && cached->source_events_path
                            == event_source_signature.path
                        && cached->source_events_bytes
                            == event_source_signature.bytes
                        && cached->source_events_write_time
                            == event_source_signature.write_time
                        && cached->source_events_fingerprint
                            == event_source_signature.fingerprint;

                    std::optional<std::vector<ChartEvent>> cached_events;
                    std::uint64_t committed_pfe_bytes{};
                    if (event_signature_matches) {
                        filesystem_error.clear();
                        const auto actual_pfe_bytes = std::filesystem::file_size(
                            pfe_path,
                            filesystem_error
                        );
                        if (!filesystem_error
                            && actual_pfe_bytes == cached->pfe_bytes) {
                            std::string event_error;
                            cached_events = read_event_sidecar(
                                pfe_path,
                                cached->event_count,
                                event_error
                            );
                            if (cached_events.has_value()) {
                                committed_pfe_bytes =
                                    static_cast<std::uint64_t>(
                                        actual_pfe_bytes
                                    );
                            }
                        }
                    }

                    auto updated_manifest = *cached;
                    bool event_manifest_changed = false;
                    if (!cached_events.has_value()) {
                        std::vector<ChartEvent> events;
                        std::uint64_t event_content_end_us =
                            cached->content_end_us;

                        if (is_midi_chart_path(source)
                            || is_pfm_chart_path(source)
                            || is_pfm_source_path(source)) {
                            // The current MIDI/PFM contracts carry no runtime
                            // chart-event timeline. Keep an explicit empty PFE1
                            // so cache validation remains deterministic.
                            events.clear();
                        } else {
                            const auto scanned = scan_json_event_timeline(
                                source,
                                adjacent_events_path,
                                options.import,
                                source_bytes,
                                write_time_value
                            );
                            if (scanned.source_fingerprint
                                != cached->source_fingerprint) {
                                source_matches = false;
                            } else {
                                fingerprint = scanned.source_fingerprint;
                                event_content_end_us = std::max(
                                    event_content_end_us,
                                    scanned.content_end_us
                                );
                                events = scanned.events;
                            }
                        }

                        if (source_matches) {
                            std::string event_error;
                            if (!write_event_sidecar(
                                    pfe_path,
                                    events,
                                    event_error
                                )) {
                                throw ImportError(event_error);
                            }
                            filesystem_error.clear();
                            const auto bytes = std::filesystem::file_size(
                                pfe_path,
                                filesystem_error
                            );
                            if (filesystem_error
                                || bytes > static_cast<std::uintmax_t>(
                                    std::numeric_limits<std::uint64_t>::max()
                                )) {
                                throw ImportError(
                                    "cannot inspect repaired PFE1 event sidecar"
                                );
                            }
                            committed_pfe_bytes =
                                static_cast<std::uint64_t>(bytes);
                            cached_events = std::move(events);

                            updated_manifest.schema = runtime_cache_schema;
                            updated_manifest.source_events_path =
                                event_source_signature.path;
                            updated_manifest.source_events_bytes =
                                event_source_signature.bytes;
                            updated_manifest.source_events_write_time =
                                event_source_signature.write_time;
                            updated_manifest.source_events_fingerprint =
                                event_source_signature.fingerprint;
                            updated_manifest.pfe_bytes =
                                committed_pfe_bytes;
                            updated_manifest.event_count =
                                static_cast<std::uint64_t>(
                                    cached_events->size()
                                );
                            updated_manifest.content_end_us =
                                event_content_end_us;
                            event_manifest_changed = true;
                        }
                    }

                    if (!source_matches || !cached_events.has_value()) {
                        // Fall through to the ordinary source-cache rebuild
                        // path. A mismatched source fingerprint is never
                        // accepted merely to preserve a derivative.
                    } else {
                        // PVD1 is a rendering accelerator, not authoritative
                        // chart data. A missing/corrupt PVD must therefore never
                        // turn an otherwise valid PFC1 cache hit into a long,
                        // synchronous chart-loading operation.
                        std::string visual_error;
                        filesystem_error.clear();
                        const auto actual_pvd_bytes = std::filesystem::file_size(
                            pvd_path,
                            filesystem_error
                        );
                        const bool pvd_size_matches = !filesystem_error
                            && cached->pvd_bytes != 0U
                            && actual_pvd_bytes == cached->pvd_bytes;
                        auto visual = pvd_size_matches
                            ? VisualDensityIndexReader::open(
                                pvd_path,
                                &visual_error
                            )
                            : std::optional<VisualDensityIndexReader>{};
                        bool visual_valid = visual.has_value()
                            && visual->key_count() == reader->key_count();
                        std::uintmax_t committed_pvd_bytes =
                            visual_valid ? actual_pvd_bytes : 0U;

                        if (!visual_valid && options.require_visual_density) {
                            filesystem_error.clear();
                            std::filesystem::remove(
                                pvd_path,
                                filesystem_error
                            );
                            visual_error.clear();
                            if (build_visual_density_sidecar(
                                    *reader,
                                    pvd_path,
                                    visual_error
                                )) {
                                visual = VisualDensityIndexReader::open(
                                    pvd_path,
                                    &visual_error
                                );
                                visual_valid = visual.has_value()
                                    && visual->key_count()
                                        == reader->key_count();
                                if (visual_valid) {
                                    filesystem_error.clear();
                                    committed_pvd_bytes =
                                        std::filesystem::file_size(
                                            pvd_path,
                                            filesystem_error
                                        );
                                    if (filesystem_error) {
                                        visual_valid = false;
                                        committed_pvd_bytes = 0U;
                                        visual.reset();
                                    }
                                }
                            }
                        }

                        const bool manifest_needs_upgrade =
                            cached->schema != runtime_cache_schema;
                        const bool manifest_pvd_changed =
                            cached->pvd_bytes != committed_pvd_bytes;
                        if (manifest_needs_upgrade
                            || manifest_pvd_changed
                            || event_manifest_changed) {
                            updated_manifest.schema = runtime_cache_schema;
                            updated_manifest.pvd_bytes =
                                committed_pvd_bytes;
                            updated_manifest.pfe_bytes =
                                committed_pfe_bytes;
                            updated_manifest.event_count =
                                static_cast<std::uint64_t>(
                                    cached_events->size()
                                );
                            updated_manifest.source_events_path =
                                event_source_signature.path;
                            updated_manifest.source_events_bytes =
                                event_source_signature.bytes;
                            updated_manifest.source_events_write_time =
                                event_source_signature.write_time;
                            updated_manifest.source_events_fingerprint =
                                event_source_signature.fingerprint;
                            std::string manifest_error;
                            if (!write_cache_manifest(
                                    manifest_path,
                                    updated_manifest,
                                    manifest_error
                                )) {
                                throw ImportError(manifest_error);
                            }
                        }

                        if (!visual_valid) {
                            filesystem_error.clear();
                            std::filesystem::remove(
                                pvd_path,
                                filesystem_error
                            );
                            visual.reset();
                        }

                        result.success = true;
                        result.reused = true;
                        result.cache_path = pfc_path;
                        result.event_sidecar_path = pfe_path;
                        if (visual.has_value()
                            && visual->key_count() == reader->key_count()) {
                            result.visual_density_path = pvd_path;
                        }
                        result.reader = std::move(reader);
                        result.chart_metadata = updated_manifest.chart;
                        result.chart_metadata.events =
                            std::move(*cached_events);
                        result.chart_metadata.difficulty =
                            options.difficulty;
                        result.source_bytes = source_bytes;
                        result.source_fingerprint = fingerprint;
                        result.content_end_us =
                            updated_manifest.content_end_us;
                        resolve_metadata_audio(
                            result.chart_metadata,
                            source,
                            updated_manifest.requested_song_id,
                            updated_manifest.discover_vocals
                        );
                        return result;
                    }
                }
            }
        }

        // Only private, deterministic cache paths are removed. The source JSON
        // and every mod asset remain untouched.
        filesystem_error.clear();
        std::filesystem::remove(manifest_path, filesystem_error);
        filesystem_error.clear();
        std::filesystem::remove(pfc_path, filesystem_error);
        filesystem_error.clear();
        std::filesystem::remove(pvd_path, filesystem_error);
        filesystem_error.clear();
        std::filesystem::remove(pfe_path, filesystem_error);

        StreamingChartImportResult compiled;
        if (is_midi_chart_path(source)
            || is_pfm_chart_path(source)
            || is_pfm_source_path(source)) {
            MusicalChartCompileResult musical;
            if (is_midi_chart_path(source)) {
                MidiChartOptions musical_options;
                musical_options.limits.max_source_bytes =
                    options.import.max_source_bytes;
                musical_options.limits.max_midi_notes = std::min(
                    musical_options.limits.max_midi_notes,
                    options.import.max_notes
                );
                musical_options.limits.max_sort_notes_in_memory =
                    options.import.max_sort_notes_in_memory;
                musical_options.limits.max_logical_notes =
                    options.import.packed_limits.max_logical_notes;
                musical_options.packed.max_notes_per_chunk =
                    options.import.notes_per_pfc_chunk;
                musical_options.packed.limits =
                    options.import.packed_limits;
                musical = compile_midi_chart_to_pfc(
                    source,
                    pfc_path,
                    musical_options
                );
            } else {
                PfmChartOptions musical_options;
                musical_options.limits.max_source_bytes =
                    options.import.max_source_bytes;
                musical_options.limits.max_pfm_explicit_notes =
                    options.import.max_notes;
                musical_options.limits.max_sort_notes_in_memory =
                    options.import.max_sort_notes_in_memory;
                musical_options.limits.max_logical_notes =
                    options.import.packed_limits.max_logical_notes;
                musical_options.limits.max_pfm_patterns =
                    options.import.packed_limits.max_patterns;
                musical_options.limits.max_pattern_lanes =
                    options.import.packed_limits.max_pattern_lanes;
                musical_options.packed.max_notes_per_chunk =
                    options.import.notes_per_pfc_chunk;
                musical_options.packed.limits =
                    options.import.packed_limits;
                musical = is_pfm_source_path(source)
                    ? compile_pfm_source_to_pfc(
                        source,
                        pfc_path,
                        musical_options
                    )
                    : compile_pfm_chart_to_pfc(
                        source,
                        pfc_path,
                        musical_options
                    );
            }
            if (!musical) {
                compiled.error = musical.error;
            } else {
                compiled.success = true;
                compiled.source_format = musical.source_format;
                compiled.source_bytes = musical.source_bytes;
                compiled.source_fingerprint = musical.source_fingerprint;
                compiled.explicit_note_count = musical.explicit_note_count;
                compiled.logical_note_count = musical.logical_note_count;
                compiled.key_count = musical.key_count;
                compiled.kind_count = 0U;
                compiled.content_end_us = musical.content_end_us;
                compiled.chart_metadata = std::move(musical.chart_metadata);
                compiled.requested_song_id = cache_path_utf8(source.stem());
                if (is_pfm_source_path(source)) {
                    const auto filename = cache_path_utf8(source.filename());
                    constexpr std::string_view suffix{".pfm.json"};
                    if (filename.size() > suffix.size()) {
                        compiled.requested_song_id = filename.substr(
                            0U,
                            filename.size() - suffix.size()
                        );
                    }
                }
                compiled.discover_vocals = true;
                compiled.input_was_time_sorted = true;
                compiled.used_external_sort = is_midi_chart_path(source);
            }
        } else {
            compiled = compile_streaming_json_chart_to_pfc_with_events(
                source,
                pfc_path,
                options.import,
                adjacent_events_path
            );
        }
        if (!compiled) {
            throw ImportError(
                "cannot compile runtime PFC1 cache: " + compiled.error
            );
        }
        std::string reader_error;
        auto reader = PackedChartReader::open(
            pfc_path,
            &reader_error,
            options.import.packed_limits
        );
        if (!reader.has_value()
            || reader->explicit_note_count() != compiled.explicit_note_count
            || reader->logical_note_count() != compiled.logical_note_count
            || reader->key_count() != compiled.key_count) {
            std::filesystem::remove(pfc_path, filesystem_error);
            filesystem_error.clear();
            std::filesystem::remove(pvd_path, filesystem_error);
            throw ImportError(
                "new runtime PFC1 cache failed verification: " + reader_error
            );
        }
        const auto final_size = source_size(
            source,
            options.import.max_source_bytes
        );
        const auto final_time = std::filesystem::last_write_time(
            source,
            filesystem_error
        );
        const auto final_metadata_signature = musical_metadata_signature(
            source,
            options.import.input_buffer_bytes
        );
        const auto final_event_source_signature = psych_events_signature(
            adjacent_events_path,
            options.import.max_adjacent_event_bytes,
            options.import.input_buffer_bytes
        );
        if (filesystem_error
            || final_size != compiled.source_bytes
            || !same_musical_metadata_signature(
                metadata_signature,
                final_metadata_signature
            )
            || !same_musical_metadata_signature(
                event_source_signature,
                final_event_source_signature
            )) {
            std::filesystem::remove(pfc_path, filesystem_error);
            filesystem_error.clear();
            std::filesystem::remove(pvd_path, filesystem_error);
            throw ImportError("source chart changed before cache commit");
        }
        const auto pfc_bytes = std::filesystem::file_size(
            pfc_path,
            filesystem_error
        );
        if (filesystem_error) {
            std::filesystem::remove(pfc_path, filesystem_error);
            filesystem_error.clear();
            std::filesystem::remove(pvd_path, filesystem_error);
            throw ImportError("cannot inspect new runtime PFC1 cache");
        }
        std::uint64_t pvd_bytes{};
        bool visual_valid{};
        if (compiled.visual_density_path.empty()) {
            std::string visual_error;
            if (build_visual_density_sidecar(
                    *reader,
                    pvd_path,
                    visual_error
                )) {
                compiled.visual_density_path = pvd_path;
            } else {
                compiled.visual_density_error = visual_error;
            }
        }
        if (!compiled.visual_density_path.empty()) {
            filesystem_error.clear();
            pvd_bytes = std::filesystem::file_size(pvd_path, filesystem_error);
            std::string visual_error;
            const auto visual = VisualDensityIndexReader::open(
                pvd_path,
                &visual_error
            );
            visual_valid = !filesystem_error && visual.has_value()
                && visual->key_count() == compiled.key_count
                && compiled.visual_density_path.lexically_normal()
                    == pvd_path.lexically_normal();
            if (!visual_valid) {
                filesystem_error.clear();
                std::filesystem::remove(pvd_path, filesystem_error);
                pvd_bytes = 0U;
            }
        }
        std::string event_sidecar_error;
        if (!write_event_sidecar(
                pfe_path,
                compiled.chart_metadata.events,
                event_sidecar_error
            )) {
            std::filesystem::remove(pfc_path, filesystem_error);
            filesystem_error.clear();
            std::filesystem::remove(pvd_path, filesystem_error);
            throw ImportError(event_sidecar_error);
        }
        filesystem_error.clear();
        const auto pfe_bytes = std::filesystem::file_size(
            pfe_path,
            filesystem_error
        );
        if (filesystem_error) {
            std::filesystem::remove(pfc_path, filesystem_error);
            filesystem_error.clear();
            std::filesystem::remove(pvd_path, filesystem_error);
            filesystem_error.clear();
            std::filesystem::remove(pfe_path, filesystem_error);
            throw ImportError("cannot inspect new PFE1 event sidecar");
        }

        RuntimeCacheManifest manifest;
        manifest.source_path = source_name;
        manifest.source_bytes = compiled.source_bytes;
        manifest.source_write_time = static_cast<std::int64_t>(
            final_time.time_since_epoch().count()
        );
        manifest.source_fingerprint = compiled.source_fingerprint;
        manifest.metadata_sidecar_path = metadata_signature.path;
        manifest.metadata_sidecar_bytes = metadata_signature.bytes;
        manifest.metadata_sidecar_write_time = metadata_signature.write_time;
        manifest.metadata_sidecar_fingerprint = metadata_signature.fingerprint;
        manifest.source_events_path = event_source_signature.path;
        manifest.source_events_bytes = event_source_signature.bytes;
        manifest.source_events_write_time = event_source_signature.write_time;
        manifest.source_events_fingerprint = event_source_signature.fingerprint;
        manifest.pfc_bytes = pfc_bytes;
        manifest.pvd_bytes = pvd_bytes;
        manifest.pfe_bytes = static_cast<std::uint64_t>(pfe_bytes);
        manifest.event_count = static_cast<std::uint64_t>(
            compiled.chart_metadata.events.size()
        );
        manifest.explicit_notes = compiled.explicit_note_count;
        manifest.logical_notes = compiled.logical_note_count;
        manifest.content_end_us = compiled.content_end_us;
        manifest.requested_song_id = compiled.requested_song_id;
        manifest.discover_vocals = compiled.discover_vocals;
        manifest.chart = compiled.chart_metadata;
        std::string manifest_error;
        if (!write_cache_manifest(
                manifest_path,
                manifest,
                manifest_error
            )) {
            std::filesystem::remove(pfc_path, filesystem_error);
            filesystem_error.clear();
            std::filesystem::remove(pvd_path, filesystem_error);
            filesystem_error.clear();
            std::filesystem::remove(pfe_path, filesystem_error);
            throw ImportError(manifest_error);
        }

        result.success = true;
        result.cache_path = pfc_path;
        result.event_sidecar_path = pfe_path;
        if (visual_valid) {
            result.visual_density_path = pvd_path;
        }
        result.reader = std::move(reader);
        result.chart_metadata = compiled.chart_metadata;
        result.chart_metadata.difficulty = options.difficulty;
        result.source_bytes = compiled.source_bytes;
        result.source_fingerprint = compiled.source_fingerprint;
        result.content_end_us = compiled.content_end_us;
        resolve_metadata_audio(
            result.chart_metadata,
            source,
            compiled.requested_song_id,
            compiled.discover_vocals
        );
        return result;
    } catch (const std::exception& exception) {
        result.error = exception.what();
        return result;
    } catch (...) {
        result.error = "streaming chart cache preparation failed";
        return result;
    }
}

}  // namespace pulseforge
