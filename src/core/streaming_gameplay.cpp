#include "pulseforge/streaming_gameplay.hpp"
#include "pulseforge/note_types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

namespace pulseforge {
namespace {

constexpr std::int64_t minimum_song_time_us = -60'000'000;
constexpr std::int64_t input_tail_us = 10'000'000;
constexpr StreamingNoteId no_streaming_note{
    StreamingNoteOrigin::explicit_note,
    std::numeric_limits<std::uint64_t>::max(),
    0U,
};

class SessionError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

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

[[nodiscard]] double microseconds_to_milliseconds(
    const std::int64_t microseconds
) noexcept {
    return static_cast<double>(microseconds) / 1'000.0;
}

[[nodiscard]] std::int64_t saturating_add_time(
    const std::int64_t time,
    const std::int64_t delta
) noexcept {
    if (delta > 0 && time > std::numeric_limits<std::int64_t>::max() - delta) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (delta < 0 && time < std::numeric_limits<std::int64_t>::min() - delta) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return time + delta;
}

[[nodiscard]] std::int64_t saturating_subtract_time(
    const std::int64_t left,
    const std::int64_t right
) noexcept {
    if (right > 0
        && left < std::numeric_limits<std::int64_t>::min() + right) {
        return std::numeric_limits<std::int64_t>::min();
    }
    if (right < 0
        && left > std::numeric_limits<std::int64_t>::max() + right) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return left - right;
}

[[nodiscard]] std::int64_t note_end_us(const PackedNote& note) noexcept {
    if (note.time_us >= 0) {
        const auto room = static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max() - note.time_us
        );
        if (note.duration_us > room) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return note.time_us + static_cast<std::int64_t>(note.duration_us);
    }
    const auto magnitude = static_cast<std::uint64_t>(-(note.time_us + 1)) + 1U;
    if (note.duration_us < magnitude) {
        const auto remaining = magnitude - note.duration_us;
        const auto minimum_magnitude = std::uint64_t{1U} << 63U;
        return remaining == minimum_magnitude
            ? std::numeric_limits<std::int64_t>::min()
            : -static_cast<std::int64_t>(remaining);
    }
    const auto positive = note.duration_us - magnitude;
    return positive > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        ? std::numeric_limits<std::int64_t>::max()
        : static_cast<std::int64_t>(positive);
}

void saturating_add(std::uint64_t& target, const std::uint64_t amount) noexcept {
    target = amount > std::numeric_limits<std::uint64_t>::max() - target
        ? std::numeric_limits<std::uint64_t>::max()
        : target + amount;
}

[[nodiscard]] std::uint64_t saturating_multiply(
    const std::uint64_t left,
    const std::uint64_t right
) noexcept {
    if (left != 0U
        && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left * right;
}

[[nodiscard]] std::uint64_t inclusive_signed_count(
    const std::int64_t first,
    const std::int64_t last
) noexcept {
    if (last < first) {
        return 0U;
    }
    const auto distance = static_cast<std::uint64_t>(last)
        - static_cast<std::uint64_t>(first);
    return distance == std::numeric_limits<std::uint64_t>::max()
        ? distance
        : distance + 1U;
}

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    auto value = state;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

void deterministic_shuffle(
    std::vector<std::uint16_t>& lanes,
    std::uint64_t seed
) noexcept {
    for (std::size_t remaining = lanes.size(); remaining > 1U; --remaining) {
        const auto range = static_cast<std::uint64_t>(remaining);
        const auto rejection_limit = std::numeric_limits<std::uint64_t>::max()
            - (std::numeric_limits<std::uint64_t>::max() % range);
        std::uint64_t value{};
        do {
            value = splitmix64(seed);
        } while (value >= rejection_limit);
        std::swap(
            lanes[remaining - 1U],
            lanes[static_cast<std::size_t>(value % range)]
        );
    }
}

[[nodiscard]] bool terminal(const NoteState state) noexcept {
    return state == NoteState::completed
        || state == NoteState::missed
        || state == NoteState::ignored;
}

struct SourceNote final {
    StreamingNoteId id;
    PackedNote note;
};

// A contiguous sliding buffer specialized for the gameplay judgment window.
//
// std::vector::erase(begin, begin + N) moves every surviving element. On a
// saturated 262k-note window that became an O(N) memmove in the per-frame hot
// path. This wrapper retires a prefix by advancing head_ in O(1), while keeping
// the active region contiguous so the public span API and all existing indexed
// gameplay code remain unchanged.
//
// A deliberately large spare tail amortizes physical reclamation: reclaiming
// at 3x logical capacity means roughly two complete windows can be consumed
// before one O(active-size) move is necessary.
template <typename T>
class SlidingContiguousBuffer final {
public:
    using container_type = std::vector<T>;
    using value_type = T;
    using iterator = typename container_type::iterator;
    using const_iterator = typename container_type::const_iterator;

    void reserve(const std::size_t logical_capacity) {
        logical_capacity_ = logical_capacity;
        const auto maximum = std::numeric_limits<std::size_t>::max();
        const auto spare = logical_capacity > maximum / 3U
            ? maximum
            : logical_capacity * 3U;
        storage_.reserve(spare);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return storage_.size() - head_;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0U; }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return storage_.capacity();
    }

    [[nodiscard]] std::size_t retired_prefix() const noexcept { return head_; }

    [[nodiscard]] T* data() noexcept {
        return size() == 0U ? nullptr : storage_.data() + head_;
    }
    [[nodiscard]] const T* data() const noexcept {
        return size() == 0U ? nullptr : storage_.data() + head_;
    }

    [[nodiscard]] iterator begin() noexcept {
        return storage_.begin() + static_cast<std::ptrdiff_t>(head_);
    }
    [[nodiscard]] const_iterator begin() const noexcept {
        return storage_.begin() + static_cast<std::ptrdiff_t>(head_);
    }
    [[nodiscard]] iterator end() noexcept { return storage_.end(); }
    [[nodiscard]] const_iterator end() const noexcept { return storage_.end(); }

    [[nodiscard]] T& operator[](const std::size_t index) noexcept {
        return storage_[head_ + index];
    }
    [[nodiscard]] const T& operator[](const std::size_t index) const noexcept {
        return storage_[head_ + index];
    }

    void clear() noexcept {
        storage_.clear();
        head_ = 0U;
    }

    void push_back(T value) {
        ensure_tail_room();
        storage_.push_back(std::move(value));
    }

    void discard_prefix(const std::size_t count) noexcept {
        const auto removed = std::min(count, size());
        head_ += removed;
        if (head_ == storage_.size()) {
            storage_.clear();
            head_ = 0U;
        }
    }

    template <typename Predicate>
    [[nodiscard]] std::size_t compact_remove_if(Predicate predicate) {
        auto first = begin();
        auto last = end();
        const auto new_end = std::remove_if(first, last, predicate);
        const auto removed = static_cast<std::size_t>(
            std::distance(new_end, last)
        );
        storage_.erase(new_end, storage_.end());
        return removed;
    }

    // Called only when the amortized spare tail is exhausted. This is the only
    // path that physically shifts the whole active window toward index zero.
    [[nodiscard]] std::size_t reclaim_prefix() {
        if (head_ == 0U) return 0U;
        const auto active = size();
        std::move(begin(), end(), storage_.begin());
        storage_.resize(active);
        const auto retired = head_;
        head_ = 0U;
        return retired;
    }

private:
    void ensure_tail_room() {
        if (storage_.size() < storage_.capacity()) return;
        if (head_ != 0U) {
            static_cast<void>(reclaim_prefix());
            if (storage_.size() < storage_.capacity()) return;
        }
        // A zero logical capacity is rejected by validate_options(); this is a
        // defensive fallback for unusual allocator/capacity behavior.
        const auto growth = std::max<std::size_t>(logical_capacity_, 1U);
        if (storage_.capacity()
            <= std::numeric_limits<std::size_t>::max() - growth) {
            storage_.reserve(storage_.capacity() + growth);
        }
    }

    container_type storage_;
    std::size_t head_{};
    std::size_t logical_capacity_{};
};

class ExplicitCursor final {
public:
    ExplicitCursor(
        const PackedChartReader& reader,
        const std::uint32_t maximum_chunk_notes,
        const std::size_t maximum_decoded_chunk_bytes
    ) : reader_(&reader),
        maximum_chunk_notes_(maximum_chunk_notes),
        maximum_decoded_chunk_bytes_(maximum_decoded_chunk_bytes) {}

    void reset() noexcept {
        chunk_index_ = 0U;
        global_index_ = 0U;
        offset_ = 0U;
        cache_.clear();
    }

    [[nodiscard]] std::optional<SourceNote> peek() {
        ensure_cache();
        if (global_index_ >= reader_->explicit_note_count()) {
            return std::nullopt;
        }
        return SourceNote{
            {
                StreamingNoteOrigin::explicit_note,
                global_index_,
                0U,
            },
            cache_[offset_],
        };
    }

    [[nodiscard]] std::optional<SourceNote> take() {
        auto result = peek();
        if (!result.has_value()) {
            return std::nullopt;
        }
        ++global_index_;
        ++offset_;
        return result;
    }

    [[nodiscard]] bool exhausted() const noexcept {
        return global_index_ >= reader_->explicit_note_count();
    }

    [[nodiscard]] std::size_t cached_notes() const noexcept {
        return cache_.capacity();
    }

    [[nodiscard]] std::uint64_t global_index() const noexcept {
        return global_index_;
    }

    [[nodiscard]] std::span<const PackedNote> remaining_chunk() {
        ensure_cache();
        if (global_index_ >= reader_->explicit_note_count()
            || offset_ >= cache_.size()) {
            return {};
        }
        return std::span<const PackedNote>(cache_).subspan(offset_);
    }

    void consume(const std::size_t count) {
        if (count > cache_.size() - offset_) {
            throw SessionError("explicit PFC1 bulk consume exceeds decoded chunk");
        }
        global_index_ += static_cast<std::uint64_t>(count);
        offset_ += count;
    }

    [[nodiscard]] bool at_unloaded_chunk_boundary() const noexcept {
        return offset_ >= cache_.size()
            && global_index_ < reader_->explicit_note_count()
            && chunk_index_ < reader_->chunk_count();
    }

    [[nodiscard]] std::uint64_t next_chunk_index() const noexcept {
        return chunk_index_;
    }

    void skip_unloaded_chunk(const PackedChartChunkInfo& info) {
        if (!at_unloaded_chunk_boundary()
            || info.first_note_index != global_index_
            || info.note_count == 0U) {
            throw SessionError(
                "explicit PFC1 summary skip is not aligned to a chunk boundary"
            );
        }
        global_index_ += static_cast<std::uint64_t>(info.note_count);
        ++chunk_index_;
        cache_.clear();
        offset_ = 0U;
    }

private:
    void ensure_cache() {
        if (global_index_ >= reader_->explicit_note_count()
            || offset_ < cache_.size()) {
            return;
        }
        std::string error;
        const auto info = reader_->chunk_info(chunk_index_, &error);
        if (!info.has_value()) {
            throw SessionError("cannot inspect explicit PFC1 chunk: " + error);
        }
        if (maximum_chunk_notes_ != 0U
            && info->note_count > maximum_chunk_notes_) {
            throw SessionError(
                "PFC1 chunk exceeds the configured explicit-note count policy"
            );
        }
        constexpr auto packed_note_bytes = sizeof(PackedNote);
        const auto note_count = static_cast<std::size_t>(info->note_count);
        if (info->note_count > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max() / packed_note_bytes
            )
            || note_count > maximum_decoded_chunk_bytes_ / packed_note_bytes) {
            const auto required_bytes =
                static_cast<std::uint64_t>(info->note_count)
                * static_cast<std::uint64_t>(packed_note_bytes);
            throw SessionError(
                "PFC1 chunk requires " + std::to_string(required_bytes)
                + " decoded bytes for " + std::to_string(info->note_count)
                + " explicit notes; StreamingGameplaySession budget is "
                + std::to_string(maximum_decoded_chunk_bytes_)
                + " bytes. Repack PFC1 with a smaller max_notes_per_chunk "
                  "or raise max_explicit_chunk_decoded_bytes."
            );
        }
        auto decoded = reader_->read_chunk(chunk_index_);
        if (!decoded) {
            throw SessionError("cannot decode explicit PFC1 chunk: " + decoded.error);
        }
        if (decoded.notes.size() != info->note_count
            || info->first_note_index != global_index_) {
            throw SessionError("explicit PFC1 cursor/chunk identity is inconsistent");
        }
        cache_ = std::move(decoded.notes);
        offset_ = 0U;
        ++chunk_index_;
    }

    const PackedChartReader* reader_{};
    std::uint64_t chunk_index_{};
    std::uint64_t global_index_{};
    std::size_t offset_{};
    std::uint32_t maximum_chunk_notes_{};
    std::size_t maximum_decoded_chunk_bytes_{};
    std::vector<PackedNote> cache_;
};

struct PatternHeapNode final {
    std::int64_t time_us{};
    std::uint64_t pattern_index{};
    std::uint64_t note_index{};
};

struct PatternHeapLater final {
    [[nodiscard]] bool operator()(
        const PatternHeapNode& left,
        const PatternHeapNode& right
    ) const noexcept {
        if (left.time_us != right.time_us) {
            return left.time_us > right.time_us;
        }
        return left.pattern_index > right.pattern_index;
    }
};

[[nodiscard]] std::optional<std::uint64_t> last_pattern_index_before(
    const PatternRun& pattern,
    const std::uint64_t first_index,
    const std::int64_t exclusive_time
) noexcept {
    if (first_index >= pattern.count) {
        return std::nullopt;
    }
    const auto first = pattern.note_at(first_index);
    if (!first.has_value() || first->time_us >= exclusive_time) {
        return std::nullopt;
    }
    if (pattern.interval_us == 0U) {
        return pattern.count - 1U;
    }
    // Rational PPQN-derived PatternRuns cannot use difference/interval_us
    // directly. Binary-search note_at(), which performs exact overflow-safe
    // rational time arithmetic and remains O(log count) even at trillion scale.
    std::uint64_t low = first_index;
    std::uint64_t high = pattern.count;
    while (low < high) {
        const auto span = high - low;
        const auto middle = low + span / 2U;
        const auto note = pattern.note_at(middle);
        if (note.has_value() && note->time_us < exclusive_time) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return low == first_index
        ? std::optional<std::uint64_t>{first_index}
        : std::optional<std::uint64_t>{low - 1U};
}

struct StreamingNoteIdHash final {
    [[nodiscard]] std::size_t operator()(
        const StreamingNoteId& id
    ) const noexcept {
        auto value = static_cast<std::uint64_t>(id.origin);
        value ^= id.note_index + 0x9E3779B97F4A7C15ULL
            + (value << 6U) + (value >> 2U);
        value ^= id.pattern_index + 0x9E3779B97F4A7C15ULL
            + (value << 6U) + (value >> 2U);
        // Portable 64->size_t fold. One unconditional return avoids the
        // Win64 discarded-branch C4702 diagnostic emitted by MSVC/LTCG /WX.
        return static_cast<std::size_t>(value ^ (value >> 32U));
    }
};

[[nodiscard]] double finite_product(
    const double value,
    const std::uint64_t count
) noexcept {
    const auto product = static_cast<long double>(value)
        * static_cast<long double>(count);
    if (product >= static_cast<long double>(
            std::numeric_limits<double>::max())) {
        return std::numeric_limits<double>::max();
    }
    return static_cast<double>(product);
}

[[nodiscard]] std::string lower_ascii_copy(std::string_view value) {
    std::string result(value);
    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](const unsigned char character) noexcept {
            return character >= 'A' && character <= 'Z'
                ? static_cast<char>(character - 'A' + 'a')
                : static_cast<char>(character);
        }
    );
    return result;
}

