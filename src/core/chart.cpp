#include "pulseforge/chart.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace pulseforge {
namespace {

constexpr double minimum_chart_time_ms = -60'000.0;
// Matches the procedural-audio clock ceiling and comfortably includes the
// multi-hour stress songs used by Denpa-family builds. Explicit audio remains
// subject to the decoder's bounded-memory policy until streaming is selected.
constexpr double maximum_chart_time_ms = 12.0 * 60.0 * 60.0 * 1'000.0;
constexpr double maximum_chart_scroll_speed = 100.0;
constexpr std::size_t maximum_generated_gameplay_events = 2'000'000;
constexpr std::size_t maximum_metadata_text_bytes = 1'024;
constexpr std::size_t maximum_audio_path_characters = 32'768;
constexpr std::size_t maximum_vocal_stems = 8;
constexpr std::size_t maximum_validation_issues = 256;

struct PendingValidationIssue {
    ValidationSeverity severity;
    std::string_view message;
    std::size_t item_index;
};

[[nodiscard]] bool repaired_by_normalization(
    const PendingValidationIssue& issue
) noexcept {
    return issue.severity == ValidationSeverity::error
        && (issue.message.find("not sorted by time") != std::string::npos
            || issue.message
                == "the chart must contain at least one tempo change");
}

class ValidationIssueCollector final {
public:
    void reserve(const std::size_t count) {
        issues_.reserve(std::min(count, maximum_validation_issues));
    }

    void push_back(const PendingValidationIssue issue) {
        const bool is_error =
            issue.severity == ValidationSeverity::error;
        const bool is_unrecoverable_error =
            is_error && !repaired_by_normalization(issue);
        if (issues_.size() < maximum_validation_issues) {
            has_error_ = has_error_ || is_error;
            has_unrecoverable_error_ = has_unrecoverable_error_
                || is_unrecoverable_error;
            issues_.push_back({
                issue.severity,
                std::string(issue.message),
                issue.item_index,
            });
            return;
        }
        // Warnings and normalization-repairable ordering errors must never
        // crowd the first later structural error out of the bounded set.
        if (is_unrecoverable_error && !has_unrecoverable_error_) {
            issues_.back() = {
                issue.severity,
                std::string(issue.message),
                issue.item_index,
            };
            has_error_ = true;
            has_unrecoverable_error_ = true;
        } else if (is_error && !has_error_) {
            issues_.back() = {
                issue.severity,
                std::string(issue.message),
                issue.item_index,
            };
            has_error_ = true;
        }
    }

    [[nodiscard]] std::vector<ValidationIssue> take() && {
        return std::move(issues_);
    }

private:
    std::vector<ValidationIssue> issues_;
    bool has_error_{};
    bool has_unrecoverable_error_{};
};

[[nodiscard]] bool finite(const double value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] double sortable_time(const double value) noexcept {
    return finite(value) ? value : std::numeric_limits<double>::infinity();
}

}  // namespace

