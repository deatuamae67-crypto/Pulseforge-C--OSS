#include "pulseforge/musical_chart.hpp"

#include "pulseforge/chart_loader.hpp"
#include "pulseforge/packed_chart_stream.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <new>
#include <numeric>
#include <optional>
#include <queue>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulseforge {
namespace {

using Json = nlohmann::json;

constexpr std::array<char, 4> pfm_magic{'P', 'F', 'M', '1'};
constexpr std::uint16_t pfm_version = 1U;
constexpr std::uint16_t pfm_flag_embedded_metadata = 1U << 0U;
constexpr std::uint64_t fnv_offset = 1'469'598'103'934'665'603ULL;
constexpr std::uint64_t fnv_prime = 1'099'511'628'211ULL;

class MusicalError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] std::string lower_ascii(std::string value) {
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(encoded.data()),
        encoded.size(),
    };
}

[[nodiscard]] std::filesystem::path sidecar_path(
    const std::filesystem::path& source
) {
    const auto filename = path_utf8(source.filename());
    const auto lower = lower_ascii(filename);
    if (lower.ends_with(".pfm.json")) {
        return source.parent_path()
            / (filename.substr(
                0U,
                filename.size() - std::string_view{".pfm.json"}.size()
            ) + ".pfmeta.json");
    }
    auto result = source;
    result.replace_extension(".pfmeta.json");
    return result;
}

[[nodiscard]] std::uint64_t file_size_bounded(
    const std::filesystem::path& path,
    const std::uint64_t maximum
) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw MusicalError("cannot inspect musical chart file size");
    }
    if (size > static_cast<std::uintmax_t>(
            std::numeric_limits<std::uint64_t>::max())) {
        throw MusicalError(
            "musical chart file size is not representable by uint64"
        );
    }
    if (static_cast<std::uint64_t>(size) > maximum) {
        throw MusicalError("musical chart exceeds configured source-byte limit");
    }
    return static_cast<std::uint64_t>(size);
}

[[nodiscard]] std::uint64_t fingerprint_file(
    const std::filesystem::path& path,
    const std::uint64_t maximum
) {
    static_cast<void>(file_size_bounded(path, maximum));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw MusicalError("cannot open musical chart for fingerprinting");
    }
    std::array<char, 256U * 1024U> buffer{};
    std::uint64_t hash = fnv_offset;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(
                buffer[static_cast<std::size_t>(index)]
            );
            hash *= fnv_prime;
        }
    }
    if (!input.eof()) {
        throw MusicalError("cannot fingerprint musical chart");
    }
    return hash;
}

template <typename T>
void write_le(std::ostream& output, const T value) {
    static_assert(std::is_integral_v<T>);
    using U = std::make_unsigned_t<T>;
    U encoded = static_cast<U>(value);
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        output.put(static_cast<char>(encoded & U{0xFFU}));
        encoded >>= 8U;
    }
    if (!output) {
        throw MusicalError("cannot write musical chart");
    }
}

template <typename T>
[[nodiscard]] T read_le(std::istream& input) {
    static_assert(std::is_integral_v<T>);
    using U = std::make_unsigned_t<T>;
    U value{};
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        const auto byte = input.get();
        if (byte < 0) {
            throw MusicalError("musical chart is truncated");
        }
        value |= static_cast<U>(static_cast<unsigned char>(byte))
            << (index * 8U);
    }
    return static_cast<T>(value);
}

void write_uvar(std::ostream& output, std::uint64_t value) {
    while (value >= 0x80U) {
        output.put(static_cast<char>(
            static_cast<std::uint8_t>(value) | 0x80U
        ));
        value >>= 7U;
    }
    output.put(static_cast<char>(value));
    if (!output) {
        throw MusicalError("cannot write PFM varint");
    }
}

[[nodiscard]] std::uint64_t read_uvar(std::istream& input) {
    std::uint64_t value{};
    unsigned shift{};
    for (unsigned index = 0U; index < 10U; ++index) {
        const auto raw = input.get();
        if (raw < 0) {
            throw MusicalError("PFM varint is truncated");
        }
        const auto byte = static_cast<std::uint8_t>(raw);
        if (shift >= 64U && (byte & 0x7FU) != 0U) {
            throw MusicalError("PFM varint overflows 64 bits");
        }
        value |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0U) {
            return value;
        }
        shift += 7U;
    }
    throw MusicalError("PFM varint is too long");
}

[[nodiscard]] std::uint64_t zigzag_encode(const std::int64_t value) noexcept {
    return (static_cast<std::uint64_t>(value) << 1U)
        ^ static_cast<std::uint64_t>(value >> 63);
}

[[nodiscard]] std::int64_t zigzag_decode(const std::uint64_t value) noexcept {
    return static_cast<std::int64_t>(
        (value >> 1U) ^ (~(value & 1U) + 1U)
    );
}

void write_svar(std::ostream& output, const std::int64_t value) {
    write_uvar(output, zigzag_encode(value));
}

[[nodiscard]] std::int64_t read_svar(std::istream& input) {
    return zigzag_decode(read_uvar(input));
}

void write_string(std::ostream& output, const std::string_view value) {
    write_uvar(output, static_cast<std::uint64_t>(value.size()));
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!output) {
        throw MusicalError("cannot write PFM string");
    }
}

[[nodiscard]] std::string read_string(
    std::istream& input,
    const std::size_t maximum
) {
    const auto size = read_uvar(input);
    if (size > maximum
        || size > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()
        )) {
        throw MusicalError("PFM string exceeds configured limit");
    }
    std::string value(static_cast<std::size_t>(size), '\0');
    if (!value.empty()) {
        input.read(value.data(), static_cast<std::streamsize>(value.size()));
    }
    if (!input) {
        throw MusicalError("PFM string is truncated");
    }
    return value;
}

struct TickTempo {
    std::uint64_t tick{};
    std::uint32_t microseconds_per_quarter{500'000U};
    std::uint16_t numerator{4U};
    std::uint16_t denominator{4U};
};

[[nodiscard]] std::vector<TickTempo> normalized_tick_tempos(
    std::vector<TickTempo> tempos
) {
    if (tempos.empty()) {
        tempos.push_back({});
    }
    std::sort(tempos.begin(), tempos.end(), [](const auto& left, const auto& right) {
        return left.tick < right.tick;
    });
    std::vector<TickTempo> result;
    result.reserve(tempos.size());
    for (const auto& tempo : tempos) {
        if (tempo.microseconds_per_quarter == 0U) {
            continue;
        }
        if (!result.empty() && result.back().tick == tempo.tick) {
            result.back() = tempo;
        } else {
            result.push_back(tempo);
        }
    }
    if (result.empty() || result.front().tick != 0U) {
        result.insert(result.begin(), TickTempo{});
    }
    return result;
}

// Exact floor(a*b/d) for d <= 2^32 without a non-portable 128-bit integer.
[[nodiscard]] bool mul_div_floor_u64(
    const std::uint64_t a,
    const std::uint64_t b,
    const std::uint32_t d,
    std::uint64_t& result
) noexcept {
    if (d == 0U) return false;
    const auto divisor = static_cast<std::uint64_t>(d);
    const auto aq = a / divisor;
    const auto ar = a % divisor;
    const auto bq = b / divisor;
    const auto br = b % divisor;

    if (b != 0U && aq > std::numeric_limits<std::uint64_t>::max() / b) {
        return false;
    }
    auto total = aq * b;

    if (bq != 0U
        && ar > (std::numeric_limits<std::uint64_t>::max() - total) / bq) {
        return false;
    }
    total += ar * bq;

    // ar,br < d <= 2^32, so ar*br fits uint64_t.
    const auto tail = (ar * br) / divisor;
    if (tail > std::numeric_limits<std::uint64_t>::max() - total) {
        return false;
    }
    result = total + tail;
    return true;
}

class TickTimeMap final {
public:
    TickTimeMap(std::uint32_t ppqn, std::vector<TickTempo> tempos)
        : ppqn_(ppqn), tempos_(normalized_tick_tempos(std::move(tempos))) {
        if (ppqn_ == 0U) {
            throw MusicalError("PPQN cannot be zero");
        }
        starts_us_.resize(tempos_.size(), 0U);
        for (std::size_t index = 1U; index < tempos_.size(); ++index) {
            const auto delta_tick = tempos_[index].tick
                - tempos_[index - 1U].tick;
            std::uint64_t delta_us{};
            if (!mul_div_floor_u64(
                    delta_tick,
                    tempos_[index - 1U].microseconds_per_quarter,
                    ppqn_,
                    delta_us
                )
                || delta_us > std::numeric_limits<std::uint64_t>::max()
                    - starts_us_[index - 1U]) {
                throw MusicalError("tempo-map tick conversion overflows");
            }
            starts_us_[index] = starts_us_[index - 1U] + delta_us;
        }
    }

    [[nodiscard]] std::uint32_t ppqn() const noexcept { return ppqn_; }
    [[nodiscard]] const std::vector<TickTempo>& tempos() const noexcept {
        return tempos_;
    }

    [[nodiscard]] std::uint64_t tick_to_us(const std::uint64_t tick) const {
        const auto iterator = std::upper_bound(
            tempos_.begin(),
            tempos_.end(),
            tick,
            [](const std::uint64_t value, const TickTempo& tempo) {
                return value < tempo.tick;
            }
        );
        const auto index = iterator == tempos_.begin()
            ? std::size_t{0U}
            : static_cast<std::size_t>(
                std::distance(tempos_.begin(), iterator) - 1
            );
        const auto delta_tick = tick - tempos_[index].tick;
        std::uint64_t delta_us{};
        if (!mul_div_floor_u64(
                delta_tick,
                tempos_[index].microseconds_per_quarter,
                ppqn_,
                delta_us
            )
            || delta_us > std::numeric_limits<std::uint64_t>::max()
                - starts_us_[index]) {
            throw MusicalError("tick-to-microsecond conversion overflows");
        }
        return starts_us_[index] + delta_us;
    }

    [[nodiscard]] std::uint64_t us_to_tick(const std::uint64_t target_us) const {
        const auto iterator = std::upper_bound(
            starts_us_.begin(),
            starts_us_.end(),
            target_us
        );
        const auto index = iterator == starts_us_.begin()
            ? std::size_t{0U}
            : static_cast<std::size_t>(
                std::distance(starts_us_.begin(), iterator) - 1
            );
        const auto delta_us = target_us - starts_us_[index];

        // Nearest musical tick. All arithmetic is bounded in long double only
        // for this export/quantization path; runtime PatternRuns remain rational.
        const long double ticks =
            static_cast<long double>(delta_us)
            * static_cast<long double>(ppqn_)
            / static_cast<long double>(
                tempos_[index].microseconds_per_quarter
            );
        const auto rounded = static_cast<std::uint64_t>(
            std::max<long double>(0.0L, std::floor(ticks + 0.5L))
        );
        if (rounded > std::numeric_limits<std::uint64_t>::max()
                - tempos_[index].tick) {
            throw MusicalError("microsecond-to-tick conversion overflows");
        }
        return tempos_[index].tick + rounded;
    }

