#include "pulseforge/note_types.hpp"
#include "pulseforge/ascii_number.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace pulseforge {
namespace {

constexpr double minimum_health_delta = -2.0;
constexpr double maximum_health_delta = 2.0;
constexpr float minimum_scale = 0.05F;
constexpr float maximum_scale = 8.0F;
constexpr float minimum_scroll_multiplier = 0.05F;
constexpr float maximum_scroll_multiplier = 16.0F;
constexpr std::size_t maximum_definition_id_bytes = 128U;
constexpr std::size_t maximum_logical_id_bytes = 128U;
constexpr std::size_t maximum_animation_suffix_bytes = 64U;

constexpr std::array<std::string_view, 8U> builtin_ids{
    "normal",
    "Alt Animation",
    "GF Sing",
    "Hurt Note",
    "Hey!",
    "No Animation",
    "Cross Fade",
    "GF Cross Fade",
};

[[nodiscard]] constexpr bool ascii_space(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n'
        || value == '\f' || value == '\v';
}

[[nodiscard]] std::string_view trim(const std::string_view value) noexcept {
    std::size_t first = 0U;
    while (first < value.size() && ascii_space(value[first])) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && ascii_space(value[last - 1U])) {
        --last;
    }
    return value.substr(first, last - first);
}

[[nodiscard]] constexpr char lower_ascii_char(const char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value + ('a' - 'A'))
        : value;
}

[[nodiscard]] std::string lower_ascii(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        result.push_back(lower_ascii_char(character));
    }
    return result;
}

[[nodiscard]] bool equal_ascii_case_insensitive(
    const std::string_view left,
    const std::string_view right
) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (lower_ascii_char(left[index]) != lower_ascii_char(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool has_ascii_control(const std::string_view value) noexcept {
    return std::any_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 32U || byte == 127U;
    });
}

[[nodiscard]] bool valid_definition_id(const std::string_view value) noexcept {
    if (value.empty() || value.size() > maximum_definition_id_bytes
        || trim(value).size() != value.size() || has_ascii_control(value)
        || value.find('/') != std::string_view::npos
        || value.find('\\') != std::string_view::npos
        || value.find(':') != std::string_view::npos
        || value.find("..") != std::string_view::npos) {
        return false;
    }
    return true;
}