void Chart::normalize() {
    if (key_count == 0) {
        key_count = 4;
    }
    if (note_payloads.empty()) {
        note_payloads.emplace_back();
    }

    // JSON parsers do not agree on whether an integer token written as -0
    // becomes negative or positive floating-point zero. They are the same
    // chart time, so canonicalize the sign before sorting and fingerprinting.
    for (auto& tempo : tempos) {
        if (tempo.time_ms == 0.0) {
            tempo.time_ms = 0.0;
        }
    }
    for (auto& note : notes) {
        if (note.time_ms == 0.0) {
            note.time_ms = 0.0;
        }
        if (note.duration_ms == 0.0) {
            note.duration_ms = 0.0;
        }
    }
    for (auto& event : events) {
        if (event.time_ms == 0.0) {
            event.time_ms = 0.0;
        }
    }

    const auto tempo_less = [](const TempoChange& left, const TempoChange& right) {
        return sortable_time(left.time_ms) < sortable_time(right.time_ms);
    };
    if (!std::is_sorted(tempos.begin(), tempos.end(), tempo_less)) {
        std::stable_sort(tempos.begin(), tempos.end(), tempo_less);
    }

    std::vector<TempoChange> unique_tempos;
    unique_tempos.reserve(tempos.size() + 1);
    for (const auto& tempo : tempos) {
        if (!unique_tempos.empty()
            && std::abs(unique_tempos.back().time_ms - tempo.time_ms) < 0.0001) {
            unique_tempos.back() = tempo;
        } else {
            unique_tempos.push_back(tempo);
        }
    }
    tempos = std::move(unique_tempos);
    if (tempos.empty()) {
        tempos.push_back(TempoChange{});
    } else if (tempos.front().time_ms > 0.0) {
        auto initial = tempos.front();
        initial.time_ms = 0.0;
        tempos.insert(tempos.begin(), initial);
    }

    bool saw_secondary_opponent = false;
    for (auto& note : notes) {
        if (note.kind.empty()) {
            note.kind = "normal";
        }
        // PULSEFORGE_P1_4_0_DENPA_THIRD_STRUM_CANONICAL_OWNER_V1
        // DenpaEx stores the third strumline as a NoteType.  Normalization is
        // the common post-loader boundary, so every materialized Denpa path
        // (DOM and fast parser) acquires the same canonical owner identity.
        if (source_format == ChartFormat::denpa && note.kind == "Third Strum") {
            note.owner = NoteOwner::secondary_opponent;
        }
        saw_secondary_opponent = saw_secondary_opponent
            || note.owner == NoteOwner::secondary_opponent;
    }
    secondary_opponent_enabled = secondary_opponent_enabled
        || saw_secondary_opponent;
    if (secondary_opponent_enabled && secondary_opponent_character.empty()) {
        secondary_opponent_character = opponent_character;
    }
    const auto note_less = [](const Note& left, const Note& right) {
        if (sortable_time(left.time_ms) != sortable_time(right.time_ms)) {
            return sortable_time(left.time_ms) < sortable_time(right.time_ms);
        }
        if (left.owner != right.owner) {
            return left.owner < right.owner;
        }
        return left.lane < right.lane;
    };
    if (!std::is_sorted(notes.begin(), notes.end(), note_less)) {
        std::stable_sort(notes.begin(), notes.end(), note_less);
    }

    const auto event_less = [](const ChartEvent& left, const ChartEvent& right) {
        return sortable_time(left.time_ms) < sortable_time(right.time_ms);
    };
    if (!std::is_sorted(events.begin(), events.end(), event_less)) {
        std::stable_sort(events.begin(), events.end(), event_less);
    }
}

double Chart::duration_ms() const noexcept {
    double duration = 0.0;
    for (const auto& note : notes) {
        if (finite(note.end_time_ms())) {
            duration = std::max(duration, note.end_time_ms());
        }
    }
    for (const auto& event : events) {
        if (finite(event.time_ms)) {
            duration = std::max(duration, event.time_ms);
        }
    }
    return duration + 2'000.0;
}