    [[nodiscard]] std::size_t tempo_index_for_tick(
        const std::uint64_t tick
    ) const noexcept {
        const auto iterator = std::upper_bound(
            tempos_.begin(),
            tempos_.end(),
            tick,
            [](const std::uint64_t value, const TickTempo& tempo) {
                return value < tempo.tick;
            }
        );
        return iterator == tempos_.begin()
            ? 0U
            : static_cast<std::size_t>(
                std::distance(tempos_.begin(), iterator) - 1
            );
    }

private:
    std::uint32_t ppqn_{};
    std::vector<TickTempo> tempos_;
    std::vector<std::uint64_t> starts_us_;
};

[[nodiscard]] Json metadata_json(const Chart& chart) {
    Json events = Json::array();
    for (const auto& event : chart.events) {
        events.push_back({
            {"timeMs", event.time_ms},
            {"name", event.name},
            {"value1", event.value1},
            {"value2", event.value2},
            {"payloadJson", event.payload_json},
        });
    }
    Json vocals = Json::array();
    for (const auto& vocal : chart.audio.vocals) {
        vocals.push_back(path_utf8(vocal));
    }
    return {
        {"schema", "pulseforge-musical-metadata-v1"},
        {"title", chart.title},
        {"artist", chart.artist},
        {"charter", chart.charter},
        {"difficulty", chart.difficulty},
        {"stage", chart.stage_id},
        {"player", chart.player_character},
        {"opponent", chart.opponent_character},
        {"girlfriend", chart.girlfriend_character},
        {"noteStyle", chart.note_style},
        {"keyCount", chart.key_count},
        {"scrollSpeed", chart.chart_scroll_speed},
        {"audio", {
            {"instrumental", path_utf8(chart.audio.instrumental)},
            {"vocals", std::move(vocals)},
        }},
        {"events", std::move(events)},
    };
}

void apply_metadata_json(Chart& chart, const Json& json) {
    if (!json.is_object()) return;
    const auto string_value = [&](const char* key, std::string& target) {
        const auto found = json.find(key);
        if (found != json.end() && found->is_string()) {
            target = found->get<std::string>();
        }
    };
    string_value("title", chart.title);
    string_value("artist", chart.artist);
    string_value("charter", chart.charter);
    string_value("difficulty", chart.difficulty);
    string_value("stage", chart.stage_id);
    string_value("player", chart.player_character);
    string_value("opponent", chart.opponent_character);
    string_value("girlfriend", chart.girlfriend_character);
    string_value("noteStyle", chart.note_style);

    if (const auto found = json.find("keyCount");
        found != json.end() && found->is_number_unsigned()) {
        const auto value = found->get<std::uint64_t>();
        if (value >= 1U && value <= maximum_supported_key_count) {
            chart.key_count = static_cast<std::uint16_t>(value);
        }
    }
    if (const auto found = json.find("scrollSpeed");
        found != json.end() && found->is_number()) {
        const auto value = found->get<double>();
        if (std::isfinite(value) && value > 0.0) {
            chart.chart_scroll_speed = value;
        }
    }
    if (const auto audio = json.find("audio");
        audio != json.end() && audio->is_object()) {
        if (const auto instrumental = audio->find("instrumental");
            instrumental != audio->end() && instrumental->is_string()) {
            chart.audio.instrumental =
                std::filesystem::path(instrumental->get<std::string>());
        }
        if (const auto vocals = audio->find("vocals");
            vocals != audio->end() && vocals->is_array()) {
            chart.audio.vocals.clear();
            for (const auto& item : *vocals) {
                if (item.is_string()) {
                    chart.audio.vocals.emplace_back(item.get<std::string>());
                }
            }
        }
    }
    if (const auto events = json.find("events");
        events != json.end() && events->is_array()) {
        chart.events.clear();
        const auto limit = std::min<std::size_t>(
            events->size(),
            maximum_chart_events
        );
        chart.events.reserve(limit);
        for (std::size_t index = 0U; index < limit; ++index) {
            const auto& item = (*events)[index];
            if (!item.is_object()) continue;
            ChartEvent event;
            event.time_ms = item.value("timeMs", 0.0);
            event.name = item.value("name", std::string{});
            event.value1 = item.value("value1", std::string{});
            event.value2 = item.value("value2", std::string{});
            event.payload_json = item.value("payloadJson", std::string{});
            if (std::isfinite(event.time_ms)) {
                chart.events.push_back(std::move(event));
            }
        }
    }
}

[[nodiscard]] std::optional<Json> read_sidecar_metadata(
    const std::filesystem::path& source,
    const std::size_t maximum
) {
    const auto path = sidecar_path(source);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return std::nullopt;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    try {
        Json json;
        input >> json;
        return json;
    } catch (...) {
        return std::nullopt;
    }
}

void write_sidecar_metadata(
    const std::filesystem::path& destination,
    const Chart& chart
) {
    auto path = sidecar_path(destination);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw MusicalError("cannot create musical-chart metadata sidecar");
    }
    output << metadata_json(chart).dump(2);
    output.flush();
    if (!output) {
        throw MusicalError("cannot flush musical-chart metadata sidecar");
    }
}

[[nodiscard]] std::vector<TickTempo> tempos_from_chart(
    const Chart& chart,
    const std::uint32_t ppqn
) {
    std::vector<TickTempo> result;
    auto tempos = chart.tempos;
    if (tempos.empty()) {
        tempos.push_back({0.0, 120.0, 4U, 4U});
    }
    std::sort(tempos.begin(), tempos.end(), [](const auto& a, const auto& b) {
        return a.time_ms < b.time_ms;
    });

    long double tick = 0.0L;
    double previous_ms = 0.0;
    double previous_bpm = 120.0;
    std::uint16_t previous_num = 4U;
    std::uint16_t previous_den = 4U;
    bool first = true;

    for (const auto& tempo : tempos) {
        if (!std::isfinite(tempo.time_ms)
            || !std::isfinite(tempo.bpm)
            || tempo.bpm <= 0.0) {
            continue;
        }
        if (!first) {
            const auto delta_ms = std::max(0.0, tempo.time_ms - previous_ms);
            tick += static_cast<long double>(delta_ms)
                * static_cast<long double>(previous_bpm)
                * static_cast<long double>(ppqn)
                / 60'000.0L;
        } else if (tempo.time_ms > 0.0) {
            tick += static_cast<long double>(tempo.time_ms)
                * 120.0L * static_cast<long double>(ppqn)
                / 60'000.0L;
        }
        const auto rounded_tick = static_cast<std::uint64_t>(
            std::max<long double>(0.0L, std::floor(tick + 0.5L))
        );
        const long double microseconds =
            60'000'000.0L / static_cast<long double>(tempo.bpm);
        const auto mpq = static_cast<std::uint32_t>(std::clamp<long double>(
            std::floor(microseconds + 0.5L),
            1.0L,
            16'777'215.0L
        ));
        result.push_back({
            rounded_tick,
            mpq,
            tempo.numerator == 0U ? previous_num : tempo.numerator,
            tempo.denominator == 0U ? previous_den : tempo.denominator,
        });
        previous_ms = tempo.time_ms;
        previous_bpm = tempo.bpm;
        previous_num = tempo.numerator;
        previous_den = tempo.denominator;
        first = false;
    }
    return normalized_tick_tempos(std::move(result));
}

[[nodiscard]] std::vector<TempoChange> chart_tempos_from_ticks(
    const TickTimeMap& map
) {
    std::vector<TempoChange> result;
    result.reserve(map.tempos().size());
    for (const auto& tempo : map.tempos()) {
        const auto time_us = map.tick_to_us(tempo.tick);
        result.push_back({
            static_cast<double>(time_us) / 1'000.0,
            60'000'000.0
                / static_cast<double>(tempo.microseconds_per_quarter),
            tempo.numerator,
            tempo.denominator,
        });
    }
    return result;
}

[[nodiscard]] std::uint32_t kind_id_for(
    std::vector<std::string>& kinds,
    const std::string_view kind,
    const std::size_t maximum_kind_bytes
) {
    const auto normalized = kind.empty() ? std::string_view{"normal"} : kind;
    if (normalized.size() > maximum_kind_bytes) {
        throw MusicalError("note type exceeds configured byte limit");
    }
    const auto found = std::find(kinds.begin(), kinds.end(), normalized);
    if (found != kinds.end()) {
        return static_cast<std::uint32_t>(
            std::distance(kinds.begin(), found)
        );
    }
    if (kinds.size() >= std::numeric_limits<std::uint32_t>::max()) {
        throw MusicalError("too many musical-chart note types");
    }
    kinds.emplace_back(normalized);
    return static_cast<std::uint32_t>(kinds.size() - 1U);
}

// -------------------------------------------------------------------------
// PFM binary format.
// -------------------------------------------------------------------------

struct PfmPatternRecord {
    enum class Domain : std::uint8_t {
        ticks = 0U,
        microseconds = 1U,
    };

    Domain domain{Domain::ticks};
    std::uint64_t start_tick{};
    std::int64_t start_us{};
    std::uint64_t interval{};
    std::uint32_t interval_denominator{1U};
    std::uint64_t count{};
    std::uint64_t duration{};
    std::vector<std::uint16_t> lanes;
    PackedNoteOwner owner{PackedNoteOwner::player};
    std::uint16_t flags{};
    std::uint32_t kind_id{};
};

struct PfmSourcePatternEvent {
    std::uint64_t offset_tick{};
    std::uint64_t duration_tick{};
    std::uint16_t lane{};
    std::uint16_t flags{};
    std::uint32_t kind_id{};
};

struct PfmSourcePatternDefinition {
    std::uint64_t length_tick{};
    std::vector<PfmSourcePatternEvent> events;
};

void write_pfm_pattern(std::ostream& output, const PfmPatternRecord& pattern) {
    output.put(static_cast<char>(pattern.domain));
    if (pattern.domain == PfmPatternRecord::Domain::ticks) {
        write_uvar(output, pattern.start_tick);
        write_uvar(output, pattern.interval);
        write_uvar(output, pattern.duration);
    } else {
        write_svar(output, pattern.start_us);
        write_uvar(output, pattern.interval);
        write_uvar(output, pattern.interval_denominator);
        write_uvar(output, pattern.duration);
    }
    write_uvar(output, pattern.count);
    write_uvar(output, static_cast<std::uint64_t>(pattern.lanes.size()));
    for (const auto lane : pattern.lanes) {
        write_uvar(output, lane);
    }
    output.put(static_cast<char>(pattern.owner));
    write_uvar(output, pattern.flags);
    write_uvar(output, pattern.kind_id);
    if (!output) throw MusicalError("cannot write PFM PatternRun");
}

[[nodiscard]] PfmPatternRecord read_pfm_pattern(
    std::istream& input,
    const MusicalChartLimits& limits,
    const std::uint16_t key_count,
    const std::size_t kind_count
) {
    PfmPatternRecord pattern;
    const auto domain = input.get();
    if (domain < 0 || domain > 1) {
        throw MusicalError("PFM PatternRun has invalid time domain");
    }
    pattern.domain = static_cast<PfmPatternRecord::Domain>(domain);
    if (pattern.domain == PfmPatternRecord::Domain::ticks) {
        pattern.start_tick = read_uvar(input);
        pattern.interval = read_uvar(input);
        pattern.duration = read_uvar(input);
    } else {
        pattern.start_us = read_svar(input);
        pattern.interval = read_uvar(input);
        const auto denominator = read_uvar(input);
        if (denominator == 0U
            || denominator > std::numeric_limits<std::uint32_t>::max()) {
            throw MusicalError("PFM PatternRun denominator is invalid");
        }
        pattern.interval_denominator =
            static_cast<std::uint32_t>(denominator);
        pattern.duration = read_uvar(input);
    }
    pattern.count = read_uvar(input);
    if (pattern.count == 0U
        || pattern.count > limits.max_logical_notes) {
        throw MusicalError("PFM PatternRun count is invalid");
    }
    const auto lane_count = read_uvar(input);
    if (lane_count == 0U || lane_count > limits.max_pattern_lanes
        || lane_count > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()
        )) {
        throw MusicalError("PFM PatternRun lane cycle is invalid or too large");
    }
    pattern.lanes.reserve(static_cast<std::size_t>(lane_count));
    for (std::uint64_t index = 0U; index < lane_count; ++index) {
        const auto lane = read_uvar(input);
        if (lane >= key_count) {
            throw MusicalError("PFM PatternRun lane exceeds key count");
        }
        pattern.lanes.push_back(static_cast<std::uint16_t>(lane));
    }
    const auto owner = input.get();
    if (owner < 0 || owner > 1) {
        throw MusicalError("PFM PatternRun owner is invalid");
    }
    pattern.owner = static_cast<PackedNoteOwner>(owner);
    const auto flags = read_uvar(input);
    if (flags > std::numeric_limits<std::uint16_t>::max()) {
        throw MusicalError("PFM PatternRun flags overflow");
    }
    pattern.flags = static_cast<std::uint16_t>(flags);
    const auto kind = read_uvar(input);
    if (kind >= kind_count) {
        throw MusicalError("PFM PatternRun kind is invalid");
    }
    pattern.kind_id = static_cast<std::uint32_t>(kind);
    return pattern;
}

[[nodiscard]] std::uint64_t ceil_div(
    const std::uint64_t numerator,
    const std::uint64_t denominator
) noexcept {
    if (denominator == 0U) return 0U;
    return numerator / denominator
        + static_cast<std::uint64_t>(numerator % denominator != 0U);
}