[[nodiscard]] std::optional<double> parse_finite_decimal(
    const std::string_view text
) noexcept {
    if (text.empty() || text.size() > 128U) {
        return std::nullopt;
    }
    std::array<char, 129U> buffer{};
    std::copy(text.begin(), text.end(), buffer.begin());
    char* end = nullptr;
    const double value = std::strtod(buffer.data(), &end);
    if (end == buffer.data() || *end != '\0' || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::uint64_t logical_note_count(
    const double multiplier
) noexcept {
    // PULSEFORGE_P1_1_19_STREAMING_UNBOUNDED_NOTE_MULTIPLIER_V1
    // Match GameplaySession: no artificial gameplay cap. PatternRun aggregation
    // remains bounded by saturating uint64 arithmetic, while Lua fan-out has a
    // separate per-frame callback budget.
    if (!std::isfinite(multiplier) || multiplier <= 0.0) {
        return 0U;
    }
    const long double rounded =
        std::floor(static_cast<long double>(multiplier) + 0.5L);
    const long double maximum = static_cast<long double>(
        std::numeric_limits<std::uint64_t>::max()
    );
    if (rounded >= maximum) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return std::max<std::uint64_t>(
        1U,
        static_cast<std::uint64_t>(rounded)
    );
}

[[nodiscard]] double normalized_note_multiplier(
    const std::string_view text
) noexcept {
    const double parsed = parse_finite_decimal(text).value_or(1.0);
    return static_cast<double>(logical_note_count(parsed));
}

// PULSEFORGE_P1_3_0_STREAMING_DYNAMIC_MANIA_PARSE_V1
[[nodiscard]] std::optional<std::uint16_t> parse_dynamic_key_count(
    const std::string_view text,
    const std::uint16_t maximum
) noexcept {
    const auto parsed = parse_finite_decimal(text);
    if (!parsed.has_value() || *parsed < 1.0
        || std::trunc(*parsed) != *parsed
        || *parsed > static_cast<double>(maximum)) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(*parsed);
}

[[nodiscard]] std::int64_t scaled_score(
    const std::int64_t base,
    const double multiplier
) noexcept {
    const long double scaled = static_cast<long double>(base)
        * static_cast<long double>(multiplier);
    const auto minimum = static_cast<long double>(
        std::numeric_limits<std::int64_t>::min()
    );
    const auto maximum = static_cast<long double>(
        std::numeric_limits<std::int64_t>::max()
    );
    if (scaled <= minimum) return std::numeric_limits<std::int64_t>::min();
    if (scaled >= maximum) return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(std::llround(scaled));
}

[[nodiscard]] std::uint64_t scaled_logical_count(
    const std::uint64_t physical_count,
    const double multiplier
) noexcept {
    return saturating_multiply(physical_count, logical_note_count(multiplier));
}



}  // namespace

class StreamingGameplaySession::Impl final {
public:
    Impl(
        const PackedChartReader& reader,
        GameplaySettings settings,
        StreamingGameplayOptions options,
        std::vector<TempoChange> tempos,
        std::vector<ChartEvent> events
    ) : reader_(&reader),
        timing_(std::move(tempos)),
        settings_(std::move(settings)),
        options_(options),
        explicit_cursor_(
            reader,
            options.max_explicit_chunk_notes,
            options.max_explicit_chunk_decoded_bytes
        ),
        chart_events_(std::move(events)) {
        sanitize_gameplay_settings(settings_);
        if (chart_events_.size() > maximum_chart_events) {
            throw SessionError("chart event count exceeds StreamingGameplaySession limit");
        }
        for (const auto& event : chart_events_) {
            if (!std::isfinite(event.time_ms)
                || event.name.size() > maximum_chart_event_name_bytes
                || event.value1.size() > maximum_chart_event_value_bytes
                || event.value2.size() > maximum_chart_event_value_bytes
                || event.payload_json.size() > maximum_chart_note_payload_bytes) {
                throw SessionError("chart event timeline contains an invalid event");
            }
        }
        std::stable_sort(
            chart_events_.begin(),
            chart_events_.end(),
            [](const ChartEvent& left, const ChartEvent& right) noexcept {
                return left.time_ms < right.time_ms;
            }
        );
    }

    void initialize() {
        validate_options();
        player_lane_map_.resize(reader_->key_count());
        opponent_lane_map_.resize(reader_->key_count());
        held_lanes_.resize(reader_->key_count());
        recent_hit_time_.resize(reader_->key_count());
        recent_hit_rating_.resize(reader_->key_count());
        lane_candidate_cursor_.assign(
            reader_->key_count(),
            0U
        );
        mine_kinds_.reserve(reader_->kinds().size());
        for (const auto& kind : reader_->kinds()) {
            mine_kinds_.push_back(builtin_note_type_causes_miss(kind));
        }
        bulk_player_lanes_.resize(reader_->key_count());
        bulk_opponent_lanes_.resize(reader_->key_count());
        prepare_chunk_summaries();
        window_.reserve(options_.max_window_notes);
        // Unlimited active holds must not turn into reserve(SIZE_MAX). Keep a
        // modest amortization reserve and grow only when simultaneous sustain
        // semantics genuinely require it.
        constexpr std::size_t initial_active_hold_reserve = 4'096U;
        active_holds_.reserve(
            options_.max_active_holds == 0U
                ? initial_active_hold_reserve
                : std::min(
                    options_.max_active_holds,
                    initial_active_hold_reserve
                )
        );
        frame_events_.reserve(std::min<std::size_t>(
            options_.max_events_per_frame,
            4'096U
        ));
        recorded_inputs_.reserve(std::min<std::size_t>(
            options_.max_recorded_inputs,
            4'096U
        ));
        if (!reset(nullptr)) {
            throw SessionError(error_);
        }
    }

    [[nodiscard]] bool reset(std::string* const error) {
        clear_external_error(error);
        try {
            healthy_ = true;
            error_.clear();
            summary_ = {};
            summary_.health = 1.0;
            explicit_cursor_.reset();
            window_.clear();
            retired_window_notes_ = 0U;
            deep_compacted_window_notes_ = 0U;
            pressure_compactions_ = 0U;
            reset_window_scan_cursors();
            active_holds_.clear();
            frame_events_.clear();
            recorded_inputs_.clear();
            std::fill(held_lanes_.begin(), held_lanes_.end(), false);
            std::fill(recent_hit_time_.begin(), recent_hit_time_.end(), std::nullopt);
            std::fill(recent_hit_rating_.begin(), recent_hit_rating_.end(), std::nullopt);
            input_recording_overflowed_ = false;
            dropped_frame_events_ = 0U;
            total_resolved_notes_ = 0U;
            catchup_pending_ = false;
            window_saturated_ = false;
            complete_ = false;
            completion_emitted_ = false;
            failure_emitted_ = false;
            song_time_us_ = 0;
            content_end_seen_us_ = 0;
            last_step_ = -1;
            last_beat_ = -1;
            chart_event_cursor_ = 0U;
            player_note_multiplier_ = 1.0;
            opponent_note_multiplier_ = 1.0;
            player_key_count_ = reader_->key_count();
            opponent_key_count_ = reader_->key_count();
            if (!chart_events_.empty()) {
                const auto event_end = milliseconds_to_microseconds(
                    chart_events_.back().time_ms
                );
                if (!event_end.has_value()) {
                    throw SessionError("chart event time is invalid");
                }
                content_end_seen_us_ = std::max(content_end_seen_us_, *event_end);
            }

            rebuild_lane_map(player_lane_map_, player_key_count_);
            rebuild_lane_map(opponent_lane_map_, opponent_key_count_);

            const auto patterns = reader_->patterns();
            pattern_next_.assign(patterns.size(), 0U);
            pattern_heap_.clear();
            pattern_heap_.reserve(patterns.size());
            for (std::size_t index = 0U; index < patterns.size(); ++index) {
                if (patterns[index].count == 0U) {
                    continue;
                }
                const auto first = patterns[index].note_at(0U);
                if (!first.has_value()) {
                    throw SessionError("PFC1 PatternRun first note is invalid");
                }
                pattern_heap_.push_back({
                    first->time_us,
                    static_cast<std::uint64_t>(index),
                    0U,
                });
            }
            std::make_heap(
                pattern_heap_.begin(),
                pattern_heap_.end(),
                PatternHeapLater{}
            );
            return true;
        } catch (const std::bad_alloc&) {
            return fail(
                "insufficient memory for streaming gameplay state",
                error
            );
        } catch (const std::exception& exception) {
            return fail(exception.what(), error);
        } catch (...) {
            return fail("streaming gameplay reset failed", error);
        }
    }

    void begin_frame() noexcept {
        frame_events_.clear();
        dropped_frame_events_ = 0U;
        frame_stats_ = {};
        frame_stats_.peak_window_notes = window_.size();
    }

    [[nodiscard]] bool update(const double song_time_ms) {
        try {
            if (!healthy_ || summary_.failed || complete_) {
                return healthy_;
            }
            const auto requested = milliseconds_to_microseconds(song_time_ms);
            if (!requested.has_value() || *requested < minimum_song_time_us) {
                return set_runtime_error("song time is invalid");
            }
            auto time_us = *requested;
            if (time_us < song_time_us_) {
                if (saturating_subtract_time(song_time_us_, time_us)
                    > options_.maximum_backward_jitter_us) {
                    return set_runtime_error(
                        "backward seek requires StreamingGameplaySession::reset"
                    );
                }
                time_us = song_time_us_;
            }
            return advance_to(time_us, true);
        } catch (const std::bad_alloc&) {
            return set_runtime_error(
                "insufficient memory while advancing streaming gameplay"
            );
        } catch (const std::exception& exception) {
            return set_runtime_error(exception.what());
        } catch (...) {
            return set_runtime_error("streaming gameplay update failed");
        }
    }

    [[nodiscard]] bool finish_song(const double media_end_time_ms) {
        try {
            if (!update(media_end_time_ms) || catchup_pending_
                || summary_.failed || complete_) {
                return healthy_;
            }
            // Audio is authoritative. Drain the remaining PFC sources with the
            // existing per-update work budget; exact-end notes, saturated
            // coincident clusters and chart data extending past the media can
            // therefore never strand the result screen on a stopped clock.
            drain_at_media_end();
            return healthy_;
        } catch (const std::bad_alloc&) {
            return set_runtime_error(
                "insufficient memory while draining streaming gameplay"
            );
        } catch (const std::exception& exception) {
            return set_runtime_error(exception.what());
        } catch (...) {
            return set_runtime_error("streaming gameplay media-end drain failed");
        }
    }

    [[nodiscard]] bool press(
        const std::uint16_t lane,
        const double song_time_ms
    ) {
        try {
            if (!healthy_ || summary_.failed || complete_) {
                return healthy_;
            }
            const auto requested = milliseconds_to_microseconds(song_time_ms);
            if (!requested.has_value() || *requested < minimum_song_time_us) {
                return true;
            }
            if (*requested > song_time_us_) {
                if (!advance_to(*requested, false)) {
                    return false;
                }
            }
            if (catchup_pending_) {
                return true;
            }
            if (lane >= player_key_count_) {
                return healthy_;
            }
            record_input({song_time_ms, lane, true});
            held_lanes_[lane] = true;
            const auto offset = milliseconds_to_microseconds(
                settings_.input_offset_ms
            ).value_or(0);
            const auto adjusted = saturating_add_time(*requested, offset);
            const auto candidate = candidate_for_lane(lane, adjusted);
            if (candidate.has_value()) {
                return hit_window_note(*candidate, adjusted);
            }
            if (!settings_.ghost_tapping) {
                summary_.combo = 0U;
                saturating_add(summary_.misses, 1U);
                summary_.judged_notes += 1.0;
                summary_.health = std::max(
                    0.0,
                    summary_.health - settings_.ghost_tap_health_loss
                );
                emit({
                    GameplayEventType::ghost_tap,
                    no_streaming_note,
                    Rating::miss,
                    *requested,
                    0,
                    static_cast<std::int64_t>(lane),
                    1U,
                });
                check_failure(*requested);
            }
            return healthy_;
        } catch (const std::bad_alloc&) {
            return set_runtime_error(
                "insufficient memory while processing streaming input"
            );
        } catch (const std::exception& exception) {
            return set_runtime_error(exception.what());
        } catch (...) {
            return set_runtime_error("streaming gameplay press failed");
        }
    }

    [[nodiscard]] bool release(
        const std::uint16_t lane,
        const double song_time_ms
    ) {
        try {
            if (!healthy_ || summary_.failed || complete_) {
                return healthy_;
            }
            const auto requested = milliseconds_to_microseconds(song_time_ms);
            if (!requested.has_value() || *requested < minimum_song_time_us) {
                return true;
            }
            if (*requested > song_time_us_) {
                if (!advance_to(*requested, false)) {
                    return false;
                }
            }
            if (lane >= player_key_count_) {
                return healthy_;
            }
            record_input({song_time_ms, lane, false});
            held_lanes_[lane] = false;
            if (settings_.autoplay) {
                return true;
            }
            const auto offset = milliseconds_to_microseconds(
                settings_.input_offset_ms
            ).value_or(0);
            const auto adjusted = saturating_add_time(*requested, offset);
            std::size_t index{};
            while (index < active_holds_.size()) {
                auto& hold = active_holds_[index];
                // PULSEFORGE_P1_3_0_STREAMING_OPPONENT_SUSTAIN_LIFETIME_V1
                // Opponent visual holds must not react to player releases.
                if (hold.note.owner != PackedNoteOwner::player
                    || hold.display_lane != lane) {
                    ++index;
                    continue;
                }
                advance_hold_ticks(hold, adjusted);
                const auto grace = milliseconds_to_microseconds(
                    settings_.release_grace_ms
                ).value_or(0);
                if (saturating_add_time(adjusted, grace)
                    >= note_end_us(hold.note)) {
                    complete_hold(hold, adjusted);
                } else {
                    drop_hold(hold, *requested);
                }
                active_holds_[index] = active_holds_.back();
                active_holds_.pop_back();
            }
            return healthy_;
        } catch (const std::bad_alloc&) {
            return set_runtime_error(
                "insufficient memory while processing streaming input"
            );
        } catch (const std::exception& exception) {
            return set_runtime_error(exception.what());
        } catch (...) {
            return set_runtime_error("streaming gameplay release failed");
        }
    }

    void add_score(const std::int64_t amount) noexcept {
        constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
        constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
        if (amount > 0 && summary_.score > maximum - amount) {
            summary_.score = maximum;
        } else if (amount < 0 && summary_.score < minimum - amount) {
            summary_.score = minimum;
        } else {
            summary_.score += amount;
        }
    }

    void set_health(const double health) {
        if (std::isfinite(health)) {
            summary_.health = std::clamp(health, 0.0, 2.0);
            check_failure(song_time_us_);
        }
    }

    [[nodiscard]] bool apply_event(
        const std::string_view name,
        const std::string_view value1,
        const std::string_view value2
    ) noexcept {
        const auto event_name = lower_ascii_copy(name);

        // PULSEFORGE_P1_3_0_STREAMING_DYNAMIC_MANIA_EVENTS_V1
        if (event_name == "change p1 mania"
            || event_name == "change player mania") {
            if (const auto count = parse_dynamic_key_count(
                    value1, reader_->key_count()
                ); count.has_value()) {
                set_player_key_count(*count);
            }
            return true;
        }
        if (event_name == "change p2 mania"
            || event_name == "change opponent mania") {
            if (const auto count = parse_dynamic_key_count(
                    value1, reader_->key_count()
                ); count.has_value()) {
                set_opponent_key_count(*count);
            }
            return true;
        }
        if (event_name == "change mania") {
            if (const auto count = parse_dynamic_key_count(
                    value1, reader_->key_count()
                ); count.has_value()) {
                set_player_key_count(*count);
                set_opponent_key_count(*count);
            }
            return true;
        }

        if (event_name != "change note multiplier"
            && event_name != "note multiplier") {
            return false;
        }

        const double multiplier = normalized_note_multiplier(value1);
        const auto side = lower_ascii_copy(value2);
        if (side.empty() || side == "0" || side == "3" || side == "both"
            || side == "all") {
            opponent_note_multiplier_ = multiplier;
            player_note_multiplier_ = multiplier;
        } else if (side == "1" || side == "opponent" || side == "enemy"
                   || side == "dad") {
            opponent_note_multiplier_ = multiplier;
        } else if (side == "2" || side == "player" || side == "boyfriend"
                   || side == "bf") {
            player_note_multiplier_ = multiplier;
        } else {
            return true;
        }
        return true;
    }

    [[nodiscard]] const PackedChartReader& reader() const noexcept {
        return *reader_;
    }
    [[nodiscard]] const TimingMap& timing() const noexcept { return timing_; }
    [[nodiscard]] const GameplaySettings& settings() const noexcept { return settings_; }
    [[nodiscard]] GameplaySettings& settings() noexcept { return settings_; }
    [[nodiscard]] const StreamingGameplayOptions& options() const noexcept { return options_; }
    [[nodiscard]] const ScoreSummary& summary() const noexcept { return summary_; }
    [[nodiscard]] std::span<const ChartEvent> chart_events() const noexcept {
        return chart_events_;
    }
    [[nodiscard]] double player_note_multiplier() const noexcept {
        return player_note_multiplier_;
    }
    [[nodiscard]] double opponent_note_multiplier() const noexcept {
        return opponent_note_multiplier_;
    }
    [[nodiscard]] std::uint16_t player_key_count() const noexcept {
        return player_key_count_;
    }
    [[nodiscard]] std::uint16_t opponent_key_count() const noexcept {
        return opponent_key_count_;
    }
    [[nodiscard]] std::span<const StreamingWindowNote> window() const noexcept {
        return {window_.data(), window_.size()};
    }
    [[nodiscard]] std::span<const StreamingGameplayEvent> events() const noexcept {
        return frame_events_;
    }
    [[nodiscard]] std::span<const InputRecord> inputs() const noexcept {
        return recorded_inputs_;
    }
    [[nodiscard]] std::uint64_t dropped_events() const noexcept {
        return dropped_frame_events_;
    }
    [[nodiscard]] std::uint64_t resolved() const noexcept {
        return total_resolved_notes_;
    }
    [[nodiscard]] bool input_overflow() const noexcept {
        return input_recording_overflowed_;
    }
    [[nodiscard]] bool lane_held(const std::uint16_t lane) const noexcept {
        return lane < held_lanes_.size() && held_lanes_[lane];
    }
    [[nodiscard]] std::uint16_t public_display_lane(
        const std::uint16_t lane
    ) const noexcept {
        return display_lane(PackedNoteOwner::player, lane);
    }
    [[nodiscard]] std::uint16_t public_display_lane(
        const NoteOwner owner,
        const std::uint16_t lane
    ) const noexcept {
        return display_lane(
            owner == NoteOwner::player
                ? PackedNoteOwner::player
                : PackedNoteOwner::opponent,
            lane
        );
    }
    [[nodiscard]] bool complete() const noexcept { return complete_; }
    [[nodiscard]] bool healthy() const noexcept { return healthy_; }
    [[nodiscard]] bool catchup() const noexcept { return catchup_pending_; }
    [[nodiscard]] bool saturated() const noexcept { return window_saturated_; }
    [[nodiscard]] double song_time_ms() const noexcept {
        return microseconds_to_milliseconds(song_time_us_);
    }
    [[nodiscard]] std::string_view error() const noexcept { return error_; }
    [[nodiscard]] StreamingGameplayFrameStats frame_stats() const noexcept {
        auto result = frame_stats_;
        result.catchup_pending = catchup_pending_;
        result.window_saturated = window_saturated_;
        result.peak_window_notes = std::max(
            result.peak_window_notes,
            window_.size()
        );
        return result;
    }
    [[nodiscard]] StreamingGameplayAccelerationStats
    acceleration_stats() const noexcept {
        return acceleration_stats_;
    }

    [[nodiscard]] StreamingGameplayMemoryStats memory_stats() const noexcept {
        StreamingGameplayMemoryStats result;
        result.window_notes = window_.size();
        result.window_capacity = window_.capacity();
        result.active_holds = active_holds_.size();
        result.active_hold_capacity = active_holds_.capacity();
        result.cached_explicit_notes = explicit_cursor_.cached_notes();
        result.pattern_cursors = pattern_next_.size();
        result.pattern_heap_capacity = pattern_heap_.capacity();
        result.frame_event_capacity = frame_events_.capacity();
        result.recorded_input_capacity = recorded_inputs_.capacity();
        const auto bytes = static_cast<long double>(window_.capacity())
                * sizeof(StreamingWindowNote)
            + static_cast<long double>(active_holds_.capacity())
                * sizeof(ActiveHold)
            + static_cast<long double>(explicit_cursor_.cached_notes())
                * sizeof(PackedNote)
            + static_cast<long double>(pattern_next_.capacity())
                * sizeof(std::uint64_t)
            + static_cast<long double>(pattern_heap_.capacity())
                * sizeof(PatternHeapNode)
            + static_cast<long double>(frame_events_.capacity())
                * sizeof(StreamingGameplayEvent)
            + static_cast<long double>(recorded_inputs_.capacity())
                * sizeof(InputRecord);
        result.approximate_dynamic_bytes = bytes
                >= static_cast<long double>(
                    std::numeric_limits<std::uint64_t>::max())
            ? std::numeric_limits<std::uint64_t>::max()
            : static_cast<std::uint64_t>(bytes);
        return result;
    }

    [[nodiscard]] bool fail(
        const std::string_view message,
        std::string* const external_error
    ) {
        healthy_ = false;
        error_.assign(message);
        if (external_error != nullptr) {
            *external_error = error_;
        }
        return false;
    }

private:
    struct ActiveHold final {
        StreamingNoteId id;
        PackedNote note;
        std::uint16_t display_lane{};
        std::int64_t next_step{};
        // Logical index in the active SlidingContiguousBuffer region. Prefix
        // retirement shifts this index arithmetically; rare deep compaction
        // rebuilds it once. This removes the previous O(window_size) ID search
        // whenever a sustain completed or dropped.
        std::size_t window_index{};
    };

    struct BulkLaneAccumulator final {
        std::uint64_t count{};
        StreamingNoteId first_id{};
        PackedNote first_note{};
        std::int64_t latest_time_us{};
        bool occupied{};
    };

    struct ChunkLaneSummary final {
        std::uint64_t count{};
        std::uint64_t first_offset{};
        std::int64_t first_time_us{};
        std::int64_t latest_time_us{};
        std::uint16_t flags{};
        std::uint32_t kind_id{};
        bool occupied{};
    };

    struct ExplicitChunkSummary final {
        PackedChartChunkInfo info;
        std::uint64_t ignored_player_mines{};
        std::vector<ChunkLaneSummary> player;
        std::vector<ChunkLaneSummary> opponent;
        bool simple_taps_only{};
    };

    struct SourceChoice final {
        enum class Kind : std::uint8_t { none, explicit_note, pattern } kind{Kind::none};
        SourceNote explicit_note;
        PatternHeapNode pattern;
        std::int64_t time_us{};
    };

    [[nodiscard]] static std::int64_t file_time_signature(
        const std::filesystem::path& path
    ) noexcept {
        std::error_code error;
        const auto value = std::filesystem::last_write_time(path, error);
        if (error) return 0;
        return static_cast<std::int64_t>(
            value.time_since_epoch().count()
        );
    }

    [[nodiscard]] std::filesystem::path chunk_summary_path() const {
        auto result = reader_->path();
        result += ".pfsum.json";
        return result;
    }

    [[nodiscard]] bool load_chunk_summaries() {
        const auto sidecar = chunk_summary_path();
        std::error_code error;
        const auto pfc_bytes = std::filesystem::file_size(
            reader_->path(),
            error
        );
        if (error) return false;

        std::ifstream input(sidecar, std::ios::binary);
        if (!input) return false;

        try {
            const auto root = nlohmann::json::parse(
                input,
                nullptr,
                true,
                true
            );
            if (!root.is_object()
                || root.value("schema", 0U) != 1U
                || root.value("pfc_bytes", std::uint64_t{0U}) != pfc_bytes
                || root.value("pfc_write_time", std::int64_t{0})
                    != file_time_signature(reader_->path())
                || root.value("chunk_count", std::uint64_t{0U})
                    != reader_->chunk_count()
                || root.value("key_count", std::uint64_t{0U})
                    != static_cast<std::uint64_t>(reader_->key_count())
                || !root.contains("chunks")
                || !root.at("chunks").is_array()
                || root.at("chunks").size()
                    != static_cast<std::size_t>(reader_->chunk_count())) {
                return false;
            }

            std::vector<ExplicitChunkSummary> loaded;
            loaded.reserve(root.at("chunks").size());
            for (std::size_t chunk_index = 0U;
                 chunk_index < root.at("chunks").size();
                 ++chunk_index) {
                const auto& node = root.at("chunks").at(chunk_index);
                std::string info_error;
                const auto actual_info = reader_->chunk_info(
                    static_cast<std::uint64_t>(chunk_index),
                    &info_error
                );
                if (!actual_info.has_value()) return false;

                ExplicitChunkSummary summary;
                summary.info = *actual_info;
                summary.simple_taps_only =
                    node.value("simple_taps_only", false);
                summary.ignored_player_mines =
                    node.value("ignored_player_mines", std::uint64_t{0U});
                summary.player.resize(reader_->key_count());
                summary.opponent.resize(reader_->key_count());

                const auto load_lanes =
                    [&](const char* name,
                        std::vector<ChunkLaneSummary>& destination) {
                        if (!node.contains(name)
                            || !node.at(name).is_array()
                            || node.at(name).size() != destination.size()) {
                            return false;
                        }
                        for (std::size_t lane = 0U;
                             lane < destination.size();
                             ++lane) {
                            const auto& entry = node.at(name).at(lane);
                            if (!entry.is_array() || entry.size() != 6U) {
                                return false;
                            }
                            auto& target = destination[lane];
                            target.count =
                                entry.at(0).get<std::uint64_t>();
                            target.first_offset =
                                entry.at(1).get<std::uint64_t>();
                            target.first_time_us =
                                entry.at(2).get<std::int64_t>();
                            target.latest_time_us =
                                entry.at(3).get<std::int64_t>();
                            target.flags =
                                entry.at(4).get<std::uint16_t>();
                            target.kind_id =
                                entry.at(5).get<std::uint32_t>();
                            target.occupied = target.count != 0U;
                            if (target.first_offset
                                >= summary.info.note_count
                                && target.occupied) {
                                return false;
                            }
                        }
                        return true;
                    };

                if (!load_lanes("player", summary.player)
                    || !load_lanes("opponent", summary.opponent)) {
                    return false;
                }
                loaded.push_back(std::move(summary));
            }
            chunk_summaries_ = std::move(loaded);
            return true;
        } catch (...) {
            return false;
        }
    }

    void save_chunk_summaries() const noexcept {
        if (chunk_summaries_.empty()) return;
        try {
            std::error_code error;
            const auto pfc_bytes = std::filesystem::file_size(
                reader_->path(),
                error
            );
            if (error) return;

            nlohmann::json root;
            root["schema"] = 1U;
            root["pfc_bytes"] = pfc_bytes;
            root["pfc_write_time"] = file_time_signature(reader_->path());
            root["chunk_count"] = reader_->chunk_count();
            root["key_count"] = reader_->key_count();
            auto chunks = nlohmann::json::array();

            const auto lanes_json =
                [](const std::vector<ChunkLaneSummary>& lanes) {
                    auto result = nlohmann::json::array();
                    for (const auto& lane : lanes) {
                        result.push_back({
                            lane.count,
                            lane.first_offset,
                            lane.first_time_us,
                            lane.latest_time_us,
                            lane.flags,
                            lane.kind_id,
                        });
                    }
                    return result;
                };

            for (const auto& summary : chunk_summaries_) {
                nlohmann::json node;
                node["simple_taps_only"] = summary.simple_taps_only;
                node["ignored_player_mines"] =
                    summary.ignored_player_mines;
                node["player"] = lanes_json(summary.player);
                node["opponent"] = lanes_json(summary.opponent);
                chunks.push_back(std::move(node));
            }
            root["chunks"] = std::move(chunks);

            const auto target = chunk_summary_path();
            auto temporary = target;
            temporary += ".tmp";
            {
                std::ofstream output(
                    temporary,
                    std::ios::binary | std::ios::trunc
                );
                if (!output) return;
                output << root.dump();
                output.flush();
                if (!output) return;
            }
            // Windows rename does not replace an existing file. The
            // sidecar is optional/private, so remove the stale generation
            // immediately before the atomic-ish same-directory publish.
            std::filesystem::remove(target, error);
            error.clear();
            std::filesystem::rename(temporary, target, error);
            if (error) {
                std::filesystem::remove(temporary, error);
            }
        } catch (...) {
            // A summary is an optional acceleration sidecar. Failure to cache
            // it must never make authoritative PFC1 gameplay fail.
        }
    }

    void prepare_chunk_summaries() {
        acceleration_stats_.fast_offline_autoplay =
            options_.fast_offline_autoplay;
        if (!options_.fast_offline_autoplay
            || reader_->chunk_count() == 0U
            || reader_->chunk_count() > options_.max_summary_chunks) {
            return;
        }

        const auto started = std::chrono::steady_clock::now();
        if (load_chunk_summaries()) {
            acceleration_stats_.chunk_summary_ready = true;
            acceleration_stats_.chunk_summary_reused = true;
            acceleration_stats_.chunk_summary_chunks =
                static_cast<std::uint64_t>(chunk_summaries_.size());
            acceleration_stats_.chunk_summary_prepare_ns =
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - started
                    ).count()
                );
            std::cout
                << "[PulseForge][render] reused PFC chunk summaries: "
                << chunk_summaries_.size() << " chunks\n";
            return;
        }

        std::cout
            << "[PulseForge][render] preparing PFC chunk summaries for "
            << reader_->chunk_count()
            << " chunks (one-time render acceleration)...\n";

        chunk_summaries_.clear();
        chunk_summaries_.reserve(
            static_cast<std::size_t>(reader_->chunk_count())
        );
        std::uint64_t scanned{};
        for (std::uint64_t chunk_index = 0U;
             chunk_index < reader_->chunk_count();
             ++chunk_index) {
            std::string info_error;
            const auto info = reader_->chunk_info(
                chunk_index,
                &info_error
            );
            if (!info.has_value()) {
                chunk_summaries_.clear();
                return;
            }
            auto decoded = reader_->read_chunk(chunk_index);
            if (!decoded) {
                chunk_summaries_.clear();
                return;
            }

            ExplicitChunkSummary summary;
            summary.info = *info;
            summary.player.resize(reader_->key_count());
            summary.opponent.resize(reader_->key_count());
            summary.simple_taps_only = true;

            for (std::size_t offset = 0U;
                 offset < decoded.notes.size();
                 ++offset) {
                const auto& note = decoded.notes[offset];
                saturating_add(scanned, 1U);
                if (note.duration_us != 0U
                    || note.lane >= reader_->key_count()) {
                    summary.simple_taps_only = false;
                    continue;
                }

                if (note.owner == PackedNoteOwner::opponent) {
                    auto& lane = summary.opponent[note.lane];
                    if (!lane.occupied) {
                        lane.occupied = true;
                        lane.first_offset =
                            static_cast<std::uint64_t>(offset);
                        lane.first_time_us = note.time_us;
                        lane.flags = note.flags;
                        lane.kind_id = note.kind_id;
                    }
                    saturating_add(lane.count, 1U);
                    lane.latest_time_us = note.time_us;
                } else if (is_mine(note)) {
                    saturating_add(summary.ignored_player_mines, 1U);
                } else {
                    auto& lane = summary.player[note.lane];
                    if (!lane.occupied) {
                        lane.occupied = true;
                        lane.first_offset =
                            static_cast<std::uint64_t>(offset);
                        lane.first_time_us = note.time_us;
                        lane.flags = note.flags;
                        lane.kind_id = note.kind_id;
                    }
                    saturating_add(lane.count, 1U);
                    lane.latest_time_us = note.time_us;
                }
            }
            chunk_summaries_.push_back(std::move(summary));
        }

        acceleration_stats_.chunk_summary_ready =
            chunk_summaries_.size()
                == static_cast<std::size_t>(reader_->chunk_count());
        acceleration_stats_.chunk_summary_reused = false;
        acceleration_stats_.chunk_summary_chunks =
            static_cast<std::uint64_t>(chunk_summaries_.size());
        acceleration_stats_.chunk_summary_notes_scanned = scanned;
        acceleration_stats_.chunk_summary_prepare_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started
                ).count()
            );
        if (acceleration_stats_.chunk_summary_ready) {
            save_chunk_summaries();
        }
    }

    void validate_options() const {
        if (reader_->key_count() == 0U
            || options_.look_ahead_us < 0
            || options_.terminal_retention_us < 0
            || options_.completion_tail_us < 0
            || options_.maximum_backward_jitter_us < 0
            || options_.max_window_notes == 0U
            || options_.max_events_per_frame == 0U
            || options_.max_explicit_chunk_decoded_bytes == 0U
            || options_.max_explicit_catchup_notes_per_update == 0U
            || options_.max_fast_bulk_chunks_per_update == 0U
            || options_.max_summary_chunks == 0U) {
            throw SessionError("StreamingGameplayOptions are invalid");
        }
    }

    static void clear_external_error(std::string* const error) {
        if (error != nullptr) {
            error->clear();
        }
    }

    [[nodiscard]] bool set_runtime_error(const std::string_view message) {
        healthy_ = false;
        error_.assign(message);
        return false;
    }

    [[nodiscard]] std::uint16_t display_lane(
        const PackedNoteOwner owner,
        const std::uint16_t source_lane
    ) const noexcept {
        const auto& map = owner == PackedNoteOwner::player
            ? player_lane_map_
            : opponent_lane_map_;
        return source_lane < map.size() ? map[source_lane] : source_lane;
    }

    void rebuild_lane_map(
        std::vector<std::uint16_t>& map,
        const std::uint16_t active_key_count
    ) noexcept {
        map.resize(reader_->key_count());
        const auto active = std::min<std::size_t>(
            active_key_count,
            map.size()
        );
        if (active == 0U) {
            std::fill(map.begin(), map.end(), std::uint16_t{0U});
            return;
        }

        // PULSEFORGE_P1_5_0C3_STREAMING_COMPLETE_DYNAMIC_LANE_PROJECTION_V1
        // Match materialized gameplay: every immutable PFC source lane projects
        // proportionally through the current active receptor permutation after a
        // mania shrink.
        std::vector<std::uint16_t> active_projection(active);
        std::iota(
            active_projection.begin(),
            active_projection.end(),
            std::uint16_t{0U}
        );
        if (settings_.mirror) {
            std::reverse(active_projection.begin(), active_projection.end());
        }
        if (settings_.randomize_lanes && active > 1U) {
            deterministic_shuffle(active_projection, settings_.random_seed);
        }
        for (std::size_t source = 0U; source < map.size(); ++source) {
            const auto receptor_slot = source * active / map.size();
            map[source] = active_projection[receptor_slot];
        }
    }

    void reproject_window_lanes() noexcept {
        for (auto& entry : window_) {
            entry.display_lane = display_lane(entry.note.owner, entry.note.lane);
        }
        for (auto& hold : active_holds_) {
            hold.display_lane = display_lane(hold.note.owner, hold.note.lane);
        }
        reset_window_scan_cursors();
    }

    void set_player_key_count(const std::uint16_t key_count) noexcept {
        if (key_count == 0U || key_count > reader_->key_count()
            || key_count == player_key_count_) {
            return;
        }

        std::vector<bool> source_held(reader_->key_count(), false);
        for (std::size_t source = 0U;
             source < player_lane_map_.size();
             ++source) {
            const auto displayed = player_lane_map_[source];
            if (displayed < held_lanes_.size() && held_lanes_[displayed]) {
                source_held[source] = true;
            }
        }

        player_key_count_ = key_count;
        rebuild_lane_map(player_lane_map_, player_key_count_);
        std::fill(held_lanes_.begin(), held_lanes_.end(), false);
        for (std::size_t source = 0U;
             source < player_lane_map_.size();
             ++source) {
            if (!source_held[source]) continue;
            const auto displayed = player_lane_map_[source];
            if (displayed < held_lanes_.size()) {
                held_lanes_[displayed] = true;
            }
        }
        std::fill(
            recent_hit_time_.begin(),
            recent_hit_time_.end(),
            std::nullopt
        );
        std::fill(
            recent_hit_rating_.begin(),
            recent_hit_rating_.end(),
            std::nullopt
        );
        reproject_window_lanes();
    }

    void set_opponent_key_count(const std::uint16_t key_count) noexcept {
        if (key_count == 0U || key_count > reader_->key_count()
            || key_count == opponent_key_count_) {
            return;
        }
        opponent_key_count_ = key_count;
        rebuild_lane_map(opponent_lane_map_, opponent_key_count_);
        reproject_window_lanes();
    }

    static constexpr std::size_t maximum_runtime_note_kind_behaviors = 4'096U;

    [[nodiscard]] const NoteKindRuntimeBehavior* behavior_for(
        const PackedNote& note
    ) const noexcept {
        const auto found = note_kind_behaviors_.find(note.kind_id);
        return found == note_kind_behaviors_.end() ? nullptr : &found->second;
    }

    // PULSEFORGE_P1_5_0C_STREAMING_DECLARATIVE_SUSTAIN_POLICY_V1
    [[nodiscard]] bool effective_sustain_enabled(
        const PackedNote& note
    ) const noexcept {
        const auto* behavior = behavior_for(note);
        return note.duration_us != 0U
            && (behavior == nullptr || behavior->sustain_enabled.value_or(true));
    }

    [[nodiscard]] bool sustain_inherits_note_type(
        const PackedNote& note
    ) const noexcept {
        const auto* behavior = behavior_for(note);
        return effective_sustain_enabled(note)
            && (behavior == nullptr
                || behavior->sustain_inherits_type.value_or(true));
    }

