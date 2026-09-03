#include "pulseforge/content_descriptors.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>

namespace pulseforge {
namespace {

using Json = nlohmann::json;

class ParseContext final {
public:
    explicit ParseContext(const DescriptorParseOptions& options)
        : options_(options),
          diagnostic_limit_(std::max<std::size_t>(
              options.limits.max_diagnostics,
              1U
          )) {
        const auto& limits = options_.limits;
        if (limits.max_input_bytes == 0U || limits.max_string_bytes == 0U
            || limits.max_array_items == 0U
            || limits.max_object_members == 0U
            || limits.max_total_nodes == 0U || limits.max_depth == 0U
            || limits.max_preserved_json_bytes == 0U
            || limits.max_diagnostics == 0U) {
            fatal(
                DescriptorDiagnosticCode::invalid_limits,
                "$",
                "all descriptor parser limits must be greater than zero"
            );
            limits_valid_ = false;
        }
    }

    [[nodiscard]] bool strict() const noexcept {
        return options_.mode == DescriptorParseMode::strict;
    }

    [[nodiscard]] bool limits_valid() const noexcept {
        return limits_valid_;
    }

    [[nodiscard]] const DescriptorParseLimits& limits() const noexcept {
        return options_.limits;
    }

    void fatal(
        const DescriptorDiagnosticCode code,
        std::string path,
        std::string message
    ) {
        add(
            DescriptorDiagnosticSeverity::error,
            code,
            std::move(path),
            std::move(message)
        );
    }

    void schema(
        const DescriptorDiagnosticCode code,
        std::string path,
        std::string message
    ) {
        add(
            strict() ? DescriptorDiagnosticSeverity::error
                     : DescriptorDiagnosticSeverity::warning,
            code,
            std::move(path),
            std::move(message)
        );
    }

    void warning(
        const DescriptorDiagnosticCode code,
        std::string path,
        std::string message
    ) {
        add(
            DescriptorDiagnosticSeverity::warning,
            code,
            std::move(path),
            std::move(message)
        );
    }

    [[nodiscard]] bool failed() const noexcept {
        return failed_;
    }

    [[nodiscard]] bool diagnostics_truncated() const noexcept {
        return diagnostics_truncated_;
    }

    [[nodiscard]] std::vector<DescriptorDiagnostic> take_diagnostics() {
        return std::move(diagnostics_);
    }

    [[nodiscard]] std::string preserve_json(
        const Json& value,
        const std::string_view path
    ) {
        if ((value.is_object() || value.is_array()) && value.empty()) {
            return {};
        }

        std::string canonical;
        try {
            canonical = value.dump();
        } catch (const std::exception& exception) {
            schema(
                DescriptorDiagnosticCode::invalid_value,
                std::string(path),
                "cannot canonicalize preserved JSON: "
                    + std::string(exception.what())
            );
            return {};
        }

        const auto limit = options_.limits.max_preserved_json_bytes;
        if (preserved_json_bytes_ > limit
            || canonical.size() > limit - preserved_json_bytes_) {
            schema(
                DescriptorDiagnosticCode::preserved_json_too_large,
                std::string(path),
                "canonical extension JSON exceeds the preservation budget"
            );
            return {};
        }
        preserved_json_bytes_ += canonical.size();
        return canonical;
    }

private:
    void add(
        const DescriptorDiagnosticSeverity severity,
        const DescriptorDiagnosticCode code,
        std::string path,
        std::string message
    ) {
        if (severity == DescriptorDiagnosticSeverity::error) {
            failed_ = true;
        }
        if (diagnostics_.size() >= diagnostic_limit_) {
            diagnostics_truncated_ = true;
            return;
        }
        diagnostics_.push_back(DescriptorDiagnostic{
            severity,
            code,
            std::move(path),
            std::move(message),
        });
    }

    DescriptorParseOptions options_;
    std::vector<DescriptorDiagnostic> diagnostics_;
    std::size_t diagnostic_limit_{};
    std::size_t preserved_json_bytes_{};
    bool failed_{};
    bool limits_valid_{true};
    bool diagnostics_truncated_{};
};

[[nodiscard]] std::string member_path(
    const std::string_view parent,
    const std::string_view member
) {
    std::string result(parent);
    result.push_back('.');
    result.append(member);
    return result;
}

[[nodiscard]] std::string item_path(
    const std::string_view parent,
    const std::size_t index
) {
    std::string result(parent);
    result.push_back('[');
    result.append(std::to_string(index));
    result.push_back(']');
    return result;
}

[[nodiscard]] bool preflight_depth(
    const std::string_view text,
    ParseContext& context
) {
    std::size_t depth = 0U;
    bool in_string = false;
    bool escaped = false;
    bool line_comment = false;
    bool block_comment = false;

    for (std::size_t index = 0U; index < text.size(); ++index) {
        const char current = text[index];
        const char next = index + 1U < text.size() ? text[index + 1U] : '\0';

        if (line_comment) {
            if (current == '\n' || current == '\r') {
                line_comment = false;
            }
            continue;
        }
        if (block_comment) {
            if (current == '*' && next == '/') {
                block_comment = false;
                ++index;
            }
            continue;
        }
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (current == '\\') {
                escaped = true;
            } else if (current == '"') {
                in_string = false;
            }
            continue;
        }

        if (current == '"') {
            in_string = true;
            continue;
        }
        if (current == '/' && next == '/') {
            line_comment = true;
            ++index;
            continue;
        }
        if (current == '/' && next == '*') {
            block_comment = true;
            ++index;
            continue;
        }
        if (current == '{' || current == '[') {
            ++depth;
            if (depth > context.limits().max_depth) {
                context.fatal(
                    DescriptorDiagnosticCode::nesting_too_deep,
                    "$",
                    "JSON nesting exceeds the configured depth limit"
                );
                return false;
            }
        } else if ((current == '}' || current == ']') && depth > 0U) {
            --depth;
        }
    }
    return true;
}

[[nodiscard]] bool validate_json_tree(
    const Json& root,
    ParseContext& context
) {
    struct PendingNode {
        const Json* value{};
        std::size_t depth{};
    };

    std::vector<PendingNode> pending;
    pending.push_back(PendingNode{&root, 1U});
    std::size_t total_nodes = 0U;

    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        ++total_nodes;
        if (total_nodes > context.limits().max_total_nodes) {
            context.fatal(
                DescriptorDiagnosticCode::node_limit_exceeded,
                "$",
                "JSON node count exceeds the configured limit"
            );
            return false;
        }
        if (current.depth > context.limits().max_depth) {
            context.fatal(
                DescriptorDiagnosticCode::nesting_too_deep,
                "$",
                "JSON nesting exceeds the configured depth limit"
            );
            return false;
        }

        if (current.value->is_string()) {
            const auto& string_value =
                current.value->get_ref<const std::string&>();
            if (string_value.size() > context.limits().max_string_bytes) {
                context.fatal(
                    DescriptorDiagnosticCode::string_too_long,
                    "$",
                    "a JSON string exceeds the configured byte limit"
                );
                return false;
            }
            continue;
        }

        if (current.value->is_array()) {
            if (current.value->size() > context.limits().max_array_items) {
                context.fatal(
                    DescriptorDiagnosticCode::array_too_large,
                    "$",
                    "a JSON array exceeds the configured item limit"
                );
                return false;
            }
            for (const auto& child : *current.value) {
                pending.push_back(PendingNode{&child, current.depth + 1U});
            }
            continue;
        }

        if (current.value->is_object()) {
            if (current.value->size() > context.limits().max_object_members) {
                context.fatal(
                    DescriptorDiagnosticCode::object_too_large,
                    "$",
                    "a JSON object exceeds the configured member limit"
                );
                return false;
            }
            for (auto iterator = current.value->begin();
                 iterator != current.value->end(); ++iterator) {
                if (iterator.key().size()
                    > context.limits().max_string_bytes) {
                    context.fatal(
                        DescriptorDiagnosticCode::string_too_long,
                        "$",
                        "a JSON object key exceeds the configured byte limit"
                    );
                    return false;
                }
                pending.push_back(
                    PendingNode{&iterator.value(), current.depth + 1U}
                );
            }
        }
    }
    return true;
}

