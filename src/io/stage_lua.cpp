#include "pulseforge/stage_lua.hpp"
#include "pulseforge/ascii_number.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace pulseforge {
namespace {

using Value = std::variant<std::string, double, bool>;

[[nodiscard]] bool identifier_start(const char value) noexcept {
    return (value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z') || value == '_';
}

[[nodiscard]] bool identifier_continue(const char value) noexcept {
    return identifier_start(value) || (value >= '0' && value <= '9');
}

[[nodiscard]] bool space(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n'
        || value == '\f' || value == '\v';
}

struct Call {
    std::string name;
    std::vector<Value> arguments;
    std::size_t line{};
};

enum class ScopeKind : std::uint8_t {
    safe_callback,
    unsafe_function,
    dynamic_block,
    transparent_block,
};

class Scanner final {
public:
    Scanner(
        const std::string_view source,
        const StaticStageLuaLimits& limits,
        StaticStageLuaResult& result
    ) : source_(source), limits_(limits), result_(result) {}

    [[nodiscard]] std::vector<Call> run() {
        std::vector<Call> calls;
        calls.reserve(std::min<std::size_t>(limits_.maximum_calls, 2'048U));
        while (cursor_ < source_.size() && calls.size() < limits_.maximum_calls
               && tokens_ < limits_.maximum_tokens) {
            skip_space_and_comments();
            if (cursor_ >= source_.size()) break;
            if (source_[cursor_] == '\'' || source_[cursor_] == '"') {
                skip_quoted_literal();
                boolean_operator_pending_ = false;
                continue;
            }
            if (long_bracket_level(cursor_).has_value()) {
                skip_long_bracket_literal();
                boolean_operator_pending_ = false;
                continue;
            }
            if (!identifier_start(source_[cursor_])) {
                if (source_[cursor_] == ';') {
                    statement_unsafe_ = false;
                    boolean_operator_pending_ = false;
                    expression_parentheses_ = 0U;
                } else if (source_[cursor_] == '(') {
                    ++expression_parentheses_;
                    boolean_operator_pending_ = false;
                } else if (source_[cursor_] == ')') {
                    expression_parentheses_ -= expression_parentheses_ != 0U
                        ? 1U
                        : 0U;
                    boolean_operator_pending_ = false;
                } else {
                    boolean_operator_pending_ = false;
                }
                advance();
                continue;
            }
            const std::size_t call_line = line_;
            const auto name = read_identifier();
            if (name == "and" || name == "or") {
                statement_unsafe_ = true;
                boolean_operator_pending_ = true;
                continue;
            }
            const bool unsafe_statement = statement_unsafe_;
            boolean_operator_pending_ = false;
            if (name == "function") {
                begin_function(call_line);
                continue;
            }
            if (name == "if" || name == "for" || name == "while"
                || name == "repeat") {
                scopes_.push_back(ScopeKind::dynamic_block);
                pending_loop_do_ += name == "for" || name == "while" ? 1U : 0U;
                diagnose(
                    call_line,
                    "dynamic stage Lua branch was ignored by the static importer"
                );
                continue;
            }
            if (name == "do") {
                if (pending_loop_do_ != 0U) {
                    --pending_loop_do_;
                } else {
                    scopes_.push_back(ScopeKind::transparent_block);
                }
                continue;
            }
            if (name == "return") {
                // Reachability after a return can depend on a preceding
                // lowQuality/platform condition.  The static importer does not
                // execute that condition, so conservatively ignore the rest of
                // this allowed callback instead of materializing sprites that
                // may never exist at runtime.
                static_reachability_terminated_ = true;
                diagnose(
                    call_line,
                    "stage Lua after return was ignored by the static importer"
                );
                continue;
            }
            if (name == "end") {
                if (!scopes_.empty()) {
                    const auto ended = scopes_.back();
                    scopes_.pop_back();
                    if (ended == ScopeKind::safe_callback) {
                        static_reachability_terminated_ = false;
                    }
                }
                continue;
            }
            if (name == "until") {
                if (!scopes_.empty()) scopes_.pop_back();
                // `until` closes the repeat block syntactically, but its
                // condition is still evaluated dynamically. Calls in that
                // expression must never become unconditional stage data.
                statement_unsafe_ = true;
                boolean_operator_pending_ = false;
                continue;
            }
            skip_space_and_comments();
            if (cursor_ >= source_.size() || source_[cursor_] != '(') continue;
            advance();
            auto arguments = read_arguments();
            if (arguments.has_value()) {
                if (safe_context() && !unsafe_statement) {
                    calls.push_back({
                        std::string(name),
                        std::move(*arguments),
                        call_line,
                    });
                } else {
                    diagnose(
                        call_line,
                        "dynamic call " + std::string(name)
                            + " was ignored by the static importer"
                    );
                }
            }
        }
        if ((calls.size() >= limits_.maximum_calls
             || tokens_ >= limits_.maximum_tokens)
            && cursor_ < source_.size()) {
            result_.truncated = true;
            diagnose(line_, "stage Lua scan budget was exhausted");
        }
        return calls;
    }

private:
    [[nodiscard]] std::optional<std::size_t> long_bracket_level(
        const std::size_t offset
    ) const noexcept {
        if (offset >= source_.size() || source_[offset] != '[') {
            return std::nullopt;
        }
        std::size_t cursor = offset + 1U;
        while (cursor < source_.size() && source_[cursor] == '=') ++cursor;
        return cursor < source_.size() && source_[cursor] == '['
            ? std::optional<std::size_t>{cursor - offset - 1U}
            : std::nullopt;
    }

    void skip_quoted_literal() noexcept {
        if (cursor_ >= source_.size()
            || (source_[cursor_] != '\'' && source_[cursor_] != '"')) {
            return;
        }
        const char quote = source_[cursor_];
        advance();
        while (cursor_ < source_.size()) {
            const char value = source_[cursor_];
            advance();
            if (value == quote) return;
            if (value == '\\' && cursor_ < source_.size()) advance();
        }
    }

    void skip_long_bracket_literal() noexcept {
        const auto level = long_bracket_level(cursor_);
        if (!level.has_value()) return;
        for (std::size_t index = 0U; index < *level + 2U; ++index) advance();
        while (cursor_ < source_.size()) {
            if (source_[cursor_] == ']') {
                std::size_t closing = cursor_ + 1U;
                std::size_t equals{};
                while (closing < source_.size() && source_[closing] == '=') {
                    ++closing;
                    ++equals;
                }
                if (equals == *level && closing < source_.size()
                    && source_[closing] == ']') {
                    while (cursor_ <= closing) advance();
                    return;
                }
            }
            advance();
        }
    }

    [[nodiscard]] bool safe_context() const noexcept {
        if (static_reachability_terminated_) return false;
        std::size_t callbacks{};
        for (const auto scope : scopes_) {
            if (scope == ScopeKind::transparent_block) continue;
            if (scope != ScopeKind::safe_callback) return false;
            ++callbacks;
        }
        return callbacks <= 1U;
    }

    void begin_function(const std::size_t line) {
        skip_space_and_comments();
        std::string_view name;
        if (cursor_ < source_.size() && identifier_start(source_[cursor_])) {
            name = read_identifier();
        }
        skip_space_and_comments();
        if (cursor_ < source_.size() && source_[cursor_] == '(') {
            advance();
            skip_to_call_end();
        }
        const bool safe = name == "onCreate" || name == "onCreatePost";
        if (safe) static_reachability_terminated_ = false;
        scopes_.push_back(
            safe ? ScopeKind::safe_callback : ScopeKind::unsafe_function
        );
        if (!safe) {
            diagnose(
                line,
                "callback " + std::string(
                    name.empty() ? std::string_view{"<anonymous>"} : name
                )
                    + " was ignored by the static importer"
            );
        }
    }

    void diagnose(const std::size_t line, std::string message) {
        if (result_.diagnostics.size() < limits_.maximum_diagnostics) {
            result_.diagnostics.push_back({line, std::move(message)});
        }
    }

    void advance() noexcept {
        if (cursor_ < source_.size()) {
            line_ += source_[cursor_] == '\n' ? 1U : 0U;
            ++cursor_;
            ++tokens_;
        }
    }

    void skip_space_and_comments() noexcept {
        for (;;) {
            while (cursor_ < source_.size() && space(source_[cursor_])) {
                if (source_[cursor_] == '\n' && !boolean_operator_pending_
                    && expression_parentheses_ == 0U) {
                    statement_unsafe_ = false;
                }
                advance();
            }
            if (cursor_ + 1U >= source_.size() || source_[cursor_] != '-'
                || source_[cursor_ + 1U] != '-') return;
            advance();
            advance();
            if (long_bracket_level(cursor_).has_value()) {
                skip_long_bracket_literal();
            } else {
                while (cursor_ < source_.size() && source_[cursor_] != '\n') advance();
            }
        }
    }

    [[nodiscard]] std::string_view read_identifier() noexcept {
        const auto first = cursor_;
        advance();
        while (cursor_ < source_.size() && identifier_continue(source_[cursor_])) {
            advance();
        }
        return source_.substr(first, cursor_ - first);
    }

    [[nodiscard]] std::optional<std::string> read_string() {
        const char quote = source_[cursor_];
        advance();
        std::string value;
        value.reserve(64U);
        while (cursor_ < source_.size()) {
            const char character = source_[cursor_];
            advance();
            if (character == quote) return value;
            if (character == '\r' || character == '\n') return std::nullopt;
            if (character == '\\' && cursor_ < source_.size()) {
                const char escaped = source_[cursor_];
                advance();
                if (escaped == 'n') value.push_back('\n');
                else if (escaped == 'r') value.push_back('\r');
                else if (escaped == 't') value.push_back('\t');
                else value.push_back(escaped);
            } else {
                value.push_back(character);
            }
            if (value.size() > limits_.maximum_string_bytes) return std::nullopt;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<double> read_number() noexcept {
        const auto first = cursor_;
        if (source_[cursor_] == '+' || source_[cursor_] == '-') advance();
        bool digits{};
        while (cursor_ < source_.size() && source_[cursor_] >= '0'
               && source_[cursor_] <= '9') {
            digits = true;
            advance();
        }
        if (cursor_ < source_.size() && source_[cursor_] == '.') {
            advance();
            while (cursor_ < source_.size() && source_[cursor_] >= '0'
                   && source_[cursor_] <= '9') {
                digits = true;
                advance();
            }
        }
        if (!digits) return std::nullopt;
        if (cursor_ < source_.size()
            && (source_[cursor_] == 'e' || source_[cursor_] == 'E')) {
            advance();
            if (cursor_ < source_.size()
                && (source_[cursor_] == '+' || source_[cursor_] == '-')) advance();
            while (cursor_ < source_.size() && source_[cursor_] >= '0'
                   && source_[cursor_] <= '9') advance();
        }
        const auto source = source_.substr(first, cursor_ - first);
        double value{};
        return parse_ascii_floating(source, value)
            ? std::optional<double>{value}
            : std::nullopt;
    }

    void skip_to_call_end() noexcept {
        std::size_t depth{1U};
        while (cursor_ < source_.size() && depth != 0U) {
            if (source_[cursor_] == '\'' || source_[cursor_] == '"') {
                skip_quoted_literal();
                continue;
            }
            if (long_bracket_level(cursor_).has_value()) {
                skip_long_bracket_literal();
                continue;
            }
            const char value = source_[cursor_];
            if (value == '(') ++depth;
            else if (value == ')') --depth;
            advance();
        }
    }

    [[nodiscard]] std::optional<std::vector<Value>> read_arguments() {
        std::vector<Value> values;
        bool expect_value{true};
        while (cursor_ < source_.size()) {
            skip_space_and_comments();
            if (cursor_ >= source_.size()) return std::nullopt;
            if (source_[cursor_] == ')') {
                advance();
                return values;
            }
            if (!expect_value) {
                if (source_[cursor_] == ',') {
                    advance();
                    expect_value = true;
                    continue;
                }
                skip_to_call_end();
                return std::nullopt;
            }
            if (source_[cursor_] == '\'' || source_[cursor_] == '"') {
                auto value = read_string();
                if (!value.has_value()) {
                    skip_to_call_end();
                    return std::nullopt;
                }
                values.emplace_back(std::move(*value));
            } else if (source_[cursor_] == '+' || source_[cursor_] == '-'
                       || (source_[cursor_] >= '0' && source_[cursor_] <= '9')) {
                auto value = read_number();
                if (!value.has_value()) {
                    skip_to_call_end();
                    return std::nullopt;
                }
                values.emplace_back(*value);
            } else if (identifier_start(source_[cursor_])) {
                const auto value = read_identifier();
                if (value == "true") values.emplace_back(true);
                else if (value == "false") values.emplace_back(false);
                else {
                    skip_to_call_end();
                    return std::nullopt;
                }
            } else {
                skip_to_call_end();
                return std::nullopt;
            }
            expect_value = false;
        }
        return std::nullopt;
    }

    std::string_view source_;
    const StaticStageLuaLimits& limits_;
    StaticStageLuaResult& result_;
    std::size_t cursor_{};
    std::size_t line_{1U};
    std::size_t tokens_{};
    std::vector<ScopeKind> scopes_;
    std::size_t pending_loop_do_{};
    std::size_t expression_parentheses_{};
    bool statement_unsafe_{};
    bool boolean_operator_pending_{};
    bool static_reachability_terminated_{};
};

[[nodiscard]] const std::string* text(
    const std::vector<Value>& values,
    const std::size_t index
) noexcept {
    return index < values.size() ? std::get_if<std::string>(&values[index]) : nullptr;
}

[[nodiscard]] const double* number(
    const std::vector<Value>& values,
    const std::size_t index
) noexcept {
    return index < values.size() ? std::get_if<double>(&values[index]) : nullptr;
}

[[nodiscard]] const bool* boolean(
    const std::vector<Value>& values,
    const std::size_t index
) noexcept {
    return index < values.size() ? std::get_if<bool>(&values[index]) : nullptr;
}

struct SpriteState {
    StageObjectDescriptor object;
    bool added{};
    bool visible{true};
    std::int64_t order{};
    std::size_t declaration_order{};
};

class StaticStageBuilder final {
public:
    StaticStageBuilder(
        const std::string_view id,
        const StaticStageLuaLimits& limits,
        StaticStageLuaResult& result
    ) : limits_(limits), result_(result) {
        stage_.id.assign(id.empty() ? "stage" : id);
    }

    void apply(const Call& call) {
        const auto& name = call.name;
        if (name == "makeLuaSprite" || name == "makeAnimatedLuaSprite") {
            make_sprite(call, name == "makeAnimatedLuaSprite");
        } else if (name == "addLuaSprite" || name == "addBehindGF"
                   || name == "addBehindDad" || name == "addBehindBF") {
            if (auto* value = find(call.arguments); value != nullptr) {
                value->added = true;
                if (name == "addLuaSprite") {
                    if (const auto* foreground = boolean(call.arguments, 1U)) {
                        value->object.foreground = *foreground;
                        value->object.layer = *foreground
                            ? StageObjectLayer::foreground
                            : StageObjectLayer::background;
                    }
                } else if (name == "addBehindGF") {
                    value->object.layer = StageObjectLayer::behind_girlfriend;
                    value->object.foreground = false;
                } else if (name == "addBehindDad") {
                    value->object.layer = StageObjectLayer::behind_opponent;
                    value->object.foreground = false;
                } else {
                    value->object.layer = StageObjectLayer::behind_player;
                    value->object.foreground = false;
                }
            }
        } else if (name == "scaleObject") {
            if (auto* value = find(call.arguments); value != nullptr) {
                if (const auto* x = number(call.arguments, 1U)) value->object.scale.x = *x;
                if (const auto* y = number(call.arguments, 2U)) value->object.scale.y = *y;
            }
        } else if (name == "setScrollFactor"
                   || name == "setLuaSpriteScrollFactor") {
            if (auto* value = find(call.arguments); value != nullptr) {
                if (const auto* x = number(call.arguments, 1U)) value->object.scroll.x = *x;
                if (const auto* y = number(call.arguments, 2U)) value->object.scroll.y = *y;
            }
        } else if (name == "addAnimationByPrefix") {
            add_animation(call, false, false);
        } else if (name == "addAnimationByIndices"
                   || name == "addAnimationByIndicesLoop") {
            add_animation(
                call,
                true,
                name == "addAnimationByIndicesLoop"
            );
        } else if (name == "objectPlayAnimation" || name == "playAnim") {
            if (auto* value = find(call.arguments); value != nullptr) {
                if (const auto* id = text(call.arguments, 1U)) {
                    value->object.first_animation = *id;
                }
            }
        } else if (name == "setProperty") {
            set_property(call.arguments);
        } else if (name == "setPropertyLuaSprite") {
            set_sprite_property(call.arguments);
        } else if (name == "makeGraphic") {
            make_graphic(call.arguments);
        } else if (name == "setObjectCamera") {
            set_object_camera(call.arguments);
        } else if (name == "setBlendMode") {
            set_blend_mode(call.arguments);
        } else if (name == "setObjectOrder") {
            if (auto* value = find(call.arguments); value != nullptr) {
                if (const auto* order = number(call.arguments, 1U);
                    order != nullptr && *order >= -1'000'000.0
                    && *order <= 1'000'000.0) {
                    value->order = static_cast<std::int64_t>(*order);
                }
            }
        }
    }

    [[nodiscard]] StageDescriptor finish() {
        std::vector<SpriteState*> ordered;
        ordered.reserve(sprites_.size());
        for (auto& [tag, value] : sprites_) {
            static_cast<void>(tag);
            if (value.added && value.visible) ordered.push_back(&value);
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
            return left->order == right->order
                ? left->declaration_order < right->declaration_order
                : left->order < right->order;
        });
        stage_.objects.reserve(ordered.size());
        for (auto* value : ordered) stage_.objects.push_back(std::move(value->object));
        return std::move(stage_);
    }

private:
    void diagnose(const std::size_t line, std::string message) {
        if (result_.diagnostics.size() < limits_.maximum_diagnostics) {
            result_.diagnostics.push_back({line, std::move(message)});
        }
    }

    [[nodiscard]] SpriteState* find(const std::vector<Value>& arguments) {
        const auto* tag = text(arguments, 0U);
        if (tag == nullptr) return nullptr;
        const auto found = sprites_.find(*tag);
        return found == sprites_.end() ? nullptr : &found->second;
    }

    void make_sprite(const Call& call, const bool animated) {
        const auto* tag = text(call.arguments, 0U);
        const auto* image = text(call.arguments, 1U);
        const auto* x = number(call.arguments, 2U);
        const auto* y = number(call.arguments, 3U);
        if (tag == nullptr || image == nullptr || x == nullptr || y == nullptr
            || tag->empty()) return;
        if (sprites_.size() >= limits_.maximum_sprites
            && sprites_.find(*tag) == sprites_.end()) {
            result_.truncated = true;
            diagnose(call.line, "stage Lua sprite budget was exhausted");
            return;
        }
        SpriteState state;
        state.object.type = animated ? "animated" : "sprite";
        state.object.name = *tag;
        state.object.image = *image;
        state.object.x = *x;
        state.object.y = *y;
        state.declaration_order = declaration_order_++;
        sprites_.insert_or_assign(*tag, std::move(state));
    }

    [[nodiscard]] static std::vector<std::int32_t> parse_indices(
        const std::string& source
    ) {
        std::vector<std::int32_t> result;
        result.reserve(std::min<std::size_t>(
            64U,
            static_cast<std::size_t>(
                std::count(source.begin(), source.end(), ',') + 1
            )
        ));
        std::size_t first{};
        while (first < source.size() && result.size() < 4'096U) {
            while (first < source.size()
                   && (source[first] == ' ' || source[first] == '\t'
                       || source[first] == ',')) ++first;
            if (first >= source.size()) break;
            const auto last = source.find(',', first);
            const auto end = last == std::string::npos ? source.size() : last;
            std::int32_t value{};
            const auto parsed = std::from_chars(
                source.data() + first,
                source.data() + end,
                value
            );
            if (parsed.ec == std::errc{} && parsed.ptr == source.data() + end
                && value >= 0) {
                result.push_back(value);
            }
            if (last == std::string::npos) break;
            first = last + 1U;
        }
        return result;
    }

    void add_animation(
        const Call& call,
        const bool indexed,
        const bool force_loop
    ) {
        auto* value = find(call.arguments);
        const auto* id = text(call.arguments, 1U);
        const auto* prefix = text(call.arguments, 2U);
        if (value == nullptr || id == nullptr || prefix == nullptr) return;
        if (animation_count_ >= limits_.maximum_animations) {
            result_.truncated = true;
            diagnose(call.line, "stage Lua animation budget was exhausted");
            return;
        }
        AnimationDescriptor animation;
        animation.id = *id;
        animation.name = *prefix;
        const std::size_t fps_index = indexed ? 4U : 3U;
        if (indexed) {
            if (const auto* indices = text(call.arguments, 3U)) {
                animation.indices = parse_indices(*indices);
            }
        }
        if (const auto* fps = number(call.arguments, fps_index);
            fps != nullptr && *fps >= 1.0 && *fps <= 1'000.0) {
            animation.fps = static_cast<std::int32_t>(*fps);
        }
        animation.loop = force_loop;
        if (const auto* loop = boolean(call.arguments, fps_index + 1U)) {
            animation.loop = *loop;
        }
        value->object.animations.push_back(std::move(animation));
        ++animation_count_;
    }

    void set_property(const std::vector<Value>& arguments) {
        const auto* path = text(arguments, 0U);
        if (path == nullptr) return;
        const auto separator = path->rfind('.');
        if (separator == std::string::npos) return;
        const auto tag = path->substr(0U, separator);
        const auto property = path->substr(separator + 1U);
        if (tag == "gf" && property == "visible") {
            if (const auto* value = boolean(arguments, 1U)) stage_.hide_girlfriend = !*value;
            return;
        }
        const auto found = sprites_.find(tag);
        if (found == sprites_.end()) return;
        auto& value = found->second;
        if (property == "visible") {
            if (const auto* input = boolean(arguments, 1U)) value.visible = *input;
        } else if (property == "antialiasing") {
            if (const auto* input = boolean(arguments, 1U)) value.object.antialiasing = *input;
        } else if (property == "flipX") {
            if (const auto* input = boolean(arguments, 1U)) value.object.flip_x = *input;
        } else if (property == "flipY") {
            if (const auto* input = boolean(arguments, 1U)) value.object.flip_y = *input;
        } else if (const auto* input = number(arguments, 1U)) {
            if (property == "x") value.object.x = *input;
            else if (property == "y") value.object.y = *input;
            else if (property == "alpha") value.object.alpha = *input;
            else if (property == "angle") value.object.angle = *input;
        }
    }

    void set_sprite_property(const std::vector<Value>& arguments) {
        const auto* tag = text(arguments, 0U);
        const auto* property = text(arguments, 1U);
        if (tag == nullptr || property == nullptr || arguments.size() < 3U) return;
        std::vector<Value> translated;
        translated.reserve(2U);
        translated.emplace_back(*tag + "." + *property);
        translated.push_back(arguments[2U]);
        set_property(translated);
    }

    void make_graphic(const std::vector<Value>& arguments) {
        auto* value = find(arguments);
        const auto* width = number(arguments, 1U);
        const auto* height = number(arguments, 2U);
        const auto* color = text(arguments, 3U);
        if (value == nullptr || width == nullptr || height == nullptr
            || *width <= 0.0 || *height <= 0.0) return;
        value->object.type = "solid";
        value->object.image.clear();
        value->object.width = *width;
        value->object.height = *height;
        if (color != nullptr && !color->empty()) value->object.color = *color;
    }

    void set_object_camera(const std::vector<Value>& arguments) {
        auto* value = find(arguments);
        const auto* camera = text(arguments, 1U);
        if (value == nullptr || camera == nullptr) return;
        const auto equals_camera = [&](const std::string_view expected) {
            return camera->size() == expected.size()
                && std::equal(
                    camera->begin(),
                    camera->end(),
                    expected.begin(),
                    [](const char left, const char right) {
                        const auto lower = [](const unsigned char value) {
                            return value >= 'A' && value <= 'Z'
                                ? static_cast<unsigned char>(value - 'A' + 'a')
                                : value;
                        };
                        return lower(static_cast<unsigned char>(left))
                            == lower(static_cast<unsigned char>(right));
                    }
                );
        };
        value->object.screen_space = equals_camera("hud")
            || equals_camera("camHUD")
            || equals_camera("other")
            || equals_camera("camOther");
    }

    void set_blend_mode(const std::vector<Value>& arguments) {
        auto* value = find(arguments);
        const auto* blend = text(arguments, 1U);
        if (value != nullptr && blend != nullptr) value->object.blend = *blend;
    }

    const StaticStageLuaLimits& limits_;
    StaticStageLuaResult& result_;
    StageDescriptor stage_;
    std::map<std::string, SpriteState, std::less<>> sprites_;
    std::size_t animation_count_{};
    std::size_t declaration_order_{};
};

}  // namespace

StaticStageLuaResult parse_static_psych_stage_lua(
    const std::string_view source,
    const std::string_view stage_id,
    const StaticStageLuaLimits& limits
) {
    StaticStageLuaResult result;
    if (source.size() > limits.maximum_source_bytes || limits.maximum_tokens == 0U
        || limits.maximum_calls == 0U || limits.maximum_sprites == 0U
        || limits.maximum_animations == 0U || limits.maximum_string_bytes == 0U
        || limits.maximum_diagnostics == 0U) {
        result.diagnostics.push_back({0U, "invalid or exhausted stage Lua limits"});
        return result;
    }
    Scanner scanner(source, limits, result);
    const auto calls = scanner.run();
    StaticStageBuilder builder(stage_id, limits, result);
    for (const auto& call : calls) builder.apply(call);
    result.stage = builder.finish();
    return result;
}

}  // namespace pulseforge