public:
    // PULSEFORGE_P1_5_0C1_STREAMING_NOTE_KIND_BEHAVIOR_ACCESS_FIX_V1
    // Public only so StreamingGameplaySession's facade can forward the API.
    // All surrounding implementation helpers remain private.
    [[nodiscard]] bool set_note_kind_behavior(
        const std::string_view kind,
        const NoteKindRuntimeBehavior& behavior
    ) {
        const auto kinds = reader_->kinds();
        const auto found_kind = std::find(kinds.begin(), kinds.end(), kind);
        if (found_kind == kinds.end()) return false;
        const auto kind_id = static_cast<std::uint32_t>(
            std::distance(kinds.begin(), found_kind)
        );
        if (!note_kind_behaviors_.contains(kind_id)
            && note_kind_behaviors_.size() >= maximum_runtime_note_kind_behaviors) {
            return false;
        }
        note_kind_behaviors_[kind_id] = behavior;
        return true;
    }

    [[nodiscard]] const NoteKindRuntimeBehavior* note_kind_behavior(
        const std::string_view kind
    ) const noexcept {
        const auto kinds = reader_->kinds();
        const auto found_kind = std::find(kinds.begin(), kinds.end(), kind);
        if (found_kind == kinds.end()) return nullptr;
        const auto kind_id = static_cast<std::uint32_t>(
            std::distance(kinds.begin(), found_kind)
        );
        const auto found = note_kind_behaviors_.find(kind_id);
        return found == note_kind_behaviors_.end() ? nullptr : &found->second;
    }