[[nodiscard]] std::optional<Json> parse_root(
    const std::string_view json_text,
    ParseContext& context
) {
    if (!context.limits_valid()) {
        return std::nullopt;
    }
    if (json_text.size() > context.limits().max_input_bytes) {
        context.fatal(
            DescriptorDiagnosticCode::input_too_large,
            "$",
            "descriptor JSON exceeds the configured input limit"
        );
        return std::nullopt;
    }
    if (!preflight_depth(json_text, context)) {
        return std::nullopt;
    }

    Json root;
    try {
        root = Json::parse(
            json_text.begin(),
            json_text.end(),
            nullptr,
            true,
            !context.strict()
        );
    } catch (const std::exception& exception) {
        context.fatal(
            DescriptorDiagnosticCode::invalid_json,
            "$",
            "invalid descriptor JSON: " + std::string(exception.what())
        );
        return std::nullopt;
    }

    if (!root.is_object()) {
        context.fatal(
            DescriptorDiagnosticCode::root_not_object,
            "$",
            "descriptor root must be a JSON object"
        );
        return std::nullopt;
    }
    if (!validate_json_tree(root, context)) {
        return std::nullopt;
    }
    return root;
}

[[nodiscard]] const Json* find_member(
    const Json& object,
    const std::string_view name
) {
    const auto iterator = object.find(std::string(name));
    return iterator == object.end() ? nullptr : &iterator.value();
}

[[nodiscard]] std::optional<std::string> as_string(
    const Json& value,
    ParseContext& context,
    const std::string_view path
) {
    if (!value.is_string()) {
        context.schema(
            DescriptorDiagnosticCode::wrong_type,
            std::string(path),
            "expected a string"
        );
        return std::nullopt;
    }
    return value.get_ref<const std::string&>();
}

