#pragma once

#include "pulseforge/chart.hpp"
#include "pulseforge/timing_map.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge {


// PULSEFORGE_P1_5_0_NOTE_KIND_RUNTIME_BEHAVIOR_V1
// Bounded, type-wide overrides used by safe custom note definitions and the
// Psych Lua unspawnNotes compatibility bridge. Keeping these keyed by note
// kind avoids allocating state per physical note on multi-million/PFC1 charts.
struct NoteKindRuntimeBehavior {
    std::optional<double> hit_health;
    std::optional<double> miss_health;
    std::optional<double> sustain_miss_health;
    std::optional<bool> hit_causes_miss;
    std::optional<bool> sustain_hit_causes_miss;
    // PULSEFORGE_P1_5_0C_DECLARATIVE_SUSTAIN_POLICY_V1
    // These are type-wide and therefore remain O(kinds), never O(notes).
    // sustain_enabled=false makes a source sustain behave as a tap at runtime.
    // sustain_inherits_type=false keeps the custom head but returns tail
    // health/hazard/ticks to ordinary gameplay semantics.
    std::optional<bool> sustain_enabled;
    std::optional<bool> sustain_inherits_type;
    std::optional<bool> ignore_note;

    [[nodiscard]] bool operator==(const NoteKindRuntimeBehavior&) const = default;
};

enum class Rating : std::uint8_t {
    marvelous,
    sick,
    good,
    bad,
    miss,
    mine,
};

[[nodiscard]] std::string_view to_string(Rating rating) noexcept;

struct JudgmentWindows {
    double marvelous_ms{22.5};
    double sick_ms{45.0};
    double good_ms{90.0};
    double bad_ms{135.0};
    double miss_ms{180.0};
};

struct GameplaySettings {
    JudgmentWindows windows{};
    double input_offset_ms{};
    double visual_offset_ms{};
    double release_grace_ms{35.0};
    double stacked_note_tolerance_ms{1.0};
    double health_gain{0.023};
    double health_loss{0.080};
    double ghost_tap_health_loss{0.035};
    double scroll_speed{1.0};
    std::uint64_t random_seed{0x50554C5345464F52ULL};
    bool ghost_tapping{true};
    bool autoplay{false};
    bool practice{false};
    bool no_fail{false};
    bool mirror{false};
    bool randomize_lanes{false};
    bool downscroll{false};
    bool middle_scroll{false};
    bool hide_opponent_notes{false};
};

void sanitize_gameplay_settings(GameplaySettings& settings) noexcept;

struct ScoreSummary {
    std::int64_t score{};
    std::uint64_t combo{};
    std::uint64_t max_combo{};
    std::uint64_t marvelous{};
    std::uint64_t sick{};
    std::uint64_t good{};
    std::uint64_t bad{};
    std::uint64_t misses{};
    std::uint64_t hold_ticks{};
    std::uint64_t hold_drops{};
    double weighted_hits{};
    double judged_notes{};
    double health{1.0};
    bool failed{false};
    // PULSEFORGE_P1_5_0E_AUTHORITATIVE_CHART_TOTAL_V1
    // Logical note heads resolved by either side (hit, miss or AI hit). This is
    // deliberately independent from the bounded callback/event queue. Appended
    // for source compatibility with older aggregate initializers.
    std::uint64_t chart_total{};

    [[nodiscard]] double accuracy_percent() const noexcept;
    [[nodiscard]] std::string_view clear_type() const noexcept;
};

enum class NoteState : std::uint8_t {
    pending,
    head_hit,
    holding,
    completed,
    missed,
    ignored,
};

enum class GameplayEventType : std::uint8_t {
    note_hit,
    note_miss,
    hold_tick,
    hold_complete,
    hold_drop,
    ghost_tap,
    opponent_hit,
    chart_event,
    beat,
    step,
    song_complete,
    failed,
};

struct GameplayEvent {
    GameplayEventType type{};
    std::size_t note_index{std::numeric_limits<std::size_t>::max()};
    std::size_t chart_event_index{std::numeric_limits<std::size_t>::max()};
    Rating rating{Rating::miss};
    double song_time_ms{};
    double offset_ms{};
    std::int64_t musical_index{};
    // PULSEFORGE_P1_1_18_NOTE_MULTIPLIER_EVENT_WEIGHT_V1
    // One rendered/source note may represent several logical notes after a
    // Change Note Multiplier event. Visual consumers still process this event
    // once; script/statistical consumers can use the bounded logical weight.
    std::uint64_t logical_occurrence_count{1U};
};

struct InputRecord {
    double time_ms{};
    std::uint16_t lane{};
    bool pressed{};
};

class GameplaySession {
public:
    // The referenced chart must outlive the session. Rvalues are rejected to
    // prevent accidentally retaining a pointer to a destroyed temporary.
    GameplaySession(const Chart& chart, GameplaySettings settings = {});
    GameplaySession(Chart&& chart, GameplaySettings settings = {}) = delete;
    GameplaySession(const Chart&& chart, GameplaySettings settings = {}) = delete;