std::vector<ValidationIssue> validate_chart(const Chart& chart) {
    ValidationIssueCollector issues;
    issues.reserve(16);

    if (chart.title.size() > maximum_metadata_text_bytes
        || chart.artist.size() > maximum_metadata_text_bytes
        || chart.charter.size() > maximum_metadata_text_bytes
        || chart.difficulty.size() > maximum_metadata_text_bytes
        || chart.stage_id.size() > maximum_metadata_text_bytes
        || chart.player_character.size() > maximum_metadata_text_bytes
        || chart.opponent_character.size() > maximum_metadata_text_bytes
        || chart.secondary_opponent_character.size() > maximum_metadata_text_bytes
        || chart.girlfriend_character.size() > maximum_metadata_text_bytes
        || chart.note_style.size() > maximum_metadata_text_bytes) {
        issues.push_back({
            ValidationSeverity::error,
            "chart metadata strings exceed the 1024-byte safety limit",
            0,
        });
    }
    if (chart.key_count == 0
        || chart.key_count > maximum_supported_key_count) {
        issues.push_back({
            ValidationSeverity::error,
            "key_count must be between 1 and 18",
            0,
        });
    }
    if (!finite(chart.chart_scroll_speed)
        || chart.chart_scroll_speed <= 0.0
        || chart.chart_scroll_speed > maximum_chart_scroll_speed) {
        issues.push_back({
            ValidationSeverity::error,
            "chart_scroll_speed must be finite and between 0 and 100",
            0,
        });
    }
    if (chart.tempos.empty()) {
        issues.push_back({
            ValidationSeverity::error,
            "the chart must contain at least one tempo change",
            0,
        });
    }
    if (chart.tempos.size() > maximum_chart_tempo_changes) {
        issues.push_back({
            ValidationSeverity::error,
            "chart has too many tempo changes",
            chart.tempos.size(),
        });
    }
    if (chart.notes.size() > maximum_chart_notes) {
        issues.push_back({
            ValidationSeverity::error,
            "chart has too many notes",
            chart.notes.size(),
        });
    }
    if (chart.events.size() > maximum_chart_events) {
        issues.push_back({
            ValidationSeverity::error,
            "chart has too many events",
            chart.events.size(),
        });
    }
    if (chart.audio.vocals.size() > maximum_vocal_stems
        || chart.audio.instrumental.native().size()
            > maximum_audio_path_characters) {
        issues.push_back({
            ValidationSeverity::error,
            "audio manifest exceeds its path or stem safety limit",
            chart.audio.vocals.size(),
        });
    }
    for (std::size_t index = 0; index < chart.audio.vocals.size(); ++index) {
        if (chart.audio.vocals[index].native().size()
            > maximum_audio_path_characters) {
            issues.push_back({
                ValidationSeverity::error,
                "vocal path exceeds the 32768-character safety limit",
                index,
            });
        }
    }

    double previous_tempo_time = -std::numeric_limits<double>::infinity();
    double highest_valid_bpm = 120.0;
    for (std::size_t index = 0; index < chart.tempos.size(); ++index) {
        const auto& tempo = chart.tempos[index];
        if (!finite(tempo.time_ms)
            || tempo.time_ms < minimum_chart_time_ms
            || tempo.time_ms > maximum_chart_time_ms
            || !finite(tempo.bpm)
            || tempo.bpm <= 0.0) {
            issues.push_back({
                ValidationSeverity::error,
                "tempo time/BPM is outside the supported finite range",
                index,
            });
        }
        if (tempo.numerator == 0 || tempo.denominator == 0) {
            issues.push_back({
                ValidationSeverity::error,
                "tempo time signature values must be non-zero",
                index,
            });
        }
        if (finite(tempo.bpm)
            && tempo.bpm > 0.0) {
            highest_valid_bpm = std::max(highest_valid_bpm, tempo.bpm);
        }
        if (tempo.time_ms < previous_tempo_time) {
            issues.push_back({
                ValidationSeverity::error,
                "tempo changes are not sorted by time",
                index,
            });
        }
        previous_tempo_time = tempo.time_ms;
    }

    double previous_note_time = -std::numeric_limits<double>::infinity();
    double content_end_ms = 0.0;
    long double estimated_sustain_ticks = 0.0L;
    for (std::size_t index = 0; index < chart.notes.size(); ++index) {
        const auto& note = chart.notes[index];
        if (!finite(note.time_ms)
            || !finite(note.duration_ms)
            || note.time_ms < minimum_chart_time_ms
            || note.time_ms > maximum_chart_time_ms
            || note.duration_ms < 0.0
            || note.duration_ms > maximum_chart_time_ms
            || !finite(note.end_time_ms())
            || note.end_time_ms() > maximum_chart_time_ms) {
            issues.push_back({
                ValidationSeverity::error,
                "note time, duration, or end is outside the supported finite range",
                index,
            });
        }
        if (note.lane >= chart.key_count) {
            issues.push_back({
                ValidationSeverity::error,
                "note lane is outside key_count",
                index,
            });
        }
        if (note.kind.size() > maximum_chart_note_kind_bytes) {
            issues.push_back({
                ValidationSeverity::error,
                "note kind exceeds the 128-byte safety limit",
                index,
            });
        }
        if (note.payload_id >= chart.note_payloads.size()) {
            issues.push_back({
                ValidationSeverity::error,
                "note payload id is outside the payload dictionary",
                index,
            });
        }
        if (note.time_ms < previous_note_time) {
            issues.push_back({
                ValidationSeverity::error,
                "notes are not sorted by time",
                index,
            });
        }
        if (note.time_ms < -10'000.0) {
            issues.push_back({
                ValidationSeverity::warning,
                "note occurs more than ten seconds before the song",
                index,
            });
        }
        if (finite(note.end_time_ms())) {
            content_end_ms = std::max(content_end_ms, note.end_time_ms());
        }
        if (finite(note.duration_ms) && note.duration_ms > 0.0) {
            estimated_sustain_ticks +=
                static_cast<long double>(note.duration_ms)
                * static_cast<long double>(highest_valid_bpm)
                * 4.0L / 60'000.0L;
        }
        previous_note_time = note.time_ms;
    }

    double previous_event_time = -std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < chart.events.size(); ++index) {
        const auto& event = chart.events[index];
        if (!finite(event.time_ms)
            || event.time_ms < minimum_chart_time_ms
            || event.time_ms > maximum_chart_time_ms) {
            issues.push_back({
                ValidationSeverity::error,
                "event time is outside the supported finite range",
                index,
            });
        }
        if (event.name.empty()) {
            issues.push_back({
                ValidationSeverity::warning,
                "event has an empty name",
                index,
            });
        }
        if (event.name.size() > maximum_chart_event_name_bytes
            || event.value1.size() > maximum_chart_event_value_bytes
            || event.value2.size() > maximum_chart_event_value_bytes
            || event.payload_json.size() > maximum_chart_event_value_bytes) {
            issues.push_back({
                ValidationSeverity::error,
                "event text exceeds its safety limit",
                index,
            });
        }
        if (event.time_ms < previous_event_time) {
            issues.push_back({
                ValidationSeverity::error,
                "events are not sorted by time",
                index,
            });
        }
        if (finite(event.time_ms)) {
            content_end_ms = std::max(content_end_ms, event.time_ms);
        }
        previous_event_time = event.time_ms;
    }

    if (chart.note_payloads.empty() || !chart.note_payloads.front().empty()) {
        issues.push_back({
            ValidationSeverity::error,
            "note payload dictionary entry zero must be empty",
            0,
        });
    }
    if (chart.note_payloads.size() > chart.notes.size() + 1U) {
        issues.push_back({
            ValidationSeverity::error,
            "note payload dictionary has more entries than notes",
            0,
        });
    }
    for (std::size_t index = 0; index < chart.note_payloads.size(); ++index) {
        if (chart.note_payloads[index].size()
            > maximum_chart_note_payload_bytes) {
            issues.push_back({
                ValidationSeverity::error,
                "note payload exceeds the 4096-byte safety limit",
                index,
            });
        }
    }

    // Bound the vectors that a single catch-up update can generate. The
    // estimate is deliberately conservative: every sustain uses the chart's
    // highest BPM and the musical clock budgets four steps plus one beat.
    const long double estimated_musical_callbacks =
        static_cast<long double>(std::max(0.0, content_end_ms))
        * static_cast<long double>(highest_valid_bpm)
        * 5.0L / 60'000.0L + 5.0L;
    const long double estimated_work =
        static_cast<long double>(chart.notes.size())
        + static_cast<long double>(chart.events.size())
        + estimated_sustain_ticks
        + estimated_musical_callbacks;
    if (estimated_work
        > static_cast<long double>(maximum_generated_gameplay_events)) {
        issues.push_back({
            ValidationSeverity::error,
            "chart would generate too many gameplay events",
            static_cast<std::size_t>(std::min<long double>(
                estimated_work,
                static_cast<long double>(
                    std::numeric_limits<std::size_t>::max()
                )
            )),
        });
    }

    return std::move(issues).take();
}

std::string_view to_string(const ChartFormat format) noexcept {
    switch (format) {
    case ChartFormat::native:
        return "PulseForge";
    case ChartFormat::psych:
        return "Psych-compatible";
    case ChartFormat::denpa:
        return "DenpaEx";
    case ChartFormat::vslice:
        return "V-Slice";
    case ChartFormat::midi:
        return "MIDI/PPQN";
    case ChartFormat::pfm:
        return "PFM/PPQN";
    }
    return "unknown";
}

}  // namespace pulseforge