[[nodiscard]] std::optional<double> as_number(
    const Json& value,
    ParseContext& context,
    const std::string_view path
) {
    if (!value.is_number()) {
        context.schema(
            DescriptorDiagnosticCode::wrong_type,
            std::string(path),
            "expected a finite number"
        );
        return std::nullopt;
    }
    try {
        const auto number = value.get<double>();
        if (!std::isfinite(number)) {
            context.schema(
                DescriptorDiagnosticCode::invalid_value,
                std::string(path),
                "expected a finite number"
            );
            return std::nullopt;
        }
        return number;
    } catch (const std::exception&) {
        context.schema(
            DescriptorDiagnosticCode::invalid_value,
            std::string(path),
            "number is outside the supported range"
        );
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::int64_t> as_integer(
    const Json& value,
    ParseContext& context,
    const std::string_view path
) {
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        context.schema(
            DescriptorDiagnosticCode::wrong_type,
            std::string(path),
            "expected an integer"
        );
        return std::nullopt;
    }
    try {
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            const auto maximum = static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()
            );
            if (number > maximum) {
                context.schema(
                    DescriptorDiagnosticCode::invalid_value,
                    std::string(path),
                    "integer is outside the supported range"
                );
                return std::nullopt;
            }
            return static_cast<std::int64_t>(number);
        }
        return value.get<std::int64_t>();
    } catch (const std::exception&) {
        context.schema(
            DescriptorDiagnosticCode::invalid_value,
            std::string(path),
            "integer is outside the supported range"
        );
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<bool> as_boolean(
    const Json& value,
    ParseContext& context,
    const std::string_view path
) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (!context.strict()
        && (value.is_number_integer() || value.is_number_unsigned())) {
        const auto integer = as_integer(value, context, path);
        if (integer && (*integer == 0 || *integer == 1)) {
            context.warning(
                DescriptorDiagnosticCode::wrong_type,
                std::string(path),
                "coerced legacy integer boolean"
            );
            return *integer != 0;
        }
    }
    context.schema(
        DescriptorDiagnosticCode::wrong_type,
        std::string(path),
        "expected a boolean"
    );
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> string_field(
    const Json& object,
    const std::string_view name,
    ParseContext& context,
    const std::string_view object_path,
    const bool required
) {
    const auto path = member_path(object_path, name);
    const auto* value = find_member(object, name);
    if (value == nullptr) {
        if (required) {
            context.schema(
                DescriptorDiagnosticCode::missing_required_field,
                path,
                "required string field is missing"
            );
        }
        return std::nullopt;
    }
    return as_string(*value, context, path);
}

[[nodiscard]] std::optional<double> number_field(
    const Json& object,
    const std::string_view name,
    ParseContext& context,
    const std::string_view object_path,
    const bool required
) {
    const auto path = member_path(object_path, name);
    const auto* value = find_member(object, name);
    if (value == nullptr) {
        if (required) {
            context.schema(
                DescriptorDiagnosticCode::missing_required_field,
                path,
                "required number field is missing"
            );
        }
        return std::nullopt;
    }
    return as_number(*value, context, path);
}

[[nodiscard]] std::optional<std::int64_t> integer_field(
    const Json& object,
    const std::string_view name,
    ParseContext& context,
    const std::string_view object_path,
    const bool required
) {
    const auto path = member_path(object_path, name);
    const auto* value = find_member(object, name);
    if (value == nullptr) {
        if (required) {
            context.schema(
                DescriptorDiagnosticCode::missing_required_field,
                path,
                "required integer field is missing"
            );
        }
        return std::nullopt;
    }
    return as_integer(*value, context, path);
}

[[nodiscard]] std::optional<bool> boolean_field(
    const Json& object,
    const std::string_view name,
    ParseContext& context,
    const std::string_view object_path,
    const bool required
) {
    const auto path = member_path(object_path, name);
    const auto* value = find_member(object, name);
    if (value == nullptr) {
        if (required) {
            context.schema(
                DescriptorDiagnosticCode::missing_required_field,
                path,
                "required boolean field is missing"
            );
        }
        return std::nullopt;
    }
    return as_boolean(*value, context, path);
}

[[nodiscard]] DescriptorVec2 parse_vec2(
    const Json& value,
    ParseContext& context,
    const std::string_view path,
    const DescriptorVec2 fallback
) {
    if (!value.is_array()) {
        context.schema(
            DescriptorDiagnosticCode::wrong_type,
            std::string(path),
            "expected a two-number array"
        );
        return fallback;
    }
    if (value.size() < 2U) {
        context.schema(
            DescriptorDiagnosticCode::invalid_value,
            std::string(path),
            "coordinate array must contain two numbers"
        );
        return fallback;
    }
    if (value.size() != 2U) {
        context.schema(
            DescriptorDiagnosticCode::invalid_value,
            std::string(path),
            "coordinate array contains values beyond the first two"
        );
    }

    const auto x = as_number(value[0U], context, item_path(path, 0U));
    const auto y = as_number(value[1U], context, item_path(path, 1U));
    return x && y ? DescriptorVec2{*x, *y} : fallback;
}

[[nodiscard]] DescriptorVec2 vec2_field(
    const Json& object,
    const std::string_view name,
    ParseContext& context,
    const std::string_view object_path,
    const DescriptorVec2 fallback,
    const bool required
) {
    const auto path = member_path(object_path, name);
    const auto* value = find_member(object, name);
    if (value == nullptr) {
        if (required) {
            context.schema(
                DescriptorDiagnosticCode::missing_required_field,
                path,
                "required coordinate field is missing"
            );
        }
        return fallback;
    }
    return parse_vec2(*value, context, path, fallback);
}

[[nodiscard]] DescriptorRgb parse_rgb(
    const Json& value,
    ParseContext& context,
    const std::string_view path,
    const DescriptorRgb fallback
) {
    if (!value.is_array() || value.size() < 3U) {
        context.schema(
            DescriptorDiagnosticCode::wrong_type,
            std::string(path),
            "expected an RGB array with three integers"
        );
        return fallback;
    }
    if (value.size() != 3U) {
        context.schema(
            DescriptorDiagnosticCode::invalid_value,
            std::string(path),
            "RGB array contains values beyond the first three"
        );
    }

    DescriptorRgb result = fallback;
    std::array<std::int32_t*, 3U> channels{
        &result.red,
        &result.green,
        &result.blue,
    };
    for (std::size_t index = 0U; index < channels.size(); ++index) {
        const auto component = as_integer(
            value[index],
            context,
            item_path(path, index)
        );
        if (!component) {
            continue;
        }
        auto bounded = *component;
        if (bounded < 0 || bounded > 255) {
            context.schema(
                DescriptorDiagnosticCode::invalid_value,
                item_path(path, index),
                "RGB component must be between 0 and 255"
            );
            bounded = std::clamp<std::int64_t>(bounded, 0, 255);
        }
        *channels[index] = static_cast<std::int32_t>(bounded);
    }
    return result;
}

[[nodiscard]] std::vector<std::string> parse_string_array(
    const Json& value,
    ParseContext& context,
    const std::string_view path
) {
    std::vector<std::string> result;
    if (!value.is_array()) {
        context.schema(
            DescriptorDiagnosticCode::wrong_type,
            std::string(path),
            "expected an array of strings"
        );
        return result;
    }
    result.reserve(value.size());
    for (std::size_t index = 0U; index < value.size(); ++index) {
        const auto string = as_string(
            value[index],
            context,
            item_path(path, index)
        );
        if (string) {
            result.push_back(*string);
        }
    }
    return result;
}

[[nodiscard]] std::vector<std::string> string_array_field(
    const Json& object,
    const std::string_view name,
    ParseContext& context,
    const std::string_view object_path,
    const bool required
) {
    const auto path = member_path(object_path, name);
    const auto* value = find_member(object, name);
    if (value == nullptr) {
        if (required) {
            context.schema(
                DescriptorDiagnosticCode::missing_required_field,
                path,
                "required string array is missing"
            );
        }
        return {};
    }
    return parse_string_array(*value, context, path);
}

[[nodiscard]] std::vector<std::int32_t> parse_integer_array(
    const Json& value,
    ParseContext& context,
    const std::string_view path
) {
    std::vector<std::int32_t> result;
    if (!value.is_array()) {
        context.schema(
            DescriptorDiagnosticCode::wrong_type,
            std::string(path),
            "expected an integer array"
        );
        return result;
    }
    result.reserve(value.size());
    for (std::size_t index = 0U; index < value.size(); ++index) {
        const auto integer = as_integer(
            value[index],
            context,
            item_path(path, index)
        );
        if (!integer) {
            continue;
        }
        if (*integer < std::numeric_limits<std::int32_t>::min()
            || *integer > std::numeric_limits<std::int32_t>::max()) {
            context.schema(
                DescriptorDiagnosticCode::invalid_value,
                item_path(path, index),
                "integer is outside the 32-bit range"
            );
            continue;
        }
        result.push_back(static_cast<std::int32_t>(*integer));
    }
    return result;
}

[[nodiscard]] bool is_known_key(
    const std::string_view key,
    const std::initializer_list<std::string_view> known_keys
) {
    return std::find(known_keys.begin(), known_keys.end(), key)
        != known_keys.end();
}

[[nodiscard]] Json unknown_members(
    const Json& object,
    const std::initializer_list<std::string_view> known_keys
) {
    Json unknown = Json::object();
    for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
        if (!is_known_key(iterator.key(), known_keys)) {
            unknown[iterator.key()] = iterator.value();
        }
    }
    return unknown;
}

void add_extension_section(
    Json& extensions,
    const std::string_view section,
    Json values
) {
    if (!values.empty()) {
        extensions[std::string(section)] = std::move(values);
    }
}

[[nodiscard]] std::string validated_id(
    const std::string_view id,
    ParseContext& context
) {
    if (id.empty()) {
        context.schema(
            DescriptorDiagnosticCode::invalid_value,
            "$id",
            "descriptor id cannot be empty"
        );
        return "unnamed";
    }
    if (id.size() > context.limits().max_string_bytes) {
        context.fatal(
            DescriptorDiagnosticCode::string_too_long,
            "$id",
            "descriptor id exceeds the configured string limit"
        );
        return {};
    }
    return std::string(id);
}

[[nodiscard]] std::vector<NamedOffsetDescriptor> parse_named_offsets(
    const Json& value,
    ParseContext& context,
    const std::string_view path
) {
    std::vector<NamedOffsetDescriptor> result;
    if (!value.is_object()) {
        context.schema(
            DescriptorDiagnosticCode::wrong_type,
            std::string(path),
            "expected an object mapping ids to offsets"
        );
        return result;
    }
    result.reserve(value.size());
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
        const auto offset = as_number(
            iterator.value(),
            context,
            member_path(path, iterator.key())
        );
        if (offset) {
            result.push_back(NamedOffsetDescriptor{iterator.key(), *offset});
        }
    }
    return result;
}