private:
    [[nodiscard]] bool custom_hit_causes_miss(const PackedNote& note) const noexcept {
        const auto* behavior = behavior_for(note);
        if (behavior == nullptr) return false;
        if (sustain_inherits_note_type(note)
            && behavior->sustain_hit_causes_miss.has_value()) {
            return *behavior->sustain_hit_causes_miss;
        }
        return behavior->hit_causes_miss.value_or(false);
    }

    [[nodiscard]] bool ignored_on_miss(const PackedNote& note) const noexcept {
        const auto* behavior = behavior_for(note);
        return behavior != nullptr && behavior->ignore_note.value_or(false);
    }

    [[nodiscard]] double hit_health_for(const PackedNote& note) const noexcept {
        const auto* behavior = behavior_for(note);
        return behavior == nullptr
            ? settings_.health_gain
            : behavior->hit_health.value_or(settings_.health_gain);
    }

    [[nodiscard]] double hold_hit_health_for(
        const PackedNote& note
    ) const noexcept {
        const auto* behavior = behavior_for(note);
        return behavior == nullptr || !sustain_inherits_note_type(note)
            ? settings_.health_gain
            : behavior->hit_health.value_or(settings_.health_gain);
    }

    [[nodiscard]] double miss_health_for(
        const PackedNote& note, const bool hold_drop
    ) const noexcept {
        const auto* behavior = behavior_for(note);
        if (behavior == nullptr) return settings_.health_loss;
        if (hold_drop) {
            if (!sustain_inherits_note_type(note)) {
                return settings_.health_loss;
            }
            return behavior->sustain_miss_health.value_or(
                behavior->miss_health.value_or(settings_.health_loss)
            );
        }
        return behavior->miss_health.value_or(settings_.health_loss);
    }

    // PULSEFORGE_P1_5_0D_STREAMING_TYPED_HAZARD_DAMAGE_V1
    // Mirror materialized semantics: a bare built-in mine keeps the historical
    // 1.5x fallback, while an installed NoteType/Lua behavior is authoritative.
    [[nodiscard]] double hit_causes_miss_health_for(
        const PackedNote& note
    ) const noexcept {
        const auto* behavior = behavior_for(note);
        if (behavior == nullptr) {
            return settings_.health_loss * 1.5;
        }
        if (sustain_inherits_note_type(note)
            && behavior->sustain_hit_causes_miss.value_or(false)
            && behavior->sustain_miss_health.has_value()) {
            return *behavior->sustain_miss_health;
        }
        return behavior->miss_health.value_or(settings_.health_loss);
    }

    [[nodiscard]] bool is_mine(const PackedNote& note) const noexcept {
        return note.kind_id < mine_kinds_.size() && mine_kinds_[note.kind_id];
    }

    void record_input(const InputRecord& input) {
        if (recorded_inputs_.size() == options_.max_recorded_inputs) {
            input_recording_overflowed_ = true;
            return;
        }
        recorded_inputs_.push_back(input);
    }

    void emit(StreamingGameplayEvent event) noexcept {
        // PULSEFORGE_P1_1_18_STREAMING_LOGICAL_EVENT_WEIGHT_IMPL_V1
        // Keep occurrence_count physical for PatternRun compaction; carry the
        // Note Multiplier-expanded count separately for script/stat consumers.
        switch (event.type) {
        case GameplayEventType::note_hit:
        case GameplayEventType::note_miss:
        case GameplayEventType::hold_tick:
        case GameplayEventType::hold_complete:
        case GameplayEventType::hold_drop:
            event.logical_occurrence_count = scaled_logical_count(
                event.occurrence_count,
                player_note_multiplier_
            );
            break;
        case GameplayEventType::opponent_hit:
            event.logical_occurrence_count = scaled_logical_count(
                event.occurrence_count,
                opponent_note_multiplier_
            );
            break;
        case GameplayEventType::ghost_tap:
        case GameplayEventType::chart_event:
        case GameplayEventType::beat:
        case GameplayEventType::step:
        case GameplayEventType::song_complete:
        case GameplayEventType::failed:
            event.logical_occurrence_count = event.occurrence_count;
            break;
        }

        // PULSEFORGE_P1_5_0E_AUTHORITATIVE_CHART_TOTAL_STREAMING_V1
        // PatternRun events remain compact: logical_occurrence_count already
        // represents occurrence_count multiplied by the active owner multiplier.
        if (event.type == GameplayEventType::note_hit
            || event.type == GameplayEventType::note_miss
            || event.type == GameplayEventType::opponent_hit) {
            saturating_add(summary_.chart_total, event.logical_occurrence_count);
        }

        if (!frame_events_.empty()) {
            auto& previous = frame_events_.back();
            const auto contiguous_pattern =
                previous.note_id.origin == StreamingNoteOrigin::pattern_run
                && event.note_id.origin == StreamingNoteOrigin::pattern_run
                && previous.note_id.pattern_index == event.note_id.pattern_index
                && previous.note_id.note_index
                    <= std::numeric_limits<std::uint64_t>::max()
                        - previous.occurrence_count
                && previous.note_id.note_index + previous.occurrence_count
                    == event.note_id.note_index;
            if (contiguous_pattern
                && previous.type == event.type
                && previous.rating == event.rating
                && previous.song_time_us == event.song_time_us) {
                saturating_add(
                    previous.occurrence_count,
                    event.occurrence_count
                );
                saturating_add(
                    previous.logical_occurrence_count,
                    event.logical_occurrence_count
                );
                return;
            }
        }
        if (frame_events_.size() == options_.max_events_per_frame) {
            saturating_add(dropped_frame_events_, event.occurrence_count);
            return;
        }
        frame_events_.push_back(std::move(event));
    }

    void emit_note(
        StreamingGameplayEvent event,
        const PackedNote& note
    ) noexcept {
        event.visual_note = note;
        event.visual_display_lane = display_lane(note.owner, note.lane);
        event.has_visual_note = true;
        emit(std::move(event));
    }

    void mark_resolved(const std::uint64_t count = 1U) noexcept {
        saturating_add(total_resolved_notes_, count);
    }

    [[nodiscard]] SourceChoice peek_source() {
        SourceChoice result;
        const auto explicit_note = explicit_cursor_.peek();
        const auto has_pattern = !pattern_heap_.empty();
        if (!explicit_note.has_value() && !has_pattern) {
            return result;
        }
        if (explicit_note.has_value()
            && (!has_pattern
                || explicit_note->note.time_us <= pattern_heap_.front().time_us)) {
            result.kind = SourceChoice::Kind::explicit_note;
            result.explicit_note = *explicit_note;
            result.time_us = explicit_note->note.time_us;
        } else {
            result.kind = SourceChoice::Kind::pattern;
            result.pattern = pattern_heap_.front();
            result.time_us = result.pattern.time_us;
        }
        return result;
    }

    [[nodiscard]] SourceNote take_one_source(const SourceChoice& choice) {
        if (choice.kind == SourceChoice::Kind::explicit_note) {
            const auto note = explicit_cursor_.take();
            if (!note.has_value()) {
                throw SessionError("explicit PFC1 source ended unexpectedly");
            }
            observe_content_end(note->note);
            return *note;
        }
        if (choice.kind != SourceChoice::Kind::pattern) {
            throw SessionError("cannot take an empty PFC1 source");
        }

        std::pop_heap(
            pattern_heap_.begin(),
            pattern_heap_.end(),
            PatternHeapLater{}
        );
        const auto node = pattern_heap_.back();
        pattern_heap_.pop_back();
        const auto pattern_index = static_cast<std::size_t>(node.pattern_index);
        const auto patterns = reader_->patterns();
        if (pattern_index >= patterns.size()
            || pattern_next_[pattern_index] != node.note_index) {
            throw SessionError("PFC1 PatternRun cursor is inconsistent");
        }
        const auto note = patterns[pattern_index].note_at(node.note_index);
        if (!note.has_value()) {
            throw SessionError("PFC1 PatternRun note arithmetic overflowed");
        }
        pattern_next_[pattern_index] = node.note_index + 1U;
        push_pattern_if_remaining(pattern_index);
        observe_content_end(*note);
        return {
            {
                StreamingNoteOrigin::pattern_run,
                node.note_index,
                node.pattern_index,
            },
            *note,
        };
    }

    void push_pattern_if_remaining(const std::size_t pattern_index) {
        const auto patterns = reader_->patterns();
        const auto next = pattern_next_[pattern_index];
        if (next >= patterns[pattern_index].count) {
            return;
        }
        const auto note = patterns[pattern_index].note_at(next);
        if (!note.has_value()) {
            throw SessionError("PFC1 PatternRun next note arithmetic overflowed");
        }
        pattern_heap_.push_back({
            note->time_us,
            static_cast<std::uint64_t>(pattern_index),
            next,
        });
        std::push_heap(
            pattern_heap_.begin(),
            pattern_heap_.end(),
            PatternHeapLater{}
        );
    }

    void observe_content_end(const PackedNote& note) noexcept {
        content_end_seen_us_ = std::max(content_end_seen_us_, note_end_us(note));
    }

    void reset_bulk_lane_accumulators() noexcept {
        std::fill(
            bulk_player_lanes_.begin(),
            bulk_player_lanes_.end(),
            BulkLaneAccumulator{}
        );
        std::fill(
            bulk_opponent_lanes_.begin(),
            bulk_opponent_lanes_.end(),
            BulkLaneAccumulator{}
        );
    }

    static void add_bulk_lane(
        BulkLaneAccumulator& accumulator,
        const StreamingNoteId id,
        const PackedNote& note
    ) noexcept {
        if (!accumulator.occupied) {
            accumulator.occupied = true;
            accumulator.first_id = id;
            accumulator.first_note = note;
        }
        saturating_add(accumulator.count, 1U);
        accumulator.latest_time_us = note.time_us;
    }

    void flush_bulk_due(
        const std::int64_t event_time_us,
        const std::uint64_t ignored_mines
    ) {
        for (std::size_t lane = 0U;
             lane < bulk_opponent_lanes_.size();
             ++lane) {
            const auto& batch = bulk_opponent_lanes_[lane];
            if (!batch.occupied || batch.count == 0U) {
                continue;
            }
            mark_resolved(batch.count);
            emit_note({
                GameplayEventType::opponent_hit,
                batch.first_id,
                Rating::marvelous,
                event_time_us,
                saturating_subtract_time(
                    event_time_us,
                    batch.first_note.time_us
                ),
                0,
                batch.count,
            }, batch.first_note);
        }

        for (std::size_t lane = 0U;
             lane < bulk_player_lanes_.size();
             ++lane) {
            const auto& batch = bulk_player_lanes_[lane];
            if (!batch.occupied || batch.count == 0U) {
                continue;
            }
            award_hits(
                batch.count,
                batch.first_id,
                event_time_us,
                Rating::marvelous,
                0,
                &batch.first_note
            );
            recent_hit_time_[lane] = batch.latest_time_us;
            recent_hit_rating_[lane] = Rating::marvelous;
        }

        if (ignored_mines != 0U) {
            mark_resolved(ignored_mines);
        }
    }

    static void add_bulk_lane_count(
        BulkLaneAccumulator& accumulator,
        const StreamingNoteId id,
        const PackedNote& note,
        const std::uint64_t count,
        const std::int64_t latest_time_us
    ) noexcept {
        if (count == 0U) return;
        if (!accumulator.occupied) {
            accumulator.occupied = true;
            accumulator.first_id = id;
            accumulator.first_note = note;
            accumulator.latest_time_us = latest_time_us;
        } else {
            accumulator.latest_time_us = std::max(
                accumulator.latest_time_us,
                latest_time_us
            );
        }
        saturating_add(accumulator.count, count);
    }

    [[nodiscard]] std::uint32_t consume_summarized_explicit_chunks(
        const std::int64_t inclusive_time_us,
        const std::int64_t event_time_us,
        const std::uint32_t maximum_chunks
    ) {
        if (!note_kind_behaviors_.empty()
            || !options_.fast_offline_autoplay
            || !settings_.autoplay
            || !acceleration_stats_.chunk_summary_ready
            || maximum_chunks == 0U) {
            return 0U;
        }

        // Aggregate every skipped chunk into one bounded lane batch for this
        // scheduler pass. This means 1000 dense chunks still produce only O(key
        // count) gameplay events rather than O(chunks * key count).
        reset_bulk_lane_accumulators();
        std::uint64_t ignored_mines{};
        std::uint32_t consumed_chunks{};
        std::uint64_t consumed_notes{};

        while (consumed_chunks < maximum_chunks
            && explicit_cursor_.at_unloaded_chunk_boundary()) {
            const auto chunk_index = explicit_cursor_.next_chunk_index();
            if (chunk_index >= chunk_summaries_.size()) break;

            const auto& summary = chunk_summaries_[
                static_cast<std::size_t>(chunk_index)
            ];
            if (!summary.simple_taps_only
                || summary.info.last_time_us > inclusive_time_us) {
                break;
            }

            // Do not jump over a PatternRun event. Mixed explicit/procedural
            // charts keep exact source ordering; pure explicit charts such as
            // Nullifier use the O(chunks) fast path.
            if (!pattern_heap_.empty()
                && pattern_heap_.front().time_us
                    <= summary.info.last_time_us) {
                break;
            }

            for (std::size_t source_lane = 0U;
                 source_lane < summary.opponent.size();
                 ++source_lane) {
                const auto& lane = summary.opponent[source_lane];
                if (!lane.occupied || lane.count == 0U) continue;
                const auto displayed = static_cast<std::size_t>(
                    display_lane(
                        PackedNoteOwner::opponent,
                        static_cast<std::uint16_t>(source_lane)
                    )
                );
                if (displayed >= bulk_opponent_lanes_.size()) continue;
                PackedNote note;
                note.time_us = lane.first_time_us;
                note.lane = static_cast<std::uint16_t>(source_lane);
                note.owner = PackedNoteOwner::opponent;
                note.flags = lane.flags;
                note.kind_id = lane.kind_id;
                add_bulk_lane_count(
                    bulk_opponent_lanes_[displayed],
                    {
                        StreamingNoteOrigin::explicit_note,
                        summary.info.first_note_index + lane.first_offset,
                        0U,
                    },
                    note,
                    lane.count,
                    lane.latest_time_us
                );
            }
            for (std::size_t source_lane = 0U;
                 source_lane < summary.player.size();
                 ++source_lane) {
                const auto& lane = summary.player[source_lane];
                if (!lane.occupied || lane.count == 0U) continue;
                const auto displayed = static_cast<std::size_t>(
                    display_lane(
                        PackedNoteOwner::player,
                        static_cast<std::uint16_t>(source_lane)
                    )
                );
                if (displayed >= bulk_player_lanes_.size()) continue;
                PackedNote note;
                note.time_us = lane.first_time_us;
                note.lane = static_cast<std::uint16_t>(source_lane);
                note.owner = PackedNoteOwner::player;
                note.flags = lane.flags;
                note.kind_id = lane.kind_id;
                add_bulk_lane_count(
                    bulk_player_lanes_[displayed],
                    {
                        StreamingNoteOrigin::explicit_note,
                        summary.info.first_note_index + lane.first_offset,
                        0U,
                    },
                    note,
                    lane.count,
                    lane.latest_time_us
                );
            }

            explicit_cursor_.skip_unloaded_chunk(summary.info);
            PackedNote tail;
            tail.time_us = summary.info.last_time_us;
            observe_content_end(tail);
            saturating_add(
                ignored_mines,
                summary.ignored_player_mines
            );
            saturating_add(
                consumed_notes,
                static_cast<std::uint64_t>(summary.info.note_count)
            );
            ++consumed_chunks;
        }

        if (consumed_chunks != 0U) {
            flush_bulk_due(event_time_us, ignored_mines);
            saturating_add(
                frame_stats_.summarized_explicit_notes,
                consumed_notes
            );
            saturating_add(
                frame_stats_.summarized_explicit_chunks,
                consumed_chunks
            );
        }
        return consumed_chunks;
    }

    // Consume a sequential prefix directly from the decoded PFC1 chunk without
    // creating StreamingWindowNote objects. This is safe for opponent taps and
    // autoplay tap heads: their result is deterministic and occurrence_count
    // already exists specifically for dense bounded event aggregation.
    [[nodiscard]] bool consume_bulk_explicit_due_taps(
        const std::int64_t inclusive_time_us,
        const std::int64_t event_time_us
    ) {
        const auto notes = explicit_cursor_.remaining_chunk();
        if (!note_kind_behaviors_.empty()
            || notes.empty()
            || notes.front().time_us > inclusive_time_us
            || notes.front().duration_us != 0U
            || (notes.front().owner == PackedNoteOwner::player
                && !settings_.autoplay)) {
            return false;
        }

        reset_bulk_lane_accumulators();
        const auto first_global = explicit_cursor_.global_index();
        std::uint64_t ignored_mines{};
        std::size_t consumed{};

        for (; consumed < notes.size(); ++consumed) {
            const auto& note = notes[consumed];
            if (note.time_us > inclusive_time_us
                || note.duration_us != 0U
                || (note.owner == PackedNoteOwner::player
                    && !settings_.autoplay)) {
                break;
            }

            const StreamingNoteId id{
                StreamingNoteOrigin::explicit_note,
                first_global + static_cast<std::uint64_t>(consumed),
                0U,
            };
            const auto lane = static_cast<std::size_t>(
                display_lane(note.owner, note.lane)
            );

            if (note.owner == PackedNoteOwner::opponent) {
                if (lane < bulk_opponent_lanes_.size()) {
                    add_bulk_lane(
                        bulk_opponent_lanes_[lane],
                        id,
                        note
                    );
                } else {
                    mark_resolved();
                }
            } else if (is_mine(note)) {
                saturating_add(ignored_mines, 1U);
            } else if (lane < bulk_player_lanes_.size()) {
                add_bulk_lane(
                    bulk_player_lanes_[lane],
                    id,
                    note
                );
            }
        }

        if (consumed == 0U) {
            return false;
        }

        observe_content_end(notes[consumed - 1U]);
        explicit_cursor_.consume(consumed);
        flush_bulk_due(event_time_us, ignored_mines);
        saturating_add(
            frame_stats_.decoded_bulk_explicit_notes,
            static_cast<std::uint64_t>(consumed)
        );
        saturating_add(
            frame_stats_.decoded_bulk_explicit_batches,
            1U
        );
        return true;
    }

    // PatternRuns should remain arithmetic for as long as possible. Previously
    // fill_window() expanded due procedural taps one occurrence at a time into
    // the 262k judgment window, throwing away the compression advantage.
    [[nodiscard]] bool resolve_due_pattern_taps_fast(
        const PatternHeapNode expected,
        const std::int64_t inclusive_time_us
    ) {
        const auto pattern_index =
            static_cast<std::size_t>(expected.pattern_index);
        const auto patterns = reader_->patterns();
        if (pattern_index >= patterns.size()) {
            throw SessionError("PFC1 PatternRun index is invalid");
        }
        const auto& pattern = patterns[pattern_index];
        if (pattern.duration_us != 0U
            || (pattern.owner == PackedNoteOwner::player
                && (!settings_.autoplay || !note_kind_behaviors_.empty()))) {
            // Runtime custom-note behavior is keyed by kind. Keep player
            // PatternRuns in the bounded exact window path when overrides are
            // active so hazard/health semantics cannot be aggregated away.
            return false;
        }

        const auto exclusive_time = inclusive_time_us
                == std::numeric_limits<std::int64_t>::max()
            ? inclusive_time_us
            : inclusive_time_us + 1;
        const auto last = last_pattern_index_before(
            pattern,
            expected.note_index,
            exclusive_time
        );
        if (!last.has_value()) {
            return false;
        }

        std::pop_heap(
            pattern_heap_.begin(),
            pattern_heap_.end(),
            PatternHeapLater{}
        );
        const auto node = pattern_heap_.back();
        pattern_heap_.pop_back();
        if (node.pattern_index != expected.pattern_index
            || node.note_index != expected.note_index) {
            throw SessionError(
                "PFC1 PatternRun heap changed during fast due resolution"
            );
        }

        const auto first_note = pattern.note_at(node.note_index);
        const auto last_note = pattern.note_at(*last);
        if (!first_note.has_value() || !last_note.has_value()) {
            throw SessionError(
                "PFC1 PatternRun fast due arithmetic overflowed"
            );
        }
        const auto count = *last - node.note_index + 1U;
        pattern_next_[pattern_index] = *last + 1U;
        observe_content_end(*last_note);
        push_pattern_if_remaining(pattern_index);

        const StreamingNoteId first_id{
            StreamingNoteOrigin::pattern_run,
            node.note_index,
            node.pattern_index,
        };

        if (pattern.owner == PackedNoteOwner::opponent) {
            mark_resolved(count);
            emit_note({
                GameplayEventType::opponent_hit,
                first_id,
                Rating::marvelous,
                song_time_us_,
                saturating_subtract_time(
                    song_time_us_,
                    first_note->time_us
                ),
                0,
                count,
            }, *first_note);
            return true;
        }

        if (is_mine(*first_note)) {
            mark_resolved(count);
            return true;
        }

        award_marvelous_hits(
            count,
            first_id,
            song_time_us_,
            *first_note
        );
        const auto lane = static_cast<std::size_t>(
            display_lane(first_note->owner, first_note->lane)
        );
        if (lane < recent_hit_time_.size()) {
            recent_hit_time_[lane] = last_note->time_us;
            recent_hit_rating_[lane] = Rating::marvelous;
        }
        return true;
    }

    void fill_window(const std::int64_t deadline_us) {
        window_saturated_ = false;
        std::uint32_t bulk_explicit_chunks{};
        const std::uint32_t maximum_bulk_explicit_chunks_per_fill =
            options_.fast_offline_autoplay
                ? options_.max_fast_bulk_chunks_per_update
                : 2U;
        while (window_.size() < options_.max_window_notes) {
            if (bulk_explicit_chunks
                    < maximum_bulk_explicit_chunks_per_fill) {
                const auto summarized = consume_summarized_explicit_chunks(
                    song_time_us_,
                    song_time_us_,
                    maximum_bulk_explicit_chunks_per_fill
                        - bulk_explicit_chunks
                );
                bulk_explicit_chunks += summarized;
                if (summarized != 0U) {
                    frame_stats_.peak_window_notes = std::max(
                        frame_stats_.peak_window_notes,
                        window_.size()
                    );
                    continue;
                }
            }

            const auto source = peek_source();
            if (source.kind == SourceChoice::Kind::none
                || source.time_us > deadline_us) {
                return;
            }

            // Deterministic due taps never need to become mutable window
            // objects. Keep them in their compact PFC/PatternRun form until
            // the instant they are resolved.
            if (source.time_us <= song_time_us_) {
                if (source.kind == SourceChoice::Kind::explicit_note) {
                    if (bulk_explicit_chunks
                            < maximum_bulk_explicit_chunks_per_fill
                        && consume_bulk_explicit_due_taps(
                            song_time_us_,
                            song_time_us_
                        )) {
                        ++bulk_explicit_chunks;
                        continue;
                    }
                    const auto& note = source.explicit_note.note;
                    const bool bulk_eligible = note.duration_us == 0U
                        && (note.owner == PackedNoteOwner::opponent
                            || settings_.autoplay);
                    if (bulk_eligible
                        && bulk_explicit_chunks
                            >= maximum_bulk_explicit_chunks_per_fill) {
                        // Defer another raw chunk to the second fill/pass or
                        // next frame instead of turning a bulk-resolvable
                        // avalanche back into per-note window objects.
                        window_saturated_ = true;
                        return;
                    }
                }
                if (source.kind == SourceChoice::Kind::pattern
                    && resolve_due_pattern_taps_fast(
                        source.pattern,
                        song_time_us_
                    )) {
                    continue;
                }
            }

            const auto item = take_one_source(source);
            window_.push_back({
                item.id,
                item.note,
                NoteState::pending,
                display_lane(item.note.owner, item.note.lane),
            });
            frame_stats_.peak_window_notes = std::max(
                frame_stats_.peak_window_notes,
                window_.size()
            );
        }
        const auto source = peek_source();
        window_saturated_ = source.kind != SourceChoice::Kind::none
            && source.time_us <= deadline_us;
    }
    void reset_window_scan_cursors() noexcept {
        due_cursor_ = 0U;
        miss_cursor_ = 0U;

        std::fill(
            lane_candidate_cursor_.begin(),
            lane_candidate_cursor_.end(),
            0U
        );

        due_cursor_autoplay_mode_ = settings_.autoplay;
    }

    void shift_window_scan_cursors(
        const std::size_t removed
    ) noexcept {
        const auto shift =
            [removed](std::size_t& cursor) {
                cursor = cursor >= removed
                    ? cursor - removed
                    : 0U;
            };

        shift(due_cursor_);
        shift(miss_cursor_);

        for (auto& cursor : lane_candidate_cursor_) {
            shift(cursor);
        }
        for (auto& hold : active_holds_) {
            shift(hold.window_index);
        }
    }

    void rebuild_active_hold_window_indices() {
        if (active_holds_.empty()) {
            return;
        }
        constexpr auto invalid = std::numeric_limits<std::size_t>::max();
        std::unordered_map<
            StreamingNoteId,
            std::size_t,
            StreamingNoteIdHash
        > hold_by_id;
        hold_by_id.reserve(active_holds_.size() * 2U);
        for (std::size_t hold_index = 0U;
             hold_index < active_holds_.size();
             ++hold_index) {
            active_holds_[hold_index].window_index = invalid;
            hold_by_id.emplace(active_holds_[hold_index].id, hold_index);
        }

        // Deep compaction is already an O(N) rare path. Rebuild sustain handles
        // in the same single pass; hash lookup avoids O(window * active_holds).
        for (std::size_t index = 0U; index < window_.size(); ++index) {
            const auto& entry = window_[index];
            if (entry.state != NoteState::holding) {
                continue;
            }
            const auto found = hold_by_id.find(entry.id);
            if (found != hold_by_id.end()) {
                active_holds_[found->second].window_index = index;
            }
        }
    }

    void rebuild_window_scan_cursors_after_compaction() noexcept {
        const auto due = std::upper_bound(
            window_.begin(),
            window_.end(),
            song_time_us_,
            [](const std::int64_t time, const StreamingWindowNote& entry) {
                return time < entry.note.time_us;
            }
        );
        due_cursor_ = static_cast<std::size_t>(
            std::distance(window_.begin(), due)
        );

        const auto miss_window = milliseconds_to_microseconds(
            settings_.windows.miss_ms
        ).value_or(180'000);
        const auto cutoff = saturating_add_time(song_time_us_, -miss_window);
        const auto miss = std::lower_bound(
            window_.begin(),
            window_.end(),
            cutoff,
            [](const StreamingWindowNote& entry, const std::int64_t time) {
                return entry.note.time_us < time;
            }
        );
        miss_cursor_ = static_cast<std::size_t>(
            std::distance(window_.begin(), miss)
        );

        // Entries before miss_cursor_ are already outside every legal hit
        // window, so lane searches never need to restart from zero.
        std::fill(
            lane_candidate_cursor_.begin(),
            lane_candidate_cursor_.end(),
            miss_cursor_
        );
        due_cursor_autoplay_mode_ = settings_.autoplay;
    }

    void compact_window(const bool pressure) {
        const auto retention_boundary = saturating_add_time(
            song_time_us_,
            -options_.terminal_retention_us
        );

        // Normal retirement: O(prefix length) inspection but O(1) removal.
        std::size_t removable_prefix = 0U;
        while (removable_prefix < window_.size()) {
            const auto& note = window_[removable_prefix];
            if (!terminal(note.state)
                || note.note.time_us >= retention_boundary) {
                break;
            }
            ++removable_prefix;
        }
        if (removable_prefix != 0U) {
            window_.discard_prefix(removable_prefix);
            shift_window_scan_cursors(removable_prefix);
            retired_window_notes_ += removable_prefix;
        }

        if (!pressure || window_.size() < options_.max_window_notes) {
            return;
        }

        // Under pressure, retention is less important than admitting notes
        // approaching the judgment window. Retire every contiguous terminal
        // entry at the front without moving a single surviving note.
        std::size_t pressure_prefix = 0U;
        while (pressure_prefix < window_.size()
               && terminal(window_[pressure_prefix].state)) {
            ++pressure_prefix;
        }
        if (pressure_prefix != 0U) {
            window_.discard_prefix(pressure_prefix);
            shift_window_scan_cursors(pressure_prefix);
            retired_window_notes_ += pressure_prefix;
        }
        if (window_.size() < options_.max_window_notes) {
            return;
        }

        // Only if a non-terminal note blocks the front do we pay for a full
        // active-range pass. Unlike the old implementation this happens at
        // most once per advance_runtime() and cursor recovery is O(log N), not
        // a reset to zero followed by a 262k-entry rescan next frame.
        const auto removed = window_.compact_remove_if(
            [](const StreamingWindowNote& note) {
                return terminal(note.state);
            }
        );
        if (removed != 0U) {
            deep_compacted_window_notes_ += removed;
            ++pressure_compactions_;
            rebuild_window_scan_cursors_after_compaction();
            rebuild_active_hold_window_indices();
        }
    }

    [[nodiscard]] bool advance_to(
        const std::int64_t target_us,
        const bool emit_musical_clock
    ) {
        while (chart_event_cursor_ < chart_events_.size()) {
            const auto& chart_event = chart_events_[chart_event_cursor_];
            const auto event_time = milliseconds_to_microseconds(
                chart_event.time_ms
            );
            if (!event_time.has_value()) {
                return set_runtime_error("chart event time is invalid");
            }
            if (*event_time > target_us) {
                break;
            }

            // Resolve everything strictly before the event with the previous
            // multiplier. Notes exactly on the event timestamp are left for
            // the next advance_runtime() call so the event takes effect first.
            if (*event_time > song_time_us_) {
                const auto before_event = *event_time
                        == std::numeric_limits<std::int64_t>::min()
                    ? *event_time
                    : *event_time - 1;
                if (before_event > song_time_us_) {
                    song_time_us_ = before_event;
                    if (!advance_runtime(false)) {
                        return false;
                    }
                    if (catchup_pending_ || summary_.failed) {
                        return healthy_;
                    }
                }
            }

            static_cast<void>(apply_event(
                chart_event.name,
                chart_event.value1,
                chart_event.value2
            ));
            song_time_us_ = std::max(song_time_us_, *event_time);
            StreamingGameplayEvent event;
            event.type = GameplayEventType::chart_event;
            event.note_id = no_streaming_note;
            event.rating = Rating::marvelous;
            event.song_time_us = *event_time;
            event.offset_us = saturating_subtract_time(
                target_us,
                *event_time
            );
            event.chart_event_index = chart_event_cursor_;
            emit(std::move(event));
            ++chart_event_cursor_;
        }

        song_time_us_ = target_us;
        return advance_runtime(emit_musical_clock);
    }

    [[nodiscard]] bool advance_runtime(const bool emit_musical_clock) {
        catchup_pending_ = false;
        compact_window(false);
        const auto deadline = saturating_add_time(
            song_time_us_,
            options_.look_ahead_us
        );
        fill_window(deadline);
        process_due_heads(song_time_us_);
        update_holds(song_time_us_);
        if (summary_.failed || !healthy_) {
            return healthy_;
        }
        process_window_misses(song_time_us_);

        // This is the single pressure-compaction point for the update. Most
        // saturated autoplay/dense cases are satisfied by O(1) prefix retire.
        compact_window(window_saturated_);
        if (!resolve_unbuffered_overdue(song_time_us_)) {
            return healthy_;
        }
        if (catchup_pending_ || summary_.failed) {
            return healthy_;
        }
        fill_window(deadline);
        process_due_heads(song_time_us_);
        update_holds(song_time_us_);
        process_window_misses(song_time_us_);

        // Never perform a second full remove_if pass in the same frame.
        compact_window(false);
        if (emit_musical_clock) {
            update_musical_clock(song_time_us_);
        }
        return healthy_;
    }

    [[nodiscard]] bool begin_opponent_window_note(
        const std::size_t index,
        const std::int64_t event_time_us
    ) {
        if (index >= window_.size()) {
            return set_runtime_error(
                "opponent sustain window index is out of range"
            );
        }
        auto& entry = window_[index];
        if (entry.state != NoteState::pending
            || entry.note.owner != PackedNoteOwner::opponent) {
            return healthy_;
        }

        // PULSEFORGE_P1_3_0_STREAMING_OPPONENT_SUSTAIN_LIFETIME_V1
        // Taps resolve at their head. Sustains remain as bounded active holds
        // until their tail so the renderer can clip/consume the body at the
        // receptor exactly like materialized/player sustains.
        if (!effective_sustain_enabled(entry.note)
            || event_time_us >= note_end_us(entry.note)) {
            entry.state = NoteState::completed;
            mark_resolved();
        } else {
            if (options_.max_active_holds != 0U
                && active_holds_.size() >= options_.max_active_holds) {
                return set_runtime_error(
                    "active sustain count exceeds the configured streaming "
                    "memory policy"
                );
            }
            entry.state = NoteState::holding;
            active_holds_.push_back({
                entry.id,
                entry.note,
                entry.display_lane,
                0,
                index,
            });
        }

        emit_note({
            GameplayEventType::opponent_hit,
            entry.id,
            Rating::marvelous,
            event_time_us,
            saturating_subtract_time(event_time_us, entry.note.time_us),
            0,
            1U,
        }, entry.note);
        return healthy_;
    }

    void process_due_heads(const std::int64_t time_us) {
        // settings() is mutable. If autoplay changes during the song, revisit
        // the live temporal range once. The sliding buffer has already retired
        // permanently old notes.
        if (due_cursor_autoplay_mode_ != settings_.autoplay) {
            due_cursor_ = 0U;
            due_cursor_autoplay_mode_ = settings_.autoplay;
        }

        reset_bulk_lane_accumulators();
        std::uint64_t ignored_mines{};

        const auto flush = [&]() {
            flush_bulk_due(time_us, ignored_mines);
            ignored_mines = 0U;
            reset_bulk_lane_accumulators();
        };

        while (due_cursor_ < window_.size()) {
            const auto index = due_cursor_;
            auto& entry = window_[index];
            if (entry.note.time_us > time_us) {
                break;
            }
            ++due_cursor_;

            if (entry.state != NoteState::pending) {
                continue;
            }

            const auto lane =
                static_cast<std::size_t>(entry.display_lane);

            if (entry.note.owner == PackedNoteOwner::opponent) {
                if (!effective_sustain_enabled(entry.note)) {
                    entry.state = NoteState::completed;
                    if (lane < bulk_opponent_lanes_.size()) {
                        add_bulk_lane(
                            bulk_opponent_lanes_[lane],
                            entry.id,
                            entry.note
                        );
                    } else {
                        mark_resolved();
                        emit_note({
                            GameplayEventType::opponent_hit,
                            entry.id,
                            Rating::marvelous,
                            time_us,
                            saturating_subtract_time(
                                time_us,
                                entry.note.time_us
                            ),
                            0,
                            1U,
                        }, entry.note);
                    }
                } else {
                    // Preserve ordering relative to already batched taps, then
                    // retain this sustain until its tail rather than resolving
                    // the whole note at the head.
                    flush();
                    if (!begin_opponent_window_note(index, time_us)) {
                        return;
                    }
                }
                continue;
            }

            if (!settings_.autoplay) {
                continue;
            }

            if (is_mine(entry.note) || custom_hit_causes_miss(entry.note)) {
                entry.state = NoteState::ignored;
                saturating_add(ignored_mines, 1U);
                continue;
            }

            if (!effective_sustain_enabled(entry.note)
                && !note_kind_behaviors_.empty()) {
                flush();
                if (!hit_window_note(index, entry.note.time_us, false)) return;
                continue;
            }

            if (!effective_sustain_enabled(entry.note)) {
                // This replaces hit_exact_tap_stack() in botplay. Every due tap
                // is visited exactly once, so a 200k coincident cluster is O(N)
                // rather than repeatedly rescanning the same timestamp range.
                entry.state = NoteState::completed;
                if (lane < bulk_player_lanes_.size()) {
                    add_bulk_lane(
                        bulk_player_lanes_[lane],
                        entry.id,
                        entry.note
                    );
                } else {
                    mark_resolved();
                }
                continue;
            }

            // Preserve full sustain timing semantics. Flush accumulated tap
            // batches before the sustain so event ordering remains bounded and
            // intuitive.
            flush();
            if (!hit_window_note(
                    index,
                    entry.note.time_us,
                    false
                )) {
                return;
            }
        }

        flush();
    }

    void resolve_window_at_media_end(StreamingWindowNote& entry) {
        if (entry.state != NoteState::pending) {
            return;
        }
        if (entry.note.time_us > song_time_us_) {
            entry.state = NoteState::ignored;
            mark_resolved();
            return;
        }
        if (entry.note.owner == PackedNoteOwner::opponent) {
            entry.state = NoteState::completed;
            mark_resolved();
            emit_note({
                GameplayEventType::opponent_hit,
                entry.id,
                Rating::marvelous,
                song_time_us_,
                saturating_subtract_time(song_time_us_, entry.note.time_us),
                0,
                1U,
            }, entry.note);
            return;
        }
        if (is_mine(entry.note) || ignored_on_miss(entry.note)) {
            entry.state = NoteState::ignored;
            mark_resolved();
            return;
        }
        if (stacked_with_recent_hit(entry.note, entry.display_lane)
            || settings_.autoplay) {
            entry.state = NoteState::completed;
            award_hits(
                1U,
                entry.id,
                song_time_us_,
                settings_.autoplay
                    ? Rating::marvelous
                    : recent_rating(entry.display_lane),
                saturating_subtract_time(song_time_us_, entry.note.time_us),
                &entry.note
            );
            return;
        }
        miss_window_note(entry, song_time_us_, false);
    }

    void resolve_source_at_media_end(const SourceNote& item) {
        if (item.note.time_us > song_time_us_) {
            mark_resolved();
            return;
        }
        if (item.note.owner == PackedNoteOwner::opponent) {
            mark_resolved();
            emit_note({
                GameplayEventType::opponent_hit,
                item.id,
                Rating::marvelous,
                song_time_us_,
                saturating_subtract_time(song_time_us_, item.note.time_us),
                0,
                1U,
            }, item.note);
            return;
        }
        if (is_mine(item.note) || ignored_on_miss(item.note)) {
            mark_resolved();
            return;
        }
        const auto lane = display_lane(item.note.owner, item.note.lane);
        if (stacked_with_recent_hit(item.note, lane) || settings_.autoplay) {
            if (settings_.autoplay && effective_sustain_enabled(item.note)) {
                award_unbuffered_autoplay_sustain(item);
                return;
            }
            award_hits(
                1U,
                item.id,
                song_time_us_,
                settings_.autoplay ? Rating::marvelous : recent_rating(lane),
                saturating_subtract_time(song_time_us_, item.note.time_us),
                &item.note
            );
            return;
        }
        award_misses(1U, item.id, song_time_us_, &item.note);
    }

    void drain_pattern_at_media_end(const PatternHeapNode expected) {
        std::pop_heap(
            pattern_heap_.begin(),
            pattern_heap_.end(),
            PatternHeapLater{}
        );
        const auto node = pattern_heap_.back();
        pattern_heap_.pop_back();
        if (node.pattern_index != expected.pattern_index
            || node.note_index != expected.note_index) {
            throw SessionError("PFC1 PatternRun heap changed during media-end drain");
        }
        const auto pattern_index = static_cast<std::size_t>(node.pattern_index);
        const auto patterns = reader_->patterns();
        const auto& pattern = patterns[pattern_index];
        const auto remaining = pattern.count - node.note_index;
        std::uint64_t due{};
        const auto exclusive_end = song_time_us_
                == std::numeric_limits<std::int64_t>::max()
            ? song_time_us_
            : song_time_us_ + 1;
        if (const auto last = last_pattern_index_before(
                pattern,
                node.note_index,
                exclusive_end
            ); last.has_value()) {
            due = *last - node.note_index + 1U;
        }
        const auto future = remaining - due;
        const auto first_note = pattern.note_at(node.note_index);
        const auto last_note = pattern.note_at(pattern.count - 1U);
        if (!first_note.has_value() || !last_note.has_value()) {
            throw SessionError("PFC1 PatternRun media-end arithmetic overflowed");
        }
        pattern_next_[pattern_index] = pattern.count;
        observe_content_end(*last_note);

        const StreamingNoteId first_id{
            StreamingNoteOrigin::pattern_run,
            node.note_index,
            node.pattern_index,
        };
        if (due != 0U) {
            if (pattern.owner == PackedNoteOwner::opponent) {
                mark_resolved(due);
                emit_note({
                    GameplayEventType::opponent_hit,
                    first_id,
                    Rating::marvelous,
                    song_time_us_,
                    saturating_subtract_time(song_time_us_, first_note->time_us),
                    0,
                    due,
                }, *first_note);
            } else if (is_mine(*first_note) || ignored_on_miss(*first_note)) {
                mark_resolved(due);
            } else {
                const auto lane = display_lane(first_note->owner, first_note->lane);
                if (stacked_with_recent_hit(*first_note, lane)
                    || settings_.autoplay) {
                    if (settings_.autoplay
                        && effective_sustain_enabled(*first_note)) {
                        award_unbuffered_autoplay_pattern_sustains(
                            pattern,
                            node,
                            due
                        );
                    } else {
                        award_hits(
                            due,
                            first_id,
                            song_time_us_,
                            settings_.autoplay
                                ? Rating::marvelous
                                : recent_rating(lane),
                            saturating_subtract_time(
                                song_time_us_,
                                first_note->time_us
                            ),
                            &*first_note
                        );
                    }
                } else {
                    award_misses(due, first_id, song_time_us_, &*first_note);
                }
            }
        }
        mark_resolved(future);
    }

    void drain_at_media_end() {
        for (auto& hold : active_holds_) {
            if (hold.note.owner == PackedNoteOwner::opponent) {
                set_window_state(hold, NoteState::completed);
                mark_resolved();
            } else {
                complete_hold(hold, song_time_us_);
            }
        }
        active_holds_.clear();
        for (auto& entry : window_) {
            resolve_window_at_media_end(entry);
            if (summary_.failed || !healthy_) {
                return;
            }
        }

        std::uint64_t explicit_work{};
        while (true) {
            const auto source = peek_source();
            if (source.kind == SourceChoice::Kind::none) {
                break;
            }
            if (source.kind == SourceChoice::Kind::pattern) {
                drain_pattern_at_media_end(source.pattern);
            } else {
                if (explicit_work
                    == options_.max_explicit_catchup_notes_per_update) {
                    catchup_pending_ = true;
                    return;
                }
                resolve_source_at_media_end(take_one_source(source));
                ++explicit_work;
            }
            if (summary_.failed || !healthy_) {
                return;
            }
        }

        catchup_pending_ = false;
        const bool window_drained = std::all_of(
            window_.begin(),
            window_.end(),
            [](const StreamingWindowNote& note) { return terminal(note.state); }
        );
        if (!explicit_cursor_.exhausted() || !pattern_heap_.empty()
            || !window_drained || !active_holds_.empty()) {
            return;
        }
        complete_ = true;
        completion_emitted_ = true;
        emit({
            GameplayEventType::song_complete,
            no_streaming_note,
            Rating::marvelous,
            song_time_us_,
            0,
            0,
            1U,
        });
    }

    void process_window_misses(const std::int64_t time_us) {
        const auto miss_window = milliseconds_to_microseconds(
            settings_.windows.miss_ms
        ).value_or(180'000);
        const auto exclusive_cutoff = saturating_add_time(time_us, -miss_window);

        while (miss_cursor_ < window_.size()) {
            const auto index = miss_cursor_;
            auto& entry = window_[index];

            // window_ is time ordered. If this note has not expired, none of
            // the later notes have expired either.
            if (entry.note.time_us >= exclusive_cutoff) {
                break;
            }

            ++miss_cursor_;

            if (entry.state != NoteState::pending) {
                continue;
            }

            if (entry.note.owner == PackedNoteOwner::opponent) {
                if (!begin_opponent_window_note(index, time_us)) {
                    return;
                }
            } else if (is_mine(entry.note) || ignored_on_miss(entry.note)) {
                entry.state = NoteState::ignored;
                mark_resolved();
            } else if (stacked_with_recent_hit(entry.note, entry.display_lane)
                && !effective_sustain_enabled(entry.note)) {
                entry.state = NoteState::completed;
                award_hits(
                    1U,
                    entry.id,
                    time_us,
                    recent_rating(entry.display_lane),
                    saturating_subtract_time(time_us, entry.note.time_us),
                    &entry.note
                );
            } else if (settings_.autoplay) {
                (void)hit_window_note(index, entry.note.time_us);
            } else {
                miss_window_note(entry, time_us, false);
                if (summary_.failed) {
                    return;
                }
            }
        }
    }

    [[nodiscard]] bool resolve_unbuffered_overdue(
        const std::int64_t time_us
    ) {
        const auto miss_window = milliseconds_to_microseconds(
            settings_.windows.miss_ms
        ).value_or(180'000);
        const auto cutoff = saturating_add_time(time_us, -miss_window);
        std::uint64_t explicit_work{};
        std::uint32_t bulk_explicit_chunks{};
        const std::uint32_t maximum_bulk_explicit_chunks =
            options_.fast_offline_autoplay
                ? options_.max_fast_bulk_chunks_per_update
                : 2U;
        while (true) {
            if (bulk_explicit_chunks < maximum_bulk_explicit_chunks) {
                const auto inclusive_cutoff = saturating_add_time(
                    cutoff,
                    -1
                );
                const auto summarized = consume_summarized_explicit_chunks(
                    inclusive_cutoff,
                    time_us,
                    maximum_bulk_explicit_chunks - bulk_explicit_chunks
                );
                bulk_explicit_chunks += summarized;
                if (summarized != 0U) {
                    continue;
                }
            }
            const auto source = peek_source();
            if (source.kind == SourceChoice::Kind::none
                || source.time_us >= cutoff) {
                return true;
            }
            if (source.kind == SourceChoice::Kind::explicit_note) {
                if (bulk_explicit_chunks < maximum_bulk_explicit_chunks) {
                    const auto inclusive_cutoff = saturating_add_time(
                        cutoff,
                        -1
                    );
                    if (consume_bulk_explicit_due_taps(
                            inclusive_cutoff,
                            time_us
                        )) {
                        ++bulk_explicit_chunks;
                        continue;
                    }
                }
                if (explicit_work
                    == options_.max_explicit_catchup_notes_per_update) {
                    catchup_pending_ = true;
                    return true;
                }
                const auto note = take_one_source(source);
                resolve_single_unbuffered(note, time_us);
                ++explicit_work;
                if (summary_.failed || !healthy_) {
                    return healthy_;
                }
                continue;
            }
            resolve_pattern_batch(source.pattern, cutoff, time_us);
            if (summary_.failed || !healthy_) {
                return healthy_;
            }
        }
    }

    void resolve_single_unbuffered(
        const SourceNote& item,
        const std::int64_t event_time_us
    ) {
        if (item.id.origin == StreamingNoteOrigin::explicit_note) {
            saturating_add(frame_stats_.scalar_explicit_notes, 1U);
        }
        if (item.note.owner == PackedNoteOwner::opponent) {
            mark_resolved();
            emit_note({
                GameplayEventType::opponent_hit,
                item.id,
                Rating::marvelous,
                event_time_us,
                saturating_subtract_time(event_time_us, item.note.time_us),
                0,
                1U,
            }, item.note);
            return;
        }
        if (is_mine(item.note) || ignored_on_miss(item.note)) {
            mark_resolved();
            return;
        }
        const auto lane = display_lane(item.note.owner, item.note.lane);
        if (stacked_with_recent_hit(item.note, lane)
            && !effective_sustain_enabled(item.note)) {
            award_hits(
                1U,
                item.id,
                event_time_us,
                recent_rating(lane),
                saturating_subtract_time(event_time_us, item.note.time_us),
                &item.note
            );
            return;
        }
        if (settings_.autoplay) {
            if (effective_sustain_enabled(item.note)) {
                award_unbuffered_autoplay_sustain(item);
                return;
            }
            award_marvelous_hits(1U, item.id, item.note.time_us, item.note);
            return;
        }
        award_misses(1U, item.id, event_time_us, &item.note);
    }

    void resolve_pattern_batch(
        const PatternHeapNode node,
        const std::int64_t cutoff,
        const std::int64_t event_time_us
    ) {
        std::pop_heap(
            pattern_heap_.begin(),
            pattern_heap_.end(),
            PatternHeapLater{}
        );
        const auto popped = pattern_heap_.back();
        pattern_heap_.pop_back();
        if (popped.pattern_index != node.pattern_index
            || popped.note_index != node.note_index) {
            throw SessionError("PFC1 PatternRun heap changed unexpectedly");
        }
        const auto pattern_index = static_cast<std::size_t>(node.pattern_index);
        const auto patterns = reader_->patterns();
        const auto& pattern = patterns[pattern_index];
        const auto last = last_pattern_index_before(
            pattern,
            node.note_index,
            cutoff
        );
        if (!last.has_value()) {
            pattern_heap_.push_back(popped);
            std::push_heap(
                pattern_heap_.begin(),
                pattern_heap_.end(),
                PatternHeapLater{}
            );
            return;
        }
        const auto count = *last - node.note_index + 1U;
        const auto first_note = pattern.note_at(node.note_index);
        const auto last_note = pattern.note_at(*last);
        if (!first_note.has_value() || !last_note.has_value()) {
            throw SessionError("PFC1 PatternRun batch arithmetic overflowed");
        }
        pattern_next_[pattern_index] = *last + 1U;
        observe_content_end(*last_note);
        push_pattern_if_remaining(pattern_index);
        const StreamingNoteId first_id{
            StreamingNoteOrigin::pattern_run,
            node.note_index,
            node.pattern_index,
        };

        if (pattern.owner == PackedNoteOwner::opponent) {
            mark_resolved(count);
            emit_note({
                GameplayEventType::opponent_hit,
                first_id,
                Rating::marvelous,
                event_time_us,
                saturating_subtract_time(event_time_us, first_note->time_us),
                0,
                count,
            }, *first_note);
            return;
        }
        if (is_mine(*first_note) || ignored_on_miss(*first_note)) {
            mark_resolved(count);
            return;
        }
        const auto lane = display_lane(first_note->owner, first_note->lane);
        if (!effective_sustain_enabled(*first_note)
            && stacked_with_recent_hit(*first_note, lane)) {
            award_hits(
                count,
                first_id,
                event_time_us,
                recent_rating(lane),
                saturating_subtract_time(event_time_us, first_note->time_us),
                &*first_note
            );
            return;
        }
        if (settings_.autoplay) {
            if (effective_sustain_enabled(*first_note)) {
                award_unbuffered_autoplay_pattern_sustains(
                    pattern,
                    node,
                    count
                );
                return;
            }
            award_marvelous_hits(count, first_id, first_note->time_us, *first_note);
            return;
        }
        award_misses(count, first_id, event_time_us, &*first_note);
    }

    [[nodiscard]] std::optional<std::size_t> candidate_for_lane(
        const std::uint16_t lane,
        const std::int64_t adjusted_time_us
    ) noexcept {
        if (lane >= lane_candidate_cursor_.size()) {
            return std::nullopt;
        }

        const auto miss_window = milliseconds_to_microseconds(
            settings_.windows.miss_ms
        ).value_or(180'000);

        auto& cursor = lane_candidate_cursor_[lane];

        while (cursor < window_.size()) {
            const auto& entry = window_[cursor];

            if (entry.state != NoteState::pending
                || entry.note.owner != PackedNoteOwner::player
                || entry.display_lane != lane) {
                ++cursor;
                continue;
            }

            const auto delta = saturating_subtract_time(
                adjusted_time_us,
                entry.note.time_us
            );

            // This lane note is already permanently outside the hit window.
            if (delta > miss_window) {
                ++cursor;
                continue;
            }

            // The first relevant note for this lane is still in the future.
            if (delta < -miss_window) {
                return std::nullopt;
            }

            return cursor;
        }

        return std::nullopt;
    }

    [[nodiscard]] Rating judge(const std::int64_t absolute_offset_us) const noexcept {
        const auto as_ms = microseconds_to_milliseconds(absolute_offset_us);
        if (as_ms <= settings_.windows.marvelous_ms) {
            return Rating::marvelous;
        }
        if (as_ms <= settings_.windows.sick_ms) {
            return Rating::sick;
        }
        if (as_ms <= settings_.windows.good_ms) {
            return Rating::good;
        }
        if (as_ms <= settings_.windows.bad_ms) {
            return Rating::bad;
        }
        return Rating::miss;
    }

    [[nodiscard]] static std::int32_t rating_score(const Rating rating) noexcept {
        switch (rating) {
        case Rating::marvelous: return 350;
        case Rating::sick: return 300;
        case Rating::good: return 200;
        case Rating::bad: return 100;
        case Rating::mine: return -200;
        case Rating::miss: return 0;
        }
        return 0;
    }

    [[nodiscard]] static double rating_weight(const Rating rating) noexcept {
        switch (rating) {
        case Rating::marvelous: return 1.0;
        case Rating::sick: return 0.95;
        case Rating::good: return 0.75;
        case Rating::bad: return 0.50;
        case Rating::mine:
        case Rating::miss: return 0.0;
        }
        return 0.0;
    }

    [[nodiscard]] bool exact_same_tap(
        const StreamingWindowNote& first,
        const StreamingWindowNote& candidate
    ) const noexcept {
        return candidate.state == NoteState::pending
            && candidate.note.owner == PackedNoteOwner::player
            && !effective_sustain_enabled(candidate.note)
            && candidate.note.time_us == first.note.time_us
            && candidate.note.lane == first.note.lane
            && candidate.display_lane == first.display_lane
            && candidate.note.flags == first.note.flags
            && candidate.note.kind_id == first.note.kind_id
            && !is_mine(candidate.note);
    }

    [[nodiscard]] bool hit_exact_tap_stack(
        const std::size_t index,
        const std::int64_t adjusted_time_us,
        const Rating rating,
        const std::int64_t offset
    ) {
        if (index >= window_.size()) {
            return true;
        }

        auto& first = window_[index];
        const auto hit_time = first.note.time_us;
        const auto display_lane = first.display_lane;
        const auto first_id = first.id;
        std::uint64_t count = 0U;

        // Resolve an exact coincident tap stack with one score/event update.
        for (std::size_t current = index; current < window_.size(); ++current) {
            auto& candidate = window_[current];
            if (candidate.note.time_us > hit_time) {
                break;
            }
            if (!exact_same_tap(first, candidate)) {
                continue;
            }
            candidate.state = NoteState::completed;
            saturating_add(count, 1U);
        }

        recent_hit_time_[display_lane] = hit_time;
        recent_hit_rating_[display_lane] = rating;

        award_hits(
            count,
            first_id,
            song_time_us_,
            rating,
            offset,
            &first.note
        );

        // Preserve the existing tolerance semantics for nearby, non-identical
        // notes after the exact coincident group has been resolved in bulk.
        const auto tolerance = milliseconds_to_microseconds(
            settings_.stacked_note_tolerance_ms
        ).value_or(0);

        for (std::size_t stacked = index + 1U;
             stacked < window_.size();
             ++stacked) {
            auto& candidate = window_[stacked];

            if (candidate.note.time_us
                > saturating_add_time(hit_time, tolerance)) {
                break;
            }

            if (candidate.state != NoteState::pending
                || candidate.note.owner != PackedNoteOwner::player
                || candidate.display_lane != display_lane
                || candidate.note.time_us
                    < saturating_add_time(hit_time, -tolerance)) {
                continue;
            }

            if (!hit_window_note(stacked, adjusted_time_us, false)) {
                return false;
            }
        }

        return healthy_;
    }

    [[nodiscard]] bool hit_window_note(
        const std::size_t index,
        const std::int64_t adjusted_time_us,
        const bool resolve_stack = true
    ) {
        if (index >= window_.size()) {
            return true;
        }
        auto& entry = window_[index];
        if (entry.state != NoteState::pending) {
            return true;
        }
        const auto offset = saturating_subtract_time(
            adjusted_time_us,
            entry.note.time_us
        );
        const bool builtin_mine = is_mine(entry.note);
        if (builtin_mine || custom_hit_causes_miss(entry.note)) {
            entry.state = NoteState::completed;
            const auto logical_count =
                logical_note_count(player_note_multiplier_);
            const double logical_weight =
                static_cast<double>(logical_count);
            add_score(scaled_score(
                rating_score(Rating::mine),
                logical_weight
            ));
            summary_.combo = 0U;
            saturating_add(summary_.misses, logical_count);
            summary_.judged_notes += logical_weight;
            const double damage = hit_causes_miss_health_for(entry.note);
            summary_.health = std::clamp(
                summary_.health - damage, 0.0, 2.0
            );
            mark_resolved();
            emit_note({
                GameplayEventType::note_hit,
                entry.id,
                Rating::mine,
                song_time_us_,
                offset,
                0,
                1U,
            }, entry.note);
            check_failure(song_time_us_);
            return true;
        }

        const auto absolute_offset = offset == std::numeric_limits<std::int64_t>::min()
            ? std::numeric_limits<std::int64_t>::max()
            : std::abs(offset);
        const auto rating = judge(absolute_offset);
        if (rating == Rating::miss) {
            miss_window_note(entry, song_time_us_, false);
            return true;
        }

        if (resolve_stack
            && !effective_sustain_enabled(entry.note)
            && !is_mine(entry.note)) {
            return hit_exact_tap_stack(
                index,
                adjusted_time_us,
                rating,
                offset
            );
        }
        entry.state = effective_sustain_enabled(entry.note)
            ? NoteState::holding
            : NoteState::completed;
        const auto logical_count =
            logical_note_count(player_note_multiplier_);
        const double logical_weight =
            static_cast<double>(logical_count);
        add_score(scaled_score(rating_score(rating), logical_weight));
        saturating_add(summary_.combo, logical_count);
        summary_.max_combo = std::max(summary_.max_combo, summary_.combo);
        summary_.weighted_hits += rating_weight(rating) * logical_weight;
        summary_.judged_notes += logical_weight;
        summary_.health = std::clamp(
            summary_.health + hit_health_for(entry.note) * logical_weight,
            0.0,
            2.0
        );
        switch (rating) {
        case Rating::marvelous: saturating_add(summary_.marvelous, logical_count); break;
        case Rating::sick: saturating_add(summary_.sick, logical_count); break;
        case Rating::good: saturating_add(summary_.good, logical_count); break;
        case Rating::bad: saturating_add(summary_.bad, logical_count); break;
        case Rating::miss:
        case Rating::mine: break;
        }
        recent_hit_time_[entry.display_lane] = entry.note.time_us;
        recent_hit_rating_[entry.display_lane] = rating;

        if (entry.state == NoteState::holding) {
            if (options_.max_active_holds != 0U
                && active_holds_.size() >= options_.max_active_holds) {
                return set_runtime_error(
                    "active sustain count exceeds the configured streaming "
                    "memory policy"
                );
            }
            const auto step = timing_.step_at(
                microseconds_to_milliseconds(entry.note.time_us)
            );
            if (!std::isfinite(step)
                || step >= static_cast<double>(
                    std::numeric_limits<std::int64_t>::max())
                || step < static_cast<double>(
                    std::numeric_limits<std::int64_t>::min())) {
                return set_runtime_error("sustain musical-step index overflows");
            }
            active_holds_.push_back({
                entry.id,
                entry.note,
                entry.display_lane,
                static_cast<std::int64_t>(std::floor(step + 0.000001)) + 1,
                index,
            });
        } else {
            mark_resolved();
        }
        const auto hit_time = entry.note.time_us;
        const auto display = entry.display_lane;
        const auto tolerance = milliseconds_to_microseconds(
            settings_.stacked_note_tolerance_ms
        ).value_or(0);
        if (resolve_stack) {
            for (std::size_t stacked = index + 1U;
                 stacked < window_.size();
                 ++stacked) {
                const auto& candidate = window_[stacked];
                if (candidate.note.time_us
                    > saturating_add_time(hit_time, tolerance)) {
                    break;
                }
                if (candidate.state == NoteState::pending
                    && candidate.note.owner == PackedNoteOwner::player
                    && candidate.display_lane == display
                    && candidate.note.time_us
                        >= saturating_add_time(hit_time, -tolerance)) {
                    if (!hit_window_note(stacked, adjusted_time_us, false)) {
                        return false;
                    }
                }
            }
        }
        emit_note({
            GameplayEventType::note_hit,
            entry.id,
            rating,
            song_time_us_,
            offset,
            0,
            1U,
        }, entry.note);
        return healthy_;
    }

    [[nodiscard]] bool stacked_with_recent_hit(
        const PackedNote& note,
        const std::uint16_t lane
    ) const noexcept {
        if (lane >= recent_hit_time_.size()
            || !recent_hit_time_[lane].has_value()) {
            return false;
        }
        const auto tolerance = milliseconds_to_microseconds(
            settings_.stacked_note_tolerance_ms
        ).value_or(0);
        const auto hit = *recent_hit_time_[lane];
        return note.time_us >= saturating_add_time(hit, -tolerance)
            && note.time_us <= saturating_add_time(hit, tolerance);
    }

    [[nodiscard]] Rating recent_rating(const std::uint16_t lane) const noexcept {
        return lane < recent_hit_rating_.size()
                && recent_hit_rating_[lane].has_value()
            ? *recent_hit_rating_[lane]
            : Rating::marvelous;
    }

    void miss_window_note(
        StreamingWindowNote& entry,
        const std::int64_t event_time_us,
        const bool hold_drop
    ) {
        if (terminal(entry.state)) {
            return;
        }
        entry.state = NoteState::missed;
        const auto logical_count =
            logical_note_count(player_note_multiplier_);
        const double logical_weight =
            static_cast<double>(logical_count);
        summary_.combo = 0U;
        saturating_add(summary_.misses, logical_count);
        if (hold_drop) {
            saturating_add(summary_.hold_drops, logical_count);
        } else {
            summary_.judged_notes += logical_weight;
        }
        // A missed physical note costs health once; only its logical weight is
        // multiplied for score/combo/statistics parity. Custom note kinds can
        // override head and sustain-drop damage without per-note allocations.
        summary_.health = std::clamp(
            summary_.health - miss_health_for(entry.note, hold_drop),
            0.0,
            2.0
        );
        mark_resolved();
        emit_note({
            hold_drop ? GameplayEventType::hold_drop : GameplayEventType::note_miss,
            entry.id,
            Rating::miss,
            event_time_us,
            saturating_subtract_time(
                saturating_add_time(
                    event_time_us,
                    milliseconds_to_microseconds(
                        settings_.input_offset_ms
                    ).value_or(0)
                ),
                entry.note.time_us
            ),
            0,
            1U,
        }, entry.note);
        check_failure(event_time_us);
    }

    void award_misses(
        const std::uint64_t requested_count,
        const StreamingNoteId first_id,
        const std::int64_t event_time_us,
        const PackedNote* visual_note = nullptr
    ) {
        auto count = requested_count;
        const double physical_damage = visual_note == nullptr
            ? settings_.health_loss
            : miss_health_for(*visual_note, false);
        if (!settings_.practice && !settings_.no_fail
            && physical_damage > 0.0 && summary_.health > 0.0) {
            const auto until_failure = static_cast<long double>(summary_.health)
                / static_cast<long double>(physical_damage);
            const auto rounded = static_cast<std::uint64_t>(std::min<long double>(
                std::ceil(until_failure),
                static_cast<long double>(std::numeric_limits<std::uint64_t>::max())
            ));
            count = std::min(count, std::max<std::uint64_t>(1U, rounded));
        }
        const auto logical_count = scaled_logical_count(
            count,
            player_note_multiplier_
        );
        summary_.combo = 0U;
        saturating_add(summary_.misses, logical_count);
        summary_.judged_notes += static_cast<double>(logical_count);
        summary_.health = std::clamp(
            summary_.health - finite_product(physical_damage, count),
            0.0,
            2.0
        );
        mark_resolved(count);
        StreamingGameplayEvent event{
            GameplayEventType::note_miss,
            first_id,
            Rating::miss,
            event_time_us,
            0,
            0,
            count,
        };
        if (visual_note != nullptr) {
            emit_note(std::move(event), *visual_note);
        } else {
            emit(std::move(event));
        }
        check_failure(event_time_us);
    }

    void award_hits(
        const std::uint64_t count,
        const StreamingNoteId first_id,
        const std::int64_t event_time_us,
        const Rating rating,
        const std::int64_t offset_us,
        const PackedNote* visual_note = nullptr
    ) {
        if (visual_note != nullptr && custom_hit_causes_miss(*visual_note)) {
            const auto logical_count = scaled_logical_count(
                count, player_note_multiplier_
            );
            add_score(scaled_score(
                rating_score(Rating::mine),
                static_cast<double>(logical_count)
            ));
            summary_.combo = 0U;
            saturating_add(summary_.misses, logical_count);
            summary_.judged_notes += static_cast<double>(logical_count);
            summary_.health = std::clamp(
                summary_.health - finite_product(
                    hit_causes_miss_health_for(*visual_note), count
                ),
                0.0,
                2.0
            );
            mark_resolved(count);
            emit_note({
                GameplayEventType::note_hit,
                first_id,
                Rating::mine,
                event_time_us,
                offset_us,
                0,
                count,
            }, *visual_note);
            check_failure(event_time_us);
            return;
        }
        const auto logical_count = scaled_logical_count(
            count,
            player_note_multiplier_
        );
        add_score(scaled_score(
            rating_score(rating),
            static_cast<double>(logical_count)
        ));
        saturating_add(summary_.combo, logical_count);
        summary_.max_combo = std::max(summary_.max_combo, summary_.combo);
        switch (rating) {
        case Rating::marvelous: saturating_add(summary_.marvelous, logical_count); break;
        case Rating::sick: saturating_add(summary_.sick, logical_count); break;
        case Rating::good: saturating_add(summary_.good, logical_count); break;
        case Rating::bad: saturating_add(summary_.bad, logical_count); break;
        case Rating::miss:
        case Rating::mine: break;
        }
        const double logical_weight =
            static_cast<double>(logical_count);
        summary_.weighted_hits += rating_weight(rating) * logical_weight;
        summary_.judged_notes += logical_weight;
        const double physical_hit_health = visual_note == nullptr
            ? settings_.health_gain
            : hit_health_for(*visual_note);
        summary_.health = std::clamp(
            summary_.health + physical_hit_health * logical_weight,
            0.0,
            2.0
        );
        mark_resolved(count);
        StreamingGameplayEvent event{
            GameplayEventType::note_hit,
            first_id,
            rating,
            event_time_us,
            offset_us,
            0,
            count,
        };
        if (visual_note != nullptr) {
            emit_note(std::move(event), *visual_note);
        } else {
            emit(std::move(event));
        }
    }

    void award_marvelous_hits(
        const std::uint64_t count,
        const StreamingNoteId first_id,
        const std::int64_t event_time_us,
        const PackedNote& visual_note
    ) {
        award_hits(
            count,
            first_id,
            event_time_us,
            Rating::marvelous,
            0,
            &visual_note
        );
    }

    // A dense explicit cluster can exceed max_window_notes before an autoplay
    // sustain reaches the temporal window. Keeping one ActiveHold per overflow
    // note would make memory scale with chart density. Resolve that sustain's
    // head and complete tick range arithmetically instead: final score, health,
    // tick count and completion stay identical to the buffered path while the
    // event stream remains bounded. Only event timing is accelerated for this
    // overflow-only path; ordinary sustains continue through ActiveHold.
    void award_unbuffered_autoplay_sustain(const SourceNote& item) {
        award_marvelous_hits(1U, item.id, item.note.time_us, item.note);
        if (custom_hit_causes_miss(item.note)) {
            return;
        }

        const auto end_us = note_end_us(item.note);
        const auto last_tick_time = saturating_add_time(end_us, -1);
        if (last_tick_time >= item.note.time_us) {
            const auto first_step_value = timing_.step_at(
                microseconds_to_milliseconds(item.note.time_us)
            );
            const auto last_step_value = timing_.step_at(
                microseconds_to_milliseconds(last_tick_time)
            );
            if (!std::isfinite(first_step_value)
                || !std::isfinite(last_step_value)
                || first_step_value > static_cast<double>(
                    std::numeric_limits<std::int64_t>::max())
                || first_step_value < static_cast<double>(
                    std::numeric_limits<std::int64_t>::min())
                || last_step_value > static_cast<double>(
                    std::numeric_limits<std::int64_t>::max())
                || last_step_value < static_cast<double>(
                    std::numeric_limits<std::int64_t>::min())) {
                throw SessionError("sustain musical-step index overflows");
            }
            const auto head_step = static_cast<std::int64_t>(
                std::floor(first_step_value + 0.000001)
            );
            if (head_step == std::numeric_limits<std::int64_t>::max()) {
                throw SessionError("sustain musical-step index overflows");
            }
            const auto first_step = head_step + 1;
            const auto last_step = static_cast<std::int64_t>(
                std::floor(last_step_value + 0.000001)
            );
            const auto count = inclusive_signed_count(first_step, last_step);
            if (count != 0U) {
                const auto logical_ticks = scaled_logical_count(
                    count,
                    player_note_multiplier_
                );
                saturating_add(summary_.hold_ticks, logical_ticks);
                const auto tick_score =
                    saturating_multiply(logical_ticks, 10U);
                add_score(tick_score > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())
                    ? std::numeric_limits<std::int64_t>::max()
                    : static_cast<std::int64_t>(tick_score));
                summary_.health = std::min(
                    2.0,
                    summary_.health + finite_product(
                        hold_hit_health_for(item.note) * 0.08,
                        logical_ticks
                    )
                );
                emit({
                    GameplayEventType::hold_tick,
                    item.id,
                    Rating::marvelous,
                    milliseconds_to_microseconds(
                        timing_.time_at_step(static_cast<double>(first_step))
                    ).value_or(item.note.time_us),
                    0,
                    first_step,
                    count,
                });
            }
        }
        emit_note({
            GameplayEventType::hold_complete,
            item.id,
            Rating::marvelous,
            end_us,
            0,
            0,
            1U,
        }, item.note);
    }

    void award_unbuffered_autoplay_pattern_sustains(
        const PatternRun& pattern,
        const PatternHeapNode first,
        const std::uint64_t count
    ) {
        if (count == 0U) {
            return;
        }
        const auto first_note = pattern.note_at(first.note_index);
        const auto last_note = pattern.note_at(first.note_index + count - 1U);
        if (!first_note.has_value() || !last_note.has_value()) {
            throw SessionError("PatternRun sustain arithmetic overflowed");
        }
        const StreamingNoteId first_id{
            StreamingNoteOrigin::pattern_run,
            first.note_index,
            first.pattern_index,
        };

        const auto ticks_for = [&](const PackedNote& current) {
            const auto head_step = static_cast<std::int64_t>(std::floor(
                timing_.step_at(microseconds_to_milliseconds(current.time_us))
                    + 0.000001
            ));
            const auto tail_step = static_cast<std::int64_t>(std::floor(
                timing_.step_at(microseconds_to_milliseconds(
                    saturating_add_time(note_end_us(current), -1)
                )) + 0.000001
            ));
            return inclusive_signed_count(head_step + 1, tail_step);
        };
        const auto first_ticks = ticks_for(*first_note);
        const auto last_ticks = ticks_for(*last_note);
        const auto first_bpm = timing_.bpm_at(
            microseconds_to_milliseconds(first_note->time_us)
        );
        const auto last_bpm = timing_.bpm_at(
            microseconds_to_milliseconds(note_end_us(*last_note))
        );
        const auto first_head_ms = microseconds_to_milliseconds(
            first_note->time_us
        );
        const auto last_tail_ms = microseconds_to_milliseconds(
            note_end_us(*last_note)
        );
        const auto& tempo_changes = timing_.changes();
        const bool internal_tempo_boundary = std::any_of(
            tempo_changes.begin(),
            tempo_changes.end(),
            [first_head_ms, last_tail_ms](const TempoChange& change) {
                return change.time_ms > first_head_ms
                    && change.time_ms <= last_tail_ms;
            }
        );
        const auto interval_steps = static_cast<long double>(pattern.interval_us)
            * static_cast<long double>(first_bpm)
            / (15'000'000.0L
                * static_cast<long double>(pattern.interval_denominator));
        const auto near_integer = [](const long double value) {
            return std::abs(value - std::round(value)) <= 1.0e-9L;
        };
        // Endpoint equality alone is insufficient: e.g. 120 BPM, 50 ms
        // interval and 100 ms duration produces 0,1,1,0,1,0 ticks. Constant
        // aggregation is exact only when phase cannot change the floor result.
        if (internal_tempo_boundary
            || std::abs(first_bpm - last_bpm) > 1.0e-9
            || first_ticks != last_ticks
            || !near_integer(interval_steps)) {
            throw SessionError(
                "PatternRun sustain phase is not uniformly aggregatable"
            );
        }
        // Commit heads only after every analytical precondition has passed.
        // A rejected phase must leave score, combo, health and resolved-note
        // counts unchanged instead of exposing a partially applied batch.
        award_marvelous_hits(count, first_id, first_note->time_us, *first_note);
        if (custom_hit_causes_miss(*first_note)) {
            return;
        }
        const auto total_ticks = saturating_multiply(count, first_ticks);
        if (total_ticks != 0U) {
            const auto logical_ticks = scaled_logical_count(
                total_ticks,
                player_note_multiplier_
            );
            saturating_add(summary_.hold_ticks, logical_ticks);
            const auto tick_score =
                saturating_multiply(logical_ticks, 10U);
            add_score(tick_score > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())
                ? std::numeric_limits<std::int64_t>::max()
                : static_cast<std::int64_t>(tick_score));
            summary_.health = std::min(
                2.0,
                summary_.health + finite_product(
                    hold_hit_health_for(*first_note) * 0.08,
                    logical_ticks
                )
            );
            emit_note({
                GameplayEventType::hold_tick,
                first_id,
                Rating::marvelous,
                first_note->time_us,
                0,
                0,
                total_ticks,
            }, *first_note);
        }
        emit_note({
            GameplayEventType::hold_complete,
            first_id,
            Rating::marvelous,
            note_end_us(*last_note),
            0,
            0,
            count,
        }, *first_note);
    }

    void check_failure(const std::int64_t event_time_us) {
        if (summary_.health > 0.0
            || settings_.practice
            || settings_.no_fail
            || failure_emitted_) {
            return;
        }
        summary_.failed = true;
        failure_emitted_ = true;
        emit({
            GameplayEventType::failed,
            no_streaming_note,
            Rating::miss,
            event_time_us,
            0,
            0,
            1U,
        });
    }

    void update_holds(const std::int64_t adjusted_time_us) {
        std::size_t index{};
        const auto grace = milliseconds_to_microseconds(
            settings_.release_grace_ms
        ).value_or(0);
        while (index < active_holds_.size()) {
            auto& hold = active_holds_[index];
            const auto end = note_end_us(hold.note);

            if (hold.note.owner == PackedNoteOwner::opponent) {
                // AI-owned sustain: retain only the visual/gameplay lifetime.
                // No player hold ticks, score, health, release or drop logic.
                if (adjusted_time_us >= end) {
                    set_window_state(hold, NoteState::completed);
                    mark_resolved();
                    active_holds_[index] = active_holds_.back();
                    active_holds_.pop_back();
                    continue;
                }
                ++index;
                continue;
            }

            if (!settings_.autoplay
                && (hold.display_lane >= held_lanes_.size()
                    || !held_lanes_[hold.display_lane])
                && saturating_add_time(adjusted_time_us, grace) < end) {
                drop_hold(hold, song_time_us_);
                active_holds_[index] = active_holds_.back();
                active_holds_.pop_back();
                if (summary_.failed) {
                    return;
                }
                continue;
            }
            advance_hold_ticks(hold, adjusted_time_us);
            if (saturating_add_time(adjusted_time_us, grace) >= end) {
                complete_hold(hold, adjusted_time_us);
                active_holds_[index] = active_holds_.back();
                active_holds_.pop_back();
                continue;
            }
            ++index;
        }
    }

    void advance_hold_ticks(
        ActiveHold& hold,
        const std::int64_t adjusted_time_us
    ) {
        const auto end_us = note_end_us(hold.note);
        const auto maximum_us = std::min(
            adjusted_time_us,
            saturating_add_time(end_us, -1)
        );
        if (maximum_us < hold.note.time_us) {
            return;
        }
        const auto step_value = timing_.step_at(
            microseconds_to_milliseconds(maximum_us)
        );
        if (!std::isfinite(step_value)
            || step_value > static_cast<double>(
                std::numeric_limits<std::int64_t>::max())
            || step_value < static_cast<double>(
                std::numeric_limits<std::int64_t>::min())) {
            throw SessionError("sustain musical-step index overflows");
        }
        const auto last_step = static_cast<std::int64_t>(
            std::floor(step_value + 0.000001)
        );
        if (last_step < hold.next_step) {
            return;
        }
        const auto count = inclusive_signed_count(hold.next_step, last_step);
        const auto logical_ticks = scaled_logical_count(
            count,
            player_note_multiplier_
        );
        saturating_add(summary_.hold_ticks, logical_ticks);
        const auto tick_score = saturating_multiply(logical_ticks, 10U);
        add_score(tick_score > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(tick_score));
        summary_.health = std::min(
            2.0,
            summary_.health + finite_product(
                hold_hit_health_for(hold.note) * 0.08,
                logical_ticks
            )
        );
        const auto first_step = hold.next_step;
        hold.next_step = last_step == std::numeric_limits<std::int64_t>::max()
            ? last_step
            : last_step + 1;
        emit({
            GameplayEventType::hold_tick,
            hold.id,
            Rating::marvelous,
            milliseconds_to_microseconds(
                timing_.time_at_step(static_cast<double>(first_step))
            ).value_or(adjusted_time_us),
            0,
            first_step,
            count,
        });
    }

    void complete_hold(
        const ActiveHold& hold,
        const std::int64_t adjusted_time_us
    ) {
        set_window_state(hold, NoteState::completed);
        mark_resolved();
        emit_note({
            GameplayEventType::hold_complete,
            hold.id,
            Rating::marvelous,
            song_time_us_,
            saturating_subtract_time(adjusted_time_us, note_end_us(hold.note)),
            0,
            1U,
        }, hold.note);
    }

    void drop_hold(const ActiveHold& hold, const std::int64_t event_time_us) {
        set_window_state(hold, NoteState::missed);
        summary_.combo = 0U;
        const auto logical_count =
            logical_note_count(player_note_multiplier_);
        saturating_add(summary_.misses, logical_count);
        saturating_add(summary_.hold_drops, logical_count);
        summary_.health = std::max(
            0.0,
            summary_.health - miss_health_for(hold.note, true)
        );
        mark_resolved();
        emit_note({
            GameplayEventType::hold_drop,
            hold.id,
            Rating::miss,
            event_time_us,
            saturating_subtract_time(event_time_us, hold.note.time_us),
            0,
            1U,
        }, hold.note);
        check_failure(event_time_us);
    }

    void set_window_state(
        const ActiveHold& hold,
        const NoteState state
    ) noexcept {
        if (hold.window_index < window_.size()) {
            auto& entry = window_[hold.window_index];
            if (entry.id == hold.id) {
                entry.state = state;
                return;
            }
        }

        // Defensive fallback only after an unexpected handle mismatch. Normal
        // prefix retirement and deep compaction maintain hold.window_index.
        for (std::size_t index = 0U; index < window_.size(); ++index) {
            auto& entry = window_[index];
            if (entry.id == hold.id) {
                entry.state = state;
                return;
            }
        }
    }

    void update_musical_clock(const std::int64_t time_us) {
        if (time_us < 0) {
            return;
        }
        const auto time_ms = microseconds_to_milliseconds(time_us);
        const auto step_value = timing_.step_at(time_ms);
        const auto beat_value = timing_.beat_at(time_ms);
        if (!std::isfinite(step_value) || !std::isfinite(beat_value)
            || step_value > static_cast<double>(
                std::numeric_limits<std::int64_t>::max())
            || beat_value > static_cast<double>(
                std::numeric_limits<std::int64_t>::max())) {
            throw SessionError("musical callback index overflows");
        }
        const auto step = static_cast<std::int64_t>(
            std::floor(step_value + 0.000001)
        );
        const auto beat = static_cast<std::int64_t>(
            std::floor(beat_value + 0.000001)
        );
        if (step > last_step_) {
            const auto first = last_step_ + 1;
            const auto count = static_cast<std::uint64_t>(step)
                - static_cast<std::uint64_t>(first) + 1U;
            emit({
                GameplayEventType::step,
                no_streaming_note,
                Rating::marvelous,
                milliseconds_to_microseconds(
                    timing_.time_at_step(static_cast<double>(first))
                ).value_or(time_us),
                0,
                first,
                count,
            });
            last_step_ = step;
        }
        if (beat > last_beat_) {
            const auto first = last_beat_ + 1;
            const auto count = static_cast<std::uint64_t>(beat)
                - static_cast<std::uint64_t>(first) + 1U;
            emit({
                GameplayEventType::beat,
                no_streaming_note,
                Rating::marvelous,
                milliseconds_to_microseconds(
                    timing_.time_at_beat(static_cast<double>(first))
                ).value_or(time_us),
                0,
                first,
                count,
            });
            last_beat_ = beat;
        }
    }

    void update_completion(const std::int64_t time_us) {
        if (completion_emitted_ || summary_.failed) {
            return;
        }
        const auto source_exhausted = explicit_cursor_.exhausted()
            && pattern_heap_.empty()
            && chart_event_cursor_ >= chart_events_.size();
        const auto window_drained = std::all_of(
            window_.begin(),
            window_.end(),
            [](const StreamingWindowNote& note) { return terminal(note.state); }
        );
        if (!source_exhausted || !window_drained || !active_holds_.empty()) {
            return;
        }
        if (time_us < saturating_add_time(
                content_end_seen_us_,
                options_.completion_tail_us
            )) {
            return;
        }
        complete_ = true;
        completion_emitted_ = true;
        emit({
            GameplayEventType::song_complete,
            no_streaming_note,
            Rating::marvelous,
            time_us,
            0,
            0,
            1U,
        });
    }

    const PackedChartReader* reader_{};
    TimingMap timing_;
    GameplaySettings settings_;
    StreamingGameplayOptions options_;
    ExplicitCursor explicit_cursor_;
    ScoreSummary summary_;
    std::vector<ChartEvent> chart_events_;
    std::size_t chart_event_cursor_{};
    double player_note_multiplier_{1.0};
    double opponent_note_multiplier_{1.0};
    std::uint16_t player_key_count_{4U};
    std::uint16_t opponent_key_count_{4U};
    SlidingContiguousBuffer<StreamingWindowNote> window_;
    std::uint64_t retired_window_notes_{};
    std::uint64_t deep_compacted_window_notes_{};
    std::uint64_t pressure_compactions_{};
    std::size_t due_cursor_{};
    std::size_t miss_cursor_{};
    std::vector<std::size_t> lane_candidate_cursor_;
    bool due_cursor_autoplay_mode_{};
    std::vector<ActiveHold> active_holds_;
    std::vector<std::uint64_t> pattern_next_;
    std::vector<PatternHeapNode> pattern_heap_;
    std::vector<bool> mine_kinds_;
    // PULSEFORGE_P1_5_0_STREAMING_NOTE_KIND_RUNTIME_BEHAVIOR_V1
    std::map<std::uint32_t, NoteKindRuntimeBehavior> note_kind_behaviors_;
    std::vector<BulkLaneAccumulator> bulk_player_lanes_;
    std::vector<BulkLaneAccumulator> bulk_opponent_lanes_;
    std::vector<ExplicitChunkSummary> chunk_summaries_;
    StreamingGameplayFrameStats frame_stats_{};
    StreamingGameplayAccelerationStats acceleration_stats_{};
    std::vector<bool> held_lanes_;
    std::vector<std::uint16_t> player_lane_map_;
    std::vector<std::uint16_t> opponent_lane_map_;
    std::vector<std::optional<std::int64_t>> recent_hit_time_;
    std::vector<std::optional<Rating>> recent_hit_rating_;
    std::vector<StreamingGameplayEvent> frame_events_;
    std::vector<InputRecord> recorded_inputs_;
    std::string error_;
    std::uint64_t dropped_frame_events_{};
    std::uint64_t total_resolved_notes_{};
    std::int64_t song_time_us_{};
    std::int64_t content_end_seen_us_{};
    std::int64_t last_step_{-1};
    std::int64_t last_beat_{-1};
    bool healthy_{true};
    bool catchup_pending_{};
    bool window_saturated_{};
    bool input_recording_overflowed_{};
    bool complete_{};
    bool completion_emitted_{};
    bool failure_emitted_{};
};

StreamingGameplaySession::~StreamingGameplaySession() = default;
StreamingGameplaySession::StreamingGameplaySession(
    StreamingGameplaySession&&
) noexcept = default;
StreamingGameplaySession& StreamingGameplaySession::operator=(
    StreamingGameplaySession&&
) noexcept = default;

StreamingGameplaySession::StreamingGameplaySession(
    std::unique_ptr<Impl> implementation
) : implementation_(std::move(implementation)) {}

std::optional<StreamingGameplaySession> StreamingGameplaySession::create(
    const PackedChartReader& reader,
    GameplaySettings settings,
    StreamingGameplayOptions options,
    std::vector<TempoChange> tempos,
    std::string* const error
) {
    return create(
        reader,
        std::move(settings),
        options,
        std::move(tempos),
        {},
        error
    );
}

std::optional<StreamingGameplaySession> StreamingGameplaySession::create(
    const PackedChartReader& reader,
    GameplaySettings settings,
    StreamingGameplayOptions options,
    std::vector<TempoChange> tempos,
    std::vector<ChartEvent> events,
    std::string* const error
) {
    if (error != nullptr) {
        error->clear();
    }
    try {
        auto implementation = std::make_unique<Impl>(
            reader,
            std::move(settings),
            options,
            std::move(tempos),
            std::move(events)
        );
        implementation->initialize();
        return StreamingGameplaySession(std::move(implementation));
    } catch (const std::bad_alloc&) {
        if (error != nullptr) {
            *error = "cannot create streaming gameplay session: insufficient "
                "memory for configured streaming state";
        }
        return std::nullopt;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("cannot create streaming gameplay session: ")
                + exception.what();
        }
        return std::nullopt;
    } catch (...) {
        if (error != nullptr) {
            *error = "cannot create streaming gameplay session";
        }
        return std::nullopt;
    }
}