[[nodiscard]] std::vector<PatternRun> compile_tick_pattern(
    const PfmPatternRecord& source,
    const TickTimeMap& map
) {
    if (source.count == 0U) return {};
    if (source.interval == 0U) {
        if (source.duration
            > std::numeric_limits<std::uint64_t>::max() - source.start_tick) {
            throw MusicalError("PFM PatternRun duration tick overflows");
        }
        const auto start_us = map.tick_to_us(source.start_tick);
        const auto end_us = map.tick_to_us(source.start_tick + source.duration);
        if (start_us > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            throw MusicalError("PFM PatternRun start time exceeds int64 microseconds");
        }
        return {
            PatternRun{
                static_cast<std::int64_t>(start_us),
                0U,
                source.count,
                end_us - start_us,
                source.lanes,
                source.owner,
                source.flags,
                source.kind_id,
                1U,
            }
        };
    }

    std::vector<PatternRun> result;
    std::uint64_t first_index = 0U;
    while (first_index < source.count) {
        if (source.interval != 0U
            && first_index > (
                std::numeric_limits<std::uint64_t>::max()
                - source.start_tick
            ) / source.interval) {
            throw MusicalError("PFM PatternRun tick arithmetic overflows");
        }
        const auto first_tick =
            source.start_tick + first_index * source.interval;
        const auto tempo_index = map.tempo_index_for_tick(first_tick);
        const auto& tempo = map.tempos()[tempo_index];
        const auto next_tempo_tick = tempo_index + 1U < map.tempos().size()
            ? map.tempos()[tempo_index + 1U].tick
            : std::numeric_limits<std::uint64_t>::max();

        std::uint64_t end_index = source.count;
        if (next_tempo_tick != std::numeric_limits<std::uint64_t>::max()
            && next_tempo_tick > source.start_tick) {
            const auto relative = next_tempo_tick - source.start_tick;
            end_index = std::min(
                source.count,
                ceil_div(relative, source.interval)
            );
            end_index = std::max(end_index, first_index + 1U);
        }

        const auto count = end_index - first_index;
        const auto segment_start_tick = first_tick;
        const auto start_us = map.tick_to_us(segment_start_tick);
        if (start_us > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            throw MusicalError("PFM PatternRun start time exceeds int64 microseconds");
        }

        const auto gcd = std::gcd<std::uint64_t>(
            source.interval,
            map.ppqn()
        );
        const auto reduced_ticks = source.interval / gcd;
        const auto denominator64 =
            static_cast<std::uint64_t>(map.ppqn()) / gcd;
        if (denominator64 == 0U
            || denominator64 > std::numeric_limits<std::uint32_t>::max()
            || (tempo.microseconds_per_quarter != 0U
                && reduced_ticks
                    > std::numeric_limits<std::uint64_t>::max()
                        / tempo.microseconds_per_quarter)) {
            throw MusicalError("PFM PatternRun rational interval overflows");
        }
        const auto numerator =
            reduced_ticks * tempo.microseconds_per_quarter;

        if (source.duration
            > std::numeric_limits<std::uint64_t>::max() - segment_start_tick) {
            throw MusicalError("PFM PatternRun duration tick overflows");
        }
        if (source.duration != 0U
            && next_tempo_tick != std::numeric_limits<std::uint64_t>::max()) {
            if (end_index == 0U) {
                throw MusicalError("PFM PatternRun tempo split is invalid");
            }
            const auto last_index = end_index - 1U;
            if (source.interval != 0U
                && last_index > (
                    std::numeric_limits<std::uint64_t>::max()
                    - source.start_tick
                ) / source.interval) {
                throw MusicalError("PFM PatternRun tick arithmetic overflows");
            }
            const auto last_start = source.start_tick
                + last_index * source.interval;
            if (source.duration
                > std::numeric_limits<std::uint64_t>::max() - last_start
                || last_start + source.duration > next_tempo_tick) {
                throw MusicalError(
                    "PFM sustain RUN crosses a tempo boundary; split the RUN "
                    "at that boundary so duration remains musically exact"
                );
            }
        }
        const auto duration_end = map.tick_to_us(
            segment_start_tick + source.duration
        );
        std::vector<std::uint16_t> segment_lanes = source.lanes;
        if (!segment_lanes.empty()) {
            const auto phase = static_cast<std::size_t>(
                first_index % segment_lanes.size()
            );
            std::rotate(
                segment_lanes.begin(),
                segment_lanes.begin() + static_cast<std::ptrdiff_t>(phase),
                segment_lanes.end()
            );
        }
        result.push_back(PatternRun{
            static_cast<std::int64_t>(start_us),
            numerator,
            count,
            duration_end - start_us,
            std::move(segment_lanes),
            source.owner,
            source.flags,
            source.kind_id,
            static_cast<std::uint32_t>(denominator64),
        });
        first_index = end_index;
    }
    return result;
}

[[nodiscard]] Chart chart_from_metadata_and_tempos(
    const std::filesystem::path& source,
    const Json* embedded,
    const TickTimeMap& map,
    const ChartFormat format,
    const std::uint16_t key_count,
    const MusicalChartLimits& limits
) {
    Chart chart;
    chart.source_format = format;
    chart.title = path_utf8(source.stem());
    chart.key_count = key_count;
    if (embedded != nullptr) {
        apply_metadata_json(chart, *embedded);
    }
    if (const auto sidecar = read_sidecar_metadata(
            source,
            limits.max_metadata_bytes
        ); sidecar.has_value()) {
        apply_metadata_json(chart, *sidecar);
    }
    chart.key_count = key_count;
    chart.tempos = chart_tempos_from_ticks(map);
    return chart;
}

// -------------------------------------------------------------------------
// Standard MIDI File.
// -------------------------------------------------------------------------

[[nodiscard]] std::uint16_t read_be16(std::istream& input) {
    const auto a = input.get();
    const auto b = input.get();
    if (a < 0 || b < 0) throw MusicalError("MIDI header is truncated");
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(a) << 8U)
        | static_cast<std::uint16_t>(b)
    );
}

[[nodiscard]] std::uint32_t read_be32(std::istream& input) {
    std::uint32_t result{};
    for (unsigned index = 0U; index < 4U; ++index) {
        const auto value = input.get();
        if (value < 0) throw MusicalError("MIDI chunk is truncated");
        result = (result << 8U)
            | static_cast<std::uint32_t>(static_cast<std::uint8_t>(value));
    }
    return result;
}

void write_be16(std::ostream& output, const std::uint16_t value) {
    output.put(static_cast<char>((value >> 8U) & 0xFFU));
    output.put(static_cast<char>(value & 0xFFU));
}

void write_be32(std::ostream& output, const std::uint32_t value) {
    output.put(static_cast<char>((value >> 24U) & 0xFFU));
    output.put(static_cast<char>((value >> 16U) & 0xFFU));
    output.put(static_cast<char>((value >> 8U) & 0xFFU));
    output.put(static_cast<char>(value & 0xFFU));
}

void write_midi_vlq(std::vector<std::uint8_t>& output, std::uint32_t value) {
    std::array<std::uint8_t, 4> bytes{};
    std::size_t count = 1U;
    bytes[3] = static_cast<std::uint8_t>(value & 0x7FU);
    while ((value >>= 7U) != 0U && count < 4U) {
        bytes[3U - count] = static_cast<std::uint8_t>(
            (value & 0x7FU) | 0x80U
        );
        ++count;
    }
    output.insert(output.end(), bytes.end() - static_cast<std::ptrdiff_t>(count), bytes.end());
}

struct MidiTrack {
    std::uint64_t offset{};
    std::uint32_t length{};
    std::string name;
    std::optional<PackedNoteOwner> owner_hint;
};

struct MidiHeader {
    std::uint16_t format{};
    std::uint16_t track_count{};
    std::uint16_t ppqn{};
    std::vector<MidiTrack> tracks;
};

[[nodiscard]] MidiHeader read_midi_header(
    std::ifstream& input,
    const MusicalChartLimits& limits
) {
    std::array<char, 4> magic{};
    input.read(magic.data(), 4);
    if (!input || std::string_view(magic.data(), 4) != "MThd") {
        throw MusicalError("file is not a Standard MIDI File");
    }
    const auto header_length = read_be32(input);
    if (header_length < 6U) {
        throw MusicalError("MIDI header chunk is too short");
    }
    MidiHeader header;
    header.format = read_be16(input);
    header.track_count = read_be16(input);
    const auto division = read_be16(input);
    if ((division & 0x8000U) != 0U) {
        throw MusicalError(
            "SMPTE MIDI time division is not supported; use PPQN"
        );
    }
    header.ppqn = division;
    if (header.ppqn == 0U) {
        throw MusicalError("MIDI PPQN cannot be zero");
    }
    if (header.format > 1U) {
        throw MusicalError("only Standard MIDI File formats 0 and 1 are supported");
    }
    if (header.track_count == 0U
        || header.track_count > limits.max_midi_tracks) {
        throw MusicalError("MIDI track count exceeds configured limit");
    }
    if (header_length > 6U) {
        input.seekg(
            static_cast<std::streamoff>(header_length - 6U),
            std::ios::cur
        );
    }

    header.tracks.reserve(header.track_count);
    for (std::uint16_t index = 0U; index < header.track_count; ++index) {
        input.read(magic.data(), 4);
        if (!input || std::string_view(magic.data(), 4) != "MTrk") {
            throw MusicalError("MIDI track chunk is missing");
        }
        const auto length = read_be32(input);
        const auto offset = static_cast<std::uint64_t>(input.tellg());
        header.tracks.push_back({offset, length, {}, std::nullopt});
        input.seekg(static_cast<std::streamoff>(length), std::ios::cur);
        if (!input) {
            throw MusicalError("MIDI track payload is truncated");
        }
    }
    return header;
}

class MidiTrackCursor final {
public:
    MidiTrackCursor(
        std::ifstream& input,
        const MidiTrack& track
    ) : input_(&input), remaining_(track.length) {
        input_->clear();
        input_->seekg(static_cast<std::streamoff>(track.offset));
        if (!*input_) throw MusicalError("cannot seek MIDI track");
    }

    [[nodiscard]] bool empty() const noexcept { return remaining_ == 0U; }

    [[nodiscard]] std::uint8_t byte() {
        if (remaining_ == 0U) {
            throw MusicalError("MIDI track event is truncated");
        }
        const auto value = input_->get();
        if (value < 0) throw MusicalError("MIDI track is truncated");
        --remaining_;
        return static_cast<std::uint8_t>(value);
    }

    [[nodiscard]] std::uint32_t vlq() {
        std::uint32_t value{};
        for (unsigned index = 0U; index < 4U; ++index) {
            const auto current = byte();
            if (value > 0x01FF'FFFFU) {
                throw MusicalError("MIDI variable-length quantity overflows");
            }
            value = (value << 7U) | (current & 0x7FU);
            if ((current & 0x80U) == 0U) {
                return value;
            }
        }
        throw MusicalError("MIDI variable-length quantity exceeds four bytes");
    }

    [[nodiscard]] std::vector<std::uint8_t> payload(
        const std::uint32_t length,
        const std::size_t maximum
    ) {
        if (length > remaining_) {
            throw MusicalError("MIDI event payload exceeds track chunk");
        }
        if (length > maximum) {
            throw MusicalError("MIDI metadata payload exceeds configured limit");
        }
        std::vector<std::uint8_t> data(length);
        for (auto& value : data) value = byte();
        return data;
    }

    void skip(const std::uint32_t length) {
        if (length > remaining_) {
            throw MusicalError("MIDI event payload exceeds track chunk");
        }
        input_->seekg(static_cast<std::streamoff>(length), std::ios::cur);
        if (!*input_) throw MusicalError("cannot skip MIDI event payload");
        remaining_ -= length;
    }

private:
    std::ifstream* input_{};
    std::uint64_t remaining_{};
};

struct MidiEvent {
    std::uint64_t tick{};
    std::uint8_t status{};
    std::array<std::uint8_t, 2> data{};
    std::uint8_t data_count{};
};

template <typename ChannelCallback, typename MetaCallback>
void visit_midi_track(
    std::ifstream& input,
    const MidiTrack& track,
    const std::size_t maximum_meta_bytes,
    ChannelCallback&& channel_callback,
    MetaCallback&& meta_callback
) {
    MidiTrackCursor cursor(input, track);
    std::uint64_t tick{};
    std::uint8_t running_status{};

    while (!cursor.empty()) {
        const auto delta = cursor.vlq();
        if (delta > std::numeric_limits<std::uint64_t>::max() - tick) {
            throw MusicalError("MIDI absolute tick overflows");
        }
        tick += delta;

        auto status = cursor.byte();
        bool running{};
        std::uint8_t first_data{};
        if (status < 0x80U) {
            if (running_status < 0x80U || running_status >= 0xF0U) {
                throw MusicalError("MIDI running status is invalid");
            }
            first_data = status;
            status = running_status;
            running = true;
        }

        if (status >= 0x80U && status <= 0xEFU) {
            running_status = status;
            const auto high = status & 0xF0U;
            const auto count =
                high == 0xC0U || high == 0xD0U ? 1U : 2U;
            MidiEvent event;
            event.tick = tick;
            event.status = status;
            event.data_count = static_cast<std::uint8_t>(count);
            event.data[0] = running ? first_data : cursor.byte();
            if (count == 2U) event.data[1] = cursor.byte();
            channel_callback(event);
            continue;
        }

        running_status = 0U;
        if (status == 0xFFU) {
            const auto type = cursor.byte();
            const auto length = cursor.vlq();
            if (type == 0x2FU) {
                cursor.skip(length);
                break;
            }
            if (type == 0x51U || type == 0x58U || type == 0x03U
                || type == 0x7FU) {
                const auto payload = cursor.payload(length, maximum_meta_bytes);
                meta_callback(tick, type, payload);
            } else {
                cursor.skip(length);
            }
            continue;
        }
        if (status == 0xF0U || status == 0xF7U) {
            cursor.skip(cursor.vlq());
            continue;
        }

        // Rare system-common/realtime bytes are legal in some files. Consume
        // their fixed data length so the track remains parseable.
        std::uint8_t count{};
        switch (status) {
        case 0xF1U:
        case 0xF3U: count = 1U; break;
        case 0xF2U: count = 2U; break;
        case 0xF6U:
        case 0xF8U:
        case 0xFAU:
        case 0xFBU:
        case 0xFCU:
        case 0xFEU: count = 0U; break;
        default:
            throw MusicalError("unsupported/invalid MIDI system event");
        }
        for (std::uint8_t index = 0U; index < count; ++index) {
            static_cast<void>(cursor.byte());
        }
    }
}

