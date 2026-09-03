#include "pulseforge/gameplay.hpp"
#include "pulseforge/note_types.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <limits>
#include <array>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

namespace pulseforge {
namespace {

constexpr std::size_t maximum_frame_events = 65'536U;

constexpr double minimum_supported_song_time_ms = -60'000.0;
constexpr double input_tail_margin_ms = 10'000.0;
constexpr double maximum_supported_song_time_ms =
    12.0 * 60.0 * 60.0 * 1'000.0;
constexpr std::size_t maximum_recorded_inputs = 500'000;

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
    // PULSEFORGE_P1_1_19_UNBOUNDED_NOTE_MULTIPLIER_V1
    // Change Note Multiplier is logical polyphony. There is no engine-authored
    // gameplay cap here: values are limited only by the natural uint64 counter
    // range. Expensive Lua callback fan-out is bounded independently by the
    // Lua per-frame callback budget.
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

// PULSEFORGE_P1_3_0_DYNAMIC_MANIA_EVENT_PARSE_V1
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

void saturating_increment(
    std::uint64_t& value,
    const std::uint64_t amount
) noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    value = amount > maximum - value ? maximum : value + amount;
}

[[nodiscard]] std::int64_t scaled_score(
    const std::int32_t base,
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

[[nodiscard]] double finite_or(
    const double value,
    const double fallback
) noexcept {
    return std::isfinite(value) ? value : fallback;
}

constexpr std::size_t maximum_runtime_note_kind_behaviors = 4'096U;

[[nodiscard]] bool is_mine_kind(const std::string& kind) {
    return builtin_note_type_causes_miss(kind);
}

[[nodiscard]] bool terminal(const NoteState state) noexcept {
    return state == NoteState::completed
        || state == NoteState::missed
        || state == NoteState::ignored;
}

// PULSEFORGE_P1_5_0C_DECLARATIVE_SUSTAIN_POLICY_IMPL_V1
[[nodiscard]] bool effective_sustain_enabled(
    const Note& note,
    const NoteKindRuntimeBehavior* behavior
) noexcept {
    return note.duration_ms > 0.0
        && (behavior == nullptr || behavior->sustain_enabled.value_or(true));
}

[[nodiscard]] bool sustain_inherits_note_type(
    const Note& note,
    const NoteKindRuntimeBehavior* behavior
) noexcept {
    return effective_sustain_enabled(note, behavior)
        && (behavior == nullptr
            || behavior->sustain_inherits_type.value_or(true));
}

// PULSEFORGE_P1_5_0D_TYPED_HAZARD_DAMAGE_V1
// A built-in Hurt Note keeps the legacy 1.5x fallback when no declarative
// behavior is installed. Once a real NoteTypeDefinition/Lua override exists,
// that typed behavior becomes authoritative, including sustain-specific damage.
[[nodiscard]] double hit_causes_miss_health(
    const Note& note,
    const NoteKindRuntimeBehavior* behavior,
    const GameplaySettings& settings
) noexcept {
    if (behavior == nullptr) {
        return settings.health_loss * 1.5;
    }
    if (sustain_inherits_note_type(note, behavior)
        && behavior->sustain_hit_causes_miss.value_or(false)
        && behavior->sustain_miss_health.has_value()) {
        return *behavior->sustain_miss_health;
    }
    return behavior->miss_health.value_or(settings.health_loss);
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
    for (std::size_t remaining = lanes.size(); remaining > 1; --remaining) {
        const auto range = static_cast<std::uint64_t>(remaining);
        const auto rejection_limit =
            std::numeric_limits<std::uint64_t>::max()
            - (std::numeric_limits<std::uint64_t>::max() % range);
        std::uint64_t value = 0;
        do {
            value = splitmix64(seed);
        } while (value >= rejection_limit);
        std::swap(
            lanes[remaining - 1],
            lanes[static_cast<std::size_t>(value % range)]
        );
    }
}

}  // namespace

void sanitize_gameplay_settings(GameplaySettings& settings) noexcept {
    auto& windows = settings.windows;
    windows.marvelous_ms = std::clamp(
        finite_or(windows.marvelous_ms, 22.5),
        1.0,
        500.0
    );
    windows.sick_ms = std::clamp(
        finite_or(windows.sick_ms, 45.0),
        windows.marvelous_ms,
        500.0
    );
    windows.good_ms = std::clamp(
        finite_or(windows.good_ms, 90.0),
        windows.sick_ms,
        500.0
    );
    windows.bad_ms = std::clamp(
        finite_or(windows.bad_ms, 135.0),
        windows.good_ms,
        500.0
    );
    windows.miss_ms = std::clamp(
        finite_or(windows.miss_ms, 180.0),
        windows.bad_ms,
        1'000.0
    );
    settings.input_offset_ms = std::clamp(
        finite_or(settings.input_offset_ms, 0.0),
        -1'000.0,
        1'000.0
    );
    settings.visual_offset_ms = std::clamp(
        finite_or(settings.visual_offset_ms, 0.0),
        -1'000.0,
        1'000.0
    );
    settings.release_grace_ms = std::clamp(
        finite_or(settings.release_grace_ms, 35.0),
        0.0,
        1'000.0
    );
    settings.stacked_note_tolerance_ms = std::clamp(
        finite_or(settings.stacked_note_tolerance_ms, 1.0),
        0.0,
        100.0
    );
    settings.health_gain = std::clamp(
        finite_or(settings.health_gain, 0.023),
        0.0,
        2.0
    );
    settings.health_loss = std::clamp(
        finite_or(settings.health_loss, 0.080),
        0.0,
        2.0
    );
    settings.ghost_tap_health_loss = std::clamp(
        finite_or(settings.ghost_tap_health_loss, 0.035),
        0.0,
        2.0
    );
    settings.scroll_speed = std::clamp(
        finite_or(settings.scroll_speed, 1.0),
        0.1,
        10.0
    );
}

std::string_view to_string(const Rating rating) noexcept {
    switch (rating) {
    case Rating::marvelous:
        return "marvelous";
    case Rating::sick:
        return "sick";
    case Rating::good:
        return "good";
    case Rating::bad:
        return "bad";
    case Rating::miss:
        return "miss";
    case Rating::mine:
        return "mine";
    }
    return "unknown";
}

double ScoreSummary::accuracy_percent() const noexcept {
    if (judged_notes <= 0.0) {
        return 100.0;
    }
    return std::clamp(weighted_hits / judged_notes * 100.0, 0.0, 100.0);
}

std::string_view ScoreSummary::clear_type() const noexcept {
    if (failed) {
        return "FAILED";
    }
    if (misses > 0 || hold_drops > 0) {
        return "CLEAR";
    }
    if (bad > 0) {
        return "FC";
    }
    if (good > 0) {
        return "GFC";
    }
    if (sick > 0) {
        return "SFC";
    }
    return "MFC";
}

GameplaySession::GameplaySession(const Chart& chart, GameplaySettings settings)
    : chart_(&chart),
      timing_(chart.tempos),
      settings_(std::move(settings)) {
    sanitize_gameplay_settings(settings_);
    reset();
}

void GameplaySession::reset() {
    summary_ = {};
    summary_.health = 1.0;
    states_.assign(chart_->notes.size(), NoteState::pending);
    held_lanes_.assign(chart_->key_count, false);
    player_key_count_ = chart_->key_count;
    opponent_key_count_ = chart_->key_count;
    player_lane_map_.resize(chart_->key_count);
    opponent_lane_map_.resize(chart_->key_count);
    rebuild_lane_map(player_lane_map_, player_key_count_);
    rebuild_lane_map(opponent_lane_map_, opponent_key_count_);

    active_holds_.clear();
    active_holds_.reserve(std::min<std::size_t>(
        chart_->notes.size() / 4 + 4,
        65'536
    ));
    next_hold_tick_ms_.assign(chart_->notes.size(), 0.0);
    frame_events_.clear();
    frame_events_.reserve(512);
    dropped_frame_events_ = 0U;
    recorded_inputs_.clear();
    recorded_inputs_.reserve(std::min<std::size_t>(
        chart_->notes.size() * 2,
        65'536
    ));
    input_recording_overflowed_ = false;

    miss_cursor_ = 0;
    autoplay_cursor_ = 0;
    chart_event_cursor_ = 0;
    last_beat_ = -1;
    last_step_ = -1;
    song_time_ms_ = 0.0;
    complete_ = false;
    completion_emitted_ = false;
    failure_emitted_ = false;
    player_note_multiplier_ = 1.0;
    opponent_note_multiplier_ = 1.0;
    content_end_ms_ = 0.0;
    for (const auto& note : chart_->notes) {
        content_end_ms_ = std::max(content_end_ms_, note.end_time_ms());
    }
    for (const auto& event : chart_->events) {
        content_end_ms_ = std::max(content_end_ms_, event.time_ms);
    }
    rebuild_lane_indices();
}

void GameplaySession::begin_frame() noexcept {
    frame_events_.clear();
    dropped_frame_events_ = 0U;
}

void GameplaySession::press(const std::uint16_t lane, const double song_time_ms) {
    if (complete_
        || summary_.failed
        || !std::isfinite(song_time_ms)
        || song_time_ms < minimum_supported_song_time_ms
        || song_time_ms > content_end_ms_ + input_tail_margin_ms) {
        return;
    }

    // Chart events at the same timestamp are authoritative before input. This
    // matters for Change P1 Mania: the physical lane is interpreted in the new
    // topology, not remapped as if it had been held in the old one.
    update_chart_events(song_time_ms);
    if (lane >= player_key_count_) {
        return;
    }
    record_input({song_time_ms, lane, true});
    held_lanes_[lane] = true;
    const double adjusted_time = song_time_ms + settings_.input_offset_ms;
    update_misses(song_time_ms);

    const auto candidate = candidate_for_lane(lane, adjusted_time);
    if (candidate != no_note) {
        const double note_time = chart_->notes[candidate].time_ms;
        hit_note(candidate, adjusted_time);
        if (summary_.failed) {
            return;
        }

        // A shrunk dynamic-mania receptor can represent several immutable
        // source lanes. Judge coincident stacks across every source lane that
        // currently projects to this display lane, not only the first one.
        for (std::size_t source_lane = 0U;
             source_lane < player_notes_by_lane_.size();
             ++source_lane) {
            if (source_lane >= player_lane_map_.size()
                || player_lane_map_[source_lane] != lane) {
                continue;
            }
            auto& lane_notes = player_notes_by_lane_[source_lane];
            auto& cursor = lane_cursors_[source_lane];
            while (cursor < lane_notes.size()) {
                const auto stacked_index = lane_notes[cursor];
                if (stacked_index == candidate
                    || states_[stacked_index] != NoteState::pending) {
                    ++cursor;
                    continue;
                }
                const auto& stacked_note = chart_->notes[stacked_index];
                if (std::abs(stacked_note.time_ms - note_time)
                    <= settings_.stacked_note_tolerance_ms) {
                    // Dense charts intentionally use coincident notes as real
                    // logical notes. One physical press judges the whole stack;
                    // none of its multiplicity is silently discarded.
                    hit_note(stacked_index, adjusted_time);
                    ++cursor;
                    if (summary_.failed) {
                        return;
                    }
                    continue;
                }
                break;
            }
        }
        return;
    }

    if (!settings_.ghost_tapping) {
        summary_.combo = 0;
        ++summary_.misses;
        summary_.judged_notes += 1.0;
        summary_.health = std::max(
            0.0,
            summary_.health - settings_.ghost_tap_health_loss
        );
        emit({
            GameplayEventType::ghost_tap,
            no_note,
            no_note,
            Rating::miss,
            song_time_ms,
            0.0,
            static_cast<std::int64_t>(lane),
        });
        check_failure(song_time_ms);
    }
}

void GameplaySession::release(const std::uint16_t lane, const double song_time_ms) {
    if (complete_
        || summary_.failed
        || !std::isfinite(song_time_ms)
        || song_time_ms < minimum_supported_song_time_ms
        || song_time_ms > content_end_ms_ + input_tail_margin_ms) {
        return;
    }

    const double adjusted_time = song_time_ms + settings_.input_offset_ms;
    update_chart_events(song_time_ms);
    if (lane >= player_key_count_) {
        return;
    }
    // SDL can deliver a release before this frame's update. Advance sustain
    // ticks while the lane is still held so live input and replay order agree.
    update_holds(song_time_ms);
    record_input({song_time_ms, lane, false});
    held_lanes_[lane] = false;
    if (settings_.autoplay) {
        return;
    }

    std::size_t active_index = 0;
    while (active_index < active_holds_.size()) {
        const auto note_index = active_holds_[active_index];
        const auto& note = chart_->notes[note_index];
        // PULSEFORGE_P1_3_0_OPPONENT_SUSTAIN_LIFETIME_V1
        // Opponent sustains share the visual holding state, but player input
        // must never release/drop them merely because the display lane matches.
        if (note.owner != NoteOwner::player || display_lane(note_index) != lane) {
            ++active_index;
            continue;
        }

        // A calibrated release may be ahead of the raw audio clock. Award only
        // this lane's due ticks; input on one lane must never advance another.
        advance_hold_ticks(note_index, adjusted_time);
        if (adjusted_time + settings_.release_grace_ms >= note.end_time_ms()) {
            states_[note_index] = NoteState::completed;
            emit({
                GameplayEventType::hold_complete,
                note_index,
                no_note,
                Rating::marvelous,
                song_time_ms,
                adjusted_time - note.end_time_ms(),
                0,
            });
        } else {
            miss_note(note_index, song_time_ms, true);
        }
        active_holds_[active_index] = active_holds_.back();
        active_holds_.pop_back();
    }
}

void GameplaySession::update(const double song_time_ms) {
    if (!std::isfinite(song_time_ms) || summary_.failed || complete_) {
        return;
    }
    song_time_ms_ = std::clamp(
        song_time_ms,
        minimum_supported_song_time_ms,
        maximum_supported_song_time_ms
    );

    // Input offset belongs only to physical press/release timestamps. The
    // authoritative chart clock, AI, events, misses and musical callbacks must
    // not move when the player calibrates their controls.
    update_chart_events(song_time_ms_);
    update_autoplay(song_time_ms_);
    update_holds(song_time_ms_);
    if (!summary_.failed) {
        update_misses(song_time_ms_);
    }
    if (!summary_.failed) {
        update_musical_callbacks(song_time_ms_);
    }
    check_failure(song_time_ms_);
}

void GameplaySession::finish_song(const double media_end_time_ms) {
    if (complete_ || summary_.failed || !std::isfinite(media_end_time_ms)) {
        return;
    }
    update(media_end_time_ms);
    if (summary_.failed) {
        return;
    }

    // The media transport is authoritative. Objects at or before its end can
    // no longer receive input; objects after it belong to a malformed/longer
    // chart and are ignored rather than keeping a stopped audio clock alive.
    // This terminal pass is O(total notes) once and remains allocation-free.
    for (const auto note_index : active_holds_) {
        if (note_index < states_.size()
            && states_[note_index] == NoteState::holding) {
            states_[note_index] = NoteState::completed;
            // Opponent sustains use holding only to preserve the unconsumed
            // visual tail. hold_complete remains a player gameplay callback.
            if (chart_->notes[note_index].owner == NoteOwner::player) {
                emit({
                    GameplayEventType::hold_complete,
                    note_index,
                    no_note,
                    Rating::marvelous,
                    song_time_ms_,
                    song_time_ms_ - chart_->notes[note_index].end_time_ms(),
                    0,
                });
            }
        }
    }
    active_holds_.clear();

    for (std::size_t index = 0U; index < chart_->notes.size(); ++index) {
        if (states_[index] != NoteState::pending) {
            continue;
        }
        const auto& note = chart_->notes[index];
        if (note.time_ms > song_time_ms_) {
            states_[index] = NoteState::ignored;
        } else if (note.owner != NoteOwner::player) {
            // PULSEFORGE_P1_4_0_AI_OWNER_PARITY_V1
            states_[index] = NoteState::completed;
            emit({
                GameplayEventType::opponent_hit,
                index,
                no_note,
                Rating::marvelous,
                song_time_ms_,
                song_time_ms_ - note.time_ms,
                0,
            });
        } else if (is_mine_kind(note.kind)
                   || (note_kind_behavior(note.kind) != nullptr
                       && note_kind_behavior(note.kind)->ignore_note.value_or(false))) {
            states_[index] = NoteState::ignored;
        } else {
            miss_note(index, song_time_ms_);
            if (summary_.failed) {
                return;
            }
        }
    }
    miss_cursor_ = chart_->notes.size();
    autoplay_cursor_ = chart_->notes.size();
    // Events scheduled after the physical media end are not fired.
    chart_event_cursor_ = chart_->events.size();
    update_completion();
}

void GameplaySession::add_score(const std::int64_t amount) noexcept {
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

void GameplaySession::set_health(const double health) {
    if (std::isfinite(health)) {
        summary_.health = std::clamp(health, 0.0, 2.0);
        check_failure(song_time_ms_);
    }
}

bool GameplaySession::apply_event(
    const std::string_view name,
    const std::string_view value1,
    const std::string_view value2
) noexcept {
    const auto event_name = lower_ascii_copy(name);

    // PULSEFORGE_P1_3_0_DYNAMIC_MANIA_EVENTS_V1
    // Denpa/JS-family packs in the supplied corpus use P1/P2 events whose
    // value1 is the requested number of keys. value2 is only the optional
    // change-animation flag, so gameplay deliberately ignores it.
    if (event_name == "change p1 mania"
        || event_name == "change player mania") {
        if (const auto count = parse_dynamic_key_count(
                value1, chart_->key_count
            ); count.has_value()) {
            set_player_key_count(*count);
        }
        return true;
    }
    if (event_name == "change p2 mania"
        || event_name == "change opponent mania") {
        if (const auto count = parse_dynamic_key_count(
                value1, chart_->key_count
            ); count.has_value()) {
            set_opponent_key_count(*count);
        }
        return true;
    }
    if (event_name == "change mania") {
        if (const auto count = parse_dynamic_key_count(
                value1, chart_->key_count
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

// PULSEFORGE_P1_5_0_MATERIALIZED_NOTE_KIND_RUNTIME_BEHAVIOR_V1
bool GameplaySession::set_note_kind_behavior(
    const std::string_view kind,
    const NoteKindRuntimeBehavior& behavior
) {
    if (!valid_chart_note_kind_text(kind)) {
        return false;
    }
    const auto found = note_kind_behaviors_.find(kind);
    if (found == note_kind_behaviors_.end()
        && note_kind_behaviors_.size() >= maximum_runtime_note_kind_behaviors) {
        return false;
    }
    note_kind_behaviors_[std::string(kind)] = behavior;
    return true;
}

const NoteKindRuntimeBehavior* GameplaySession::note_kind_behavior(
    const std::string_view kind
) const noexcept {
    const auto found = note_kind_behaviors_.find(kind);
    return found == note_kind_behaviors_.end() ? nullptr : &found->second;
}

const Chart& GameplaySession::chart() const noexcept {
    return *chart_;
}

const TimingMap& GameplaySession::timing_map() const noexcept {
    return timing_;
}

const GameplaySettings& GameplaySession::settings() const noexcept {
    return settings_;
}

GameplaySettings& GameplaySession::settings() noexcept {
    return settings_;
}

const ScoreSummary& GameplaySession::summary() const noexcept {
    return summary_;
}

std::span<const GameplayEvent> GameplaySession::frame_events() const noexcept {
    return frame_events_;
}

std::span<const InputRecord> GameplaySession::recorded_inputs() const noexcept {
    return recorded_inputs_;
}

std::uint64_t GameplaySession::dropped_frame_events() const noexcept {
    return dropped_frame_events_;
}

bool GameplaySession::input_recording_overflowed() const noexcept {
    return input_recording_overflowed_;
}

NoteState GameplaySession::note_state(const std::size_t note_index) const noexcept {
    if (note_index >= states_.size()) {
        return NoteState::ignored;
    }
    return states_[note_index];
}

std::uint16_t GameplaySession::display_lane(
    const std::size_t note_index
) const noexcept {
    if (note_index >= chart_->notes.size()) {
        return 0;
    }
    const auto& note = chart_->notes[note_index];
    return display_lane(note.owner, note.lane);
}

// PULSEFORGE_P1_4_0B_MATERIALIZED_OWNER_DISPLAY_LANE_V1
std::uint16_t GameplaySession::display_lane(
    const NoteOwner owner,
    const std::uint16_t source_lane
) const noexcept {
    const auto& map = owner == NoteOwner::player
        ? player_lane_map_
        : opponent_lane_map_;
    return source_lane < map.size() ? map[source_lane] : source_lane;
}

bool GameplaySession::lane_held(const std::uint16_t lane) const noexcept {
    return lane < held_lanes_.size() && held_lanes_[lane];
}

bool GameplaySession::complete() const noexcept {
    return complete_;
}

double GameplaySession::song_time_ms() const noexcept {
    return song_time_ms_;
}

double GameplaySession::player_note_multiplier() const noexcept {
    return player_note_multiplier_;
}

double GameplaySession::opponent_note_multiplier() const noexcept {
    return opponent_note_multiplier_;
}

std::uint16_t GameplaySession::player_key_count() const noexcept {
    return player_key_count_;
}

std::uint16_t GameplaySession::opponent_key_count() const noexcept {
    return opponent_key_count_;
}

Rating GameplaySession::judge(const double absolute_offset_ms) const noexcept {
    if (absolute_offset_ms <= settings_.windows.marvelous_ms) {
        return Rating::marvelous;
    }
    if (absolute_offset_ms <= settings_.windows.sick_ms) {
        return Rating::sick;
    }
    if (absolute_offset_ms <= settings_.windows.good_ms) {
        return Rating::good;
    }
    if (absolute_offset_ms <= settings_.windows.bad_ms) {
        return Rating::bad;
    }
    return Rating::miss;
}

double GameplaySession::rating_weight(const Rating rating) const noexcept {
    switch (rating) {
    case Rating::marvelous:
        return 1.0;
    case Rating::sick:
        return 0.95;
    case Rating::good:
        return 0.75;
    case Rating::bad:
        return 0.50;
    case Rating::miss:
    case Rating::mine:
        return 0.0;
    }
    return 0.0;
}

std::int32_t GameplaySession::rating_score(const Rating rating) const noexcept {
    switch (rating) {
    case Rating::marvelous:
        return 350;
    case Rating::sick:
        return 300;
    case Rating::good:
        return 200;
    case Rating::bad:
        return 100;
    case Rating::miss:
        return 0;
    case Rating::mine:
        return -200;
    }
    return 0;
}

std::size_t GameplaySession::candidate_for_lane(
    const std::uint16_t lane,
    const double adjusted_time_ms
) noexcept {
    if (lane >= player_key_count_) {
        return no_note;
    }
    std::size_t best = no_note;
    double best_time = std::numeric_limits<double>::infinity();
    for (std::size_t source_lane = 0U;
         source_lane < player_notes_by_lane_.size();
         ++source_lane) {
        if (source_lane >= player_lane_map_.size()
            || player_lane_map_[source_lane] != lane) {
            continue;
        }

        auto& notes = player_notes_by_lane_[source_lane];
        auto& cursor = lane_cursors_[source_lane];
        while (cursor < notes.size()
               && states_[notes[cursor]] != NoteState::pending) {
            ++cursor;
        }
        if (cursor >= notes.size()) continue;

        const auto index = notes[cursor];
        const auto& note = chart_->notes[index];
        const double delta = adjusted_time_ms - note.time_ms;
        if (delta < -settings_.windows.miss_ms
            || delta > settings_.windows.miss_ms) {
            continue;
        }
        if (note.time_ms < best_time) {
            best = index;
            best_time = note.time_ms;
        }
    }
    return best;
}

void GameplaySession::hit_note(
    const std::size_t index,
    const double adjusted_time_ms
) {
    auto& state = states_[index];
    if (state != NoteState::pending) {
        return;
    }
    const auto& note = chart_->notes[index];
    const double offset = adjusted_time_ms - note.time_ms;

    const auto* behavior = note_kind_behavior(note.kind);
    const bool custom_hit_causes_miss = behavior != nullptr
        && (sustain_inherits_note_type(note, behavior)
                && behavior->sustain_hit_causes_miss.has_value()
            ? *behavior->sustain_hit_causes_miss
            : behavior->hit_causes_miss.value_or(false));
    if (is_mine_kind(note.kind) || custom_hit_causes_miss) {
        state = NoteState::completed;
        const auto count = logical_note_count(player_note_multiplier_);
        const double logical_weight = static_cast<double>(count);
        add_score(scaled_score(rating_score(Rating::mine), logical_weight));
        summary_.combo = 0;
        saturating_increment(summary_.misses, count);
        summary_.judged_notes += logical_weight;
        const double damage = hit_causes_miss_health(
            note, behavior, settings_
        );
        summary_.health = std::clamp(summary_.health - damage, 0.0, 2.0);
        emit({
            GameplayEventType::note_hit,
            index,
            no_note,
            Rating::mine,
            song_time_ms_,
            offset,
            0,
        });
        check_failure(song_time_ms_);
        return;
    }

    const auto rating = judge(std::abs(offset));
    if (rating == Rating::miss) {
        miss_note(index, song_time_ms_);
        return;
    }

    state = effective_sustain_enabled(note, behavior)
        ? NoteState::holding
        : NoteState::completed;
    const auto count = logical_note_count(player_note_multiplier_);
    const double logical_weight = static_cast<double>(count);
    add_score(scaled_score(rating_score(rating), logical_weight));
    saturating_increment(summary_.combo, count);
    summary_.max_combo = std::max(summary_.max_combo, summary_.combo);
    summary_.weighted_hits += rating_weight(rating) * logical_weight;
    summary_.judged_notes += logical_weight;
    const double hit_health = behavior != nullptr
        ? behavior->hit_health.value_or(settings_.health_gain)
        : settings_.health_gain;
    summary_.health = std::clamp(
        summary_.health + hit_health * logical_weight,
        0.0,
        2.0
    );

    switch (rating) {
    case Rating::marvelous:
        saturating_increment(summary_.marvelous, count);
        break;
    case Rating::sick:
        saturating_increment(summary_.sick, count);
        break;
    case Rating::good:
        saturating_increment(summary_.good, count);
        break;
    case Rating::bad:
        saturating_increment(summary_.bad, count);
        break;
    case Rating::miss:
    case Rating::mine:
        break;
    }

    if (state == NoteState::holding) {
        active_holds_.push_back(index);
        double next_step = std::floor(timing_.step_at(note.time_ms) + 0.000001) + 1.0;
        next_hold_tick_ms_[index] = timing_.time_at_step(next_step);
        if (next_hold_tick_ms_[index] <= note.time_ms + 0.001) {
            next_hold_tick_ms_[index] = note.time_ms + 1.0;
        }
    }

    emit({
        GameplayEventType::note_hit,
        index,
        no_note,
        rating,
        song_time_ms_,
        offset,
        0,
    });
    check_failure(song_time_ms_);
}

void GameplaySession::miss_note(
    const std::size_t index,
    const double time_ms,
    const bool hold_drop
) {
    auto& state = states_[index];
    if (terminal(state)) {
        return;
    }
    state = NoteState::missed;
    summary_.combo = 0;
    const auto count = logical_note_count(player_note_multiplier_);
    const double logical_weight = static_cast<double>(count);
    saturating_increment(summary_.misses, count);
    if (hold_drop) {
        saturating_increment(summary_.hold_drops, count);
    } else {
        summary_.judged_notes += logical_weight;
    }
    const auto& note = chart_->notes[index];
    const auto* behavior = note_kind_behavior(note.kind);
    const double damage = behavior == nullptr
        ? settings_.health_loss
        : hold_drop
            ? sustain_inherits_note_type(note, behavior)
                ? behavior->sustain_miss_health.value_or(
                    behavior->miss_health.value_or(settings_.health_loss)
                )
                : settings_.health_loss
            : behavior->miss_health.value_or(settings_.health_loss);
    summary_.health = std::clamp(summary_.health - damage, 0.0, 2.0);
    emit({
        hold_drop ? GameplayEventType::hold_drop : GameplayEventType::note_miss,
        index,
        no_note,
        Rating::miss,
        time_ms,
        time_ms + settings_.input_offset_ms - note.time_ms,
        0,
    });
    check_failure(time_ms);
}

void GameplaySession::check_failure(const double event_time_ms) {
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
        no_note,
        no_note,
        Rating::miss,
        event_time_ms,
        0.0,
        0,
    });
}

void GameplaySession::record_input(InputRecord input) {
    if (recorded_inputs_.size() >= maximum_recorded_inputs) {
        input_recording_overflowed_ = true;
        return;
    }
    recorded_inputs_.push_back(std::move(input));
}

void GameplaySession::emit(GameplayEvent event) {
    // PULSEFORGE_P1_1_18_NOTE_MULTIPLIER_EVENT_WEIGHT_IMPL_V1
    switch (event.type) {
    case GameplayEventType::note_hit:
    case GameplayEventType::note_miss:
    case GameplayEventType::hold_tick:
    case GameplayEventType::hold_complete:
    case GameplayEventType::hold_drop:
        event.logical_occurrence_count =
            logical_note_count(player_note_multiplier_);
        break;
    case GameplayEventType::opponent_hit:
        event.logical_occurrence_count =
            logical_note_count(opponent_note_multiplier_);
        break;
    case GameplayEventType::ghost_tap:
    case GameplayEventType::chart_event:
    case GameplayEventType::beat:
    case GameplayEventType::step:
    case GameplayEventType::song_complete:
    case GameplayEventType::failed:
        event.logical_occurrence_count = 1U;
        break;
    }

    // PULSEFORGE_P1_5_0E_AUTHORITATIVE_CHART_TOTAL_MATERIALIZED_V1
    // Count before callback backpressure: a full frame-event queue must never
    // make Chart Total under-report dense logical gameplay.
    if (event.type == GameplayEventType::note_hit
        || event.type == GameplayEventType::note_miss
        || event.type == GameplayEventType::opponent_hit) {
        saturating_increment(summary_.chart_total, event.logical_occurrence_count);
    }

    if (frame_events_.size() >= maximum_frame_events) {
        if (dropped_frame_events_ != std::numeric_limits<std::uint64_t>::max()) {
            ++dropped_frame_events_;
        }
        return;
    }
    frame_events_.push_back(std::move(event));
}

void GameplaySession::update_autoplay(const double adjusted_time_ms) {
    while (autoplay_cursor_ < chart_->notes.size()) {
        const auto index = autoplay_cursor_;
        const auto& note = chart_->notes[index];
        if (note.time_ms > adjusted_time_ms) {
            break;
        }
        ++autoplay_cursor_;
        if (states_[index] != NoteState::pending) {
            continue;
        }

        if (note.owner != NoteOwner::player) {
            // PULSEFORGE_P1_4_0_AI_OWNER_PARITY_V1
            // PULSEFORGE_P1_3_0_OPPONENT_SUSTAIN_LIFETIME_V1
            // Match the player's visual lifetime: after the head is played,
            // keep a sustain in `holding` so visual_note_span() clips it at
            // the receptor and consumes only the elapsed portion. Opponent
            // holds are AI-owned: they award no player score/health/ticks.
            const auto* behavior = note_kind_behavior(note.kind);
            states_[index] = effective_sustain_enabled(note, behavior)
                ? NoteState::holding
                : NoteState::completed;
            if (states_[index] == NoteState::holding) {
                active_holds_.push_back(index);
            }
            emit({
                GameplayEventType::opponent_hit,
                index,
                no_note,
                Rating::marvelous,
                song_time_ms_,
                adjusted_time_ms - note.time_ms,
                0,
            });
        } else if (settings_.autoplay) {
            const auto* behavior = note_kind_behavior(note.kind);
            const bool custom_hazard = behavior != nullptr
                && (sustain_inherits_note_type(note, behavior)
                        && behavior->sustain_hit_causes_miss.has_value()
                    ? *behavior->sustain_hit_causes_miss
                    : behavior->hit_causes_miss.value_or(false));
            if (is_mine_kind(note.kind) || custom_hazard) {
                states_[index] = NoteState::ignored;
            } else {
                hit_note(index, note.time_ms);
            }
        }
    }
}

void GameplaySession::update_misses(const double adjusted_time_ms) {
    while (miss_cursor_ < chart_->notes.size()) {
        const auto index = miss_cursor_;
        const auto& note = chart_->notes[index];
        if (note.time_ms + settings_.windows.miss_ms >= adjusted_time_ms) {
            break;
        }
        ++miss_cursor_;
        if (note.owner != NoteOwner::player || states_[index] != NoteState::pending) {
            continue;
        }
        const auto* behavior = note_kind_behavior(note.kind);
        if (is_mine_kind(note.kind)
            || (behavior != nullptr && behavior->ignore_note.value_or(false))) {
            states_[index] = NoteState::ignored;
            continue;
        }
        miss_note(index, song_time_ms_);
        if (summary_.failed) {
            break;
        }
    }
}

void GameplaySession::advance_hold_ticks(
    const std::size_t note_index,
    const double adjusted_time_ms
) {
    const auto& note = chart_->notes[note_index];
    while (next_hold_tick_ms_[note_index] <= adjusted_time_ms
           && next_hold_tick_ms_[note_index] < note.end_time_ms()) {
        const auto logical_ticks = logical_note_count(player_note_multiplier_);
        saturating_increment(summary_.hold_ticks, logical_ticks);
        add_score(scaled_score(10, static_cast<double>(logical_ticks)));
        const auto* behavior = note_kind_behavior(note.kind);
        const double hold_health = behavior == nullptr
                || !sustain_inherits_note_type(note, behavior)
            ? settings_.health_gain
            : behavior->hit_health.value_or(settings_.health_gain);
        summary_.health = std::clamp(
            summary_.health
                + hold_health * 0.08
                    * static_cast<double>(logical_ticks),
            0.0,
            2.0
        );
        emit({
            GameplayEventType::hold_tick,
            note_index,
            no_note,
            Rating::marvelous,
            next_hold_tick_ms_[note_index],
            0.0,
            static_cast<std::int64_t>(
                std::llround(timing_.step_at(next_hold_tick_ms_[note_index]))
            ),
        });
        const double current_step = timing_.step_at(next_hold_tick_ms_[note_index]);
        next_hold_tick_ms_[note_index] =
            timing_.time_at_step(std::floor(current_step + 0.000001) + 1.0);
    }
}

void GameplaySession::update_holds(const double adjusted_time_ms) {
    std::size_t active_index = 0;
    while (active_index < active_holds_.size()) {
        const auto note_index = active_holds_[active_index];
        auto& state = states_[note_index];
        const auto& note = chart_->notes[note_index];
        const auto lane = display_lane(note_index);

        if (state != NoteState::holding) {
            active_holds_[active_index] = active_holds_.back();
            active_holds_.pop_back();
            continue;
        }

        if (note.owner != NoteOwner::player) {
            // PULSEFORGE_P1_4_0_AI_OWNER_PARITY_V1
            // AI sustains are visual holds, not player-scoring holds. Keep
            // their state alive until the actual tail time; the renderer then
            // reuses the same receptor clipping path as player sustains.
            if (adjusted_time_ms >= note.end_time_ms()) {
                state = NoteState::completed;
                active_holds_[active_index] = active_holds_.back();
                active_holds_.pop_back();
                continue;
            }
            ++active_index;
            continue;
        }

        if (!settings_.autoplay
            && (lane >= held_lanes_.size() || !held_lanes_[lane])
            && adjusted_time_ms + settings_.release_grace_ms < note.end_time_ms()) {
            miss_note(note_index, song_time_ms_, true);
            active_holds_[active_index] = active_holds_.back();
            active_holds_.pop_back();
            if (summary_.failed) {
                return;
            }
            continue;
        }

        advance_hold_ticks(note_index, adjusted_time_ms);

        if (adjusted_time_ms + settings_.release_grace_ms >= note.end_time_ms()) {
            state = NoteState::completed;
            emit({
                GameplayEventType::hold_complete,
                note_index,
                no_note,
                Rating::marvelous,
                song_time_ms_,
                adjusted_time_ms - note.end_time_ms(),
                0,
            });
            active_holds_[active_index] = active_holds_.back();
            active_holds_.pop_back();
            continue;
        }
        ++active_index;
    }
}

void GameplaySession::update_chart_events(const double adjusted_time_ms) {
    while (chart_event_cursor_ < chart_->events.size()
           && chart_->events[chart_event_cursor_].time_ms <= adjusted_time_ms) {
        const auto& chart_event = chart_->events[chart_event_cursor_];
        static_cast<void>(apply_event(
            chart_event.name,
            chart_event.value1,
            chart_event.value2
        ));
        emit({
            GameplayEventType::chart_event,
            no_note,
            chart_event_cursor_,
            Rating::marvelous,
            song_time_ms_,
            adjusted_time_ms - chart_->events[chart_event_cursor_].time_ms,
            0,
        });
        ++chart_event_cursor_;
    }
}

void GameplaySession::update_musical_callbacks(const double adjusted_time_ms) {
    if (adjusted_time_ms < 0.0) {
        return;
    }

    const auto current_step = static_cast<std::int64_t>(
        std::floor(timing_.step_at(adjusted_time_ms) + 0.000001)
    );
    const auto current_beat = static_cast<std::int64_t>(
        std::floor(timing_.beat_at(adjusted_time_ms) + 0.000001)
    );
    auto first_step = last_step_ + 1;
    for (auto step = first_step; step <= current_step; ++step) {
        emit({
            GameplayEventType::step,
            no_note,
            no_note,
            Rating::marvelous,
            timing_.time_at_step(static_cast<double>(step)),
            0.0,
            step,
        });
    }
    last_step_ = std::max(last_step_, current_step);

    auto first_beat = last_beat_ + 1;
    for (auto beat = first_beat; beat <= current_beat; ++beat) {
        emit({
            GameplayEventType::beat,
            no_note,
            no_note,
            Rating::marvelous,
            timing_.time_at_beat(static_cast<double>(beat)),
            0.0,
            beat,
        });
    }
    last_beat_ = std::max(last_beat_, current_beat);
}

void GameplaySession::update_completion() {
    if (completion_emitted_ || summary_.failed) {
        return;
    }
    const bool schedulers_drained =
        miss_cursor_ >= chart_->notes.size()
        && autoplay_cursor_ >= chart_->notes.size()
        && chart_event_cursor_ >= chart_->events.size()
        && active_holds_.empty();
    if (schedulers_drained) {
        complete_ = true;
        completion_emitted_ = true;
        emit({
            GameplayEventType::song_complete,
            no_note,
            no_note,
            Rating::marvelous,
            song_time_ms_,
            0.0,
            0,
        });
    }
}

void GameplaySession::rebuild_lane_indices() {
    // Source-lane indexes remain stable. Runtime mania then changes only the
    // tiny display permutation instead of rescanning a multi-million-note chart.
    player_notes_by_lane_.clear();
    player_notes_by_lane_.resize(chart_->key_count);
    lane_cursors_.assign(chart_->key_count, 0);
    for (std::size_t index = 0; index < chart_->notes.size(); ++index) {
        const auto& note = chart_->notes[index];
        if (note.owner != NoteOwner::player || note.lane >= chart_->key_count) {
            continue;
        }
        player_notes_by_lane_[note.lane].push_back(index);
    }
}

void GameplaySession::rebuild_lane_map(
    std::vector<std::uint16_t>& map,
    const std::uint16_t active_key_count
) noexcept {
    map.resize(chart_->key_count);
    const auto active = std::min<std::size_t>(
        active_key_count,
        map.size()
    );
    if (active == 0U) {
        std::fill(map.begin(), map.end(), std::uint16_t{0U});
        return;
    }

    // PULSEFORGE_P1_5_0C3_COMPLETE_DYNAMIC_LANE_PROJECTION_V1
    // Dynamic mania changes the receptor domain, not the immutable chart/source
    // lane domain. Project every source lane proportionally into the active
    // receptor domain, then apply mirror/randomize to that receptor slot. This
    // preserves ordering and collapses contiguous source-lane bands (for
    // example 4K -> 2K maps sources 2 and 3 to receptor slot 1).
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

std::uint16_t GameplaySession::source_lane_for_player_display(
    const std::uint16_t display_lane
) const noexcept {
    for (std::size_t source = 0U; source < player_lane_map_.size(); ++source) {
        if (player_lane_map_[source] == display_lane) {
            return static_cast<std::uint16_t>(source);
        }
    }
    return chart_->key_count;
}

void GameplaySession::set_player_key_count(
    const std::uint16_t key_count
) noexcept {
    if (key_count == 0U || key_count > chart_->key_count
        || key_count == player_key_count_) {
        return;
    }

    std::vector<bool> source_held(chart_->key_count, false);
    for (std::size_t source = 0U; source < player_lane_map_.size(); ++source) {
        const auto displayed = player_lane_map_[source];
        if (displayed < held_lanes_.size() && held_lanes_[displayed]) {
            source_held[source] = true;
        }
    }

    player_key_count_ = key_count;
    rebuild_lane_map(player_lane_map_, player_key_count_);
    std::fill(held_lanes_.begin(), held_lanes_.end(), false);
    for (std::size_t source = 0U; source < player_lane_map_.size(); ++source) {
        if (!source_held[source]) continue;
        const auto displayed = player_lane_map_[source];
        if (displayed < held_lanes_.size()) {
            held_lanes_[displayed] = true;
        }
    }
}

void GameplaySession::set_opponent_key_count(
    const std::uint16_t key_count
) noexcept {
    if (key_count == 0U || key_count > chart_->key_count
        || key_count == opponent_key_count_) {
        return;
    }
    opponent_key_count_ = key_count;
    rebuild_lane_map(opponent_lane_map_, opponent_key_count_);
}

}  // namespace pulseforge