[[nodiscard]] std::vector<AlternateVocalOffsetsDescriptor>
parse_alternate_vocals(
    const Json& value,
    ParseContext& context,
    const std::string_view path
) {
    std::vector<AlternateVocalOffsetsDescriptor> result;
    if (!value.is_object()) {
        context.schema(
            DescriptorDiagnosticCode::wrong_type,
            std::string(path),
            "expected an object mapping variations to vocal offsets"
        );
        return result;
    }
    result.reserve(value.size());
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
        result.push_back(AlternateVocalOffsetsDescriptor{
            iterator.key(),
            parse_named_offsets(
                iterator.value(),
                context,
                member_path(path, iterator.key())
            ),
        });
    }
    return result;
}

[[nodiscard]] std::string trim_ascii(const std::string_view value) {
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(whitespace);
    return std::string(value.substr(first, last - first + 1U));
}

[[nodiscard]] std::vector<std::string> split_difficulties(
    const std::string_view value,
    ParseContext& context,
    const std::string_view path
) {
    std::vector<std::string> result;
    std::size_t cursor = 0U;
    while (cursor <= value.size()) {
        const auto delimiter = value.find(',', cursor);
        const auto end = delimiter == std::string_view::npos
            ? value.size()
            : delimiter;
        auto difficulty = trim_ascii(value.substr(cursor, end - cursor));
        if (!difficulty.empty()) {
            if (result.size() >= context.limits().max_array_items) {
                context.schema(
                    DescriptorDiagnosticCode::array_too_large,
                    std::string(path),
                    "difficulty list exceeds the configured item limit"
                );
                break;
            }
            result.push_back(std::move(difficulty));
        }
        if (delimiter == std::string_view::npos) {
            break;
        }
        cursor = delimiter + 1U;
    }
    return result;
}

[[nodiscard]] AnimationDescriptor parse_animation(
    const Json& value,
    ParseContext& context,
    const std::string_view path
) {
    AnimationDescriptor result;
    if (!value.is_object()) {
        context.schema(
            DescriptorDiagnosticCode::wrong_type,
            std::string(path),
            "animation entry must be an object"
        );
        return result;
    }

    if (const auto field = string_field(value, "anim", context, path, true)) {
        result.id = *field;
    }
    if (const auto field = string_field(value, "name", context, path, true)) {
        result.name = *field;
    }
    if (const auto field = integer_field(value, "fps", context, path, true)) {
        if (*field <= 0 || *field > 1'000) {
            context.schema(
                DescriptorDiagnosticCode::invalid_value,
                member_path(path, "fps"),
                "animation fps must be between 1 and 1000"
            );
        } else {
            result.fps = static_cast<std::int32_t>(*field);
        }
    }
    if (const auto field = boolean_field(value, "loop", context, path, true)) {
        result.loop = *field;
    }
    if (const auto* indices = find_member(value, "indices")) {
        result.indices = parse_integer_array(
            *indices,
            context,
            member_path(path, "indices")
        );
    } else {
        context.schema(
            DescriptorDiagnosticCode::missing_required_field,
            member_path(path, "indices"),
            "animation indices array is missing"
        );
    }
    result.offsets = vec2_field(
        value,
        "offsets",
        context,
        path,
        {},
        true
    );
    result.extensions_json = context.preserve_json(
        unknown_members(
            value,
            {"anim", "name", "fps", "loop", "indices", "offsets"}
        ),
        member_path(path, "extensions")
    );
    return result;
}

[[nodiscard]] std::vector<AnimationDescriptor> parse_animations(
    const Json& value,
    ParseContext& context,
    const std::string_view path
) {
    std::vector<AnimationDescriptor> result;
    if (!value.is_array()) {
        context.schema(
            DescriptorDiagnosticCode::wrong_type,
            std::string(path),
            "animations must be an array"
        );
        return result;
    }
    result.reserve(value.size());
    for (std::size_t index = 0U; index < value.size(); ++index) {
        result.push_back(
            parse_animation(value[index], context, item_path(path, index))
        );
    }
    return result;
}

[[nodiscard]] StageObjectDescriptor parse_stage_object(
    const Json& value,
    ParseContext& context,
    const std::string_view path
) {
    StageObjectDescriptor result;
    if (!value.is_object()) {
        context.schema(
            DescriptorDiagnosticCode::wrong_type,
            std::string(path),
            "stage object entry must be an object"
        );
        return result;
    }

    if (const auto field = string_field(value, "type", context, path, true)) {
        result.type = *field;
    }
    if (const auto field = string_field(value, "name", context, path, false)) {
        result.name = *field;
    }
    if (const auto field = string_field(value, "image", context, path, false)) {
        result.image = *field;
    }
    if (const auto field = number_field(value, "x", context, path, false)) {
        result.x = *field;
    }
    if (const auto field = number_field(value, "y", context, path, false)) {
        result.y = *field;
    }
    if (const auto field = number_field(value, "width", context, path, false)) {
        result.width = *field;
    }
    if (const auto field = number_field(value, "height", context, path, false)) {
        result.height = *field;
    }
    result.scale = vec2_field(
        value,
        "scale",
        context,
        path,
        result.scale,
        false
    );
    result.scroll = vec2_field(
        value,
        "scroll",
        context,
        path,
        result.scroll,
        false
    );
    if (const auto field = number_field(value, "alpha", context, path, false)) {
        result.alpha = *field;
    }
    if (const auto field = number_field(value, "angle", context, path, false)) {
        result.angle = *field;
    }
    if (const auto field = string_field(value, "color", context, path, false)) {
        result.color = *field;
    }
    if (const auto field = boolean_field(
            value,
            "antialiasing",
            context,
            path,
            false
        )) {
        result.antialiasing = *field;
    }
    if (const auto field = boolean_field(value, "flipX", context, path, false)) {
        result.flip_x = *field;
    }
    if (const auto field = boolean_field(value, "flipY", context, path, false)) {
        result.flip_y = *field;
    }
    if (const auto field = boolean_field(
            value,
            "foreground",
            context,
            path,
            false
    )) {
        result.foreground = *field;
        result.layer = *field
            ? StageObjectLayer::foreground
            : StageObjectLayer::background;
    }
    if (const auto field = string_field(value, "layer", context, path, false)) {
        if (*field == "background") {
            result.layer = StageObjectLayer::background;
        } else if (*field == "behindGirlfriend" || *field == "behindGF") {
            result.layer = StageObjectLayer::behind_girlfriend;
        } else if (*field == "behindOpponent" || *field == "behindDad") {
            result.layer = StageObjectLayer::behind_opponent;
        } else if (*field == "behindPlayer" || *field == "behindBF") {
            result.layer = StageObjectLayer::behind_player;
        } else if (*field == "foreground") {
            result.layer = StageObjectLayer::foreground;
        } else {
            context.schema(
                DescriptorDiagnosticCode::invalid_value,
                member_path(path, "layer"),
                "stage object layer is not recognized"
            );
        }
        result.foreground = result.layer == StageObjectLayer::foreground;
    }
    if (const auto field = boolean_field(
            value,
            "screenSpace",
            context,
            path,
            false
        )) {
        result.screen_space = *field;
    }
    if (const auto field = string_field(value, "blend", context, path, false)) {
        result.blend = *field;
    }
    if (const auto field = integer_field(value, "filters", context, path, false)) {
        if (*field < std::numeric_limits<std::int32_t>::min()
            || *field > std::numeric_limits<std::int32_t>::max()) {
            context.schema(
                DescriptorDiagnosticCode::invalid_value,
                member_path(path, "filters"),
                "stage object filters are outside the 32-bit range"
            );
        } else {
            result.filters = static_cast<std::int32_t>(*field);
        }
    }
    if (const auto field = string_field(
            value,
            "firstAnimation",
            context,
            path,
            false
        )) {
        result.first_animation = *field;
    }
    if (const auto* animations = find_member(value, "animations")) {
        result.animations = parse_animations(
            *animations,
            context,
            member_path(path, "animations")
        );
    }

    result.extensions_json = context.preserve_json(
        unknown_members(
            value,
            {
                "type", "name", "image", "x", "y", "width", "height",
                "scale", "scroll",
                "alpha", "angle", "color", "antialiasing", "flipX",
                "flipY", "foreground", "layer", "screenSpace", "blend", "filters",
                "firstAnimation", "animations",
            }
        ),
        member_path(path, "extensions")
    );
    return result;
}