struct MidiPulseMetadata {
    std::optional<Json> metadata;
    std::vector<std::string> kinds{"normal"};
    std::uint8_t lane_base{60U};
    std::uint8_t opponent_channel{0U};
    std::uint8_t player_channel{1U};
    std::uint16_t key_count{4U};
};

void parse_pulse_meta(
    const std::span<const std::uint8_t> payload,
    MidiPulseMetadata& pulse
) {
    constexpr std::string_view prefix{"PFMETA1"};
    if (payload.size() <= prefix.size()
        || !std::equal(
            prefix.begin(),
            prefix.end(),
            payload.begin()
        )) {
        return;
    }
    try {
        const std::string text(
            reinterpret_cast<const char*>(payload.data() + prefix.size()),
            payload.size() - prefix.size()
        );
        const auto json = Json::parse(text);
        pulse.metadata = json;
        pulse.lane_base = static_cast<std::uint8_t>(
            std::min<std::uint64_t>(
                json.value("laneBaseNote", std::uint64_t{60U}),
                127U
            )
        );
        pulse.opponent_channel = static_cast<std::uint8_t>(
            std::min<std::uint64_t>(
                json.value("opponentChannel", std::uint64_t{0U}),
                15U
            )
        );
        pulse.player_channel = static_cast<std::uint8_t>(
            std::min<std::uint64_t>(
                json.value("playerChannel", std::uint64_t{1U}),
                15U
            )
        );
        const auto keys = json.value("keyCount", std::uint64_t{4U});
        if (keys >= 1U && keys <= maximum_supported_key_count) {
            pulse.key_count = static_cast<std::uint16_t>(keys);
        }
        if (const auto kinds = json.find("kinds");
            kinds != json.end() && kinds->is_array()) {
            std::vector<std::string> parsed;
            for (const auto& item : *kinds) {
                if (item.is_string()) parsed.push_back(item.get<std::string>());
            }
            if (!parsed.empty()) pulse.kinds = std::move(parsed);
        }
    } catch (...) {
        // Non-PulseForge sequencer metadata is simply ignored.
    }
}

struct PendingMidiKind {
    std::uint32_t kind_id{};
    std::uint16_t flags{};
};

void parse_note_meta(
    const std::span<const std::uint8_t> payload,
    std::array<std::deque<PendingMidiKind>, 16U * 128U>& pending
) {
    constexpr std::array<std::uint8_t, 4> prefix{'P','F','N','1'};
    if (payload.size() != 12U
        || !std::equal(prefix.begin(), prefix.end(), payload.begin())) {
        return;
    }
    const auto channel = payload[4];
    const auto pitch = payload[5];
    if (channel >= 16U || pitch >= 128U) return;
    const auto kind =
        (static_cast<std::uint32_t>(payload[6]) << 24U)
        | (static_cast<std::uint32_t>(payload[7]) << 16U)
        | (static_cast<std::uint32_t>(payload[8]) << 8U)
        | static_cast<std::uint32_t>(payload[9]);
    const auto flags = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(payload[10]) << 8U)
        | payload[11]
    );
    pending[
        static_cast<std::size_t>(channel) * 128U + pitch
    ].push_back(PendingMidiKind{kind, flags});
}

struct MidiActiveNote {
    std::uint64_t tick{};
    std::uint32_t kind_id{};
    std::uint16_t flags{};
};

struct SortRun {
    std::filesystem::path path;
    std::uint64_t count{};
};

[[nodiscard]] bool packed_note_less(
    const PackedNote& left,
    const PackedNote& right
) noexcept {
    return std::tuple{
        left.time_us,
        static_cast<std::uint8_t>(left.owner),
        left.lane,
        left.kind_id,
        left.duration_us,
        left.flags
    } < std::tuple{
        right.time_us,
        static_cast<std::uint8_t>(right.owner),
        right.lane,
        right.kind_id,
        right.duration_us,
        right.flags
    };
}

void write_temp_note(std::ostream& output, const PackedNote& note) {
    write_le(output, note.time_us);
    write_le(output, note.duration_us);
    write_le(output, note.lane);
    output.put(static_cast<char>(note.owner));
    write_le(output, note.flags);
    write_le(output, note.kind_id);
}

[[nodiscard]] bool read_temp_note(std::istream& input, PackedNote& note) {
    const auto first = input.peek();
    if (first < 0) return false;
    note.time_us = read_le<std::int64_t>(input);
    note.duration_us = read_le<std::uint64_t>(input);
    note.lane = read_le<std::uint16_t>(input);
    const auto owner = input.get();
    if (owner < 0 || owner > 1) {
        throw MusicalError("temporary MIDI note run is corrupt");
    }
    note.owner = static_cast<PackedNoteOwner>(owner);
    note.flags = read_le<std::uint16_t>(input);
    note.kind_id = read_le<std::uint32_t>(input);
    return true;
}

void flush_sort_run(
    std::vector<PackedNote>& buffer,
    std::vector<SortRun>& runs,
    const std::filesystem::path& temporary_directory
) {
    if (buffer.empty()) return;
    std::sort(buffer.begin(), buffer.end(), packed_note_less);
    const auto path = temporary_directory
        / ("run-" + std::to_string(runs.size()) + ".bin");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw MusicalError("cannot create MIDI sort run");
    for (const auto& note : buffer) write_temp_note(output, note);
    output.flush();
    if (!output) throw MusicalError("cannot flush MIDI sort run");
    runs.push_back({path, static_cast<std::uint64_t>(buffer.size())});
    buffer.clear();
}

struct RunCursor {
    std::ifstream input;
    PackedNote current;
    bool valid{};
};

struct RunLater {
    const std::vector<RunCursor>* cursors{};

    bool operator()(const std::size_t left, const std::size_t right) const {
        return packed_note_less(
            (*cursors)[right].current,
            (*cursors)[left].current
        );
    }
};

[[nodiscard]] std::uint64_t merge_runs_to_pfc(
    const std::vector<SortRun>& runs,
    const std::filesystem::path& destination,
    const PackedChartStreamSpec& spec,
    const PackedChartWriteOptions& options
) {
    std::string error;
    auto writer = PackedChartStreamWriter::create(
        destination,
        spec,
        options,
        &error
    );
    if (!writer.has_value()) {
        throw MusicalError("cannot create PFC1: " + error);
    }
    std::vector<RunCursor> cursors(runs.size());
    std::vector<std::size_t> heap;
    heap.reserve(runs.size());
    for (std::size_t index = 0U; index < runs.size(); ++index) {
        cursors[index].input.open(runs[index].path, std::ios::binary);
        if (!cursors[index].input) {
            throw MusicalError("cannot reopen MIDI sort run");
        }
        cursors[index].valid =
            read_temp_note(cursors[index].input, cursors[index].current);
        if (cursors[index].valid) heap.push_back(index);
    }
    RunLater comparison{&cursors};
    std::make_heap(heap.begin(), heap.end(), comparison);
    std::uint64_t written{};
    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), comparison);
        const auto index = heap.back();
        heap.pop_back();
        if (!writer->append(cursors[index].current, &error)) {
            throw MusicalError("cannot append MIDI note to PFC1: " + error);
        }
        ++written;
        cursors[index].valid =
            read_temp_note(cursors[index].input, cursors[index].current);
        if (cursors[index].valid) {
            heap.push_back(index);
            std::push_heap(heap.begin(), heap.end(), comparison);
        }
    }
    if (!writer->finish(&error)) {
        throw MusicalError("cannot finish MIDI PFC1: " + error);
    }
    return written;
}

[[nodiscard]] std::uint8_t time_signature_denominator_power(
    std::uint16_t denominator
) noexcept {
    std::uint8_t power{};
    denominator = std::max<std::uint16_t>(1U, denominator);
    while (denominator > 1U && power < 7U) {
        denominator >>= 1U;
        ++power;
    }
    return power;
}

struct MidiWriteEvent {
    std::uint64_t tick{};
    std::uint8_t priority{};
    std::vector<std::uint8_t> data;
};

void append_meta_event(
    std::vector<MidiWriteEvent>& events,
    const std::uint64_t tick,
    const std::uint8_t type,
    std::vector<std::uint8_t> payload,
    const std::uint8_t priority = 0U
) {
    std::vector<std::uint8_t> data{0xFFU, type};
    write_midi_vlq(data, static_cast<std::uint32_t>(payload.size()));
    data.insert(data.end(), payload.begin(), payload.end());
    events.push_back({tick, priority, std::move(data)});
}

[[nodiscard]] std::vector<std::uint8_t> make_track(
    std::vector<MidiWriteEvent> events
) {
    std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
        return std::tuple{a.tick, a.priority}
            < std::tuple{b.tick, b.priority};
    });
    std::vector<std::uint8_t> bytes;
    std::uint64_t previous{};
    for (const auto& event : events) {
        auto delta = event.tick - previous;
        while (delta > 0x0FFF'FFFFULL) {
            // Preserve a legal 4-byte VLQ by inserting zero-length text meta
            // events as timing spacers.
            write_midi_vlq(bytes, 0x0FFF'FFFFU);
            bytes.insert(bytes.end(), {0xFFU, 0x01U, 0x00U});
            delta -= 0x0FFF'FFFFULL;
            previous += 0x0FFF'FFFFULL;
        }
        write_midi_vlq(bytes, static_cast<std::uint32_t>(delta));
        bytes.insert(bytes.end(), event.data.begin(), event.data.end());
        previous = event.tick;
    }
    write_midi_vlq(bytes, 0U);
    bytes.insert(bytes.end(), {0xFFU, 0x2FU, 0x00U});
    return bytes;
}

void write_midi_track(std::ostream& output, const std::vector<std::uint8_t>& data) {
    if (data.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw MusicalError("MIDI track exceeds 32-bit SMF chunk length");
    }
    output.write("MTrk", 4);
    write_be32(output, static_cast<std::uint32_t>(data.size()));
    output.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(data.size())
    );
    if (!output) throw MusicalError("cannot write MIDI track");
}

struct DetectedExplicitRun {
    std::uint64_t first_index{};
    std::uint64_t count{};
    PfmPatternRecord pattern;
};

struct ArithmeticRunCandidate {
    std::uint64_t first_index{};
    std::uint64_t count{};
    std::uint64_t first_tick{};
    std::uint64_t last_tick{};
    std::uint64_t interval_tick{};
    std::uint64_t duration_tick{};
    PackedNoteOwner owner{PackedNoteOwner::player};
    std::uint16_t flags{};
    std::uint32_t kind_id{};
    std::vector<std::uint16_t> lane_probe;
    std::size_t lane_period{};
    bool have_interval{};
};

[[nodiscard]] std::size_t detect_lane_period(
    const std::vector<std::uint16_t>& lanes,
    const std::size_t maximum_period
) noexcept {
    if (lanes.size() < 2U) return 0U;
    const auto limit = std::min(maximum_period, lanes.size() / 2U);
    for (std::size_t period = 1U; period <= limit; ++period) {
        if (lanes.size() < period * 2U) continue;
        bool matches = true;
        for (std::size_t index = period; index < lanes.size(); ++index) {
            if (lanes[index] != lanes[index % period]) {
                matches = false;
                break;
            }
        }
        if (matches) return period;
    }
    return 0U;
}