[[nodiscard]] bool valid_logical_id(const std::string_view value) noexcept {
    if (value.size() > maximum_logical_id_bytes || has_ascii_control(value)
        || value.find('/') != std::string_view::npos
        || value.find('\\') != std::string_view::npos
        || value.find(':') != std::string_view::npos
        || value.find("..") != std::string_view::npos) {
        return false;
    }
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= 128U) {
            continue;
        }
        const bool allowed = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9') || character == '_'
            || character == '-' || character == '.' || character == ' ';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_animation_suffix(const std::string_view value) noexcept {
    if (value.size() > maximum_animation_suffix_bytes || has_ascii_control(value)) {
        return false;
    }
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= 128U) {
            continue;
        }
        const bool allowed = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9') || character == '_'
            || character == '-' || character == '+';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_extra_key(
    const std::string_view value,
    const std::size_t maximum_size
) noexcept {
    if (value.empty() || value.size() > maximum_size
        || value.front() == '.' || value.back() == '.'
        || value.find("..") != std::string_view::npos) {
        return false;
    }
    for (const char character : value) {
        const bool allowed = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9') || character == '_'
            || character == '-' || character == '.';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool parse_bool(
    const std::string_view source,
    bool& destination
) noexcept {
    if (equal_ascii_case_insensitive(source, "true") || source == "1") {
        destination = true;
        return true;
    }
    if (equal_ascii_case_insensitive(source, "false") || source == "0") {
        destination = false;
        return true;
    }
    return false;
}

template <typename Number>
[[nodiscard]] bool parse_number(
    const std::string_view source,
    Number& destination
) noexcept {
    if constexpr (std::is_floating_point_v<Number>) {
        return parse_ascii_floating(source, destination);
    } else {
        const auto* first = source.data();
        const auto* last = source.data() + source.size();
        const auto parsed = std::from_chars(first, last, destination);
        if (parsed.ec != std::errc{} || parsed.ptr != last) {
            return false;
        }
        return true;
    }
}

[[nodiscard]] bool parse_text_value(
    const std::string_view source,
    std::string& destination
) {
    const auto value = trim(source);
    if (value.size() >= 2U
        && ((value.front() == '"' && value.back() == '"')
            || (value.front() == '\'' && value.back() == '\''))) {
        destination.assign(value.substr(1U, value.size() - 2U));
        return !has_ascii_control(destination);
    }
    if ((!value.empty() && (value.front() == '"' || value.front() == '\''))
        || (!value.empty() && (value.back() == '"' || value.back() == '\''))) {
        return false;
    }
    destination.assign(value);
    return !has_ascii_control(destination);
}

void assign_error(std::string* const error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

[[nodiscard]] bool bounded_health(const double value) noexcept {
    return std::isfinite(value) && value >= minimum_health_delta
        && value <= maximum_health_delta;
}

[[nodiscard]] constexpr bool valid_animation_target(
    const NoteAnimationTarget value
) noexcept {
    switch (value) {
    case NoteAnimationTarget::owner:
    case NoteAnimationTarget::player:
    case NoteAnimationTarget::opponent:
    case NoteAnimationTarget::girlfriend:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool valid_animation_cue(
    const NoteAnimationCue value
) noexcept {
    switch (value) {
    case NoteAnimationCue::sing:
    case NoteAnimationCue::hey:
    case NoteAnimationCue::none:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool valid_cross_fade_target(
    const NoteCrossFadeTarget value
) noexcept {
    switch (value) {
    case NoteCrossFadeTarget::none:
    case NoteCrossFadeTarget::performer:
    case NoteCrossFadeTarget::girlfriend:
        return true;
    }
    return false;
}

[[nodiscard]] bool validate_definition(
    const NoteTypeDefinition& definition,
    std::string* const error,
    const NoteTypeParseLimits& storage_limits = {}
) {
    if (!valid_definition_id(definition.id)) {
        assign_error(error, "note type ID is empty, unsafe, or exceeds 128 bytes");
        return false;
    }
    if (!bounded_health(definition.health.hit)
        || !bounded_health(definition.health.miss)) {
        assign_error(error, "health deltas must be finite and within [-2, 2]");
        return false;
    }
    if (definition.sustain.miss_health.has_value()
        && !bounded_health(*definition.sustain.miss_health)) {
        assign_error(error, "sustain miss health must be finite and within [-2, 2]");
        return false;
    }
    if (!valid_animation_suffix(definition.animation.suffix)) {
        assign_error(error, "animation suffix is not a bounded logical suffix");
        return false;
    }
    if (!valid_animation_target(definition.animation.target)
        || !valid_animation_cue(definition.animation.cue)
        || !valid_cross_fade_target(definition.cross_fade)) {
        assign_error(error, "note type contains an invalid enum value");
        return false;
    }
    if (!valid_logical_id(definition.visual.texture_id)
        || !valid_logical_id(definition.feedback.splash_id)
        || !valid_logical_id(definition.feedback.hitsound_id)) {
        assign_error(error, "texture, splash, and hitsound values must be logical IDs");
        return false;
    }
    if (!std::isfinite(definition.visual.alpha)
        || definition.visual.alpha < 0.0F || definition.visual.alpha > 1.0F) {
        assign_error(error, "visual alpha must be finite and within [0, 1]");
        return false;
    }
    if (!std::isfinite(definition.visual.scale)
        || definition.visual.scale < minimum_scale
        || definition.visual.scale > maximum_scale) {
        assign_error(error, "visual scale must be finite and within [0.05, 8]");
        return false;
    }
    if (!std::isfinite(definition.scroll_multiplier)
        || definition.scroll_multiplier < minimum_scroll_multiplier
        || definition.scroll_multiplier > maximum_scroll_multiplier) {
        assign_error(error, "scroll multiplier must be finite and within [0.05, 16]");
        return false;
    }

    if (definition.extra_data.size() > storage_limits.maximum_extra_entries) {
        assign_error(error, "extraData contains too many entries");
        return false;
    }
    std::size_t total = 0U;
    for (const auto& [key, value] : definition.extra_data) {
        if (!valid_extra_key(key, storage_limits.maximum_extra_key_bytes)) {
            assign_error(error, "extraData contains an invalid key");
            return false;
        }
        std::size_t value_size = 0U;
        if (const auto* text = std::get_if<std::string>(&value)) {
            value_size = text->size();
            if (has_ascii_control(*text)) {
                assign_error(error, "extraData strings cannot contain control bytes");
                return false;
            }
        } else {
            value_size = sizeof(value);
        }
        if (value_size > storage_limits.maximum_extra_value_bytes
            || key.size() > storage_limits.maximum_extra_total_bytes - total
            || value_size > storage_limits.maximum_extra_total_bytes
                - total - key.size()) {
            assign_error(error, "extraData exceeds its bounded storage budget");
            return false;
        }
        total += key.size() + value_size;
    }
    return true;
}

[[nodiscard]] NoteTypeDefinition make_builtin(const std::string_view id) {
    NoteTypeDefinition result;
    result.id.assign(id);
    result.builtin = true;
    if (id == "Alt Animation") {
        result.animation.suffix = "-alt";
    } else if (id == "GF Sing") {
        result.animation.target = NoteAnimationTarget::girlfriend;
    } else if (id == "Hurt Note") {
        result.health.miss = 0.3;
        result.health.hit_causes_miss = true;
        result.visual.texture_id = "HURTNOTE_assets";
        result.feedback.splash_id = "HURTnoteSplashes";
        result.sustain.miss_health = 0.1;
        result.sustain.hit_causes_miss = true;
    } else if (id == "Hey!") {
        result.animation.cue = NoteAnimationCue::hey;
    } else if (id == "No Animation") {
        result.animation.cue = NoteAnimationCue::none;
    } else if (id == "Cross Fade") {
        result.cross_fade = NoteCrossFadeTarget::performer;
    } else if (id == "GF Cross Fade") {
        result.animation.target = NoteAnimationTarget::girlfriend;
        result.cross_fade = NoteCrossFadeTarget::girlfriend;
    }
    return result;
}

[[nodiscard]] const std::string_view* builtin_id_for(
    const std::string_view id
) noexcept {
    const auto cleaned = trim(id);
    for (const auto& candidate : builtin_ids) {
        if (equal_ascii_case_insensitive(cleaned, candidate)) {
            return &candidate;
        }
    }
    if (builtin_note_type_causes_miss(cleaned)) {
        return &builtin_ids[3U];
    }
    return nullptr;
}

class TextParser final {
public:
    TextParser(
        const std::string_view id,
        const std::string_view source,
        const NoteTypeParseLimits& limits
    ) : source_(source), limits_(limits) {
        definition_.id.assign(id);
    }

    [[nodiscard]] NoteTypeParseResult parse() {
        if (!valid_definition_id(definition_.id)) {
            error(0U, "id", "note type ID is empty, unsafe, or exceeds 128 bytes");
        }
        if (source_.size() > limits_.maximum_source_bytes) {
            error(0U, {}, "note type source exceeds the configured byte budget");
            return finish();
        }

        std::size_t offset = 0U;
        std::size_t line_number = 1U;
        while (offset < source_.size()) {
            if (line_number > limits_.maximum_lines) {
                error(line_number, {}, "note type source exceeds the line budget");
                break;
            }
            const auto newline = source_.find('\n', offset);
            const auto end = newline == std::string_view::npos
                ? source_.size()
                : newline;
            auto line = source_.substr(offset, end - offset);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1U);
            }
            if (line_number == 1U && line.size() >= 3U
                && static_cast<unsigned char>(line[0]) == 0xEFU
                && static_cast<unsigned char>(line[1]) == 0xBBU
                && static_cast<unsigned char>(line[2]) == 0xBFU) {
                line.remove_prefix(3U);
            }
            parse_line(line_number, line);
            if (newline == std::string_view::npos) {
                break;
            }
            offset = newline + 1U;
            ++line_number;
        }
        return finish();
    }

private:
    void error(
        const std::size_t line,
        const std::string_view property,
        std::string message
    ) {
        failed_ = true;
        if (result_.diagnostics.size() >= limits_.maximum_diagnostics) {
            return;
        }
        constexpr std::size_t maximum_reported_property_bytes = 128U;
        result_.diagnostics.push_back({
            line,
            std::string(property.substr(0U, maximum_reported_property_bytes)),
            std::move(message),
        });
    }

    [[nodiscard]] bool claim(
        const std::size_t line,
        const std::string_view property,
        std::string semantic_key
    ) {
        if (!seen_.insert(std::move(semantic_key)).second) {
            error(line, property, "duplicate property or alias");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool number(
        const std::size_t line,
        const std::string_view property,
        const std::string_view value,
        double& destination,
        const double minimum,
        const double maximum
    ) {
        double parsed = 0.0;
        if (!parse_number(value, parsed) || parsed < minimum || parsed > maximum) {
            error(line, property, "expected a finite number within the allowed range");
            return false;
        }
        destination = parsed;
        return true;
    }

    [[nodiscard]] bool floating(
        const std::size_t line,
        const std::string_view property,
        const std::string_view value,
        float& destination,
        const float minimum,
        const float maximum
    ) {
        float parsed = 0.0F;
        if (!parse_number(value, parsed) || parsed < minimum || parsed > maximum) {
            error(line, property, "expected a finite number within the allowed range");
            return false;
        }
        destination = parsed;
        return true;
    }

    [[nodiscard]] bool boolean(
        const std::size_t line,
        const std::string_view property,
        const std::string_view value,
        bool& destination
    ) {
        if (!parse_bool(value, destination)) {
            error(line, property, "expected true, false, 1, or 0");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool logical_id(
        const std::size_t line,
        const std::string_view property,
        const std::string_view value,
        std::string& destination
    ) {
        std::string parsed;
        if (!parse_text_value(value, parsed) || !valid_logical_id(parsed)) {
            error(line, property, "expected a logical ID, not a path or arbitrary property");
            return false;
        }
        destination = std::move(parsed);
        return true;
    }

    void parse_extra(
        const std::size_t line,
        const std::string_view property,
        const std::string_view original_property,
        const std::string_view value
    ) {
        constexpr std::string_view prefix{"extradata."};
        const auto key = original_property.substr(prefix.size());
        if (!valid_extra_key(key, limits_.maximum_extra_key_bytes)) {
            error(line, property, "extraData key is empty, unsafe, or too long");
            return;
        }
        if (definition_.extra_data.size() >= limits_.maximum_extra_entries) {
            error(line, property, "extraData entry budget exceeded");
            return;
        }
        if (!claim(line, property, "extra:" + std::string(key))) {
            return;
        }
        const auto scalar = trim(value);
        if (scalar.size() > limits_.maximum_extra_value_bytes) {
            error(line, property, "extraData value exceeds its byte budget");
            return;
        }
        if (key.size() > limits_.maximum_extra_total_bytes - extra_total_
            || scalar.size() > limits_.maximum_extra_total_bytes
                - extra_total_ - key.size()) {
            error(line, property, "extraData total byte budget exceeded");
            return;
        }

        NoteTypeExtraValue parsed;
        std::string text_value;
        if (scalar == "null") {
            parsed = std::monostate{};
    } else {
            std::int64_t integer_value = 0;
            double real_value = 0.0;
            if (equal_ascii_case_insensitive(scalar, "true")) {
                parsed = true;
            } else if (equal_ascii_case_insensitive(scalar, "false")) {
                parsed = false;
            } else if (parse_number(scalar, integer_value)) {
                parsed = integer_value;
            } else if (parse_number(scalar, real_value)) {
                parsed = real_value;
            } else if (parse_text_value(scalar, text_value)
                && !text_value.empty()) {
                parsed = std::move(text_value);
            } else {
                error(line, property, "extraData value is not a bounded scalar");
                return;
            }
        }
        definition_.extra_data.emplace(std::string(key), std::move(parsed));
        extra_total_ += key.size() + scalar.size();
    }

    void parse_line(const std::size_t line_number, std::string_view line) {
        if (line.size() > limits_.maximum_line_bytes) {
            error(line_number, {}, "line exceeds the configured byte budget");
            return;
        }
        line = trim(line);
        if (line.empty() || line.starts_with('#') || line.starts_with(';')
            || line.starts_with("//")) {
            return;
        }

        const auto equal = line.find('=');
        const auto colon = line.find(':');
        std::size_t separator = std::string_view::npos;
        if (equal != std::string_view::npos && colon != std::string_view::npos) {
            separator = (std::min)(equal, colon);
        } else if (equal != std::string_view::npos) {
            separator = equal;
        } else {
            separator = colon;
        }
        if (separator == std::string_view::npos) {
            error(line_number, {}, "expected property.path = value or property.path: value");
            return;
        }

        const auto original_property = trim(line.substr(0U, separator));
        const auto value = trim(line.substr(separator + 1U));
        if (original_property.empty()) {
            error(line_number, {}, "property name cannot be empty");
            return;
        }
        const auto property = lower_ascii(original_property);
        if (property.starts_with("extradata.")) {
            parse_extra(line_number, property, original_property, value);
            return;
        }

        if (property == "health.hit" || property == "hithealth") {
            if (claim(line_number, property, "health.hit")) {
                static_cast<void>(number(
                    line_number, property, value, definition_.health.hit,
                    minimum_health_delta, maximum_health_delta
                ));
            }
        } else if (property == "health.miss" || property == "misshealth") {
            if (claim(line_number, property, "health.miss")) {
                static_cast<void>(number(
                    line_number, property, value, definition_.health.miss,
                    minimum_health_delta, maximum_health_delta
                ));
            }
        } else if (property == "health.hitcausesmiss"
            || property == "hitcausesmiss") {
            if (claim(line_number, property, "health.hitcausesmiss")) {
                static_cast<void>(boolean(
                    line_number, property, value,
                    definition_.health.hit_causes_miss
                ));
            }
        } else if (property == "animation.target") {
            if (!claim(line_number, property, "animation.target")) {
                return;
            }
            std::string parsed_target;
            if (!parse_text_value(value, parsed_target)) {
                error(line_number, property, "invalid quoted animation target");
                return;
            }
            const auto target = lower_ascii(parsed_target);
            if (target == "owner" || target == "automatic" || target == "auto") {
                definition_.animation.target = NoteAnimationTarget::owner;
            } else if (target == "player") {
                definition_.animation.target = NoteAnimationTarget::player;
            } else if (target == "opponent") {
                definition_.animation.target = NoteAnimationTarget::opponent;
            } else if (target == "girlfriend" || target == "gf") {
                definition_.animation.target = NoteAnimationTarget::girlfriend;
            } else {
                error(line_number, property, "unknown animation target");
            }
        } else if (property == "animation.suffix" || property == "animsuffix") {
            if (!claim(line_number, property, "animation.suffix")) {
                return;
            }
            std::string parsed;
            if (!parse_text_value(value, parsed) || !valid_animation_suffix(parsed)) {
                error(line_number, property, "invalid animation suffix");
            } else {
                definition_.animation.suffix = std::move(parsed);
            }
        } else if (property == "animation.cue") {
            if (!claim(line_number, property, "animation.cue")) {
                return;
            }
            std::string parsed_cue;
            if (!parse_text_value(value, parsed_cue)) {
                error(line_number, property, "invalid quoted animation cue");
                return;
            }
            const auto cue = lower_ascii(parsed_cue);
            if (cue == "sing") {
                definition_.animation.cue = NoteAnimationCue::sing;
            } else if (cue == "hey") {
                definition_.animation.cue = NoteAnimationCue::hey;
            } else if (cue == "none") {
                definition_.animation.cue = NoteAnimationCue::none;
            } else {
                error(line_number, property, "animation cue must be sing, hey, or none");
            }
        } else if (property == "gfnote") {
            if (!claim(line_number, property, "animation.target")) {
                return;
            }
            bool gf_note = false;
            if (boolean(line_number, property, value, gf_note)) {
                definition_.animation.target = gf_note
                    ? NoteAnimationTarget::girlfriend
                    : NoteAnimationTarget::owner;
            }
        } else if (property == "noanimation") {
            if (!claim(line_number, property, "animation.cue")) {
                return;
            }
            bool no_animation = false;
            if (boolean(line_number, property, value, no_animation)) {
                definition_.animation.cue = no_animation
                    ? NoteAnimationCue::none
                    : NoteAnimationCue::sing;
            }
        } else if (property == "effect.crossfade" || property == "crossfade") {
            if (!claim(line_number, property, "effect.crossfade")) {
                return;
            }
            std::string parsed_target;
            if (!parse_text_value(value, parsed_target)) {
                error(line_number, property, "invalid quoted cross fade target");
                return;
            }
            const auto target = lower_ascii(parsed_target);
            if (target == "none" || target == "false" || target == "off") {
                definition_.cross_fade = NoteCrossFadeTarget::none;
            } else if (target == "performer" || target == "true" || target == "on") {
                definition_.cross_fade = NoteCrossFadeTarget::performer;
            } else if (target == "girlfriend" || target == "gf") {
                definition_.cross_fade = NoteCrossFadeTarget::girlfriend;
            } else {
                error(line_number, property, "cross fade target is invalid");
            }
        } else if (property == "texture" || property == "texture.id"
            || property == "visual.texture") {
            if (claim(line_number, property, "visual.texture")) {
                static_cast<void>(logical_id(
                    line_number, property, value, definition_.visual.texture_id
                ));
            }
        } else if (property == "color.r" || property == "rgb.r"
            || property == "rgbshader.r") {
            parse_color(line_number, property, value, 0U);
        } else if (property == "color.g" || property == "rgb.g"
            || property == "rgbshader.g") {
            parse_color(line_number, property, value, 1U);
        } else if (property == "color.b" || property == "rgb.b"
            || property == "rgbshader.b") {
            parse_color(line_number, property, value, 2U);
        } else if (property == "alpha" || property == "visual.alpha") {
            if (claim(line_number, property, "visual.alpha")) {
                static_cast<void>(floating(
                    line_number, property, value, definition_.visual.alpha,
                    0.0F, 1.0F
                ));
            }
        } else if (property == "scale" || property == "visual.scale") {
            if (claim(line_number, property, "visual.scale")) {
                static_cast<void>(floating(
                    line_number, property, value, definition_.visual.scale,
                    minimum_scale, maximum_scale
                ));
            }
        } else if (property == "splash.enabled") {
            if (claim(line_number, property, "splash.enabled")) {
                static_cast<void>(boolean(
                    line_number, property, value,
                    definition_.feedback.splash_enabled
                ));
            }
        } else if (property == "splash.id" || property == "notesplashtexture") {
            if (claim(line_number, property, "splash.id")) {
                static_cast<void>(logical_id(
                    line_number, property, value, definition_.feedback.splash_id
                ));
            }
        } else if (property == "hitsound.enabled") {
            if (claim(line_number, property, "hitsound.enabled")) {
                static_cast<void>(boolean(
                    line_number, property, value,
                    definition_.feedback.hitsound_enabled
                ));
            }
        } else if (property == "hitsound.id" || property == "hitsound") {
            if (claim(line_number, property, "hitsound.id")) {
                static_cast<void>(logical_id(
                    line_number, property, value, definition_.feedback.hitsound_id
                ));
            }
        } else if (property == "sustain.enabled" || property == "sustain.allowed") {
            if (claim(line_number, property, "sustain.enabled")) {
                static_cast<void>(boolean(
                    line_number, property, value, definition_.sustain.enabled
                ));
            }
        } else if (property == "sustain.inheritstype"
            || property == "sustain.inherits") {
            if (claim(line_number, property, "sustain.inheritstype")) {
                static_cast<void>(boolean(
                    line_number, property, value,
                    definition_.sustain.inherits_type
                ));
            }
        } else if (property == "sustain.misshealth") {
            if (!claim(line_number, property, "sustain.misshealth")) {
                return;
            }
            if (equal_ascii_case_insensitive(value, "null")) {
                definition_.sustain.miss_health.reset();
            } else {
                double parsed = 0.0;
                if (number(
                    line_number, property, value, parsed,
                    minimum_health_delta, maximum_health_delta
                )) {
                    definition_.sustain.miss_health = parsed;
                }
            }
        } else if (property == "sustain.hitcausesmiss") {
            if (!claim(line_number, property, "sustain.hitcausesmiss")) {
                return;
            }
            if (equal_ascii_case_insensitive(value, "null")) {
                definition_.sustain.hit_causes_miss.reset();
            } else {
                bool parsed = false;
                if (boolean(line_number, property, value, parsed)) {
                    definition_.sustain.hit_causes_miss = parsed;
                }
            }
        } else if (property == "scroll.multiplier"
            || property == "scrollmultiplier" || property == "multspeed") {
            if (claim(line_number, property, "scroll.multiplier")) {
                static_cast<void>(floating(
                    line_number, property, value, definition_.scroll_multiplier,
                    minimum_scroll_multiplier, maximum_scroll_multiplier
                ));
            }
        } else {
            error(line_number, property, "property is not in the note-type whitelist");
        }
    }

    void parse_color(
        const std::size_t line,
        const std::string_view property,
        const std::string_view value,
        const std::size_t component
    ) {
        constexpr std::array<std::string_view, 3U> semantic_keys{
            "visual.color.r", "visual.color.g", "visual.color.b",
        };
        const std::string semantic(semantic_keys[component]);
        if (!claim(line, property, semantic)) {
            return;
        }
        unsigned int parsed = 0U;
        if (!parse_number(value, parsed) || parsed > 255U) {
            error(line, property, "RGB component must be an integer within [0, 255]");
            return;
        }
        if (!definition_.visual.rgb.has_value()) {
            definition_.visual.rgb = std::array<std::uint8_t, 3U>{255U, 255U, 255U};
        }
        (*definition_.visual.rgb)[component] = static_cast<std::uint8_t>(parsed);
    }

    [[nodiscard]] NoteTypeParseResult finish() {
        if (!failed_) {
            std::string validation_error;
            if (!validate_definition(definition_, &validation_error, limits_)) {
                error(0U, {}, std::move(validation_error));
            }
        }
        if (!failed_) {
            definition_.builtin = false;
            result_.definition = std::move(definition_);
        }
        return std::move(result_);
    }

    std::string_view source_;
    const NoteTypeParseLimits& limits_;
    NoteTypeDefinition definition_;
    NoteTypeParseResult result_;
    std::set<std::string, std::less<>> seen_;
    std::size_t extra_total_{};
    bool failed_{false};
};

}  // namespace

NoteTypeParseResult parse_note_type_text(
    const std::string_view id,
    const std::string_view source,
    const NoteTypeParseLimits& limits
) {
    return TextParser(id, source, limits).parse();
}

NoteTypeRegistry::NoteTypeRegistry() {
    for (const auto id : builtin_ids) {
        auto definition = make_builtin(id);
        auto key = definition.id;
        definitions_.emplace(std::move(key), std::move(definition));
    }
}

const NoteTypeDefinition* NoteTypeRegistry::find(
    const std::string_view id
) const noexcept {
    if (id.empty()) {
        const auto normal = definitions_.find("normal");
        return normal == definitions_.end() ? nullptr : &normal->second;
    }
    const auto exact = definitions_.find(id);
    if (exact != definitions_.end()) {
        return &exact->second;
    }
    if (const auto* builtin_id = builtin_id_for(id); builtin_id != nullptr) {
        const auto builtin = definitions_.find(*builtin_id);
        if (builtin != definitions_.end()) {
            return &builtin->second;
        }
    }
    return nullptr;
}

ResolvedNoteType NoteTypeRegistry::resolve(const std::string_view chart_id) const {
    if (const auto* definition = find(chart_id); definition != nullptr) {
        return {std::string(chart_id), definition, false};
    }
    const auto normal = definitions_.find("normal");
    return {
        std::string(chart_id),
        normal == definitions_.end() ? nullptr : &normal->second,
        true,
    };
}

std::vector<std::string> NoteTypeRegistry::ids() const {
    std::vector<std::string> result;
    result.reserve(definitions_.size());
    for (const auto id : builtin_ids) {
        result.emplace_back(id);
    }
    for (const auto& [id, definition] : definitions_) {
        if (!definition.builtin) {
            result.push_back(id);
        }
    }
    return result;
}

bool NoteTypeRegistry::register_definition(
    NoteTypeDefinition definition,
    const NoteTypeReplacePolicy policy,
    std::string* const error,
    const NoteTypeParseLimits& storage_limits
) {
    definition.builtin = false;
    if (!validate_definition(definition, error, storage_limits)) {
        return false;
    }
    if (builtin_id_for(definition.id) != nullptr) {
        assign_error(error, "built-in note types cannot be replaced");
        return false;
    }
    const auto existing = definitions_.find(definition.id);
    if (existing != definitions_.end()) {
        if (existing->second.builtin) {
            assign_error(error, "built-in note types cannot be replaced");
            return false;
        }
        if (policy != NoteTypeReplacePolicy::replace_custom) {
            assign_error(error, "custom note type already exists");
            return false;
        }
        existing->second = std::move(definition);
        return true;
    }
    auto key = definition.id;
    definitions_.emplace(std::move(key), std::move(definition));
    return true;
}

bool NoteTypeRegistry::register_text(
    const std::string_view id,
    const std::string_view source,
    const NoteTypeReplacePolicy policy,
    std::vector<NoteTypeParseDiagnostic>* const diagnostics,
    std::string* const error,
    const NoteTypeParseLimits& limits
) {
    auto parsed = parse_note_type_text(id, source, limits);
    if (diagnostics != nullptr) {
        *diagnostics = parsed.diagnostics;
    }
    if (!parsed.definition.has_value()) {
        if (!parsed.diagnostics.empty()) {
            assign_error(
                error,
                "note type parse failed at line "
                    + std::to_string(parsed.diagnostics.front().line) + ": "
                    + parsed.diagnostics.front().message
            );
        } else {
            assign_error(error, "note type parse failed within configured budgets");
        }
        return false;
    }
    return register_definition(
        std::move(*parsed.definition),
        policy,
        error,
        limits
    );
}

std::span<const std::string_view> builtin_note_type_ids() noexcept {
    return builtin_ids;
}

bool builtin_note_type_causes_miss(const std::string_view id) noexcept {
    const auto value = trim(id);
    if (equal_ascii_case_insensitive(value, "hurt")
        || equal_ascii_case_insensitive(value, "mine")) {
        return true;
    }

    constexpr std::string_view hurt_note{"hurtnote"};
    std::size_t matched = 0U;
    for (const char character : value) {
        if (character == ' ' || character == '_' || character == '-') {
            continue;
        }
        if (matched >= hurt_note.size()
            || lower_ascii_char(character) != hurt_note[matched]) {
            return false;
        }
        ++matched;
    }
    return matched == hurt_note.size();
}

}  // namespace pulseforge