template <typename Descriptor>
[[nodiscard]] DescriptorParseResult<Descriptor> finish_result(
    std::optional<Descriptor> value,
    ParseContext& context
) {
    if (context.failed()) {
        value.reset();
    }
    return DescriptorParseResult<Descriptor>{
        std::move(value),
        context.take_diagnostics(),
        context.diagnostics_truncated(),
    };
}

}  // namespace

DescriptorParseResult<SongDescriptor>
ContentDescriptorParser::parse_vslice_song(
    const std::string_view json_text,
    const std::string_view id,
    const DescriptorParseOptions& options
) {
    ParseContext context(options);
    auto root = parse_root(json_text, context);
    if (!root) {
        return finish_result<SongDescriptor>(std::nullopt, context);
    }

    SongDescriptor descriptor;
    descriptor.id = validated_id(id, context);
    if (const auto field = string_field(*root, "version", context, "$", true)) {
        descriptor.version = *field;
    }
    if (const auto field = string_field(
            *root,
            "songName",
            context,
            "$",
            true
        )) {
        descriptor.name = *field;
    } else {
        descriptor.name = descriptor.id;
    }
    if (const auto field = string_field(*root, "artist", context, "$", true)) {
        descriptor.artist = *field;
    } else {
        descriptor.artist = "Unknown Artist";
    }
    if (const auto field = string_field(*root, "charter", context, "$", false)) {
        descriptor.charter = *field;
    }

    Json extensions = Json::object();
    add_extension_section(
        extensions,
        "root",
        unknown_members(
            *root,
            {"version", "songName", "artist", "charter", "playData", "offsets"}
        )
    );

    if (const auto* play_data = find_member(*root, "playData")) {
        if (!play_data->is_object()) {
            context.schema(
                DescriptorDiagnosticCode::wrong_type,
                "$.playData",
                "playData must be an object"
            );
        } else {
            descriptor.difficulties = string_array_field(
                *play_data,
                "difficulties",
                context,
                "$.playData",
                true
            );
            if (const auto* variations = find_member(
                    *play_data,
                    "songVariations"
                )) {
                descriptor.variations = parse_string_array(
                    *variations,
                    context,
                    "$.playData.songVariations"
                );
            } else if (!context.strict()) {
                if (const auto* legacy = find_member(*play_data, "variations")) {
                    context.warning(
                        DescriptorDiagnosticCode::invalid_value,
                        "$.playData.variations",
                        "accepted legacy variations alias"
                    );
                    descriptor.variations = parse_string_array(
                        *legacy,
                        context,
                        "$.playData.variations"
                    );
                }
            }
            if (const auto field = string_field(
                    *play_data,
                    "stage",
                    context,
                    "$.playData",
                    true
                )) {
                descriptor.stage = *field;
            }
            if (const auto field = string_field(
                    *play_data,
                    "noteStyle",
                    context,
                    "$.playData",
                    true
                )) {
                descriptor.note_style = *field;
            }
            if (const auto field = number_field(
                    *play_data,
                    "previewStart",
                    context,
                    "$.playData",
                    false
                )) {
                descriptor.preview_start = *field;
            }
            if (const auto field = number_field(
                    *play_data,
                    "previewEnd",
                    context,
                    "$.playData",
                    false
                )) {
                descriptor.preview_end = *field;
            }

            if (const auto* characters = find_member(*play_data, "characters")) {
                if (!characters->is_object()) {
                    context.schema(
                        DescriptorDiagnosticCode::wrong_type,
                        "$.playData.characters",
                        "characters must be an object"
                    );
                } else {
                    if (const auto field = string_field(
                            *characters,
                            "player",
                            context,
                            "$.playData.characters",
                            false
                        )) {
                        descriptor.characters.player = *field;
                    }
                    if (const auto field = string_field(
                            *characters,
                            "girlfriend",
                            context,
                            "$.playData.characters",
                            false
                        )) {
                        descriptor.characters.girlfriend = *field;
                    }
                    if (const auto field = string_field(
                            *characters,
                            "opponent",
                            context,
                            "$.playData.characters",
                            false
                        )) {
                        descriptor.characters.opponent = *field;
                    }
                    if (const auto field = string_field(
                            *characters,
                            "instrumental",
                            context,
                            "$.playData.characters",
                            false
                        )) {
                        descriptor.characters.instrumental = *field;
                    }
                    descriptor.characters.alternate_instrumentals =
                        string_array_field(
                            *characters,
                            "altInstrumentals",
                            context,
                            "$.playData.characters",
                            false
                        );
                    descriptor.characters.player_vocals = string_array_field(
                        *characters,
                        "playerVocals",
                        context,
                        "$.playData.characters",
                        false
                    );
                    descriptor.characters.opponent_vocals = string_array_field(
                        *characters,
                        "opponentVocals",
                        context,
                        "$.playData.characters",
                        false
                    );
                    add_extension_section(
                        extensions,
                        "characters",
                        unknown_members(
                            *characters,
                            {
                                "player", "girlfriend", "opponent",
                                "instrumental", "altInstrumentals",
                                "playerVocals", "opponentVocals",
                            }
                        )
                    );
                }
            } else {
                context.schema(
                    DescriptorDiagnosticCode::missing_required_field,
                    "$.playData.characters",
                    "song character mapping is missing"
                );
            }

            add_extension_section(
                extensions,
                "playData",
                unknown_members(
                    *play_data,
                    {
                        "difficulties", "songVariations", "variations",
                        "stage", "noteStyle", "characters", "previewStart",
                        "previewEnd",
                    }
                )
            );
        }
    } else {
        context.schema(
            DescriptorDiagnosticCode::missing_required_field,
            "$.playData",
            "song playData is missing"
        );
    }

    if (descriptor.preview_start && descriptor.preview_end
        && *descriptor.preview_end < *descriptor.preview_start) {
        context.schema(
            DescriptorDiagnosticCode::invalid_value,
            "$.playData.previewEnd",
            "previewEnd precedes previewStart"
        );
        if (!context.strict()) {
            descriptor.preview_end.reset();
        }
    }

    if (const auto* offsets = find_member(*root, "offsets")) {
        if (!offsets->is_object()) {
            context.schema(
                DescriptorDiagnosticCode::wrong_type,
                "$.offsets",
                "offsets must be an object"
            );
        } else {
            if (const auto field = number_field(
                    *offsets,
                    "instrumental",
                    context,
                    "$.offsets",
                    false
                )) {
                descriptor.offsets.instrumental = *field;
            }
            if (const auto* field = find_member(*offsets, "altInstrumentals")) {
                descriptor.offsets.alternate_instrumentals =
                    parse_named_offsets(
                        *field,
                        context,
                        "$.offsets.altInstrumentals"
                    );
            }
            if (const auto* field = find_member(*offsets, "vocals")) {
                descriptor.offsets.vocals = parse_named_offsets(
                    *field,
                    context,
                    "$.offsets.vocals"
                );
            }
            if (const auto* field = find_member(*offsets, "altVocals")) {
                descriptor.offsets.alternate_vocals = parse_alternate_vocals(
                    *field,
                    context,
                    "$.offsets.altVocals"
                );
            }
            add_extension_section(
                extensions,
                "offsets",
                unknown_members(
                    *offsets,
                    {"instrumental", "altInstrumentals", "vocals", "altVocals"}
                )
            );
        }
    }

    descriptor.extensions_json = context.preserve_json(
        extensions,
        "$.extensions"
    );
    return finish_result<SongDescriptor>(std::move(descriptor), context);
}