[[nodiscard]] std::vector<DetectedExplicitRun> detect_explicit_arithmetic_runs(
    const PackedChartReader& reader,
    const TickTimeMap& map,
    const std::uint64_t minimum_count = 32U
) {
    std::vector<DetectedExplicitRun> runs;
    ArithmeticRunCandidate candidate;
    bool active{};
    std::uint64_t global_index{};
    const auto max_period = std::min<std::size_t>(
        256U,
        std::max<std::size_t>(
            static_cast<std::size_t>(reader.key_count()),
            static_cast<std::size_t>(reader.key_count()) * 16U
        )
    );
    constexpr std::size_t maximum_probe_multiplier = 3U;

    const auto finalize = [&]() {
        if (!active || candidate.count < minimum_count) {
            active = false;
            candidate = {};
            return;
        }
        auto period = candidate.lane_period;
        if (period == 0U) {
            period = detect_lane_period(candidate.lane_probe, max_period);
        }
        if (period == 0U) {
            active = false;
            candidate = {};
            return;
        }
        PfmPatternRecord pattern;
        pattern.domain = PfmPatternRecord::Domain::ticks;
        pattern.start_tick = candidate.first_tick;
        pattern.interval = candidate.interval_tick;
        pattern.count = candidate.count;
        pattern.duration = candidate.duration_tick;
        pattern.owner = candidate.owner;
        pattern.flags = candidate.flags;
        pattern.kind_id = candidate.kind_id;
        pattern.lanes.assign(
            candidate.lane_probe.begin(),
            candidate.lane_probe.begin()
                + static_cast<std::ptrdiff_t>(period)
        );
        runs.push_back({candidate.first_index, candidate.count, std::move(pattern)});
        active = false;
        candidate = {};
    };

    const auto start_candidate = [&](const PackedNote& note,
                                     const std::uint64_t tick,
                                     const std::uint64_t duration_tick,
                                     const std::uint64_t index) {
        candidate = {};
        candidate.first_index = index;
        candidate.count = 1U;
        candidate.first_tick = tick;
        candidate.last_tick = tick;
        candidate.duration_tick = duration_tick;
        candidate.owner = note.owner;
        candidate.flags = note.flags;
        candidate.kind_id = note.kind_id;
        candidate.lane_probe.push_back(note.lane);
        active = true;
    };

    for (std::uint64_t chunk = 0U; chunk < reader.chunk_count(); ++chunk) {
        const auto decoded = reader.read_chunk(chunk);
        if (!decoded) {
            throw MusicalError(
                "cannot scan PFC1 for arithmetic PFM runs: " + decoded.error
            );
        }
        for (const auto& note : decoded.notes) {
            const auto start_us = static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, note.time_us)
            );
            if (note.duration_us
                > std::numeric_limits<std::uint64_t>::max() - start_us) {
                throw MusicalError("PFC1 note end time overflows during run detection");
            }
            const auto end_us = start_us + note.duration_us;
            const auto tick = map.us_to_tick(start_us);
            const auto end_tick = map.us_to_tick(end_us);
            const auto duration_tick = end_tick >= tick
                ? end_tick - tick
                : 0U;

            bool accepted = false;
            if (active
                && note.owner == candidate.owner
                && note.flags == candidate.flags
                && note.kind_id == candidate.kind_id
                && duration_tick == candidate.duration_tick
                && tick >= candidate.last_tick) {
                const auto delta = tick - candidate.last_tick;
                if (!candidate.have_interval) {
                    candidate.interval_tick = delta;
                    candidate.have_interval = true;
                    accepted = true;
                } else if (delta == candidate.interval_tick) {
                    accepted = true;
                }

                if (accepted && candidate.lane_period != 0U) {
                    const auto expected = candidate.lane_probe[
                        static_cast<std::size_t>(candidate.count)
                            % candidate.lane_period
                    ];
                    if (note.lane != expected) accepted = false;
                }
            }

            if (!active) {
                start_candidate(note, tick, duration_tick, global_index);
            } else if (accepted) {
                if (candidate.lane_period == 0U) {
                    const auto maximum_probe = std::max<std::size_t>(
                        2U,
                        max_period * maximum_probe_multiplier
                    );
                    if (candidate.lane_probe.size() < maximum_probe) {
                        candidate.lane_probe.push_back(note.lane);
                    }
                    // Wait for at least two complete maximum-size lane cycles
                    // before committing to a period. Otherwise prefixes such
                    // as [0,0] can falsely lock to period 1 even when the real
                    // motif is [0,0,1,1].
                    if (candidate.lane_probe.size()
                        >= std::max<std::size_t>(2U, max_period * 2U)) {
                        candidate.lane_period = detect_lane_period(
                            candidate.lane_probe,
                            max_period
                        );
                    }
                }
                ++candidate.count;
                candidate.last_tick = tick;
            } else {
                finalize();
                start_candidate(note, tick, duration_tick, global_index);
            }
            ++global_index;
        }
    }
    finalize();
    return runs;
}

}  // namespace

bool is_midi_chart_path(const std::filesystem::path& path) noexcept {
    const auto ext = lower_ascii(path_utf8(path.extension()));
    return ext == ".mid" || ext == ".midi";
}

bool is_pfm_chart_path(const std::filesystem::path& path) noexcept {
    return lower_ascii(path_utf8(path.extension())) == ".pfm";
}

bool is_pfm_source_path(const std::filesystem::path& path) noexcept {
    return lower_ascii(path_utf8(path.filename())).ends_with(".pfm.json");
}

MusicalChartCompileResult compile_midi_chart_to_pfc(
    const std::filesystem::path& source_midi,
    const std::filesystem::path& destination_pfc,
    const MidiChartOptions& options
) {
    MusicalChartCompileResult result;
    result.source_format = ChartFormat::midi;
    std::filesystem::path temporary_directory;
    try {
        result.source_bytes = file_size_bounded(
            source_midi,
            options.limits.max_source_bytes
        );
        result.source_fingerprint = fingerprint_file(
            source_midi,
            options.limits.max_source_bytes
        );
        std::ifstream input(source_midi, std::ios::binary);
        if (!input) throw MusicalError("cannot open MIDI chart");

        auto header = read_midi_header(input, options.limits);
        MidiPulseMetadata pulse;
        pulse.lane_base = options.lane_base_note;
        pulse.opponent_channel = options.opponent_channel;
        pulse.player_channel = options.player_channel;
        pulse.key_count = options.fallback_key_count;

        std::vector<TickTempo> tempos;
        for (auto& track : header.tracks) {
            visit_midi_track(
                input,
                track,
                options.limits.max_metadata_bytes,
                [](const MidiEvent&) {},
                [&](const std::uint64_t tick,
                    const std::uint8_t type,
                    const std::vector<std::uint8_t>& payload) {
                    if (type == 0x51U && payload.size() == 3U) {
                        const auto mpq =
                            (static_cast<std::uint32_t>(payload[0]) << 16U)
                            | (static_cast<std::uint32_t>(payload[1]) << 8U)
                            | payload[2];
                        if (mpq != 0U) tempos.push_back({tick, mpq, 4U, 4U});
                    } else if (type == 0x58U && payload.size() >= 2U) {
                        const auto denominator = static_cast<std::uint16_t>(
                            1U << std::min<std::uint8_t>(payload[1], 15U)
                        );
                        auto found = std::find_if(
                            tempos.rbegin(),
                            tempos.rend(),
                            [&](const TickTempo& tempo) {
                                return tempo.tick <= tick;
                            }
                        );
                        if (found != tempos.rend()) {
                            found->numerator = payload[0];
                            found->denominator = denominator;
                        } else {
                            tempos.push_back({
                                tick, 500'000U, payload[0], denominator
                            });
                        }
                    } else if (type == 0x03U) {
                        track.name.assign(
                            reinterpret_cast<const char*>(payload.data()),
                            payload.size()
                        );
                        const auto name = lower_ascii(track.name);
                        if (name.find("opponent") != std::string::npos) {
                            track.owner_hint = PackedNoteOwner::opponent;
                        } else if (name.find("player") != std::string::npos
                            || name.find("bf") != std::string::npos) {
                            track.owner_hint = PackedNoteOwner::player;
                        }
                    } else if (type == 0x7FU) {
                        parse_pulse_meta(payload, pulse);
                    }
                }
            );
        }

        const TickTimeMap time_map(header.ppqn, std::move(tempos));
        Chart metadata = chart_from_metadata_and_tempos(
            source_midi,
            pulse.metadata ? &*pulse.metadata : nullptr,
            time_map,
            ChartFormat::midi,
            pulse.key_count,
            options.limits
        );
        metadata.source_format = ChartFormat::midi;

        temporary_directory = destination_pfc;
        temporary_directory += ".midi-sort";
        std::error_code fs_error;
        std::filesystem::remove_all(temporary_directory, fs_error);
        fs_error.clear();
        std::filesystem::create_directories(temporary_directory, fs_error);
        if (fs_error) throw MusicalError("cannot create MIDI sort directory");

        std::vector<PackedNote> sort_buffer;
        sort_buffer.reserve(options.limits.max_sort_notes_in_memory);
        std::vector<SortRun> runs;
        std::uint64_t total_notes{};
        std::uint64_t content_end_us{};
        std::uint16_t discovered_keys = pulse.key_count;

        for (const auto& track : header.tracks) {
            std::array<std::deque<MidiActiveNote>, 16U * 128U> active;
            std::array<std::deque<PendingMidiKind>, 16U * 128U> pending;

            visit_midi_track(
                input,
                track,
                options.limits.max_metadata_bytes,
                [&](const MidiEvent& event) {
                    const auto high = event.status & 0xF0U;
                    if (high != 0x80U && high != 0x90U) return;
                    const auto channel = event.status & 0x0FU;
                    const auto pitch = event.data[0];
                    const auto velocity = event.data[1];
                    const bool on = high == 0x90U && velocity != 0U;
                    const auto key = static_cast<std::size_t>(channel) * 128U
                        + pitch;

                    if (on) {
                        MidiActiveNote active_note;
                        active_note.tick = event.tick;
                        if (!pending[key].empty()) {
                            active_note.kind_id = pending[key].front().kind_id;
                            active_note.flags = pending[key].front().flags;
                            pending[key].pop_front();
                        }
                        active[key].push_back(active_note);
                        return;
                    }
                    if (active[key].empty()) return;
                    const auto started = active[key].front();
                    active[key].pop_front();

                    if (pitch < pulse.lane_base) return;
                    const auto lane_value = static_cast<std::uint16_t>(
                        pitch - pulse.lane_base
                    );
                    if (lane_value >= maximum_supported_key_count) return;
                    discovered_keys = std::max<std::uint16_t>(
                        discovered_keys,
                        static_cast<std::uint16_t>(lane_value + 1U)
                    );

                    const auto start_us = time_map.tick_to_us(started.tick);
                    const auto end_us = time_map.tick_to_us(event.tick);
                    if (start_us > static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max())) {
                        throw MusicalError("MIDI note time exceeds PFC1 int64 range");
                    }
                    PackedNote note;
                    note.time_us = static_cast<std::int64_t>(start_us);
                    note.duration_us = end_us >= start_us
                        ? end_us - start_us
                        : 0U;
                    note.lane = lane_value;
                    note.owner = track.owner_hint.value_or(
                        channel == pulse.opponent_channel
                            ? PackedNoteOwner::opponent
                            : PackedNoteOwner::player
                    );
                    note.flags = started.flags;
                    note.kind_id = started.kind_id < pulse.kinds.size()
                        ? started.kind_id
                        : 0U;
                    sort_buffer.push_back(note);
                    ++total_notes;
                    content_end_us = std::max(
                        content_end_us,
                        end_us
                    );
                    if (total_notes > options.limits.max_midi_notes) {
                        throw MusicalError(
                            "MIDI note count exceeds configured import limit; "
                            "use PFM PatternRuns for extreme charts"
                        );
                    }
                    if (sort_buffer.size()
                        >= options.limits.max_sort_notes_in_memory) {
                        flush_sort_run(
                            sort_buffer,
                            runs,
                            temporary_directory
                        );
                    }
                },
                [&](const std::uint64_t tick,
                    const std::uint8_t type,
                    const std::vector<std::uint8_t>& payload) {
                    if (type == 0x7FU) {
                        static_cast<void>(tick);
                        parse_note_meta(payload, pending);
                    }
                }
            );
        }
        flush_sort_run(sort_buffer, runs, temporary_directory);

        if (pulse.kinds.empty()) pulse.kinds.push_back("normal");
        metadata.key_count = std::max<std::uint16_t>(
            1U,
            std::min<std::uint16_t>(
                discovered_keys,
                maximum_supported_key_count
            )
        );
        PackedChartStreamSpec spec;
        spec.key_count = metadata.key_count;
        spec.explicit_note_count = total_notes;
        spec.kinds = pulse.kinds;

        const auto written = merge_runs_to_pfc(
            runs,
            destination_pfc,
            spec,
            options.packed
        );
        if (written != total_notes) {
            throw MusicalError("MIDI PFC1 merge count mismatch");
        }

        result.success = true;
        result.explicit_note_count = total_notes;
        result.logical_note_count = total_notes;
        result.content_end_us = content_end_us;
        result.key_count = metadata.key_count;
        result.chart_metadata = std::move(metadata);

        std::filesystem::remove_all(temporary_directory, fs_error);
        return result;
    } catch (const std::bad_alloc&) {
        result.error = "MIDI import exhausted memory while holding bounded "
            "track/merge state";
        std::error_code ignored;
        if (!temporary_directory.empty()) {
            std::filesystem::remove_all(temporary_directory, ignored);
        }
        return result;
    } catch (const std::exception& exception) {
        result.error = exception.what();
        std::error_code ignored;
        if (!temporary_directory.empty()) {
            std::filesystem::remove_all(temporary_directory, ignored);
        }
        return result;
    }
}

