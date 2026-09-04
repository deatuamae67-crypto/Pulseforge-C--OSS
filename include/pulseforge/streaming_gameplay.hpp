#pragma once

#include "pulseforge/gameplay.hpp"
#include "pulseforge/packed_chart.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge {

enum class StreamingNoteOrigin : std::uint8_t {
    explicit_note,
    pattern_run,
};

struct StreamingNoteId {
    StreamingNoteOrigin origin{StreamingNoteOrigin::explicit_note};
    // Explicit: global PFC1 note index. Pattern: index inside PatternRun.
    std::uint64_t note_index{};
    // Zero for explicit notes; PatternRun index for procedural notes.
    std::uint64_t pattern_index{};

    [[nodiscard]] bool operator==(const StreamingNoteId&) const noexcept = default;
};

struct StreamingWindowNote {
    StreamingNoteId id;
    PackedNote note;
    NoteState state{NoteState::pending};
    std::uint16_t display_lane{};
};

// occurrence_count is greater than one when a dense PatternRun, an exact
// coincident tap stack, a bulk-resolved explicit lane batch, or a large
// musical-clock jump is represented as one bounded event.
struct StreamingGameplayEvent {
    GameplayEventType type{};
    StreamingNoteId note_id;
    Rating rating{Rating::miss};
    std::int64_t song_time_us{};
    std::int64_t offset_us{};
    std::int64_t musical_index{};
    std::uint64_t occurrence_count{1U};
    std::size_t chart_event_index{std::numeric_limits<std::size_t>::max()};
    // A bounded copy of the first logical note represented by this event.
    // Rendering/UI consumers must not search the mutable gameplay window after
    // update(), because terminal notes may already have been compacted.
    PackedNote visual_note{};
    std::uint16_t visual_display_lane{};
    bool has_visual_note{};
    // PULSEFORGE_P1_1_18_STREAMING_LOGICAL_EVENT_WEIGHT_V1
    // occurrence_count remains the physical/source count used by PatternRun
    // compaction. This separate field carries Note Multiplier polyphony.
    std::uint64_t logical_occurrence_count{1U};
};