bool StreamingGameplaySession::reset(std::string* const error) {
    return implementation_->reset(error);
}

void StreamingGameplaySession::begin_frame() noexcept {
    implementation_->begin_frame();
}

bool StreamingGameplaySession::update(const double song_time_ms) {
    return implementation_->update(song_time_ms);
}

bool StreamingGameplaySession::finish_song(const double media_end_time_ms) {
    return implementation_->finish_song(media_end_time_ms);
}

bool StreamingGameplaySession::press(
    const std::uint16_t lane,
    const double song_time_ms
) {
    return implementation_->press(lane, song_time_ms);
}

bool StreamingGameplaySession::release(
    const std::uint16_t lane,
    const double song_time_ms
) {
    return implementation_->release(lane, song_time_ms);
}

void StreamingGameplaySession::add_score(const std::int64_t amount) noexcept {
    implementation_->add_score(amount);
}

void StreamingGameplaySession::set_health(const double health) {
    implementation_->set_health(health);
}

bool StreamingGameplaySession::apply_event(
    const std::string_view name,
    const std::string_view value1,
    const std::string_view value2
) noexcept {
    return implementation_->apply_event(name, value1, value2);
}

bool StreamingGameplaySession::set_note_kind_behavior(
    const std::string_view kind,
    const NoteKindRuntimeBehavior& behavior
) {
    return implementation_->set_note_kind_behavior(kind, behavior);
}