MusicalChartCompileResult compile_pfm_chart_to_pfc(
    const std::filesystem::path& source_pfm,
    const std::filesystem::path& destination_pfc,
    const PfmChartOptions& options
) {
    MusicalChartCompileResult result;
    result.source_format = ChartFormat::pfm;
    try {
        result.source_bytes = file_size_bounded(
            source_pfm,
            options.limits.max_source_bytes
        );
        result.source_fingerprint = fingerprint_file(
            source_pfm,
            options.limits.max_source_bytes
        );
        std::ifstream input(source_pfm, std::ios::binary);
        if (!input) throw MusicalError("cannot open PFM chart");

        std::array<char, 4> magic{};
        input.read(magic.data(), 4);
        if (!input || !std::equal(magic.begin(), magic.end(), pfm_magic.begin())) {
            throw MusicalError("file is not a PFM1 chart");
        }
        const auto version = read_le<std::uint16_t>(input);
        const auto flags = read_le<std::uint16_t>(input);
        const auto ppqn = read_le<std::uint32_t>(input);
        const auto key_count = read_le<std::uint16_t>(input);
        static_cast<void>(read_le<std::uint16_t>(input));
        const auto kind_count = read_le<std::uint32_t>(input);
        const auto tempo_count = read_le<std::uint32_t>(input);
        const auto explicit_count = read_le<std::uint64_t>(input);
        const auto pattern_count = read_le<std::uint64_t>(input);
        const auto metadata_bytes = read_le<std::uint32_t>(input);

        if (version != pfm_version || ppqn == 0U
            || key_count == 0U || key_count > maximum_supported_key_count
            || explicit_count > options.limits.max_pfm_explicit_notes
            || pattern_count > options.limits.max_pfm_patterns
            || metadata_bytes > options.limits.max_metadata_bytes) {
            throw MusicalError("PFM header values are invalid or exceed limits");
        }

        std::optional<Json> embedded;
        if (metadata_bytes != 0U) {
            std::string text(metadata_bytes, '\0');
            input.read(text.data(), static_cast<std::streamsize>(text.size()));
            if (!input) throw MusicalError("PFM metadata is truncated");
            if ((flags & pfm_flag_embedded_metadata) != 0U) {
                embedded = Json::parse(text);
            }
        }

        std::vector<std::string> kinds;
        kinds.reserve(kind_count);
        for (std::uint32_t index = 0U; index < kind_count; ++index) {
            kinds.push_back(read_string(input, options.limits.max_kind_bytes));
        }
        if (kinds.empty()) kinds.push_back("normal");

        std::vector<TickTempo> tempos;
        tempos.reserve(tempo_count);
        std::uint64_t tick{};
        for (std::uint32_t index = 0U; index < tempo_count; ++index) {
            const auto delta = read_uvar(input);
            if (delta > std::numeric_limits<std::uint64_t>::max() - tick) {
                throw MusicalError("PFM tempo tick overflows");
            }
            tick += delta;
            const auto mpq = read_uvar(input);
            const auto numerator = read_uvar(input);
            const auto denominator = read_uvar(input);
            if (mpq == 0U || mpq > std::numeric_limits<std::uint32_t>::max()
                || numerator == 0U
                || numerator > std::numeric_limits<std::uint16_t>::max()
                || denominator == 0U
                || denominator > std::numeric_limits<std::uint16_t>::max()) {
                throw MusicalError("PFM tempo entry is invalid");
            }
            tempos.push_back({
                tick,
                static_cast<std::uint32_t>(mpq),
                static_cast<std::uint16_t>(numerator),
                static_cast<std::uint16_t>(denominator),
            });
        }
        const TickTimeMap map(ppqn, std::move(tempos));

        std::vector<PatternRun> patterns;
        std::uint64_t logical_count = explicit_count;
        patterns.reserve(static_cast<std::size_t>(
            std::min<std::uint64_t>(
                pattern_count * 2U,
                options.limits.max_pfm_patterns
            )
        ));
        for (std::uint64_t index = 0U; index < pattern_count; ++index) {
            const auto record = read_pfm_pattern(
                input,
                options.limits,
                key_count,
                kinds.size()
            );
            if (record.count > options.limits.max_logical_notes - logical_count) {
                throw MusicalError("PFM logical note count exceeds limit");
            }
            logical_count += record.count;
            if (record.domain == PfmPatternRecord::Domain::ticks) {
                auto compiled = compile_tick_pattern(record, map);
                patterns.insert(
                    patterns.end(),
                    std::make_move_iterator(compiled.begin()),
                    std::make_move_iterator(compiled.end())
                );
            } else {
                patterns.push_back(PatternRun{
                    record.start_us,
                    record.interval,
                    record.count,
                    record.duration,
                    record.lanes,
                    record.owner,
                    record.flags,
                    record.kind_id,
                    record.interval_denominator,
                });
            }
        }
        if (patterns.size() > options.packed.limits.max_patterns) {
            throw MusicalError(
                "PFM tempo splitting produced too many PFC1 PatternRuns"
            );
        }

        PackedChartStreamSpec spec;
        spec.key_count = key_count;
        spec.explicit_note_count = explicit_count;
        spec.kinds = kinds;
        spec.patterns = std::move(patterns);

        std::string writer_error;
        auto writer = PackedChartStreamWriter::create(
            destination_pfc,
            spec,
            options.packed,
            &writer_error
        );
        if (!writer.has_value()) {
            throw MusicalError("cannot create PFM PFC1: " + writer_error);
        }

        std::uint64_t previous_tick{};
        std::uint64_t content_end_us{};
        for (std::uint64_t index = 0U; index < explicit_count; ++index) {
            const auto delta_tick = read_uvar(input);
            if (delta_tick > std::numeric_limits<std::uint64_t>::max()
                    - previous_tick) {
                throw MusicalError("PFM explicit note tick overflows");
            }
            const auto note_tick = previous_tick + delta_tick;
            previous_tick = note_tick;
            const auto duration_tick = read_uvar(input);
            const auto lane = read_uvar(input);
            const auto owner = input.get();
            const auto note_flags = read_uvar(input);
            const auto kind = read_uvar(input);
            if (lane >= key_count || owner < 0 || owner > 1
                || note_flags > std::numeric_limits<std::uint16_t>::max()
                || kind >= kinds.size()) {
                throw MusicalError("PFM explicit note record is invalid");
            }
            if (duration_tick
                > std::numeric_limits<std::uint64_t>::max() - note_tick) {
                throw MusicalError("PFM explicit note duration tick overflows");
            }
            const auto start_us = map.tick_to_us(note_tick);
            const auto end_us = map.tick_to_us(note_tick + duration_tick);
            if (start_us > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
                throw MusicalError("PFM explicit note time exceeds PFC1 int64 range");
            }
            PackedNote note{
                static_cast<std::int64_t>(start_us),
                end_us >= start_us ? end_us - start_us : 0U,
                static_cast<std::uint16_t>(lane),
                static_cast<PackedNoteOwner>(owner),
                static_cast<std::uint16_t>(note_flags),
                static_cast<std::uint32_t>(kind),
            };
            if (!writer->append(note, &writer_error)) {
                throw MusicalError("cannot append PFM note: " + writer_error);
            }
            content_end_us = std::max(content_end_us, end_us);
        }
        if (!writer->finish(&writer_error)) {
            throw MusicalError("cannot finish PFM PFC1: " + writer_error);
        }

        Chart metadata = chart_from_metadata_and_tempos(
            source_pfm,
            embedded ? &*embedded : nullptr,
            map,
            ChartFormat::pfm,
            key_count,
            options.limits
        );
        metadata.source_format = ChartFormat::pfm;

        for (const auto& pattern : spec.patterns) {
            if (pattern.count == 0U) continue;
            if (const auto last = pattern.note_at(pattern.count - 1U);
                last.has_value()) {
                const auto end = static_cast<std::uint64_t>(
                    std::max<std::int64_t>(0, last->time_us)
                ) + last->duration_us;
                content_end_us = std::max(content_end_us, end);
            }
        }

        result.success = true;
        result.explicit_note_count = explicit_count;
        result.logical_note_count = logical_count;
        result.content_end_us = content_end_us;
        result.key_count = key_count;
        result.chart_metadata = std::move(metadata);
        return result;
    } catch (const std::bad_alloc&) {
        result.error = "PFM import exhausted memory while holding bounded "
            "metadata/pattern state";
        return result;
    } catch (const std::exception& exception) {
        result.error = exception.what();
        return result;
    }
}

