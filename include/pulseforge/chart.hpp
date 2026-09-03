#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge {

// Shared loader/model limits. Parsers enforce these while consuming input so
// malformed or adversarial files cannot build an oversized intermediate model
// before Chart validation gets a chance to run.
inline constexpr std::size_t maximum_chart_tempo_changes = 100'000;
// These values bound only the fully materialized Chart representation. They are
// NOT engine-wide chart-size limits: application/launcher route charts beyond
// this fast-path budget to the bounded PFC1 streaming architecture instead.
// PatternRun is the constant-storage representation for huge repetitive runs.
inline constexpr std::size_t maximum_chart_notes = 5'000'000;
inline constexpr std::size_t maximum_chart_events = 250'000;
inline constexpr std::uint64_t maximum_chart_json_bytes =
    512ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t maximum_chart_note_kind_bytes = 128;
inline constexpr std::size_t maximum_chart_note_payload_bytes = 4'096;
inline constexpr std::size_t maximum_chart_event_name_bytes = 256;
inline constexpr std::size_t maximum_chart_event_value_bytes = 4'096;
// Gameplay, input actions, render layouts and every chart importer share this
// limit. Keeping it in the chart contract prevents the DOM, fast and PFC paths
// from silently accepting different mania modes.
inline constexpr std::uint16_t maximum_supported_key_count = 18U;

// Returns the longest prefix that consists entirely of well-formed UTF-8
// scalar values, contains no ASCII control characters and fits `maximum`.
// Editor text widgets use this before byte-bounded appends so a multi-byte
// note-type ID can never be cut in half and later serialized as invalid JSON.
[[nodiscard]] inline std::size_t bounded_chart_text_prefix_bytes(
    const std::string_view value,
    const std::size_t maximum
) noexcept {
    const auto* const bytes = reinterpret_cast<const unsigned char*>(
        value.data()
    );
    std::size_t index = 0U;
    while (index < value.size() && index < maximum) {
        const auto first = bytes[index];
        std::size_t length = 0U;
        if (first <= 0x7FU) {
            if (first < 0x20U || first == 0x7FU) {
                break;
            }
            length = 1U;
        } else if (first >= 0xC2U && first <= 0xDFU) {
            length = 2U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            length = 3U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            length = 4U;
        } else {
            break;
        }
        if (length > value.size() - index || length > maximum - index) {
            break;
        }
        if (length >= 2U) {
            const auto second = bytes[index + 1U];
            const bool second_valid = length == 2U
                ? second >= 0x80U && second <= 0xBFU
                : first == 0xE0U
                    ? second >= 0xA0U && second <= 0xBFU
                    : first == 0xEDU
                        ? second >= 0x80U && second <= 0x9FU
                        : first == 0xF0U
                            ? second >= 0x90U && second <= 0xBFU
                            : first == 0xF4U
                                ? second >= 0x80U && second <= 0x8FU
                                : second >= 0x80U && second <= 0xBFU;
            if (!second_valid) {
                break;
            }
        }
        bool continuations_valid = true;
        for (std::size_t offset = 2U; offset < length; ++offset) {
            if (bytes[index + offset] < 0x80U
                || bytes[index + offset] > 0xBFU) {
                continuations_valid = false;
                break;
            }
        }
        if (!continuations_valid) {
            break;
        }
        index += length;
    }
    return index;
}

[[nodiscard]] inline bool valid_chart_note_kind_text(
    const std::string_view value
) noexcept {
    return value.size() <= maximum_chart_note_kind_bytes
        && bounded_chart_text_prefix_bytes(value, value.size()) == value.size();
}

enum class NoteOwner : std::uint8_t {
    opponent = 0,
    player = 1,
    // PULSEFORGE_P1_4_0_SECONDARY_OPPONENT_OWNER_V1
    // DenpaEx `Third Strum` is a real AI-owned third strumline, not an alias
    // for the primary opponent.  Keep it in the canonical chart model so
    // gameplay, rendering, characters and Lua can preserve that identity.
    secondary_opponent = 2,
};

struct Note {
    double time_ms{};
    double duration_ms{};
    std::uint16_t lane{};
    NoteOwner owner{NoteOwner::player};
    std::string kind{"normal"};
    // Index into Chart::note_payloads. Zero is the canonical empty payload,
    // avoiding a std::string allocation in every Note.
    std::uint32_t payload_id{};

    [[nodiscard]] double end_time_ms() const noexcept {
        return time_ms + duration_ms;
    }
};

struct TempoChange {
    double time_ms{};
    double bpm{120.0};
    std::uint16_t numerator{4};
    std::uint16_t denominator{4};
};

struct ChartEvent {
    double time_ms{};
    std::string name;
    std::string value1;
    std::string value2;
    // Canonical JSON for engines whose event payload is not limited to two
    // strings (notably V-Slice). value1/value2 remain the Lua compatibility
    // projection.
    std::string payload_json;
};

struct AudioManifest {
    std::filesystem::path instrumental;
    std::vector<std::filesystem::path> vocals;
};

enum class ChartFormat : std::uint8_t {
    native,
    psych,
    denpa,
    vslice,
    midi,
    pfm,
};

struct Chart {
    std::string title{"Untitled"};
    std::string artist{"Unknown"};
    std::string charter{"Unknown"};
    std::string difficulty{"normal"};
    std::string stage_id;
    std::string player_character;
    std::string opponent_character;
    // PULSEFORGE_P1_4_0_SECONDARY_OPPONENT_METADATA_V1
    std::string secondary_opponent_character;
    bool secondary_opponent_enabled{};
    std::string girlfriend_character;
    std::string note_style;
    ChartFormat source_format{ChartFormat::native};
    std::uint16_t key_count{4};
    double chart_scroll_speed{1.0};
    AudioManifest audio;
    std::vector<TempoChange> tempos;
    std::vector<Note> notes;
    std::vector<ChartEvent> events;
    std::vector<std::string> note_payloads{""};

    void normalize();
    [[nodiscard]] double duration_ms() const noexcept;
};

enum class ValidationSeverity : std::uint8_t {
    warning,
    error,
};

struct ValidationIssue {
    ValidationSeverity severity{ValidationSeverity::error};
    std::string message;
    std::size_t item_index{};
};

[[nodiscard]] std::vector<ValidationIssue> validate_chart(const Chart& chart);
[[nodiscard]] std::string_view to_string(ChartFormat format) noexcept;

}  // namespace pulseforge