const NoteKindRuntimeBehavior* StreamingGameplaySession::note_kind_behavior(
    const std::string_view kind
) const noexcept {
    return implementation_->note_kind_behavior(kind);
}

const PackedChartReader& StreamingGameplaySession::reader() const noexcept {
    return implementation_->reader();
}

const TimingMap& StreamingGameplaySession::timing_map() const noexcept {
    return implementation_->timing();
}

const GameplaySettings& StreamingGameplaySession::settings() const noexcept {
    return implementation_->settings();
}

GameplaySettings& StreamingGameplaySession::settings() noexcept {
    return implementation_->settings();
}

const StreamingGameplayOptions& StreamingGameplaySession::options() const noexcept {
    return implementation_->options();
}

const ScoreSummary& StreamingGameplaySession::summary() const noexcept {
    return implementation_->summary();
}

std::span<const ChartEvent>
StreamingGameplaySession::chart_events() const noexcept {
    return implementation_->chart_events();
}

double StreamingGameplaySession::player_note_multiplier() const noexcept {
    return implementation_->player_note_multiplier();
}

double StreamingGameplaySession::opponent_note_multiplier() const noexcept {
    return implementation_->opponent_note_multiplier();
}

std::uint16_t StreamingGameplaySession::player_key_count() const noexcept {
    return implementation_->player_key_count();
}