struct StreamingGameplayOptions {
    std::int64_t look_ahead_us{2'000'000};
    std::int64_t terminal_retention_us{250'000};
    std::int64_t completion_tail_us{2'000'000};
    std::int64_t maximum_backward_jitter_us{5'000};
    std::size_t max_window_notes{65'536U};
    // Zero means allocator-limited. A non-zero value remains available as an
    // explicit untrusted-input/memory policy; it is no longer an arbitrary
    // default chart-compatibility ceiling.
    std::size_t max_active_holds{};
    std::size_t max_events_per_frame{4'096U};
    std::size_t max_recorded_inputs{500'000U};
    // Zero disables the historical per-chunk note-count policy. Memory remains
    // bounded by max_explicit_chunk_decoded_bytes because ExplicitCursor
    // currently materializes one decoded PFC1 chunk at a time.
    std::uint32_t max_explicit_chunk_notes{};
    std::size_t max_explicit_chunk_decoded_bytes{256U * 1024U * 1024U};
    std::uint64_t max_explicit_catchup_notes_per_update{1'000'000U};
    // Offline rendering is deterministic BOTPLAY. Enabling this mode lets
    // the scheduler precompute a compact per-PFC-chunk tap summary and
    // consume whole fully-due chunks arithmetically instead of building a
    // per-note backlog. Interactive/manual gameplay keeps the old bounded
    // work policy.
    bool fast_offline_autoplay{false};
    std::uint32_t max_fast_bulk_chunks_per_update{4'096U};
    std::uint64_t max_summary_chunks{2'000'000U};
};


struct StreamingGameplayFrameStats {
    std::uint64_t summarized_explicit_notes{};
    std::uint64_t summarized_explicit_chunks{};
    std::uint64_t decoded_bulk_explicit_notes{};
    std::uint64_t decoded_bulk_explicit_batches{};
    std::uint64_t scalar_explicit_notes{};
    std::size_t peak_window_notes{};
    bool catchup_pending{};
    bool window_saturated{};
};

struct StreamingGameplayAccelerationStats {
    bool fast_offline_autoplay{};
    bool chunk_summary_ready{};
    bool chunk_summary_reused{};
    std::uint64_t chunk_summary_chunks{};
    std::uint64_t chunk_summary_notes_scanned{};
    std::uint64_t chunk_summary_prepare_ns{};
};

struct StreamingGameplayMemoryStats {
    std::size_t window_notes{};
    std::size_t window_capacity{};
    std::size_t active_holds{};
    std::size_t active_hold_capacity{};
    std::size_t cached_explicit_notes{};
    std::size_t pattern_cursors{};
    std::size_t pattern_heap_capacity{};
    std::size_t frame_event_capacity{};
    std::size_t recorded_input_capacity{};
    std::uint64_t approximate_dynamic_bytes{};
};

// Gameplay scheduler for PFC1. The reader must outlive the session.
//
// Semantics intentionally differ from GameplaySession in four bounded-scale
// cases:
//  * PFC1 has no tempo/event metadata, so callers supply tempo changes and a
//    bounded PFE1-backed chart-event timeline.
//  * Dense unbuffered PatternRun events are aggregated through
//    occurrence_count instead of emitting one callback per logical note.
//  * Explicit-note catch-up is work-budgeted across updates; catchup_pending()
//    reports when input should be paused until the sequential scan catches up.
//  * Autoplay sustain heads cannot be aggregated without losing tempo-aware
//    tick semantics. If they overflow the window, the session fails explicitly
//    and asks for a larger window instead of silently changing the score.
class StreamingGameplaySession final {
public:
    ~StreamingGameplaySession();
    StreamingGameplaySession(StreamingGameplaySession&&) noexcept;
    StreamingGameplaySession& operator=(StreamingGameplaySession&&) noexcept;

    StreamingGameplaySession(const StreamingGameplaySession&) = delete;
    StreamingGameplaySession& operator=(const StreamingGameplaySession&) = delete;

    [[nodiscard]] static std::optional<StreamingGameplaySession> create(
        const PackedChartReader& reader,
        GameplaySettings settings = {},
        StreamingGameplayOptions options = {},
        std::vector<TempoChange> tempos = {},
        std::string* error = nullptr
    );

    [[nodiscard]] static std::optional<StreamingGameplaySession> create(
        const PackedChartReader& reader,
        GameplaySettings settings,
        StreamingGameplayOptions options,
        std::vector<TempoChange> tempos,
        std::vector<ChartEvent> events,
        std::string* error
    );

    [[nodiscard]] bool reset(std::string* error = nullptr);
    void begin_frame() noexcept;
    [[nodiscard]] bool update(double song_time_ms);
    // The host calls this only after its authoritative AudioTransport reaches
    // ended. It performs bounded catch-up and completes once the PFC sources,
    // judgment window and active holds are drained.
    [[nodiscard]] bool finish_song(double media_end_time_ms);
    [[nodiscard]] bool press(std::uint16_t lane, double song_time_ms);
    [[nodiscard]] bool release(std::uint16_t lane, double song_time_ms);
    void add_score(std::int64_t amount) noexcept;
    void set_health(double health);
    [[nodiscard]] bool apply_event(
        std::string_view name,
        std::string_view value1 = {},
        std::string_view value2 = {}
    ) noexcept;
    [[nodiscard]] bool set_note_kind_behavior(
        std::string_view kind,
        const NoteKindRuntimeBehavior& behavior
    );
    [[nodiscard]] const NoteKindRuntimeBehavior* note_kind_behavior(
        std::string_view kind
    ) const noexcept;

    [[nodiscard]] const PackedChartReader& reader() const noexcept;
    [[nodiscard]] const TimingMap& timing_map() const noexcept;
    [[nodiscard]] const GameplaySettings& settings() const noexcept;
    [[nodiscard]] GameplaySettings& settings() noexcept;
    [[nodiscard]] const StreamingGameplayOptions& options() const noexcept;
    [[nodiscard]] const ScoreSummary& summary() const noexcept;
    [[nodiscard]] std::span<const ChartEvent> chart_events() const noexcept;
    [[nodiscard]] double player_note_multiplier() const noexcept;
    [[nodiscard]] double opponent_note_multiplier() const noexcept;
    [[nodiscard]] std::uint16_t player_key_count() const noexcept;
    [[nodiscard]] std::uint16_t opponent_key_count() const noexcept;
    [[nodiscard]] std::span<const StreamingWindowNote> window_notes() const noexcept;
    [[nodiscard]] std::span<const StreamingGameplayEvent> frame_events() const noexcept;
    [[nodiscard]] std::span<const InputRecord> recorded_inputs() const noexcept;
    [[nodiscard]] std::uint64_t dropped_frame_events() const noexcept;
    [[nodiscard]] std::uint64_t total_resolved_notes() const noexcept;
    [[nodiscard]] bool input_recording_overflowed() const noexcept;
    [[nodiscard]] bool lane_held(std::uint16_t lane) const noexcept;
    [[nodiscard]] std::uint16_t display_lane(
        std::uint16_t source_lane
    ) const noexcept;
    [[nodiscard]] std::uint16_t display_lane(
        NoteOwner owner,
        std::uint16_t source_lane
    ) const noexcept;
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] bool healthy() const noexcept;
    [[nodiscard]] bool catchup_pending() const noexcept;
    [[nodiscard]] bool window_saturated() const noexcept;
    [[nodiscard]] double song_time_ms() const noexcept;
    [[nodiscard]] std::string_view error() const noexcept;
    [[nodiscard]] StreamingGameplayMemoryStats memory_stats() const noexcept;
    [[nodiscard]] StreamingGameplayFrameStats frame_stats() const noexcept;
    [[nodiscard]] StreamingGameplayAccelerationStats acceleration_stats() const noexcept;

private:
    class Impl;
    explicit StreamingGameplaySession(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};

}  // namespace pulseforge