DescriptorParseResult<LevelDescriptor>
ContentDescriptorParser::parse_vslice_level(
    const std::string_view json_text,
    const std::string_view id,
    const DescriptorParseOptions& options
) {
    ParseContext context(options);
    auto root = parse_root(json_text, context);
    if (!root) {
        return finish_result<LevelDescriptor>(std::nullopt, context);
    }

    LevelDescriptor descriptor;
    descriptor.id = validated_id(id, context);
    if (const auto field = string_field(*root, "version", context, "$", true)) {
        descriptor.version = *field;
    }
    if (const auto field = string_field(*root, "name", context, "$", true)) {
        descriptor.name = *field;
    } else {
        descriptor.name = descriptor.id;
    }
    if (const auto field = boolean_field(*root, "visible", context, "$", false)) {
        descriptor.visible = *field;
    }
    descriptor.songs = string_array_field(
        *root,
        "songs",
        context,
        "$",
        true
    );

    Json extensions = Json::object();
    if (const auto* background = find_member(*root, "background")) {
        if (background->is_string()) {
            LevelBackgroundDescriptor parsed;
            parsed.color = background->get_ref<const std::string&>();
            descriptor.background = std::move(parsed);
        } else if (!context.strict() && background->is_object()) {
            context.warning(
                DescriptorDiagnosticCode::wrong_type,
                "$.background",
                "accepted legacy object-form level background"
            );
            LevelBackgroundDescriptor parsed;
            parsed.color = string_field(
                *background,
                "color",
                context,
                "$.background",
                false
            );
            parsed.image = string_field(
                *background,
                "image",
                context,
                "$.background",
                false
            );
            descriptor.background = std::move(parsed);
            add_extension_section(
                extensions,
                "background",
                unknown_members(*background, {"color", "image"})
            );
        } else {
            context.schema(
                DescriptorDiagnosticCode::wrong_type,
                "$.background",
                "official level background must be a string"
            );
        }
    }
    add_extension_section(
        extensions,
        "root",
        unknown_members(
            *root,
            {"version", "name", "visible", "songs", "background"}
        )
    );
    descriptor.extensions_json = context.preserve_json(
        extensions,
        "$.extensions"
    );
    return finish_result<LevelDescriptor>(std::move(descriptor), context);
}