std::uint16_t StreamingGameplaySession::opponent_key_count() const noexcept {
    return implementation_->opponent_key_count();
}

std::span<const StreamingWindowNote>
StreamingGameplaySession::window_notes() const noexcept {
    return implementation_->window();
}

std::span<const StreamingGameplayEvent>
StreamingGameplaySession::frame_events() const noexcept {
    return implementation_->events();
}

std::span<const InputRecord>
StreamingGameplaySession::recorded_inputs() const noexcept {
    return implementation_->inputs();
}

std::uint64_t StreamingGameplaySession::dropped_frame_events() const noexcept {
    return implementation_->dropped_events();
}

std::uint64_t StreamingGameplaySession::total_resolved_notes() const noexcept {
    return implementation_->resolved();
}

bool StreamingGameplaySession::input_recording_overflowed() const noexcept {
    return implementation_->input_overflow();
}

bool StreamingGameplaySession::lane_held(const std::uint16_t lane) const noexcept {
    return implementation_->lane_held(lane);
}

std::uint16_t StreamingGameplaySession::display_lane(
    const std::uint16_t source_lane
) const noexcept {
    return implementation_->public_display_lane(source_lane);
}

std::uint16_t StreamingGameplaySession::display_lane(
    const NoteOwner owner,
    const std::uint16_t source_lane
) const noexcept {
    return implementation_->public_display_lane(owner, source_lane);
}

bool StreamingGameplaySession::complete() const noexcept {
    return implementation_->complete();
}

bool StreamingGameplaySession::healthy() const noexcept {
    return implementation_->healthy();
}

bool StreamingGameplaySession::catchup_pending() const noexcept {
    return implementation_->catchup();
}

bool StreamingGameplaySession::window_saturated() const noexcept {
    return implementation_->saturated();
}

double StreamingGameplaySession::song_time_ms() const noexcept {
    return implementation_->song_time_ms();
}

std::string_view StreamingGameplaySession::error() const noexcept {
    return implementation_->error();
}

StreamingGameplayMemoryStats
StreamingGameplaySession::memory_stats() const noexcept {
    return implementation_->memory_stats();
}

StreamingGameplayFrameStats
StreamingGameplaySession::frame_stats() const noexcept {
    return implementation_->frame_stats();
}

StreamingGameplayAccelerationStats
StreamingGameplaySession::acceleration_stats() const noexcept {
    return implementation_->acceleration_stats();
}

}  // namespace pulseforge