MusicalChartCompileResult compile_pfm_source_to_pfc(
    const std::filesystem::path& source_json,
    const std::filesystem::path& destination_pfc,
    const PfmChartOptions& options
) {
    MusicalChartCompileResult result;
        result.source_format = ChartFormat::pfm;
    try {
        result.source_bytes = file_size_bounded(
            source_json,
            options.limits.max_source_bytes
        );
        result.source_fingerprint = fingerprint_file(
            source_json,
            options.limits.max_source_bytes
        );
        std::ifstream input(source_json, std::ios::binary);
        if (!input) throw MusicalError("cannot open PFM source JSON");
        Json root;
        input >> root;
        if (!root.is_object()
            || root.value("format", std::string{})
                != "pulseforge-pfm-source-v1") {
            throw MusicalError(
                "PFM source must declare format=pulseforge-pfm-source-v1"
            );
        }

        const auto ppqn64 = root.value(
            "ppqn",
            static_cast<std::uint64_t>(options.ppqn)
        );
        const auto key_count64 = root.value("keyCount", std::uint64_t{4U});
        if (ppqn64 == 0U
            || ppqn64 > std::numeric_limits<std::uint32_t>::max()
            || key_count64 == 0U
            || key_count64 > maximum_supported_key_count) {
            throw MusicalError("PFM source PPQN/keyCount is invalid");
        }
        const auto ppqn = static_cast<std::uint32_t>(ppqn64);
        const auto key_count = static_cast<std::uint16_t>(key_count64);

        std::vector<std::string> kinds{"normal"};
        if (const auto list = root.find("kinds");
            list != root.end() && list->is_array()) {
            kinds.clear();
            for (const auto& item : *list) {
                if (!item.is_string()) {
                    throw MusicalError("PFM source kind must be a string");
                }
                const auto value = item.get<std::string>();
                if (value.size() > options.limits.max_kind_bytes) {
                    throw MusicalError("PFM source kind is too long");
                }
                kinds.push_back(value);
            }
            if (kinds.empty()) kinds.push_back("normal");
        }

        std::vector<TickTempo> tempos;
        if (const auto list = root.find("tempos");
            list != root.end() && list->is_array()) {
            for (const auto& item : *list) {
                if (!item.is_object()) continue;
                const auto tick = item.value("tick", std::uint64_t{0U});
                const auto bpm = item.value("bpm", 120.0);
                if (!std::isfinite(bpm) || bpm <= 0.0) {
                    throw MusicalError("PFM source BPM is invalid");
                }
                const auto mpq = static_cast<std::uint32_t>(
                    std::clamp<long double>(
                        std::floor(60'000'000.0L / bpm + 0.5L),
                        1.0L,
                        16'777'215.0L
                    )
                );
                tempos.push_back({
                    tick,
                    mpq,
                    static_cast<std::uint16_t>(
                        item.value("numerator", std::uint64_t{4U})
                    ),
                    static_cast<std::uint16_t>(
                        item.value("denominator", std::uint64_t{4U})
                    ),
                });
            }
        }
        const TickTimeMap map(ppqn, std::move(tempos));

        const Json* metadata_json_pointer = nullptr;
        if (const auto metadata = root.find("metadata");
            metadata != root.end() && metadata->is_object()) {
            metadata_json_pointer = &*metadata;
        }

        std::vector<PatternRun> patterns;
        std::uint64_t logical_count{};

        // Arbitrary repeating motifs compile to one arithmetic PatternRun per
        // motif event. A 4-event motif repeated 250 billion times therefore
        // remains four constant-storage PFC1 PatternRuns rather than one
        // trillion explicit notes.
        std::unordered_map<std::string, PfmSourcePatternDefinition>
            source_patterns;
        if (const auto definitions = root.find("patterns");
            definitions != root.end() && definitions->is_array()) {
            if (definitions->size() > options.limits.max_pfm_patterns) {
                throw MusicalError("PFM source contains too many pattern definitions");
            }
            for (const auto& definition_json : *definitions) {
                if (!definition_json.is_object()) {
                    throw MusicalError("PFM pattern definition must be an object");
                }
                const auto id = definition_json.value("id", std::string{});
                const auto length = definition_json.value(
                    "lengthTicks",
                    std::uint64_t{0U}
                );
                if (id.empty() || id.size() > 256U || length == 0U) {
                    throw MusicalError("PFM pattern id/lengthTicks is invalid");
                }
                const auto events = definition_json.find("events");
                if (events == definition_json.end() || !events->is_array()
                    || events->empty() || events->size() > 65'536U) {
                    throw MusicalError("PFM pattern definition requires bounded events");
                }
                PfmSourcePatternDefinition definition;
                definition.length_tick = length;
                definition.events.reserve(events->size());
                for (const auto& event_json : *events) {
                    if (!event_json.is_object()) {
                        throw MusicalError("PFM pattern event must be an object");
                    }
                    PfmSourcePatternEvent event;
                    event.offset_tick = event_json.value(
                        "offsetTicks",
                        std::uint64_t{0U}
                    );
                    event.duration_tick = event_json.value(
                        "durationTicks",
                        std::uint64_t{0U}
                    );
                    const auto lane = event_json.value(
                        "lane",
                        std::uint64_t{0U}
                    );
                    const auto flags = event_json.value(
                        "flags",
                        std::uint64_t{0U}
                    );
                    if (event.offset_tick >= definition.length_tick
                        || lane >= key_count
                        || flags > std::numeric_limits<std::uint16_t>::max()) {
                        throw MusicalError("PFM pattern event is outside its bounds");
                    }
                    event.lane = static_cast<std::uint16_t>(lane);
                    event.flags = static_cast<std::uint16_t>(flags);
                    if (const auto kind = event_json.find("kind");
                        kind != event_json.end() && kind->is_string()) {
                        event.kind_id = kind_id_for(
                            kinds,
                            kind->get<std::string>(),
                            options.limits.max_kind_bytes
                        );
                    } else {
                        const auto kind_id = event_json.value(
                            "kindId",
                            std::uint64_t{0U}
                        );
                        if (kind_id >= kinds.size()) {
                            throw MusicalError("PFM pattern event kindId is invalid");
                        }
                        event.kind_id = static_cast<std::uint32_t>(kind_id);
                    }
                    definition.events.push_back(event);
                }
                if (!source_patterns.emplace(id, std::move(definition)).second) {
                    throw MusicalError("PFM pattern definition id is duplicated");
                }
            }
        }

        if (const auto repeats = root.find("repeats");
            repeats != root.end() && repeats->is_array()) {
            for (const auto& repeat : *repeats) {
                if (!repeat.is_object()) {
                    throw MusicalError("PFM repeat must be an object");
                }
                const auto id = repeat.value("pattern", std::string{});
                const auto found = source_patterns.find(id);
                if (found == source_patterns.end()) {
                    throw MusicalError("PFM repeat references an unknown pattern");
                }
                const auto count = repeat.value("count", std::uint64_t{0U});
                const auto start_tick = repeat.value(
                    "startTick",
                    std::uint64_t{0U}
                );
                if (count == 0U) {
                    throw MusicalError("PFM repeat count cannot be zero");
                }
                const auto event_count = static_cast<std::uint64_t>(
                    found->second.events.size()
                );
                if (event_count != 0U
                    && count > (options.limits.max_logical_notes - logical_count)
                        / event_count) {
                    throw MusicalError("PFM repeat logical note count exceeds limit");
                }
                logical_count += count * event_count;
                const auto owner_name = lower_ascii(
                    repeat.value("owner", std::string{"player"})
                );
                const auto owner = owner_name == "opponent"
                    ? PackedNoteOwner::opponent
                    : PackedNoteOwner::player;

                for (const auto& event : found->second.events) {
                    if (event.offset_tick
                        > std::numeric_limits<std::uint64_t>::max() - start_tick) {
                        throw MusicalError("PFM repeat start tick overflows");
                    }
                    PfmPatternRecord record;
                    record.domain = PfmPatternRecord::Domain::ticks;
                    record.start_tick = start_tick + event.offset_tick;
                    record.interval = found->second.length_tick;
                    record.count = count;
                    record.duration = event.duration_tick;
                    record.lanes = {event.lane};
                    record.owner = owner;
                    record.flags = event.flags;
                    record.kind_id = event.kind_id;
                    auto compiled = compile_tick_pattern(record, map);
                    patterns.insert(
                        patterns.end(),
                        std::make_move_iterator(compiled.begin()),
                        std::make_move_iterator(compiled.end())
                    );
                }
            }
        }

        if (const auto runs = root.find("runs");
            runs != root.end() && runs->is_array()) {
            if (runs->size() > options.limits.max_pfm_patterns) {
                throw MusicalError("PFM source contains too many runs");
            }
            for (const auto& item : *runs) {
                if (!item.is_object()) {
                    throw MusicalError("PFM source RUN must be an object");
                }
                PfmPatternRecord record;
                record.domain = PfmPatternRecord::Domain::ticks;
                record.start_tick = item.value("startTick", std::uint64_t{0U});
                record.interval = item.value(
                    "intervalTicks",
                    std::uint64_t{0U}
                );
                record.duration = item.value(
                    "durationTicks",
                    std::uint64_t{0U}
                );
                record.count = item.value("count", std::uint64_t{0U});
                if (record.count == 0U
                    || record.count > options.limits.max_logical_notes
                        - logical_count) {
                    throw MusicalError("PFM source RUN count is invalid");
                }
                logical_count += record.count;

                const auto owner = lower_ascii(
                    item.value("owner", std::string{"player"})
                );
                record.owner = owner == "opponent"
                    ? PackedNoteOwner::opponent
                    : PackedNoteOwner::player;
                const auto flags = item.value("flags", std::uint64_t{0U});
                if (flags > std::numeric_limits<std::uint16_t>::max()) {
                    throw MusicalError("PFM source RUN flags overflow");
                }
                record.flags = static_cast<std::uint16_t>(flags);

                if (const auto kind = item.find("kind");
                    kind != item.end() && kind->is_string()) {
                    record.kind_id = kind_id_for(
                        kinds,
                        kind->get<std::string>(),
                        options.limits.max_kind_bytes
                    );
                } else {
                    const auto kind_id = item.value(
                        "kindId",
                        std::uint64_t{0U}
                    );
                    if (kind_id >= kinds.size()) {
                        throw MusicalError("PFM source RUN kindId is invalid");
                    }
                    record.kind_id = static_cast<std::uint32_t>(kind_id);
                }

                const auto lanes = item.find("lanes");
                if (lanes == item.end() || !lanes->is_array()
                    || lanes->empty()) {
                    throw MusicalError("PFM source RUN requires lanes");
                }
                if (lanes->size() > options.limits.max_pattern_lanes) {
                    throw MusicalError("PFM source RUN lane cycle is too long");
                }
                for (const auto& lane : *lanes) {
                    if (!lane.is_number_unsigned()
                        || lane.get<std::uint64_t>() >= key_count) {
                        throw MusicalError("PFM source RUN lane is invalid");
                    }
                    record.lanes.push_back(
                        static_cast<std::uint16_t>(
                            lane.get<std::uint64_t>()
                        )
                    );
                }
                auto compiled = compile_tick_pattern(record, map);
                patterns.insert(
                    patterns.end(),
                    std::make_move_iterator(compiled.begin()),
                    std::make_move_iterator(compiled.end())
                );
            }
        }

        std::vector<PackedNote> explicit_notes;
        if (const auto notes = root.find("notes");
            notes != root.end() && notes->is_array()) {
            if (notes->size() > options.limits.max_sort_notes_in_memory) {
                throw MusicalError(
                    "PFM source explicit-note array exceeds bounded source "
                    "budget; use RUN records for dense charts"
                );
            }
            explicit_notes.reserve(notes->size());
            for (const auto& item : *notes) {
                if (!item.is_object()) continue;
                const auto tick = item.value("tick", std::uint64_t{0U});
                const auto duration = item.value(
                    "durationTicks",
                    std::uint64_t{0U}
                );
                const auto lane = item.value("lane", std::uint64_t{0U});
                if (lane >= key_count) {
                    throw MusicalError("PFM source note lane is invalid");
                }
                if (duration > std::numeric_limits<std::uint64_t>::max() - tick) {
                    throw MusicalError("PFM source note duration tick overflows");
                }
                const auto start_us = map.tick_to_us(tick);
                const auto end_us = map.tick_to_us(tick + duration);
                if (start_us > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
                    throw MusicalError("PFM source note time exceeds PFC1 int64 range");
                }
                const auto owner = lower_ascii(
                    item.value("owner", std::string{"player"})
                );
                std::uint32_t kind_id{};
                if (const auto kind = item.find("kind");
                    kind != item.end() && kind->is_string()) {
                    kind_id = kind_id_for(
                        kinds,
                        kind->get<std::string>(),
                        options.limits.max_kind_bytes
                    );
                } else {
                    const auto value = item.value(
                        "kindId",
                        std::uint64_t{0U}
                    );
                    if (value >= kinds.size()) {
                        throw MusicalError(
                            "PFM source explicit-note kindId is invalid"
                        );
                    }
                    kind_id = static_cast<std::uint32_t>(value);
                }
                const auto flags = item.value("flags", std::uint64_t{0U});
                if (flags > std::numeric_limits<std::uint16_t>::max()) {
                    throw MusicalError("PFM source note flags overflow");
                }
                explicit_notes.push_back({
                    static_cast<std::int64_t>(start_us),
                    end_us - start_us,
                    static_cast<std::uint16_t>(lane),
                    owner == "opponent"
                        ? PackedNoteOwner::opponent
                        : PackedNoteOwner::player,
                    static_cast<std::uint16_t>(flags),
                    kind_id,
                });
            }
        }
        std::sort(
            explicit_notes.begin(),
            explicit_notes.end(),
            packed_note_less
        );
        if (explicit_notes.size()
            > options.limits.max_pfm_explicit_notes) {
            throw MusicalError("PFM source explicit notes exceed limit");
        }
        if (explicit_notes.size()
            > options.limits.max_logical_notes - logical_count) {
            throw MusicalError("PFM source logical note count exceeds limit");
        }
        logical_count += explicit_notes.size();

        PackedChartStreamSpec spec;
        spec.key_count = key_count;
        spec.explicit_note_count = explicit_notes.size();
        spec.kinds = kinds;
        spec.patterns = std::move(patterns);

        std::string writer_error;
        auto writer = PackedChartStreamWriter::create(
            destination_pfc,
            spec,
            options.packed,
            &writer_error
        );
        if (!writer.has_value()) {
            throw MusicalError("cannot create PFM-source PFC1: " + writer_error);
        }
        if (!explicit_notes.empty()
            && !writer->append(explicit_notes, &writer_error)) {
            throw MusicalError("cannot append PFM-source notes: " + writer_error);
        }
        if (!writer->finish(&writer_error)) {
            throw MusicalError("cannot finish PFM-source PFC1: " + writer_error);
        }

        Chart metadata = chart_from_metadata_and_tempos(
            source_json,
            metadata_json_pointer,
            map,
            ChartFormat::pfm,
            key_count,
            options.limits
        );
        metadata.source_format = ChartFormat::pfm;
        std::uint64_t content_end_us{};
        if (!explicit_notes.empty()) {
            const auto& last = explicit_notes.back();
            content_end_us = static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, last.time_us)
            ) + last.duration_us;
        }
        for (const auto& pattern : spec.patterns) {
            if (const auto last = pattern.note_at(pattern.count - 1U);
                last.has_value()) {
                content_end_us = std::max(
                    content_end_us,
                    static_cast<std::uint64_t>(
                        std::max<std::int64_t>(0, last->time_us)
                    ) + last->duration_us
                );
            }
        }

        result.success = true;
        result.explicit_note_count = explicit_notes.size();
        result.logical_note_count = logical_count;
        result.content_end_us = content_end_us;
        result.key_count = key_count;
        result.chart_metadata = std::move(metadata);
        return result;
    } catch (const std::bad_alloc&) {
        result.error = "PFM source import exhausted memory while holding "
            "metadata/pattern state";
        return result;
    } catch (const std::exception& exception) {
        result.error = exception.what();
        return result;
    }
}