DescriptorParseResult<WeekDescriptor>
ContentDescriptorParser::parse_psych_week(
    const std::string_view json_text,
    const std::string_view id,
    const DescriptorParseOptions& options
) {
    ParseContext context(options);
    auto root = parse_root(json_text, context);
    if (!root) {
        return finish_result<WeekDescriptor>(std::nullopt, context);
    }

    WeekDescriptor descriptor;
    descriptor.id = validated_id(id, context);
    descriptor.story_name = descriptor.id;
    descriptor.display_name = descriptor.id;

    if (const auto* songs = find_member(*root, "songs")) {
        if (!songs->is_array()) {
            context.schema(
                DescriptorDiagnosticCode::wrong_type,
                "$.songs",
                "Psych week songs must be an array"
            );
        } else {
            descriptor.songs.reserve(songs->size());
            for (std::size_t index = 0U; index < songs->size(); ++index) {
                const auto path = item_path("$.songs", index);
                const auto& value = (*songs)[index];
                WeekSongDescriptor song;
                if (value.is_array()) {
                    if (value.size() < 2U) {
                        context.schema(
                            DescriptorDiagnosticCode::invalid_value,
                            path,
                            "week song needs at least a name and character"
                        );
                    }
                    if (value.size() > 0U) {
                        if (const auto parsed = as_string(
                                value[0U],
                                context,
                                item_path(path, 0U)
                            )) {
                            song.name = *parsed;
                        }
                    }
                    if (value.size() > 1U) {
                        if (const auto parsed = as_string(
                                value[1U],
                                context,
                                item_path(path, 1U)
                            )) {
                            song.character = *parsed;
                        }
                    }
                    if (value.size() > 2U) {
                        song.color = parse_rgb(
                            value[2U],
                            context,
                            item_path(path, 2U),
                            song.color
                        );
                    } else {
                        context.schema(
                            DescriptorDiagnosticCode::missing_required_field,
                            item_path(path, 2U),
                            "week song color is missing"
                        );
                    }
                    if (value.size() > 3U) {
                        Json extras = Json::array();
                        for (std::size_t extra = 3U; extra < value.size(); ++extra) {
                            extras.push_back(value[extra]);
                        }
                        song.extensions_json = context.preserve_json(
                            extras,
                            member_path(path, "extensions")
                        );
                    }
                } else if (!context.strict() && value.is_object()) {
                    context.warning(
                        DescriptorDiagnosticCode::wrong_type,
                        path,
                        "accepted object-form legacy week song"
                    );
                    const auto name = string_field(
                        value,
                        "name",
                        context,
                        path,
                        true
                    );
                    const auto character = string_field(
                        value,
                        "character",
                        context,
                        path,
                        true
                    );
                    if (name) {
                        song.name = *name;
                    }
                    if (character) {
                        song.character = *character;
                    }
                    if (const auto* color = find_member(value, "color")) {
                        song.color = parse_rgb(
                            *color,
                            context,
                            member_path(path, "color"),
                            song.color
                        );
                    }
                    song.extensions_json = context.preserve_json(
                        unknown_members(value, {"name", "character", "color"}),
                        member_path(path, "extensions")
                    );
                } else {
                    context.schema(
                        DescriptorDiagnosticCode::wrong_type,
                        path,
                        "week song entry must be an array"
                    );
                }
                descriptor.songs.push_back(std::move(song));
            }
        }
    } else {
        context.schema(
            DescriptorDiagnosticCode::missing_required_field,
            "$.songs",
            "Psych week songs array is missing"
        );
    }

    if (const auto* characters = find_member(*root, "weekCharacters")) {
        if (!characters->is_array() || characters->size() < 3U) {
            context.schema(
                DescriptorDiagnosticCode::wrong_type,
                "$.weekCharacters",
                "weekCharacters must contain three strings"
            );
        } else {
            if (characters->size() != 3U) {
                context.schema(
                    DescriptorDiagnosticCode::invalid_value,
                    "$.weekCharacters",
                    "weekCharacters contains entries beyond the first three"
                );
            }
            for (std::size_t index = 0U; index < descriptor.characters.size();
                 ++index) {
                if (const auto parsed = as_string(
                        (*characters)[index],
                        context,
                        item_path("$.weekCharacters", index)
                    )) {
                    descriptor.characters[index] = *parsed;
                }
            }
        }
    } else {
        context.schema(
            DescriptorDiagnosticCode::missing_required_field,
            "$.weekCharacters",
            "weekCharacters is missing"
        );
    }

    if (const auto field = string_field(
            *root,
            "weekBackground",
            context,
            "$",
            true
        )) {
        descriptor.background = *field;
    }
    if (const auto field = string_field(
            *root,
            "weekBefore",
            context,
            "$",
            true
        )) {
        descriptor.previous_week = *field;
    }
    if (const auto field = string_field(
            *root,
            "storyName",
            context,
            "$",
            true
        )) {
        descriptor.story_name = *field;
    }
    if (const auto field = string_field(
            *root,
            "weekName",
            context,
            "$",
            true
        )) {
        descriptor.display_name = *field;
    }
    if (const auto field = boolean_field(
            *root,
            "startUnlocked",
            context,
            "$",
            true
        )) {
        descriptor.start_unlocked = *field;
    }
    if (const auto field = boolean_field(
            *root,
            "hiddenUntilUnlocked",
            context,
            "$",
            true
        )) {
        descriptor.hidden_until_unlocked = *field;
    }
    if (const auto field = boolean_field(
            *root,
            "hideStoryMode",
            context,
            "$",
            context.strict()
        )) {
        descriptor.hide_story = *field;
    } else if (!context.strict()) {
        if (const auto* legacy = find_member(*root, "hideStory")) {
            context.warning(
                DescriptorDiagnosticCode::invalid_value,
                "$.hideStory",
                "accepted legacy hideStory alias"
            );
            if (const auto parsed = as_boolean(
                    *legacy,
                    context,
                    "$.hideStory"
                )) {
                descriptor.hide_story = *parsed;
            }
        }
    }
    if (const auto field = boolean_field(
            *root,
            "hideFreeplay",
            context,
            "$",
            true
        )) {
        descriptor.hide_freeplay = *field;
    }

    if (const auto* difficulties = find_member(*root, "difficulties")) {
        if (difficulties->is_string()) {
            descriptor.difficulties = split_difficulties(
                difficulties->get_ref<const std::string&>(),
                context,
                "$.difficulties"
            );
        } else if (!context.strict() && difficulties->is_array()) {
            context.warning(
                DescriptorDiagnosticCode::wrong_type,
                "$.difficulties",
                "accepted array-form legacy difficulties"
            );
            descriptor.difficulties = parse_string_array(
                *difficulties,
                context,
                "$.difficulties"
            );
        } else {
            context.schema(
                DescriptorDiagnosticCode::wrong_type,
                "$.difficulties",
                "Psych difficulties must be a comma-separated string"
            );
        }
    } else {
        context.schema(
            DescriptorDiagnosticCode::missing_required_field,
            "$.difficulties",
            "Psych difficulties field is missing"
        );
    }

    descriptor.extensions_json = context.preserve_json(
        unknown_members(
            *root,
            {
                "songs", "weekCharacters", "weekBackground", "weekBefore",
                "storyName", "weekName", "startUnlocked",
                "hiddenUntilUnlocked", "hideStoryMode", "hideStory",
                "hideFreeplay", "difficulties",
            }
        ),
        "$.extensions"
    );
    return finish_result<WeekDescriptor>(std::move(descriptor), context);
}

DescriptorParseResult<StageDescriptor>
ContentDescriptorParser::parse_psych_stage(
    const std::string_view json_text,
    const std::string_view id,
    const DescriptorParseOptions& options
) {
    ParseContext context(options);
    auto root = parse_root(json_text, context);
    if (!root) {
        return finish_result<StageDescriptor>(std::nullopt, context);
    }

    StageDescriptor descriptor;
    descriptor.id = validated_id(id, context);
    if (const auto field = string_field(*root, "directory", context, "$", true)) {
        descriptor.directory = *field;
    }
    if (const auto field = number_field(*root, "defaultZoom", context, "$", true)) {
        if (*field <= 0.0) {
            context.schema(
                DescriptorDiagnosticCode::invalid_value,
                "$.defaultZoom",
                "stage zoom must be greater than zero"
            );
        } else {
            descriptor.default_zoom = *field;
        }
    }
    if (const auto* pixel_stage = find_member(*root, "isPixelStage")) {
        if (!pixel_stage->is_null()) {
            if (const auto parsed = as_boolean(
                    *pixel_stage,
                    context,
                    "$.isPixelStage"
                )) {
                descriptor.pixel_stage = *parsed;
            }
        }
    }
    if (const auto field = string_field(*root, "stageUI", context, "$", true)) {
        descriptor.stage_ui = *field;
    }
    descriptor.boyfriend = vec2_field(
        *root,
        "boyfriend",
        context,
        "$",
        descriptor.boyfriend,
        true
    );
    descriptor.girlfriend = vec2_field(
        *root,
        "girlfriend",
        context,
        "$",
        descriptor.girlfriend,
        true
    );
    descriptor.opponent = vec2_field(
        *root,
        "opponent",
        context,
        "$",
        descriptor.opponent,
        true
    );
    // PULSEFORGE_P1_4_0_DENPA_STAGE_P4_V1
    descriptor.p4 = vec2_field(
        *root,
        "p4",
        context,
        "$",
        descriptor.p4,
        true
    );
    if (const auto field = boolean_field(
            *root,
            "hide_girlfriend",
            context,
            "$",
            true
        )) {
        descriptor.hide_girlfriend = *field;
    }
    descriptor.camera_boyfriend = vec2_field(
        *root,
        "camera_boyfriend",
        context,
        "$",
        descriptor.camera_boyfriend,
        true
    );
    descriptor.camera_girlfriend = vec2_field(
        *root,
        "camera_girlfriend",
        context,
        "$",
        descriptor.camera_girlfriend,
        true
    );
    descriptor.camera_opponent = vec2_field(
        *root,
        "camera_opponent",
        context,
        "$",
        descriptor.camera_opponent,
        true
    );
    descriptor.camera_p4 = vec2_field(
        *root,
        "camera_p4",
        context,
        "$",
        descriptor.camera_p4,
        true
    );
    if (const auto* camera_speed = find_member(*root, "camera_speed")) {
        if (!camera_speed->is_null()) {
            if (const auto parsed = as_number(
                    *camera_speed,
                    context,
                    "$.camera_speed"
                )) {
                if (*parsed <= 0.0) {
                    context.schema(
                        DescriptorDiagnosticCode::invalid_value,
                        "$.camera_speed",
                        "camera speed must be greater than zero"
                    );
                } else {
                    descriptor.camera_speed = *parsed;
                }
            }
        }
    }

    if (const auto* objects = find_member(*root, "objects")) {
        if (!objects->is_array()) {
            context.schema(
                DescriptorDiagnosticCode::wrong_type,
                "$.objects",
                "stage objects must be an array"
            );
        } else {
            descriptor.objects.reserve(objects->size());
            for (std::size_t index = 0U; index < objects->size(); ++index) {
                descriptor.objects.push_back(parse_stage_object(
                    (*objects)[index],
                    context,
                    item_path("$.objects", index)
                ));
            }
        }
    }
    if (const auto* preload = find_member(*root, "preload")) {
        descriptor.preload_json = context.preserve_json(*preload, "$.preload");
    }

    descriptor.extensions_json = context.preserve_json(
        unknown_members(
            *root,
            {
                "directory", "defaultZoom", "isPixelStage", "stageUI",
                "boyfriend", "girlfriend", "opponent", "p4", "hide_girlfriend",
                "camera_boyfriend", "camera_girlfriend", "camera_opponent",
                "camera_p4",
                "camera_speed", "objects", "preload",
            }
        ),
        "$.extensions"
    );
    return finish_result<StageDescriptor>(std::move(descriptor), context);
}