    void reset();
    void begin_frame() noexcept;
    void press(std::uint16_t lane, double song_time_ms);
    void release(std::uint16_t lane, double song_time_ms);
    void update(double song_time_ms);
    // Advances all remaining schedulers to the authoritative media end and
    // emits song_complete only when the host confirms that the song transport
    // ended. update() alone never completes a song merely because its final
    // chart object was resolved.
    void finish_song(double media_end_time_ms);
    void add_score(std::int64_t amount) noexcept;
    void set_health(double health);
    // Applies built-in chart/runtime event semantics. Unknown event names are
    // deliberately left for visual/Lua handlers. The canonical JS/Psych event
    // is "Change Note Multiplier"; "Note Multiplier" is accepted as an alias.
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

    [[nodiscard]] const Chart& chart() const noexcept;
    [[nodiscard]] const TimingMap& timing_map() const noexcept;
    [[nodiscard]] const GameplaySettings& settings() const noexcept;
    [[nodiscard]] GameplaySettings& settings() noexcept;
    [[nodiscard]] const ScoreSummary& summary() const noexcept;
    [[nodiscard]] std::span<const GameplayEvent> frame_events() const noexcept;
    [[nodiscard]] std::span<const InputRecord> recorded_inputs() const noexcept;
    [[nodiscard]] std::uint64_t dropped_frame_events() const noexcept;
    [[nodiscard]] bool input_recording_overflowed() const noexcept;
    [[nodiscard]] NoteState note_state(std::size_t note_index) const noexcept;
    [[nodiscard]] std::uint16_t display_lane(std::size_t note_index) const noexcept;
    // PULSEFORGE_P1_4_0B_MATERIALIZED_OWNER_DISPLAY_LANE_V1
    // Mirrors StreamingGameplaySession: secondary_opponent shares the
    // opponent/P2 dynamic-mania lane map while retaining its owner identity.
    [[nodiscard]] std::uint16_t display_lane(
        NoteOwner owner,
        std::uint16_t source_lane
    ) const noexcept;
    [[nodiscard]] bool lane_held(std::uint16_t lane) const noexcept;
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] double song_time_ms() const noexcept;
    [[nodiscard]] double player_note_multiplier() const noexcept;
    [[nodiscard]] double opponent_note_multiplier() const noexcept;
    // PULSEFORGE_P1_3_0_DYNAMIC_LANE_TOPOLOGY_API_V1
    // Runtime key counts may change through Psych/Denpa mania events while the
    // materialized Chart keeps the maximum/source lane domain unchanged.
    [[nodiscard]] std::uint16_t player_key_count() const noexcept;
    [[nodiscard]] std::uint16_t opponent_key_count() const noexcept;

private:
    static constexpr std::size_t no_note = std::numeric_limits<std::size_t>::max();

    [[nodiscard]] Rating judge(double absolute_offset_ms) const noexcept;
    [[nodiscard]] double rating_weight(Rating rating) const noexcept;
    [[nodiscard]] std::int32_t rating_score(Rating rating) const noexcept;
    [[nodiscard]] std::size_t candidate_for_lane(
        std::uint16_t lane,
        double adjusted_time_ms
    ) noexcept;

    void hit_note(std::size_t index, double adjusted_time_ms);
    void miss_note(std::size_t index, double time_ms, bool hold_drop = false);
    void check_failure(double event_time_ms);
    void record_input(InputRecord input);
    void emit(GameplayEvent event);
    void update_autoplay(double adjusted_time_ms);
    void update_misses(double adjusted_time_ms);
    void advance_hold_ticks(std::size_t note_index, double adjusted_time_ms);
    void update_holds(double adjusted_time_ms);
    void update_chart_events(double adjusted_time_ms);
    void update_musical_callbacks(double adjusted_time_ms);
    void update_completion();
    void rebuild_lane_indices();
    void rebuild_lane_map(
        std::vector<std::uint16_t>& map,
        std::uint16_t active_key_count
    ) noexcept;
    [[nodiscard]] std::uint16_t source_lane_for_player_display(
        std::uint16_t display_lane
    ) const noexcept;
    void set_player_key_count(std::uint16_t key_count) noexcept;
    void set_opponent_key_count(std::uint16_t key_count) noexcept;

    const Chart* chart_{};
    TimingMap timing_;
    GameplaySettings settings_;
    ScoreSummary summary_;
    std::vector<NoteState> states_;
    std::vector<std::vector<std::size_t>> player_notes_by_lane_;
    std::vector<std::size_t> lane_cursors_;
    std::vector<std::size_t> active_holds_;
    std::vector<double> next_hold_tick_ms_;
    std::vector<bool> held_lanes_;
    std::vector<std::uint16_t> player_lane_map_;
    std::vector<std::uint16_t> opponent_lane_map_;
    std::vector<GameplayEvent> frame_events_;
    std::vector<InputRecord> recorded_inputs_;
    std::map<std::string, NoteKindRuntimeBehavior, std::less<>> note_kind_behaviors_;
    std::uint64_t dropped_frame_events_{};
    bool input_recording_overflowed_{false};
    std::size_t miss_cursor_{};
    std::size_t autoplay_cursor_{};
    std::size_t chart_event_cursor_{};
    std::int64_t last_beat_{-1};
    std::int64_t last_step_{-1};
    double song_time_ms_{};
    bool complete_{false};
    bool completion_emitted_{false};
    bool failure_emitted_{false};
    double content_end_ms_{};
    double player_note_multiplier_{1.0};
    double opponent_note_multiplier_{1.0};
    std::uint16_t player_key_count_{4U};
    std::uint16_t opponent_key_count_{4U};
};

}  // namespace pulseforge
