#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace pulseforge {

enum class NoteAnimationTarget : std::uint8_t {
    owner,
    player,
    opponent,
    girlfriend,
};

enum class NoteAnimationCue : std::uint8_t {
    sing,
    hey,
    none,
};

enum class NoteCrossFadeTarget : std::uint8_t {
    none,
    performer,
    girlfriend,
};

struct NoteTypeHealth {
    double hit{0.023};
    double miss{0.0475};
    bool hit_causes_miss{false};

    [[nodiscard]] bool operator==(const NoteTypeHealth&) const = default;
};

struct NoteTypeAnimation {
    NoteAnimationTarget target{NoteAnimationTarget::owner};
    NoteAnimationCue cue{NoteAnimationCue::sing};
    std::string suffix;

    [[nodiscard]] bool operator==(const NoteTypeAnimation&) const = default;
};

struct NoteTypeVisual {
    // Asset references are logical IDs, never filesystem paths.
    std::string texture_id;
    std::optional<std::array<std::uint8_t, 3U>> rgb;
    float alpha{1.0F};
    float scale{1.0F};

    [[nodiscard]] bool operator==(const NoteTypeVisual&) const = default;
};

struct NoteTypeFeedback {
    bool splash_enabled{true};
    std::string splash_id;
    bool hitsound_enabled{false};
    std::string hitsound_id;

    [[nodiscard]] bool operator==(const NoteTypeFeedback&) const = default;
};

struct NoteTypeSustainPolicy {
    // `enabled == false` asks the chart adapter to treat duration as zero.
    bool enabled{true};
    // When false, only the head uses this definition and the tail uses normal
    // note semantics.
    bool inherits_type{true};
    std::optional<double> miss_health;
    std::optional<bool> hit_causes_miss;

    [[nodiscard]] bool operator==(const NoteTypeSustainPolicy&) const = default;
};

using NoteTypeExtraValue = std::variant<
    std::monostate,
    bool,
    std::int64_t,
    double,
    std::string
>;

struct NoteTypeDefinition {
    std::string id{"normal"};
    NoteTypeHealth health;
    NoteTypeAnimation animation;
    NoteTypeVisual visual;
    NoteTypeFeedback feedback;
    NoteTypeSustainPolicy sustain;
    float scroll_multiplier{1.0F};
    NoteCrossFadeTarget cross_fade{NoteCrossFadeTarget::none};
    std::map<std::string, NoteTypeExtraValue, std::less<>> extra_data;
    bool builtin{false};

    [[nodiscard]] bool operator==(const NoteTypeDefinition&) const = default;
};

struct NoteTypeParseLimits {
    std::size_t maximum_source_bytes{64U * 1024U};
    std::size_t maximum_lines{512U};
    std::size_t maximum_line_bytes{1'024U};
    std::size_t maximum_diagnostics{64U};
    std::size_t maximum_extra_entries{32U};
    std::size_t maximum_extra_key_bytes{64U};
    std::size_t maximum_extra_value_bytes{256U};
    std::size_t maximum_extra_total_bytes{4U * 1'024U};
};

struct NoteTypeParseDiagnostic {
    std::size_t line{};
    std::string property;
    std::string message;
};

struct NoteTypeParseResult {
    std::optional<NoteTypeDefinition> definition;
    std::vector<NoteTypeParseDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept {
        return definition.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return ok();
    }
};

// Parses H-Slice-compatible `property.path = value` and
// `property.path: value` text without reflection. Every accepted property is
// mapped to an explicit field in NoteTypeDefinition.
[[nodiscard]] NoteTypeParseResult parse_note_type_text(
    std::string_view id,
    std::string_view source,
    const NoteTypeParseLimits& limits = {}
);

enum class NoteTypeReplacePolicy : std::uint8_t {
    reject_existing,
    replace_custom,
};

struct ResolvedNoteType {
    // Exact chart spelling. Serializers must write this value, even when
    // `used_fallback` is true.
    std::string chart_id;
    const NoteTypeDefinition* definition{};
    bool used_fallback{false};

    [[nodiscard]] std::string_view round_trip_id() const noexcept {
        return chart_id;
    }

    [[nodiscard]] const NoteTypeDefinition& behavior() const noexcept {
        return *definition;
    }
};

class NoteTypeRegistry final {
public:
    NoteTypeRegistry();

    [[nodiscard]] const NoteTypeDefinition* find(std::string_view id) const noexcept;
    [[nodiscard]] ResolvedNoteType resolve(std::string_view chart_id) const;
    [[nodiscard]] std::vector<std::string> ids() const;

    // Built-ins cannot be replaced. A custom definition can only be replaced
    // when the caller opts in explicitly, which makes mod precedence visible.
    [[nodiscard]] bool register_definition(
        NoteTypeDefinition definition,
        NoteTypeReplacePolicy policy = NoteTypeReplacePolicy::reject_existing,
        std::string* error = nullptr,
        const NoteTypeParseLimits& storage_limits = {}
    );

    [[nodiscard]] bool register_text(
        std::string_view id,
        std::string_view source,
        NoteTypeReplacePolicy policy = NoteTypeReplacePolicy::reject_existing,
        std::vector<NoteTypeParseDiagnostic>* diagnostics = nullptr,
        std::string* error = nullptr,
        const NoteTypeParseLimits& limits = {}
    );

private:
    std::map<std::string, NoteTypeDefinition, std::less<>> definitions_;
};

[[nodiscard]] std::span<const std::string_view> builtin_note_type_ids() noexcept;

// Allocation-free compatibility classifier for import/hot paths. It accepts
// `Hurt Note` plus historical hurt, hurt-note, hurt_note, and mine spellings
// with ASCII case/outer whitespace variations.
[[nodiscard]] bool builtin_note_type_causes_miss(std::string_view id) noexcept;

}  // namespace pulseforge