DescriptorParseResult<CharacterDescriptor>
ContentDescriptorParser::parse_psych_character(
    const std::string_view json_text,
    const std::string_view id,
    const DescriptorParseOptions& options
) {
    ParseContext context(options);
    auto root = parse_root(json_text, context);
    if (!root) {
        return finish_result<CharacterDescriptor>(std::nullopt, context);
    }

    CharacterDescriptor descriptor;
    descriptor.id = validated_id(id, context);
    if (const auto field = string_field(*root, "image", context, "$", true)) {
        descriptor.image = *field;
    }
    if (const auto field = number_field(*root, "scale", context, "$", true)) {
        if (*field <= 0.0) {
            context.schema(
                DescriptorDiagnosticCode::invalid_value,
                "$.scale",
                "character scale must be greater than zero"
            );
        } else {
            descriptor.scale = *field;
        }
    }
    if (const auto field = number_field(
            *root,
            "sing_duration",
            context,
            "$",
            true
        )) {
        if (*field < 0.0) {
            context.schema(
                DescriptorDiagnosticCode::invalid_value,
                "$.sing_duration",
                "sing duration cannot be negative"
            );
        } else {
            descriptor.sing_duration = *field;
        }
    }
    if (const auto field = string_field(
            *root,
            "healthicon",
            context,
            "$",
            true
        )) {
        descriptor.health_icon = *field;
    }
    descriptor.position = vec2_field(
        *root,
        "position",
        context,
        "$",
        descriptor.position,
        true
    );
    descriptor.camera_position = vec2_field(
        *root,
        "camera_position",
        context,
        "$",
        descriptor.camera_position,
        true
    );
    if (const auto field = boolean_field(*root, "flip_x", context, "$", true)) {
        descriptor.flip_x = *field;
    }
    if (const auto field = boolean_field(
            *root,
            "no_antialiasing",
            context,
            "$",
            true
        )) {
        descriptor.no_antialiasing = *field;
    }
    if (const auto* color = find_member(*root, "healthbar_colors")) {
        descriptor.healthbar_color = parse_rgb(
            *color,
            context,
            "$.healthbar_colors",
            descriptor.healthbar_color
        );
    } else {
        context.schema(
            DescriptorDiagnosticCode::missing_required_field,
            "$.healthbar_colors",
            "character healthbar color is missing"
        );
    }
    if (const auto field = string_field(
            *root,
            "vocals_file",
            context,
            "$",
            false
        )) {
        descriptor.vocals_file = *field;
    }
    if (const auto* editor = find_member(*root, "_editor_isPlayer")) {
        if (!editor->is_null()) {
            descriptor.editor_is_player = as_boolean(
                *editor,
                context,
                "$._editor_isPlayer"
            );
        }
    }
    if (const auto* animations = find_member(*root, "animations")) {
        descriptor.animations = parse_animations(
            *animations,
            context,
            "$.animations"
        );
    } else {
        context.schema(
            DescriptorDiagnosticCode::missing_required_field,
            "$.animations",
            "character animations array is missing"
        );
    }

    descriptor.extensions_json = context.preserve_json(
        unknown_members(
            *root,
            {
                "animations", "image", "scale", "sing_duration",
                "healthicon", "position", "camera_position", "flip_x",
                "no_antialiasing", "healthbar_colors", "vocals_file",
                "_editor_isPlayer",
            }
        ),
        "$.extensions"
    );
    return finish_result<CharacterDescriptor>(std::move(descriptor), context);
}

std::string_view to_string(
    const DescriptorDiagnosticSeverity severity
) noexcept {
    switch (severity) {
        case DescriptorDiagnosticSeverity::warning:
            return "warning";
        case DescriptorDiagnosticSeverity::error:
            return "error";
    }
    return "unknown";
}

std::string_view to_string(const DescriptorDiagnosticCode code) noexcept {
    switch (code) {
        case DescriptorDiagnosticCode::invalid_limits:
            return "invalid_limits";
        case DescriptorDiagnosticCode::input_too_large:
            return "input_too_large";
        case DescriptorDiagnosticCode::invalid_json:
            return "invalid_json";
        case DescriptorDiagnosticCode::root_not_object:
            return "root_not_object";
        case DescriptorDiagnosticCode::nesting_too_deep:
            return "nesting_too_deep";
        case DescriptorDiagnosticCode::node_limit_exceeded:
            return "node_limit_exceeded";
        case DescriptorDiagnosticCode::array_too_large:
            return "array_too_large";
        case DescriptorDiagnosticCode::object_too_large:
            return "object_too_large";
        case DescriptorDiagnosticCode::string_too_long:
            return "string_too_long";
        case DescriptorDiagnosticCode::missing_required_field:
            return "missing_required_field";
        case DescriptorDiagnosticCode::wrong_type:
            return "wrong_type";
        case DescriptorDiagnosticCode::invalid_value:
            return "invalid_value";
        case DescriptorDiagnosticCode::preserved_json_too_large:
            return "preserved_json_too_large";
    }
    return "unknown";
}

}  // namespace pulseforge