bool export_chart_to_midi(
    const Chart& chart,
    const std::filesystem::path& destination_midi,
    const MidiChartOptions& options,
    std::string* error
) {
    try {
        if (options.ppqn == 0U || options.ppqn > 0x7FFFU) {
            throw MusicalError("MIDI PPQN must be in the range 1..32767");
        }
        if (chart.notes.size() > options.limits.max_midi_notes) {
            throw MusicalError(
                "chart has too many explicit notes for Standard MIDI; "
                "export PFM instead"
            );
        }

        const auto tempos = tempos_from_chart(chart, options.ppqn);
        const TickTimeMap time_map(options.ppqn, tempos);

        std::vector<std::string> kinds{"normal"};
        for (const auto& note : chart.notes) {
            static_cast<void>(kind_id_for(
                kinds,
                note.kind,
                options.limits.max_kind_bytes
            ));
        }

        std::vector<MidiWriteEvent> tempo_events;
        append_meta_event(
            tempo_events,
            0U,
            0x03U,
            std::vector<std::uint8_t>{'P','u','l','s','e','F','o','r','g','e'}
        );

        if (options.embed_metadata) {
            auto meta = metadata_json(chart);
            meta["kinds"] = kinds;
            meta["laneBaseNote"] = options.lane_base_note;
            meta["opponentChannel"] = options.opponent_channel;
            meta["playerChannel"] = options.player_channel;
            const auto text = meta.dump();
            if (text.size() + 7U > options.limits.max_metadata_bytes) {
                throw MusicalError("embedded MIDI PulseForge metadata is too large");
            }
            std::vector<std::uint8_t> payload{
                'P','F','M','E','T','A','1'
            };
            payload.insert(payload.end(), text.begin(), text.end());
            append_meta_event(tempo_events, 0U, 0x7FU, std::move(payload));
        }

        for (const auto& tempo : tempos) {
            std::vector<std::uint8_t> payload{
                static_cast<std::uint8_t>(
                    (tempo.microseconds_per_quarter >> 16U) & 0xFFU
                ),
                static_cast<std::uint8_t>(
                    (tempo.microseconds_per_quarter >> 8U) & 0xFFU
                ),
                static_cast<std::uint8_t>(
                    tempo.microseconds_per_quarter & 0xFFU
                ),
            };
            append_meta_event(tempo_events, tempo.tick, 0x51U, std::move(payload));
            append_meta_event(
                tempo_events,
                tempo.tick,
                0x58U,
                {
                    static_cast<std::uint8_t>(
                        std::min<std::uint16_t>(tempo.numerator, 255U)
                    ),
                    time_signature_denominator_power(tempo.denominator),
                    24U,
                    8U,
                },
                1U
            );
        }

        std::array<std::vector<MidiWriteEvent>, 2> note_tracks;
        append_meta_event(
            note_tracks[0],
            0U,
            0x03U,
            std::vector<std::uint8_t>{
                'O','p','p','o','n','e','n','t'
            }
        );
        append_meta_event(
            note_tracks[1],
            0U,
            0x03U,
            std::vector<std::uint8_t>{
                'P','l','a','y','e','r'
            }
        );

        for (const auto& note : chart.notes) {
            if (note.lane >= chart.key_count
                || note.lane >= maximum_supported_key_count) {
                continue;
            }
            // PULSEFORGE_P1_4_0_MUSICAL_SECONDARY_OPPONENT_EXPORT_V1
            // Musical exports have two physical sides. Keep player4 on the AI
            // side; its Third Strum kind is the canonical identity carrier.
            const auto channel = note.owner != NoteOwner::player
                ? options.opponent_channel
                : options.player_channel;
            const auto pitch_value = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(options.lane_base_note)
                + note.lane
            );
            if (pitch_value > 127U) {
                throw MusicalError("MIDI lane mapping exceeds note number 127");
            }
            const auto start_us = static_cast<std::uint64_t>(
                std::max(0.0, std::floor(note.time_ms * 1'000.0 + 0.5))
            );
            const auto end_us = static_cast<std::uint64_t>(
                std::max(
                    static_cast<double>(start_us),
                    std::floor(
                        (note.time_ms + note.duration_ms) * 1'000.0 + 0.5
                    )
                )
            );
            const auto start_tick = time_map.us_to_tick(start_us);
            const auto end_tick = time_map.us_to_tick(end_us);
            const auto kind = kind_id_for(
                kinds,
                note.kind,
                options.limits.max_kind_bytes
            );

            const auto track_index = note.owner != NoteOwner::player ? 0U : 1U;
            std::vector<std::uint8_t> custom{
                'P','F','N','1',
                channel,
                static_cast<std::uint8_t>(pitch_value),
                static_cast<std::uint8_t>((kind >> 24U) & 0xFFU),
                static_cast<std::uint8_t>((kind >> 16U) & 0xFFU),
                static_cast<std::uint8_t>((kind >> 8U) & 0xFFU),
                static_cast<std::uint8_t>(kind & 0xFFU),
                0U,
                0U,
            };
            append_meta_event(
                note_tracks[track_index],
                start_tick,
                0x7FU,
                std::move(custom),
                1U
            );

            note_tracks[track_index].push_back({
                start_tick,
                2U,
                {
                    static_cast<std::uint8_t>(0x90U | channel),
                    static_cast<std::uint8_t>(pitch_value),
                    100U,
                },
            });
            const auto final_tick = std::max(start_tick, end_tick);
            note_tracks[track_index].push_back({
                final_tick,
                static_cast<std::uint8_t>(
                    final_tick == start_tick ? 3U : 0U
                ),
                {
                    static_cast<std::uint8_t>(0x80U | channel),
                    static_cast<std::uint8_t>(pitch_value),
                    0U,
                },
            });
        }

        std::ofstream output(
            destination_midi,
            std::ios::binary | std::ios::trunc
        );
        if (!output) throw MusicalError("cannot create MIDI destination");
        output.write("MThd", 4);
        write_be32(output, 6U);
        write_be16(output, 1U);
        write_be16(output, 3U);
        write_be16(output, static_cast<std::uint16_t>(options.ppqn));
        write_midi_track(output, make_track(std::move(tempo_events)));
        write_midi_track(output, make_track(std::move(note_tracks[0])));
        write_midi_track(output, make_track(std::move(note_tracks[1])));
        output.flush();
        if (!output) throw MusicalError("cannot flush MIDI destination");

        if (options.write_sidecar_metadata) {
            write_sidecar_metadata(destination_midi, chart);
        }
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        return false;
    }
}

bool export_packed_chart_to_pfm(
    const PackedChartReader& reader,
    const Chart& metadata,
    const std::filesystem::path& destination_pfm,
    const PfmChartOptions& options,
    std::string* error
) {
    try {
        if (options.ppqn == 0U) {
            throw MusicalError("PFM PPQN cannot be zero");
        }
        const auto tempos = tempos_from_chart(metadata, options.ppqn);
        const TickTimeMap map(options.ppqn, tempos);

        std::vector<PfmPatternRecord> patterns;
        patterns.reserve(reader.patterns().size());
        for (const auto& pattern : reader.patterns()) {
            PfmPatternRecord record;
            record.count = pattern.count;
            record.duration = pattern.duration_us;
            record.lanes = pattern.lane_pattern;
            record.owner = pattern.owner;
            record.flags = pattern.flags;
            record.kind_id = pattern.kind_id;

            // Use tick domain when start and first interval round-trip closely.
            const auto start_nonnegative = static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, pattern.start_us)
            );
            const auto start_tick = map.us_to_tick(start_nonnegative);
            const auto start_roundtrip = map.tick_to_us(start_tick);
            bool tick_domain = pattern.start_us >= 0
                && (start_roundtrip > start_nonnegative
                    ? start_roundtrip - start_nonnegative
                    : start_nonnegative - start_roundtrip) <= 1U;

            std::uint64_t interval_ticks{};
            if (tick_domain && pattern.count > 1U) {
                const auto second = pattern.note_at(1U);
                if (!second.has_value() || second->time_us < 0) {
                    tick_domain = false;
                } else {
                    const auto second_tick = map.us_to_tick(
                        static_cast<std::uint64_t>(second->time_us)
                    );
                    if (second_tick < start_tick) {
                        tick_domain = false;
                    } else {
                        interval_ticks = second_tick - start_tick;
                        const auto second_roundtrip =
                            map.tick_to_us(second_tick);
                        const auto second_us = static_cast<std::uint64_t>(
                            second->time_us
                        );
                        if ((second_roundtrip > second_us
                                ? second_roundtrip - second_us
                                : second_us - second_roundtrip) > 1U) {
                            tick_domain = false;
                        }
                    }
                }
            }

            if (tick_domain) {
                record.domain = PfmPatternRecord::Domain::ticks;
                record.start_tick = start_tick;
                record.interval = interval_ticks;
                const auto end_tick = map.us_to_tick(
                    start_nonnegative + pattern.duration_us
                );
                record.duration = end_tick >= start_tick
                    ? end_tick - start_tick
                    : 0U;
            } else {
                record.domain = PfmPatternRecord::Domain::microseconds;
                record.start_us = pattern.start_us;
                record.interval = pattern.interval_us;
                record.interval_denominator =
                    pattern.interval_denominator;
                record.duration = pattern.duration_us;
            }
            patterns.push_back(std::move(record));
        }

        // A Standard MIDI File is event-linear, but conversion to PFM can
        // recover long arithmetic sequences. The detector is a bounded first
        // pass over PFC chunks and records only the compressed ranges.
        const auto detected_explicit_runs = detect_explicit_arithmetic_runs(
            reader,
            map
        );
        std::uint64_t compressed_explicit_notes{};
        for (const auto& detected : detected_explicit_runs) {
            if (detected.count > reader.explicit_note_count()
                    - compressed_explicit_notes) {
                throw MusicalError("detected PFM run count overflows explicit notes");
            }
            compressed_explicit_notes += detected.count;
            patterns.push_back(detected.pattern);
        }
        const auto pfm_explicit_count =
            reader.explicit_note_count() - compressed_explicit_notes;
        if (patterns.size() > options.limits.max_pfm_patterns) {
            throw MusicalError("PFM arithmetic-run detection exceeds pattern limit");
        }

        std::ofstream output(
            destination_pfm,
            std::ios::binary | std::ios::trunc
        );
        if (!output) throw MusicalError("cannot create PFM destination");
        output.write(pfm_magic.data(), 4);
        write_le(output, pfm_version);
        write_le(
            output,
            static_cast<std::uint16_t>(
                options.embed_metadata ? pfm_flag_embedded_metadata : 0U
            )
        );
        write_le(output, options.ppqn);
        write_le(output, reader.key_count());
        write_le(output, std::uint16_t{0U});
        write_le(
            output,
            static_cast<std::uint32_t>(reader.kinds().size())
        );
        write_le(
            output,
            static_cast<std::uint32_t>(tempos.size())
        );
        write_le(output, pfm_explicit_count);
        write_le(
            output,
            static_cast<std::uint64_t>(patterns.size())
        );

        const auto metadata_text = options.embed_metadata
            ? metadata_json(metadata).dump()
            : std::string{};
        if (metadata_text.size() > options.limits.max_metadata_bytes
            || metadata_text.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw MusicalError("PFM embedded metadata exceeds configured limit");
        }
        write_le(
            output,
            static_cast<std::uint32_t>(metadata_text.size())
        );
        output.write(
            metadata_text.data(),
            static_cast<std::streamsize>(metadata_text.size())
        );

        for (const auto& kind : reader.kinds()) {
            write_string(output, kind);
        }

        std::uint64_t previous_tempo_tick{};
        for (const auto& tempo : tempos) {
            write_uvar(output, tempo.tick - previous_tempo_tick);
            previous_tempo_tick = tempo.tick;
            write_uvar(output, tempo.microseconds_per_quarter);
            write_uvar(output, tempo.numerator);
            write_uvar(output, tempo.denominator);
        }

        for (const auto& pattern : patterns) {
            write_pfm_pattern(output, pattern);
        }

        std::uint64_t previous_note_tick{};
        std::uint64_t global_note_index{};
        std::size_t detected_index{};
        for (std::uint64_t chunk = 0U; chunk < reader.chunk_count(); ++chunk) {
            const auto decoded = reader.read_chunk(chunk);
            if (!decoded) {
                throw MusicalError(
                    "cannot decode PFC1 while exporting PFM: "
                    + decoded.error
                );
            }
            for (const auto& note : decoded.notes) {
                while (detected_index < detected_explicit_runs.size()
                    && global_note_index
                        >= detected_explicit_runs[detected_index].first_index
                            + detected_explicit_runs[detected_index].count) {
                    ++detected_index;
                }
                const bool compressed = detected_index
                        < detected_explicit_runs.size()
                    && global_note_index
                        >= detected_explicit_runs[detected_index].first_index
                    && global_note_index
                        < detected_explicit_runs[detected_index].first_index
                            + detected_explicit_runs[detected_index].count;
                ++global_note_index;
                if (compressed) {
                    continue;
                }
                const auto start_us = static_cast<std::uint64_t>(
                    std::max<std::int64_t>(0, note.time_us)
                );
                if (note.duration_us
                    > std::numeric_limits<std::uint64_t>::max() - start_us) {
                    throw MusicalError("PFC1 note end time overflows during PFM export");
                }
                const auto end_us = start_us + note.duration_us;
                const auto start_tick = map.us_to_tick(start_us);
                const auto end_tick = map.us_to_tick(end_us);
                if (start_tick < previous_note_tick) {
                    throw MusicalError(
                        "PFC1-to-PFM quantization changed note ordering"
                    );
                }
                write_uvar(output, start_tick - previous_note_tick);
                previous_note_tick = start_tick;
                write_uvar(
                    output,
                    end_tick >= start_tick ? end_tick - start_tick : 0U
                );
                write_uvar(output, note.lane);
                output.put(static_cast<char>(note.owner));
                write_uvar(output, note.flags);
                write_uvar(output, note.kind_id);
            }
        }
        output.flush();
        if (!output) throw MusicalError("cannot flush PFM destination");

        if (options.write_sidecar_metadata) {
            write_sidecar_metadata(destination_pfm, metadata);
        }
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        return false;
    }
}

bool write_pfm_source_template(
    const std::filesystem::path& destination,
    const std::uint32_t ppqn,
    std::string* error
) {
    try {
        if (ppqn == 0U) throw MusicalError("PPQN cannot be zero");
        Json root{
            {"format", "pulseforge-pfm-source-v1"},
            {"ppqn", ppqn},
            {"keyCount", 4},
            {"metadata", {
                {"title", "Trillion Note PFM Example"},
                {"artist", "Unknown"},
                {"charter", "Unknown"},
                {"difficulty", "insane"},
                {"stage", "stage"},
                {"player", "bf"},
                {"opponent", "dad"},
                {"girlfriend", "gf"},
                {"noteStyle", "NOTE_assets"},
                {"scrollSpeed", 1.0},
                {"audio", {
                    {"instrumental", "Inst.ogg"},
                    {"vocals", Json::array()},
                }},
            }},
            {"kinds", Json::array({"normal"})},
            {"tempos", Json::array({
                {
                    {"tick", 0},
                    {"bpm", 244.0},
                    {"numerator", 4},
                    {"denominator", 4},
                }
            })},
            {"runs", Json::array({
                {
                    {"startTick", 0},
                    {"intervalTicks", 1},
                    {"count", 1'000'000'000'000ULL},
                    {"durationTicks", 0},
                    {"lanes", Json::array({0, 1, 2, 3})},
                    {"owner", "player"},
                    {"kind", "normal"},
                    {"flags", 0},
                }
            })},
            {"notes", Json::array()},
        };
        std::ofstream output(
            destination,
            std::ios::binary | std::ios::trunc
        );
        if (!output) throw MusicalError("cannot create PFM source template");
        output << root.dump(2);
        output.flush();
        if (!output) throw MusicalError("cannot flush PFM source template");
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        return false;
    }
}

}  // namespace pulseforge
