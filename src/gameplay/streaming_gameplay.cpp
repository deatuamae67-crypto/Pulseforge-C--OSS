#include "pulseforge/streaming_gameplay.hpp"
#include "pulseforge/note_types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
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
        const std::uint32_t maximum_chunk_notes
    ) : reader_(&reader), maximum_chunk_notes_(maximum_chunk_notes) {}

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
        if (info->note_count > maximum_chunk_notes_) {
            throw SessionError(
                "PFC1 chunk exceeds StreamingGameplaySession memory policy"
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
    const auto difference = static_cast<std::uint64_t>(exclusive_time)
        - static_cast<std::uint64_t>(pattern.start_us) - 1U;
    const auto by_time = difference / pattern.interval_us;
    return std::min(pattern.count - 1U, by_time);
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
        if constexpr (sizeof(std::size_t) >= sizeof(std::uint64_t)) {
            return static_cast<std::size_t>(value);
        }
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

}  // namespace

class StreamingGameplaySession::Impl final {
public:
    Impl(
        const PackedChartReader& reader,
        GameplaySettings settings,
        StreamingGameplayOptions options,
        std::vector<TempoChange> tempos
    ) : reader_(&reader),
        timing_(std::move(tempos)),
        settings_(std::move(settings)),
        options_(options),
        explicit_cursor_(reader, options.max_explicit_chunk_notes) {
        sanitize_gameplay_settings(settings_);
    }

    void initialize() {
        validate_options();
        lane_map_.resize(reader_->key_count());
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
        window_.reserve(options_.max_window_notes);
        active_holds_.reserve(options_.max_active_holds);
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

            std::iota(lane_map_.begin(), lane_map_.end(), std::uint16_t{0U});
            if (settings_.mirror) {
                std::reverse(lane_map_.begin(), lane_map_.end());
            }
            if (settings_.randomize_lanes) {
                deterministic_shuffle(lane_map_, settings_.random_seed);
            }

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
        } catch (const std::exception& exception) {
            return fail(exception.what(), error);
        } catch (...) {
            return fail("streaming gameplay reset failed", error);
        }
    }

    void begin_frame() noexcept {
        frame_events_.clear();
        dropped_frame_events_ = 0U;
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
            song_time_us_ = time_us;
            return advance_runtime(true);
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
            if (!healthy_ || summary_.failed || complete_
                || lane >= reader_->key_count()) {
                return healthy_;
            }
            const auto requested = milliseconds_to_microseconds(song_time_ms);
            if (!requested.has_value() || *requested < minimum_song_time_us) {
                return true;
            }
            if (*requested > song_time_us_) {
                song_time_us_ = *requested;
                if (!advance_runtime(false)) {
                    return false;
                }
            }
            if (catchup_pending_) {
                return true;
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
            if (!healthy_ || summary_.failed || complete_
                || lane >= reader_->key_count()) {
                return healthy_;
            }
            const auto requested = milliseconds_to_microseconds(song_time_ms);
            if (!requested.has_value() || *requested < minimum_song_time_us) {
                return true;
            }
            if (*requested > song_time_us_) {
                song_time_us_ = *requested;
                if (!advance_runtime(false)) {
                    return false;
                }
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
                if (hold.display_lane != lane) {
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

    [[nodiscard]] const PackedChartReader& reader() const noexcept {
        return *reader_;
    }
    [[nodiscard]] const TimingMap& timing() const noexcept { return timing_; }
    [[nodiscard]] const GameplaySettings& settings() const noexcept { return settings_; }
    [[nodiscard]] GameplaySettings& settings() noexcept { return settings_; }
    [[nodiscard]] const StreamingGameplayOptions& options() const noexcept { return options_; }
    [[nodiscard]] const ScoreSummary& summary() const noexcept { return summary_; }
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
        return display_lane(lane);
    }
    [[nodiscard]] bool complete() const noexcept { return complete_; }
    [[nodiscard]] bool healthy() const noexcept { return healthy_; }
    [[nodiscard]] bool catchup() const noexcept { return catchup_pending_; }
    [[nodiscard]] bool saturated() const noexcept { return window_saturated_; }
    [[nodiscard]] double song_time_ms() const noexcept {
        return microseconds_to_milliseconds(song_time_us_);
    }
    [[nodiscard]] std::string_view error() const noexcept { return error_; }

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

    struct SourceChoice final {
        enum class Kind : std::uint8_t { none, explicit_note, pattern } kind{Kind::none};
        SourceNote explicit_note;
        PatternHeapNode pattern;
        std::int64_t time_us{};
    };

    void validate_options() const {
        if (reader_->key_count() == 0U
            || options_.look_ahead_us < 0
            || options_.terminal_retention_us < 0
            || options_.completion_tail_us < 0
            || options_.maximum_backward_jitter_us < 0
            || options_.max_window_notes == 0U
            || options_.max_active_holds == 0U
            || options_.max_events_per_frame == 0U
            || options_.max_explicit_chunk_notes == 0U
            || options_.max_explicit_catchup_notes_per_update == 0U) {
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
        const std::uint16_t source_lane
    ) const noexcept {
        return source_lane < lane_map_.size()
            ? lane_map_[source_lane]
            : source_lane;
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
        event.visual_display_lane = display_lane(note.lane);
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

    // Consume a sequential prefix directly from the decoded PFC1 chunk without
    // creating StreamingWindowNote objects. This is safe for opponent taps and
    // autoplay tap heads: their result is deterministic and occurrence_count
    // already exists specifically for dense bounded event aggregation.
    [[nodiscard]] bool consume_bulk_explicit_due_taps(
        const std::int64_t inclusive_time_us,
        const std::int64_t event_time_us
    ) {
        const auto notes = explicit_cursor_.remaining_chunk();
        if (notes.empty()
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
                display_lane(note.lane)
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
                && !settings_.autoplay)) {
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
            display_lane(first_note->lane)
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
        constexpr std::uint32_t maximum_bulk_explicit_chunks_per_fill = 2U;
        while (window_.size() < options_.max_window_notes) {
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
                display_lane(item.note.lane),
            });
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
                entry.state = NoteState::completed;
                if (lane < bulk_opponent_lanes_.size()) {
                    add_bulk_lane(
                        bulk_opponent_lanes_[lane],
                        entry.id,
                        entry.note
                    );
                } else {
                    mark_resolved();
                }
                continue;
            }

            if (!settings_.autoplay) {
                continue;
            }

            if (is_mine(entry.note)) {
                entry.state = NoteState::ignored;
                saturating_add(ignored_mines, 1U);
                continue;
            }

            if (entry.note.duration_us == 0U) {
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
        if (is_mine(entry.note)) {
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
        if (is_mine(item.note)) {
            mark_resolved();
            return;
        }
        const auto lane = display_lane(item.note.lane);
        if (stacked_with_recent_hit(item.note, lane) || settings_.autoplay) {
            if (settings_.autoplay && item.note.duration_us != 0U) {
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
            } else if (is_mine(*first_note)) {
                mark_resolved(due);
            } else {
                const auto lane = display_lane(first_note->lane);
                if (stacked_with_recent_hit(*first_note, lane)
                    || settings_.autoplay) {
                    if (settings_.autoplay && pattern.duration_us != 0U) {
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
            complete_hold(hold, song_time_us_);
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
                entry.state = NoteState::completed;
                mark_resolved();
                emit_note({
                    GameplayEventType::opponent_hit,
                    entry.id,
                    Rating::marvelous,
                    time_us,
                    saturating_subtract_time(time_us, entry.note.time_us),
                    0,
                    1U,
                }, entry.note);
            } else if (is_mine(entry.note)) {
                entry.state = NoteState::ignored;
                mark_resolved();
            } else if (stacked_with_recent_hit(entry.note, entry.display_lane)
                && entry.note.duration_us == 0U) {
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
        constexpr std::uint32_t maximum_bulk_explicit_chunks = 2U;
        while (true) {
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
        if (is_mine(item.note)) {
            mark_resolved();
            return;
        }
        const auto lane = display_lane(item.note.lane);
        if (stacked_with_recent_hit(item.note, lane)
            && item.note.duration_us == 0U) {
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
            if (item.note.duration_us != 0U) {
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
        if (is_mine(*first_note)) {
            mark_resolved(count);
            return;
        }
        const auto lane = display_lane(first_note->lane);
        if (pattern.duration_us == 0U
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
            if (pattern.duration_us != 0U) {
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
            && candidate.note.duration_us == 0U
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
        if (is_mine(entry.note)) {
            entry.state = NoteState::completed;
            add_score(rating_score(Rating::mine));
            summary_.combo = 0U;
            saturating_add(summary_.misses, 1U);
            summary_.judged_notes += 1.0;
            summary_.health = std::max(
                0.0,
                summary_.health - settings_.health_loss * 1.5
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
            && entry.note.duration_us == 0U
            && !is_mine(entry.note)) {
            return hit_exact_tap_stack(
                index,
                adjusted_time_us,
                rating,
                offset
            );
        }
        entry.state = entry.note.duration_us == 0U
            ? NoteState::completed
            : NoteState::holding;
        add_score(rating_score(rating));
        saturating_add(summary_.combo, 1U);
        summary_.max_combo = std::max(summary_.max_combo, summary_.combo);
        summary_.weighted_hits += rating_weight(rating);
        summary_.judged_notes += 1.0;
        summary_.health = std::min(2.0, summary_.health + settings_.health_gain);
        switch (rating) {
        case Rating::marvelous: saturating_add(summary_.marvelous, 1U); break;
        case Rating::sick: saturating_add(summary_.sick, 1U); break;
        case Rating::good: saturating_add(summary_.good, 1U); break;
        case Rating::bad: saturating_add(summary_.bad, 1U); break;
        case Rating::miss:
        case Rating::mine: break;
        }
        recent_hit_time_[entry.display_lane] = entry.note.time_us;
        recent_hit_rating_[entry.display_lane] = rating;

        if (entry.state == NoteState::holding) {
            if (active_holds_.size() == options_.max_active_holds) {
                return set_runtime_error(
                    "active sustain count exceeds StreamingGameplaySession limit"
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
        summary_.combo = 0U;
        saturating_add(summary_.misses, 1U);
        if (hold_drop) {
            saturating_add(summary_.hold_drops, 1U);
        } else {
            summary_.judged_notes += 1.0;
        }
        summary_.health = std::max(0.0, summary_.health - settings_.health_loss);
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
        if (!settings_.practice && !settings_.no_fail
            && settings_.health_loss > 0.0 && summary_.health > 0.0) {
            const auto until_failure = static_cast<long double>(summary_.health)
                / static_cast<long double>(settings_.health_loss);
            const auto rounded = static_cast<std::uint64_t>(std::min<long double>(
                std::ceil(until_failure),
                static_cast<long double>(std::numeric_limits<std::uint64_t>::max())
            ));
            count = std::min(count, std::max<std::uint64_t>(1U, rounded));
        }
        summary_.combo = 0U;
        saturating_add(summary_.misses, count);
        summary_.judged_notes += static_cast<double>(count);
        summary_.health = std::max(
            0.0,
            summary_.health - finite_product(settings_.health_loss, count)
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
        const auto score_per_note = static_cast<std::uint64_t>(
            std::max(0, rating_score(rating))
        );
        const auto score_amount = saturating_multiply(count, score_per_note);
        add_score(score_amount > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(score_amount));
        saturating_add(summary_.combo, count);
        summary_.max_combo = std::max(summary_.max_combo, summary_.combo);
        switch (rating) {
        case Rating::marvelous: saturating_add(summary_.marvelous, count); break;
        case Rating::sick: saturating_add(summary_.sick, count); break;
        case Rating::good: saturating_add(summary_.good, count); break;
        case Rating::bad: saturating_add(summary_.bad, count); break;
        case Rating::miss:
        case Rating::mine: break;
        }
        summary_.weighted_hits += finite_product(rating_weight(rating), count);
        summary_.judged_notes += static_cast<double>(count);
        summary_.health = std::min(
            2.0,
            summary_.health + finite_product(settings_.health_gain, count)
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
                saturating_add(summary_.hold_ticks, count);
                const auto tick_score = saturating_multiply(count, 10U);
                add_score(tick_score > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())
                    ? std::numeric_limits<std::int64_t>::max()
                    : static_cast<std::int64_t>(tick_score));
                summary_.health = std::min(
                    2.0,
                    summary_.health
                        + finite_product(settings_.health_gain * 0.08, count)
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
            * static_cast<long double>(first_bpm) / 15'000'000.0L;
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
        const auto total_ticks = saturating_multiply(count, first_ticks);
        if (total_ticks != 0U) {
            saturating_add(summary_.hold_ticks, total_ticks);
            const auto tick_score = saturating_multiply(total_ticks, 10U);
            add_score(tick_score > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())
                ? std::numeric_limits<std::int64_t>::max()
                : static_cast<std::int64_t>(tick_score));
            summary_.health = std::min(
                2.0,
                summary_.health + finite_product(
                    settings_.health_gain * 0.08,
                    total_ticks
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
        saturating_add(summary_.hold_ticks, count);
        const auto tick_score = saturating_multiply(count, 10U);
        add_score(tick_score > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(tick_score));
        summary_.health = std::min(
            2.0,
            summary_.health
                + finite_product(settings_.health_gain * 0.08, count)
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
        saturating_add(summary_.misses, 1U);
        saturating_add(summary_.hold_drops, 1U);
        summary_.health = std::max(0.0, summary_.health - settings_.health_loss);
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
            && pattern_heap_.empty();
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
    std::vector<BulkLaneAccumulator> bulk_player_lanes_;
    std::vector<BulkLaneAccumulator> bulk_opponent_lanes_;
    std::vector<bool> held_lanes_;
    std::vector<std::uint16_t> lane_map_;
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
    if (error != nullptr) {
        error->clear();
    }
    try {
        auto implementation = std::make_unique<Impl>(
            reader,
            std::move(settings),
            options,
            std::move(tempos)
        );
        implementation->initialize();
        return StreamingGameplaySession(std::move(implementation));
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

}  // namespace pulseforge
