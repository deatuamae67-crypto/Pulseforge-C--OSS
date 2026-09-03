#include "editor_ui.hpp"

#include "pulseforge/audio_transport.hpp"
#include "pulseforge/note_types.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <unordered_map>
#include <vector>

namespace pulseforge {
namespace {

constexpr float canvas_width = 1'280.0F;
constexpr float canvas_height = 720.0F;
constexpr SDL_FRect chart_grid{28.0F, 112.0F, 790.0F, 520.0F};
constexpr SDL_FRect inspector_panel{838.0F, 94.0F, 414.0F, 548.0F};

constexpr SDL_Color background_top{10, 10, 31, 255};
constexpr SDL_Color background_bottom{25, 16, 48, 255};
constexpr SDL_Color panel_color{25, 22, 52, 242};
constexpr SDL_Color panel_border{87, 73, 137, 255};
constexpr SDL_Color cyan{94, 236, 255, 255};
constexpr SDL_Color purple{154, 103, 255, 255};
constexpr SDL_Color yellow{255, 224, 105, 255};
constexpr SDL_Color white{236, 232, 248, 255};
constexpr SDL_Color muted{174, 164, 202, 255};
constexpr SDL_Color danger{255, 116, 139, 255};
constexpr SDL_Color success{112, 234, 164, 255};

[[nodiscard]] std::string printable(
    const std::string_view value,
    const std::size_t maximum
) {
    std::string output;
    output.reserve(std::min(value.size(), maximum));
    for (const unsigned char character : value) {
        if (output.size() >= maximum) {
            break;
        }
        output.push_back(
            character >= 32U && character <= 126U
                ? static_cast<char>(character)
                : '?'
        );
    }
    if (value.size() > maximum && maximum >= 3U) {
        output.resize(maximum - 3U);
        output += "...";
    }
    return output;
}

void fill(
    SDL_Renderer* renderer,
    const SDL_FRect& rectangle,
    const SDL_Color color
) {
    static_cast<void>(SDL_SetRenderDrawColor(
        renderer,
        color.r,
        color.g,
        color.b,
        color.a
    ));
    static_cast<void>(SDL_RenderFillRect(renderer, &rectangle));
}

void outline(
    SDL_Renderer* renderer,
    const SDL_FRect& rectangle,
    const SDL_Color color
) {
    static_cast<void>(SDL_SetRenderDrawColor(
        renderer,
        color.r,
        color.g,
        color.b,
        color.a
    ));
    static_cast<void>(SDL_RenderRect(renderer, &rectangle));
}

void line(
    SDL_Renderer* renderer,
    const float x1,
    const float y1,
    const float x2,
    const float y2,
    const SDL_Color color
) {
    static_cast<void>(SDL_SetRenderDrawColor(
        renderer,
        color.r,
        color.g,
        color.b,
        color.a
    ));
    static_cast<void>(SDL_RenderLine(renderer, x1, y1, x2, y2));
}

void text(
    SDL_Renderer* renderer,
    const float x,
    const float y,
    const std::string_view value,
    const SDL_Color color = white,
    const float scale = 1.25F,
    const std::size_t maximum = 160U
) {
    float previous_x = 1.0F;
    float previous_y = 1.0F;
    static_cast<void>(SDL_GetRenderScale(renderer, &previous_x, &previous_y));
    static_cast<void>(SDL_SetRenderDrawColor(
        renderer,
        color.r,
        color.g,
        color.b,
        color.a
    ));
    static_cast<void>(SDL_SetRenderScale(renderer, scale, scale));
    const auto safe = printable(value, maximum);
    static_cast<void>(SDL_RenderDebugText(
        renderer,
        x / scale,
        y / scale,
        safe.c_str()
    ));
    static_cast<void>(SDL_SetRenderScale(renderer, previous_x, previous_y));
}

[[nodiscard]] bool contains(
    const SDL_FRect& rectangle,
    const float x,
    const float y
) noexcept {
    return x >= rectangle.x && x <= rectangle.x + rectangle.w
        && y >= rectangle.y && y <= rectangle.y + rectangle.h;
}

void draw_background(
    SDL_Renderer* renderer,
    const std::uint64_t ticks,
    const SDL_Color accent
) {
    constexpr int bands = 32;
    for (int index = 0; index < bands; ++index) {
        const float ratio = static_cast<float>(index)
            / static_cast<float>(bands - 1);
        const auto red = static_cast<std::uint8_t>(
            background_top.r
            + ratio * static_cast<float>(background_bottom.r - background_top.r)
        );
        const auto green = static_cast<std::uint8_t>(
            background_top.g
            + ratio
                * static_cast<float>(background_bottom.g - background_top.g)
        );
        const auto blue = static_cast<std::uint8_t>(
            background_top.b
            + ratio
                * static_cast<float>(background_bottom.b - background_top.b)
        );
        fill(
            renderer,
            {0.0F,
             ratio * canvas_height,
             canvas_width,
             canvas_height / static_cast<float>(bands) + 1.0F},
            {red, green, blue, 255}
        );
    }
    const auto wave = static_cast<float>(
        std::sin(static_cast<double>(ticks) * 0.00055)
    );
    fill(renderer, {0.0F, 0.0F, canvas_width, 82.0F}, {22, 15, 48, 250});
    fill(renderer, {0.0F, 80.0F, canvas_width, 2.0F}, accent);
    fill(
        renderer,
        {820.0F + wave * 90.0F, -90.0F, 520.0F, 210.0F},
        {accent.r, accent.g, accent.b, 22}
    );
    for (int index = 0; index < 24; ++index) {
        const auto seed = static_cast<std::uint64_t>(index) * 11'719ULL;
        const float x = static_cast<float>((seed + ticks / 24ULL) % 1'340ULL)
            - 30.0F;
        const float y = 96.0F + static_cast<float>((seed * 7ULL) % 590ULL);
        fill(renderer, {x, y, 2.0F, 2.0F}, {accent.r, accent.g, accent.b, 75});
    }
}

void panel(
    SDL_Renderer* renderer,
    const SDL_FRect& rectangle,
    const SDL_Color border = panel_border
) {
    fill(renderer, rectangle, panel_color);
    outline(renderer, rectangle, border);
}

[[nodiscard]] bool button(
    SDL_Renderer* renderer,
    const SDL_FRect& rectangle,
    const std::string_view label,
    const bool hovered,
    const bool enabled = true,
    const SDL_Color accent = cyan
) {
    const SDL_Color base = !enabled
        ? SDL_Color{42, 39, 61, 230}
        : hovered ? SDL_Color{62, 53, 105, 245}
                  : SDL_Color{43, 37, 78, 238};
    fill(renderer, rectangle, base);
    outline(renderer, rectangle, enabled ? accent : SDL_Color{88, 84, 105, 255});
    text(
        renderer,
        rectangle.x + 10.0F,
        rectangle.y + rectangle.h * 0.5F - 5.0F,
        label,
        enabled ? white : SDL_Color{116, 111, 131, 255},
        1.05F,
        45U
    );
    return hovered && enabled;
}

[[nodiscard]] std::string format_number(
    const double value,
    const int precision = 2
) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

[[nodiscard]] std::string format_time(const double milliseconds) {
    const auto bounded = std::max(0.0, milliseconds);
    const auto total_seconds = static_cast<std::uint64_t>(bounded / 1'000.0);
    const auto minutes = total_seconds / 60U;
    const auto seconds = total_seconds % 60U;
    const auto millis = static_cast<std::uint32_t>(
        std::fmod(bounded, 1'000.0)
    );
    std::ostringstream stream;
    stream << minutes << ':' << std::setw(2) << std::setfill('0') << seconds
           << '.' << std::setw(3) << millis;
    return stream.str();
}

void erase_last_utf8(std::string& value) {
    if (value.empty()) {
        return;
    }
    std::size_t index = value.size() - 1U;
    while (index > 0U
           && (static_cast<unsigned char>(value[index]) & 0xC0U) == 0x80U) {
        --index;
    }
    value.erase(index);
}

void append_bounded_utf8(
    std::string& destination,
    const std::string_view incoming,
    const std::size_t maximum_bytes
) {
    const auto available = maximum_bytes > destination.size()
        ? maximum_bytes - destination.size()
        : 0U;
    destination.append(
        incoming.substr(
            0U,
            bounded_chart_text_prefix_bytes(incoming, available)
        )
    );
}

[[nodiscard]] std::string trim(std::string_view value) {
    while (!value.empty()
           && (value.front() == ' ' || value.front() == '\t'
               || value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1U);
    }
    while (!value.empty()
           && (value.back() == ' ' || value.back() == '\t'
               || value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1U);
    }
    return std::string(value);
}

[[nodiscard]] bool parse_double(
    const std::string_view value,
    double& result
) {
    const auto cleaned = trim(value);
    if (cleaned.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(
        cleaned.data(),
        cleaned.data() + cleaned.size(),
        result,
        std::chars_format::general
    );
    return parsed.ec == std::errc{}
        && parsed.ptr == cleaned.data() + cleaned.size()
        && std::isfinite(result);
}

[[nodiscard]] bool parse_int(
    const std::string_view value,
    int& result
) {
    const auto cleaned = trim(value);
    if (cleaned.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(
        cleaned.data(),
        cleaned.data() + cleaned.size(),
        result
    );
    return parsed.ec == std::errc{}
        && parsed.ptr == cleaned.data() + cleaned.size();
}

[[nodiscard]] std::vector<std::string> split(
    const std::string_view value,
    const char delimiter,
    const bool keep_empty = true
) {
    std::vector<std::string> result;
    std::size_t start = 0U;
    for (std::size_t index = 0U; index <= value.size(); ++index) {
        if (index == value.size() || value[index] == delimiter) {
            auto item = trim(value.substr(start, index - start));
            if (keep_empty || !item.empty()) {
                result.push_back(std::move(item));
            }
            start = index + 1U;
        }
    }
    return result;
}

class EditorSessionGuard final {
public:
    EditorSessionGuard(
        SDL_Window* window,
        SDL_Renderer* renderer,
        const std::string_view title
    ) : window_(window), renderer_(renderer) {
        const char* previous = SDL_GetWindowTitle(window_);
        previous_title_ = previous != nullptr ? previous : "PulseForge";
        static_cast<void>(SDL_GetRenderScale(renderer_, &scale_x_, &scale_y_));
        static_cast<void>(SDL_GetRenderDrawBlendMode(renderer_, &blend_mode_));
        logical_presentation_saved_ = SDL_GetRenderLogicalPresentation(
            renderer_,
            &logical_width_,
            &logical_height_,
            &logical_mode_
        );
        text_was_active_ = SDL_TextInputActive(window_);
        static_cast<void>(SDL_SetWindowTitle(
            window_,
            std::string(title).c_str()
        ));
        static_cast<void>(SDL_SetRenderDrawBlendMode(
            renderer_,
            SDL_BLENDMODE_BLEND
        ));
        static_cast<void>(SDL_SetRenderLogicalPresentation(
            renderer_,
            static_cast<int>(canvas_width),
            static_cast<int>(canvas_height),
            SDL_LOGICAL_PRESENTATION_LETTERBOX
        ));
    }

    ~EditorSessionGuard() {
        if (text_was_active_ && !SDL_TextInputActive(window_)) {
            static_cast<void>(SDL_StartTextInput(window_));
        } else if (!text_was_active_ && SDL_TextInputActive(window_)) {
            static_cast<void>(SDL_StopTextInput(window_));
        }
        static_cast<void>(SDL_SetRenderScale(renderer_, scale_x_, scale_y_));
        static_cast<void>(SDL_SetRenderDrawBlendMode(renderer_, blend_mode_));
        if (logical_presentation_saved_) {
            static_cast<void>(SDL_SetRenderLogicalPresentation(
                renderer_,
                logical_width_,
                logical_height_,
                logical_mode_
            ));
        }
        static_cast<void>(SDL_SetWindowTitle(window_, previous_title_.c_str()));
    }

    EditorSessionGuard(const EditorSessionGuard&) = delete;
    EditorSessionGuard& operator=(const EditorSessionGuard&) = delete;

private:
    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    std::string previous_title_;
    float scale_x_{1.0F};
    float scale_y_{1.0F};
    SDL_BlendMode blend_mode_{SDL_BLENDMODE_NONE};
    int logical_width_{};
    int logical_height_{};
    SDL_RendererLogicalPresentation logical_mode_{
        SDL_LOGICAL_PRESENTATION_DISABLED
    };
    bool logical_presentation_saved_{};
    bool text_was_active_{};
};

class InlineTextEditor final {
public:
    using Commit = std::function<bool(std::string_view, std::string&)>;

    void begin(
        SDL_Window* window,
        std::string label,
        std::string initial,
        const std::size_t maximum_bytes,
        Commit commit
    ) {
        cancel(window);
        label_ = std::move(label);
        value_ = std::move(initial);
        maximum_bytes_ = maximum_bytes;
        commit_ = std::move(commit);
        active_ = true;
        select_all_ = true;
        error_.clear();
        static_cast<void>(SDL_StartTextInput(window));
    }

    void cancel(SDL_Window* window) {
        if (active_) {
            static_cast<void>(SDL_StopTextInput(window));
        }
        active_ = false;
        select_all_ = false;
        commit_ = {};
        error_.clear();
    }

    [[nodiscard]] bool active() const noexcept {
        return active_;
    }

    [[nodiscard]] bool handle(SDL_Window* window, const SDL_Event& event) {
        if (!active_) {
            return false;
        }
        if (event.type == SDL_EVENT_TEXT_INPUT) {
            const std::string_view incoming(event.text.text);
            if (select_all_) {
                value_.clear();
                select_all_ = false;
            }
            const auto available = maximum_bytes_ > value_.size()
                ? maximum_bytes_ - value_.size()
                : 0U;
            value_.append(incoming.substr(0U, available));
            error_.clear();
            return true;
        }
        if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
            return true;
        }

        const bool control = (event.key.mod & SDL_KMOD_CTRL) != 0;
        if (control && event.key.scancode == SDL_SCANCODE_A) {
            select_all_ = true;
            return true;
        }
        if (control && event.key.scancode == SDL_SCANCODE_C) {
            static_cast<void>(SDL_SetClipboardText(value_.c_str()));
            return true;
        }
        if (control && event.key.scancode == SDL_SCANCODE_V) {
            char* clipboard = SDL_GetClipboardText();
            if (clipboard != nullptr) {
                if (select_all_) {
                    value_.clear();
                    select_all_ = false;
                }
                const std::string_view incoming(clipboard);
                const auto available = maximum_bytes_ > value_.size()
                    ? maximum_bytes_ - value_.size()
                    : 0U;
                value_.append(incoming.substr(0U, available));
                SDL_free(clipboard);
            }
            return true;
        }

        switch (event.key.scancode) {
        case SDL_SCANCODE_ESCAPE:
            cancel(window);
            break;
        case SDL_SCANCODE_BACKSPACE:
            if (select_all_) {
                value_.clear();
                select_all_ = false;
            } else {
                erase_last_utf8(value_);
            }
            error_.clear();
            break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: {
            std::string commit_error;
            if (commit_ && commit_(value_, commit_error)) {
                cancel(window);
            } else {
                error_ = commit_error.empty()
                    ? "Value was rejected"
                    : std::move(commit_error);
            }
            break;
        }
        default:
            select_all_ = false;
            break;
        }
        return true;
    }

    void draw(SDL_Renderer* renderer, const std::uint64_t ticks) const {
        if (!active_) {
            return;
        }
        fill(renderer, {0.0F, 0.0F, canvas_width, canvas_height}, {4, 3, 14, 180});
        const SDL_FRect box{190.0F, 246.0F, 900.0F, 210.0F};
        panel(renderer, box, cyan);
        fill(renderer, {box.x, box.y, box.w, 48.0F}, {37, 26, 76, 255});
        text(renderer, box.x + 22.0F, box.y + 17.0F, label_, cyan, 1.45F);
        const SDL_FRect input{box.x + 22.0F, box.y + 78.0F, box.w - 44.0F, 48.0F};
        fill(
            renderer,
            input,
            select_all_ ? SDL_Color{66, 50, 104, 255}
                        : SDL_Color{17, 15, 38, 255}
        );
        outline(renderer, input, select_all_ ? yellow : purple);
        auto visible = value_.size() > 105U
            ? std::string("...") + value_.substr(value_.size() - 102U)
            : value_;
        if ((ticks / 450U) % 2U == 0U) {
            visible.push_back('_');
        }
        text(renderer, input.x + 12.0F, input.y + 17.0F, visible, white, 1.15F);
        text(
            renderer,
            box.x + 22.0F,
            box.y + 145.0F,
            error_.empty()
                ? "ENTER apply   ESC cancel   CTRL+A/C/V supported"
                : error_,
            error_.empty() ? muted : danger,
            1.08F,
            115U
        );
    }

private:
    std::string label_;
    std::string value_;
    std::string error_;
    std::size_t maximum_bytes_{};
    Commit commit_;
    bool active_{};
    bool select_all_{};
};

class RenderClipGuard final {
public:
    RenderClipGuard(SDL_Renderer* renderer, const SDL_Rect rectangle)
        : renderer_(renderer), was_enabled_(SDL_RenderClipEnabled(renderer)) {
        if (was_enabled_) {
            static_cast<void>(SDL_GetRenderClipRect(renderer_, &previous_));
        }
        static_cast<void>(SDL_SetRenderClipRect(renderer_, &rectangle));
    }

    ~RenderClipGuard() {
        static_cast<void>(SDL_SetRenderClipRect(
            renderer_,
            was_enabled_ ? &previous_ : nullptr
        ));
    }

    RenderClipGuard(const RenderClipGuard&) = delete;
    RenderClipGuard& operator=(const RenderClipGuard&) = delete;

private:
    SDL_Renderer* renderer_{};
    SDL_Rect previous_{};
    bool was_enabled_{};
};

class SearchableOptionPicker final {
public:
    using Commit = std::function<void(std::string_view)>;

    void begin(
        SDL_Window* window,
        std::string label,
        std::string current,
        const std::size_t maximum_bytes,
        std::vector<std::string> options,
        const bool allow_custom,
        Commit commit,
        const bool show_note_skin_preview = false
    ) {
        cancel(window);
        label_ = std::move(label);
        current_ = std::move(current);
        maximum_bytes_ = maximum_bytes;
        allow_custom_ = allow_custom;
        show_note_skin_preview_ = show_note_skin_preview;
        commit_ = std::move(commit);
        query_.clear();
        error_.clear();
        if (!current_.empty()) {
            options.push_back(current_);
        }
        model_.set_options(std::move(options));
        model_.set_query({});
        if (!current_.empty()) {
            static_cast<void>(model_.select_value(current_));
        }
        active_ = true;
        static_cast<void>(SDL_StartTextInput(window));
    }

    void cancel(SDL_Window* window) {
        if (active_) {
            static_cast<void>(SDL_StopTextInput(window));
        }
        active_ = false;
        commit_ = {};
        error_.clear();
    }

    [[nodiscard]] bool active() const noexcept {
        return active_;
    }

    [[nodiscard]] bool handle(SDL_Window* window, const SDL_Event& event) {
        if (!active_) {
            return false;
        }
        if (event.type == SDL_EVENT_TEXT_INPUT) {
            const std::string_view incoming(event.text.text);
            append_bounded_utf8(query_, incoming, maximum_bytes_);
            model_.set_query(query_);
            error_.clear();
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            mouse_x_ = event.motion.x;
            mouse_y_ = event.motion.y;
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            mouse_x_ = event.wheel.mouse_x;
            mouse_y_ = event.wheel.mouse_y;
            if (contains(list_rectangle, mouse_x_, mouse_y_)) {
                const auto lines = static_cast<int>(std::clamp(
                    -event.wheel.y * 3.0F,
                    -30.0F,
                    30.0F
                ));
                model_.scroll_view(lines);
            }
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
            && event.button.button == SDL_BUTTON_LEFT) {
            mouse_x_ = event.button.x;
            mouse_y_ = event.button.y;
            if (allow_custom_ && contains(custom_rectangle, mouse_x_, mouse_y_)) {
                if (query_.empty()) {
                    error_ = "Type a free-form ID first";
                } else {
                    accept(window, true);
                }
                return true;
            }
            if (contains(list_rectangle, mouse_x_, mouse_y_)) {
                const auto visible_row = static_cast<std::size_t>(
                    std::max(0.0F, mouse_y_ - list_rectangle.y) / row_height
                );
                if (model_.select_visible_row(visible_row)) {
                    accept(window, false);
                }
            }
            return true;
        }
        if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
            return true;
        }

        const bool control = (event.key.mod & SDL_KMOD_CTRL) != 0;
        if (control && event.key.scancode == SDL_SCANCODE_A) {
            query_.clear();
            model_.set_query(query_);
            error_.clear();
            return true;
        }
        if (control && event.key.scancode == SDL_SCANCODE_V) {
            char* clipboard = SDL_GetClipboardText();
            if (clipboard != nullptr) {
                const std::string_view incoming(clipboard);
                append_bounded_utf8(query_, incoming, maximum_bytes_);
                SDL_free(clipboard);
                model_.set_query(query_);
                error_.clear();
            }
            return true;
        }

        switch (event.key.scancode) {
        case SDL_SCANCODE_ESCAPE:
            cancel(window);
            break;
        case SDL_SCANCODE_BACKSPACE:
            erase_last_utf8(query_);
            model_.set_query(query_);
            error_.clear();
            break;
        case SDL_SCANCODE_UP:
            model_.move_selection(-1);
            break;
        case SDL_SCANCODE_DOWN:
            model_.move_selection(1);
            break;
        case SDL_SCANCODE_PAGEUP:
            model_.page_selection(-1);
            break;
        case SDL_SCANCODE_PAGEDOWN:
            model_.page_selection(1);
            break;
        case SDL_SCANCODE_HOME:
            model_.select_home();
            break;
        case SDL_SCANCODE_END:
            model_.select_end();
            break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            accept(window, control);
            break;
        default:
            break;
        }
        return true;
    }

    void draw(SDL_Renderer* renderer, const std::uint64_t ticks) const {
        if (!active_) {
            return;
        }
        fill(renderer, {0.0F, 0.0F, canvas_width, canvas_height}, {3, 3, 12, 232});
        const SDL_FRect box{164.0F, 54.0F, 952.0F, 612.0F};
        panel(renderer, box, cyan);
        fill(renderer, {box.x, box.y, box.w, 58.0F}, {34, 25, 73, 255});
        text(
            renderer,
            box.x + 24.0F,
            box.y + 20.0F,
            label_,
            cyan,
            1.48F,
            show_note_skin_preview_ ? 66U : 105U
        );
        if (show_note_skin_preview_) {
            const auto selected = model_.selected_value();
            const auto preview_id = selected.has_value() ? *selected
                : std::string_view(current_);
            text(renderer, box.x + 636.0F, box.y + 10.0F, "SKIN", muted, 0.82F);
            constexpr std::array<std::string_view, 4U> arrows{"<", "v", "^", ">"};
            constexpr std::array<SDL_Color, 4U> colors{
                SDL_Color{194, 87, 255, 255},
                SDL_Color{54, 224, 255, 255},
                SDL_Color{73, 245, 142, 255},
                SDL_Color{255, 77, 145, 255},
            };
            for (std::size_t lane = 0U; lane < arrows.size(); ++lane) {
                const float x = box.x + 692.0F + static_cast<float>(lane) * 44.0F;
                fill(renderer, {x, box.y + 8.0F, 36.0F, 36.0F}, {15, 14, 34, 255});
                outline(renderer, {x, box.y + 8.0F, 36.0F, 36.0F}, colors[lane]);
                text(renderer, x + 12.0F, box.y + 19.0F, arrows[lane], colors[lane], 1.05F);
            }
            text(
                renderer,
                box.x + 636.0F,
                box.y + 43.0F,
                preview_id,
                white,
                0.72F,
                46U
            );
        }

        const SDL_FRect search{box.x + 24.0F, box.y + 82.0F, box.w - 48.0F, 50.0F};
        fill(renderer, search, {14, 13, 34, 255});
        outline(renderer, search, purple);
        std::string visible_query = query_.empty() ? "Search options..." : query_;
        if (!query_.empty() && (ticks / 450U) % 2U == 0U) {
            visible_query.push_back('_');
        }
        text(
            renderer,
            search.x + 14.0F,
            search.y + 18.0F,
            visible_query,
            query_.empty() ? muted : white,
            1.15F,
            105U
        );

        fill(renderer, list_rectangle, {12, 11, 30, 255});
        outline(renderer, list_rectangle, panel_border);
        {
            const SDL_Rect clip{
                static_cast<int>(list_rectangle.x + 1.0F),
                static_cast<int>(list_rectangle.y + 1.0F),
                static_cast<int>(list_rectangle.w - 2.0F),
                static_cast<int>(list_rectangle.h - 2.0F),
            };
            const RenderClipGuard clipping(renderer, clip);
            const auto first = model_.first_visible_row();
            const auto last = std::min(
                model_.result_count(),
                first + model_.visible_rows()
            );
            for (std::size_t row = first; row < last; ++row) {
                const auto visible_row = row - first;
                const SDL_FRect rectangle{
                    list_rectangle.x + 1.0F,
                    list_rectangle.y + static_cast<float>(visible_row) * row_height,
                    list_rectangle.w - 16.0F,
                    row_height,
                };
                const bool selected = model_.selected_row() == row;
                const bool hovered = contains(rectangle, mouse_x_, mouse_y_);
                fill(
                    renderer,
                    rectangle,
                    selected ? SDL_Color{65, 53, 112, 255}
                             : hovered ? SDL_Color{42, 37, 76, 255}
                                       : SDL_Color{20, 18, 43, 255}
                );
                if (selected) {
                    fill(
                        renderer,
                        {rectangle.x, rectangle.y, 5.0F, rectangle.h},
                        yellow
                    );
                }
                text(
                    renderer,
                    rectangle.x + 14.0F,
                    rectangle.y + 10.0F,
                    model_.result_at(row),
                    selected ? yellow : white,
                    1.08F,
                    100U
                );
            }
            if (model_.result_count() == 0U) {
                text(
                    renderer,
                    list_rectangle.x + 18.0F,
                    list_rectangle.y + 22.0F,
                    allow_custom_ && !query_.empty()
                        ? "No match. Use CTRL+ENTER or the FREE-FORM row below."
                        : "No options match this search.",
                    muted,
                    1.12F,
                    100U
                );
            }
        }

        if (model_.result_count() > model_.visible_rows()) {
            const float track_y = list_rectangle.y + 4.0F;
            const float track_height = list_rectangle.h - 8.0F;
            const float thumb_height = std::max(
                28.0F,
                track_height * static_cast<float>(model_.visible_rows())
                    / static_cast<float>(model_.result_count())
            );
            const auto maximum_first = model_.result_count() - model_.visible_rows();
            const float ratio = maximum_first == 0U
                ? 0.0F
                : static_cast<float>(model_.first_visible_row())
                    / static_cast<float>(maximum_first);
            fill(
                renderer,
                {list_rectangle.x + list_rectangle.w - 10.0F,
                 track_y,
                 6.0F,
                 track_height},
                {51, 45, 79, 255}
            );
            fill(
                renderer,
                {list_rectangle.x + list_rectangle.w - 10.0F,
                 track_y + ratio * (track_height - thumb_height),
                 6.0F,
                 thumb_height},
                cyan
            );
        }

        if (allow_custom_) {
            const bool available = !query_.empty();
            const bool hovered = contains(custom_rectangle, mouse_x_, mouse_y_);
            fill(
                renderer,
                custom_rectangle,
                available
                    ? hovered ? SDL_Color{73, 54, 119, 255}
                              : SDL_Color{43, 34, 82, 255}
                    : SDL_Color{22, 20, 43, 255}
            );
            outline(renderer, custom_rectangle, available ? purple : panel_border);
            text(
                renderer,
                custom_rectangle.x + 14.0F,
                custom_rectangle.y + 13.0F,
                available
                    ? "USE EXACT CUSTOM TEXT: " + query_
                    : "FREE-FORM: type an ID above, then click here or press CTRL+ENTER",
                available ? yellow : muted,
                1.0F,
                104U
            );
        }

        const auto first_display = model_.result_count() == 0U
            ? 0U
            : model_.first_visible_row() + 1U;
        const auto last_display = std::min(
            model_.result_count(),
            model_.first_visible_row() + model_.visible_rows()
        );
        text(
            renderer,
            box.x + 24.0F,
            box.y + 558.0F,
            std::to_string(first_display) + "-" + std::to_string(last_display)
                + " / " + std::to_string(model_.result_count())
                + (current_.empty() ? std::string{} : "   Current: " + current_),
            muted,
            1.02F,
            110U
        );
        text(
            renderer,
            box.x + 24.0F,
            box.y + 582.0F,
            error_.empty()
                ? "TYPE filter   ARROWS/PAGE/HOME/END navigate   ENTER selected row   ESC cancel"
                : error_,
            error_.empty() ? muted : danger,
            0.98F,
            118U
        );
        if (allow_custom_) {
            text(
                renderer,
                box.x + 708.0F,
                box.y + 558.0F,
                "CTRL+ENTER exact custom",
                purple,
                0.98F,
                28U
            );
        }
    }

private:
    void accept(SDL_Window* window, const bool force_custom) {
        std::string selected;
        if (force_custom && allow_custom_ && !query_.empty()) {
            selected = query_;
        } else if (const auto value = model_.selected_value(); value.has_value()) {
            selected = std::string(*value);
        }
        if (selected.empty()) {
            error_ = allow_custom_ && !query_.empty()
                ? "No selected match: use CTRL+ENTER or click the free-form row"
                : allow_custom_
                    ? "Select a row or type a custom value"
                : "Select one of the available rows";
            return;
        }
        auto commit = commit_;
        cancel(window);
        if (commit) {
            commit(selected);
        }
    }

    static constexpr float row_height = 32.0F;
    static constexpr SDL_FRect list_rectangle{188.0F, 206.0F, 904.0F, 320.0F};
    static constexpr SDL_FRect custom_rectangle{188.0F, 540.0F, 904.0F, 40.0F};

    SearchableOptionModel model_{10U};
    std::string label_;
    std::string current_;
    std::string query_;
    std::string error_;
    std::size_t maximum_bytes_{};
    Commit commit_;
    float mouse_x_{};
    float mouse_y_{};
    bool active_{};
    bool allow_custom_{};
    bool show_note_skin_preview_{};
};

struct IndexedNote {
    double time_ms{};
    EditorEntityId id{};
};

[[nodiscard]] std::vector<IndexedNote> build_note_index(
    const ChartEditor& editor
) {
    std::vector<IndexedNote> result;
    result.reserve(editor.notes().size());
    for (const auto& [id, note] : editor.notes()) {
        result.push_back({note.time_ms, id});
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.time_ms != right.time_ms) {
            return left.time_ms < right.time_ms;
        }
        return left.id < right.id;
    });
    return result;
}

[[nodiscard]] std::vector<IndexedNote> build_event_index(
    const ChartEditor& editor
) {
    std::vector<IndexedNote> result;
    result.reserve(editor.events().size());
    for (const auto& [id, event] : editor.events()) {
        result.push_back({event.time_ms, id});
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.time_ms != right.time_ms) {
            return left.time_ms < right.time_ms;
        }
        return left.id < right.id;
    });
    return result;
}

[[nodiscard]] std::vector<std::pair<double, TempoChange>> build_tempo_index(
    const ChartEditor& editor
) {
    std::vector<std::pair<double, TempoChange>> result;
    result.reserve(editor.tempos().size());
    for (const auto& [id, tempo] : editor.tempos()) {
        (void)id;
        result.emplace_back(tempo.time_ms, tempo);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    return result;
}

[[nodiscard]] TempoChange tempo_at(
    const std::vector<std::pair<double, TempoChange>>& tempos,
    const double time_ms
) {
    TempoChange result{};
    if (tempos.empty()) {
        return result;
    }
    result = tempos.front().second;
    const auto upper = std::upper_bound(
        tempos.begin(),
        tempos.end(),
        time_ms,
        [](const double time, const auto& item) {
            return time < item.first;
        }
    );
    if (upper != tempos.begin()) {
        result = std::prev(upper)->second;
    }
    return result;
}

[[nodiscard]] double quantize_time(
    const double time_ms,
    const TempoChange& tempo,
    const int subdivisions
) {
    const double step = 60'000.0 / tempo.bpm
        / static_cast<double>(std::max(subdivisions, 1));
    return tempo.time_ms
        + std::round((time_ms - tempo.time_ms) / step) * step;
}

[[nodiscard]] std::string scripts_as_csv(
    const std::vector<std::string>& scripts
) {
    std::string result;
    for (std::size_t index = 0U; index < scripts.size(); ++index) {
        if (index != 0U) {
            result += ", ";
        }
        result += scripts[index];
    }
    return result;
}

[[nodiscard]] std::vector<std::string> scripts_from_csv(
    const std::string_view value
) {
    return split(value, ',', false);
}

[[nodiscard]] ChartEditorChoiceCatalog build_chart_editor_choices(
    const ChartEditor& editor,
    const ChartEditorUiOptions& options
) {
    ChartEditorChoiceCatalog choices = options.choices;
    ChartEditorChoiceCatalog builtins;
    builtins.characters = {
        "bf", "bf-car", "bf-christmas", "bf-pixel", "dad", "gf",
        "gf-car", "gf-christmas", "gf-pixel", "gf-tankmen", "monster",
        "monster-christmas", "mom", "mom-car", "parents-christmas", "pico",
        "senpai", "senpai-angry", "spirit", "spooky", "tankman",
    };
    builtins.stages = {
        "stage", "spooky", "philly", "limo", "mall", "mallEvil",
        "school", "schoolEvil", "tank",
    };
    for (const auto id : builtin_note_type_ids()) {
        builtins.note_types.emplace_back(id);
    }
    builtins.note_styles = {"funkin", "normal", "pixel"};
    builtins.event_names = {
        "Add Camera Zoom", "Camera Flash", "Change Character",
        "Change Scroll Speed", "Hey!", "Kill Henchmen", "Play Animation",
        "Screen Shake", "Set GF Speed", "Trigger BG Ghouls",
    };
    builtins.difficulties = {"easy", "normal", "hard", "erect", "nightmare"};
    merge_chart_editor_choices(choices, builtins);

    auto discovery_roots = options.choice_discovery_roots;
    if (options.storage != nullptr) {
        discovery_roots.push_back(options.storage->root());
    }
    if (!discovery_roots.empty()) {
        merge_chart_editor_choices(
            choices,
            discover_chart_editor_choices(
                discovery_roots,
                options.choice_discovery_limits
            )
        );
    }

    const auto& metadata = editor.metadata();
    choices.characters.push_back(metadata.player_character);
    choices.characters.push_back(metadata.opponent_character);
    choices.characters.push_back(metadata.girlfriend_character);
    choices.stages.push_back(metadata.stage_id);
    choices.note_styles.push_back(metadata.note_style);
    choices.difficulties.push_back(metadata.difficulty);
    choices.scripts.insert(
        choices.scripts.end(),
        editor.scripts().begin(),
        editor.scripts().end()
    );
    for (const auto& [id, note] : editor.notes()) {
        static_cast<void>(id);
        choices.note_types.push_back(note.kind.empty() ? "normal" : note.kind);
    }
    for (const auto& [id, event] : editor.events()) {
        static_cast<void>(id);
        choices.event_names.push_back(event.name);
    }
    normalize_chart_editor_choices(choices);
    return choices;
}

struct ChartViewState {
    double playhead_ms{};
    double milliseconds_per_pixel{4.0};
    int subdivisions{4};
    int sustain_steps{};
    std::string note_kind{"normal"};
    std::optional<EditorEntityId> selected_note;
    float mouse_x{};
    float mouse_y{};
    std::uint64_t escape_armed_until{};
};

struct ReviewUiEntry final {
    AutoChartReviewQueueEntry queue;
    EditorEntityId entity_id{};
};

struct ChartReviewUiState final {
    std::optional<AutoChartReviewReader> reader;
    std::vector<ReviewUiEntry> entries;
    std::unordered_map<EditorEntityId, std::size_t> by_entity;
    std::optional<std::size_t> cursor;
    std::optional<AutoChartNoteReview> detail;
    std::string difficulty;
    std::string error;
    double high_threshold{0.65};
    double medium_threshold{0.40};
    bool visible{true};

    [[nodiscard]] bool active() const noexcept {
        return reader.has_value() && !entries.empty();
    }
};

[[nodiscard]] SDL_Color review_color(
    const ChartReviewUiState& state,
    const double priority
) noexcept {
    if (priority >= state.high_threshold) {
        return danger;
    }
    if (priority >= state.medium_threshold) {
        return yellow;
    }
    return purple;
}

[[nodiscard]] std::string review_band_label(
    const ChartReviewUiState& state,
    const double priority
) {
    if (priority >= state.high_threshold) {
        return "HIGH";
    }
    if (priority >= state.medium_threshold) {
        return "MED";
    }
    return "LOW";
}

[[nodiscard]] bool review_matches_note(
    const AutoChartReviewQueueEntry& entry,
    const Note& note
) noexcept {
    return entry.lane == note.lane
        && std::abs(entry.time_ms - note.time_ms) <= 0.02;
}

[[nodiscard]] ChartReviewUiState load_chart_review_state(
    const ChartEditorUiOptions& options,
    const ChartEditor& editor,
    const std::vector<IndexedNote>& note_index
) {
    ChartReviewUiState state;
    state.visible = options.autochart_review_overlay;
    if (options.autochart_review_index_path.empty()) {
        return state;
    }
    std::string error;
    auto reader = AutoChartReviewReader::open(
        options.autochart_review_index_path,
        &error
    );
    if (!reader.has_value()) {
        state.error = error.empty()
            ? "AutoChart review index could not be opened"
            : std::move(error);
        return state;
    }
    state.high_threshold = reader->index().high_priority_threshold;
    state.medium_threshold = reader->index().medium_priority_threshold;
    state.difficulty = options.autochart_review_difficulty.empty()
        ? editor.metadata().difficulty
        : options.autochart_review_difficulty;
    const auto maximum = std::clamp<std::size_t>(
        options.autochart_review_queue_limit,
        1U,
        100'000U
    );
    auto queue = reader->load_queue(state.difficulty, maximum, &error);
    if (!error.empty()) {
        state.error = std::move(error);
        return state;
    }
    state.entries.reserve(queue.size());
    state.by_entity.reserve(queue.size());
    for (auto& queued : queue) {
        if (queued.note_index >= note_index.size()) {
            continue;
        }
        const auto entity_id = note_index[
            static_cast<std::size_t>(queued.note_index)
        ].id;
        const auto found = editor.notes().find(entity_id);
        if (found == editor.notes().end()
            || !review_matches_note(queued, found->second)) {
            continue;
        }
        const auto position = state.entries.size();
        state.by_entity.emplace(entity_id, position);
        state.entries.push_back({std::move(queued), entity_id});
    }
    state.reader = std::move(*reader);
    if (state.entries.empty()) {
        state.error = "AutoChart review queue contains no notes matching this chart";
    }
    return state;
}

[[nodiscard]] std::string review_status_text(
    const ChartReviewUiState& state,
    const ChartEditor& editor
) {
    if (!state.cursor.has_value() || *state.cursor >= state.entries.size()) {
        return {};
    }
    const auto& item = state.entries[*state.cursor];
    const auto found = editor.notes().find(item.entity_id);
    const bool stale = found == editor.notes().end()
        || !review_matches_note(item.queue, found->second);
    std::ostringstream output;
    output << "REVIEW " << (*state.cursor + 1U) << '/' << state.entries.size()
           << " " << review_band_label(state, item.queue.review_priority)
           << " priority " << std::fixed << std::setprecision(0)
           << item.queue.review_priority * 100.0 << "%"
           << "  conf " << item.queue.confidence * 100.0 << "%";
    if (state.detail.has_value()) {
        if (!state.detail->review_concerns.empty()) {
            output << "  concern: " << state.detail->review_concerns.front();
        } else if (!state.detail->why.empty()) {
            output << "  why: " << state.detail->why.front();
        }
    }
    if (stale) {
        output << "  [edited: provenance describes original note]";
    }
    return output.str();
}

bool load_review_detail(
    ChartReviewUiState& state,
    const std::size_t index,
    std::string& error
) {
    if (!state.reader.has_value() || index >= state.entries.size()) {
        return false;
    }
    auto detail = state.reader->read_record(state.entries[index].queue, &error);
    if (!detail.has_value()) {
        return false;
    }
    state.cursor = index;
    state.detail = std::move(*detail);
    return true;
}

[[nodiscard]] std::optional<EditorEntityId> note_near(
    const ChartEditor& editor,
    const std::vector<IndexedNote>& index,
    const ChartViewState& view,
    const float x,
    const float y
) {
    if (!contains(chart_grid, x, y)) {
        return std::nullopt;
    }
    const auto column_width = chart_grid.w
        / static_cast<float>(editor.key_count() * 2U);
    const auto requested_column = static_cast<int>((x - chart_grid.x) / column_width);
    const double requested_time = view.playhead_ms
        + static_cast<double>(y - (chart_grid.y + chart_grid.h * 0.5F))
            * view.milliseconds_per_pixel;
    const double radius_ms = std::max(16.0, view.milliseconds_per_pixel * 11.0);
    const auto first = std::lower_bound(
        index.begin(),
        index.end(),
        requested_time - radius_ms,
        [](const IndexedNote& note, const double time) {
            return note.time_ms < time;
        }
    );
    std::optional<EditorEntityId> result;
    double best_distance = std::numeric_limits<double>::infinity();
    for (auto iterator = first;
         iterator != index.end()
             && iterator->time_ms <= requested_time + radius_ms;
         ++iterator) {
        const auto found = editor.notes().find(iterator->id);
        if (found == editor.notes().end()) {
            continue;
        }
        const auto& note = found->second;
        // PULSEFORGE_P1_4_0_EDITOR_SECONDARY_OPPONENT_COLUMN_V1
        // The current editor remains a two-side authoring surface. Preserve the
        // canonical third owner, but display every AI-owned note in the opponent
        // half instead of silently drawing player4 notes on the player side.
        const int column = note.owner != NoteOwner::player
            ? static_cast<int>(note.lane)
            : static_cast<int>(editor.key_count() + note.lane);
        if (column != requested_column) {
            continue;
        }
        const double distance = std::abs(note.time_ms - requested_time);
        if (distance < best_distance) {
            best_distance = distance;
            result = iterator->id;
        }
    }
    return result;
}

void insert_note_index(
    std::vector<IndexedNote>& index,
    const IndexedNote value
) {
    const auto position = std::lower_bound(
        index.begin(),
        index.end(),
        value,
        [](const IndexedNote& left, const IndexedNote& right) {
            if (left.time_ms != right.time_ms) {
                return left.time_ms < right.time_ms;
            }
            return left.id < right.id;
        }
    );
    index.insert(position, value);
}

void draw_chart_grid(
    SDL_Renderer* renderer,
    const ChartEditor& editor,
    const std::vector<IndexedNote>& note_index,
    const std::vector<IndexedNote>& event_index,
    const std::vector<std::pair<double, TempoChange>>& tempo_index,
    const ChartViewState& view,
    const ChartReviewUiState* const review
) {
    panel(renderer, chart_grid, purple);
    const auto columns = static_cast<std::uint32_t>(editor.key_count()) * 2U;
    const auto column_width = chart_grid.w / static_cast<float>(columns);
    const float center_x = chart_grid.x
        + column_width * static_cast<float>(editor.key_count());
    fill(
        renderer,
        {center_x - 2.0F, chart_grid.y, 4.0F, chart_grid.h},
        {221, 204, 255, 210}
    );
    for (std::uint32_t column = 1U; column < columns; ++column) {
        const float x = chart_grid.x + column_width * static_cast<float>(column);
        line(
            renderer,
            x,
            chart_grid.y,
            x,
            chart_grid.y + chart_grid.h,
            column == editor.key_count()
                ? SDL_Color{220, 205, 255, 255}
                : SDL_Color{65, 57, 96, 180}
        );
    }

    const auto active_tempo = tempo_at(tempo_index, view.playhead_ms);
    const double step_ms = 60'000.0 / active_tempo.bpm
        / static_cast<double>(view.subdivisions);
    const double view_start = view.playhead_ms
        - static_cast<double>(chart_grid.h * 0.5F)
            * view.milliseconds_per_pixel;
    const double view_end = view.playhead_ms
        + static_cast<double>(chart_grid.h * 0.5F)
            * view.milliseconds_per_pixel;
    double grid_time = active_tempo.time_ms
        + std::floor((view_start - active_tempo.time_ms) / step_ms) * step_ms;
    std::size_t grid_lines = 0U;
    while (grid_time <= view_end && grid_lines < 2'048U) {
        const float y = chart_grid.y + chart_grid.h * 0.5F
            + static_cast<float>(
                (grid_time - view.playhead_ms) / view.milliseconds_per_pixel
            );
        const auto step_number = static_cast<long long>(std::llround(
            (grid_time - active_tempo.time_ms) / step_ms
        ));
        const bool beat = step_number % view.subdivisions == 0;
        line(
            renderer,
            chart_grid.x,
            y,
            chart_grid.x + chart_grid.w,
            y,
            beat ? SDL_Color{101, 87, 150, 220}
                 : SDL_Color{51, 46, 76, 160}
        );
        grid_time += step_ms;
        ++grid_lines;
    }
    const float receptor_y = chart_grid.y + chart_grid.h * 0.5F;
    fill(
        renderer,
        {chart_grid.x, receptor_y - 2.0F, chart_grid.w, 4.0F},
        yellow
    );
    text(renderer, chart_grid.x + 8.0F, chart_grid.y + 8.0F, "OPPONENT", danger, 1.0F);
    text(renderer, center_x + 8.0F, chart_grid.y + 8.0F, "PLAYER", cyan, 1.0F);

    const auto first = std::lower_bound(
        note_index.begin(),
        note_index.end(),
        view_start,
        [](const IndexedNote& note, const double time) {
            return note.time_ms < time;
        }
    );
    for (auto iterator = first;
         iterator != note_index.end() && iterator->time_ms <= view_end;
         ++iterator) {
        const auto found = editor.notes().find(iterator->id);
        if (found == editor.notes().end()) {
            continue;
        }
        const auto& note = found->second;
        const auto column = note.owner != NoteOwner::player
            ? static_cast<std::uint32_t>(note.lane)
            : static_cast<std::uint32_t>(editor.key_count() + note.lane);
        const float x = chart_grid.x
            + static_cast<float>(column) * column_width + 3.0F;
        const float y = receptor_y + static_cast<float>(
            (note.time_ms - view.playhead_ms) / view.milliseconds_per_pixel
        );
        const float sustain_height = static_cast<float>(
            note.duration_ms / view.milliseconds_per_pixel
        );
        const bool selected = view.selected_note == iterator->id;
        const SDL_Color note_color = note.kind == "normal"
            ? (note.owner == NoteOwner::player ? cyan : danger)
            : yellow;
        if (sustain_height > 2.0F) {
            fill(
                renderer,
                {x + column_width * 0.38F,
                 y + 8.0F,
                 std::max(3.0F, column_width * 0.18F),
                 sustain_height},
                {note_color.r, note_color.g, note_color.b, 190}
            );
        }
        const SDL_FRect note_rectangle{
            x,
            y - 7.0F,
            std::max(5.0F, column_width - 6.0F),
            14.0F,
        };
        fill(renderer, note_rectangle, note_color);
        outline(renderer, note_rectangle, selected ? yellow : white);
        if (review != nullptr && review->visible) {
            const auto review_item = review->by_entity.find(iterator->id);
            if (review_item != review->by_entity.end()
                && review_item->second < review->entries.size()) {
                const auto& queued = review->entries[review_item->second].queue;
                const auto marker = review_color(*review, queued.review_priority);
                const SDL_FRect review_outline{
                    note_rectangle.x - 2.0F,
                    note_rectangle.y - 2.0F,
                    note_rectangle.w + 4.0F,
                    note_rectangle.h + 4.0F,
                };
                outline(renderer, review_outline, marker);
                fill(
                    renderer,
                    {review_outline.x + review_outline.w - 4.0F,
                     review_outline.y,
                     4.0F,
                     review_outline.h},
                    {marker.r, marker.g, marker.b, 220}
                );
            }
        }
    }

    const auto first_event = std::lower_bound(
        event_index.begin(),
        event_index.end(),
        view_start,
        [](const IndexedNote& event, const double time) {
            return event.time_ms < time;
        }
    );
    for (auto iterator = first_event;
         iterator != event_index.end() && iterator->time_ms <= view_end;
         ++iterator) {
        const auto found = editor.events().find(iterator->id);
        if (found == editor.events().end()) {
            continue;
        }
        const float y = receptor_y + static_cast<float>(
            (found->second.time_ms - view.playhead_ms)
            / view.milliseconds_per_pixel
        );
        line(
            renderer,
            chart_grid.x,
            y,
            chart_grid.x + chart_grid.w,
            y,
            {purple.r, purple.g, purple.b, 95}
        );
        fill(
            renderer,
            {chart_grid.x + chart_grid.w - 9.0F, y - 5.0F, 9.0F, 10.0F},
            purple
        );
        text(
            renderer,
            chart_grid.x + chart_grid.w - 150.0F,
            y - 5.0F,
            found->second.name,
            purple,
            0.78F,
            19U
        );
    }
}

void draw_chart_inspector(
    SDL_Renderer* renderer,
    const ChartEditor& editor,
    const ChartViewState& view,
    const std::vector<std::pair<double, TempoChange>>& tempos,
    const std::string_view status,
    const bool status_error,
    const bool can_save,
    const float mouse_x,
    const float mouse_y,
    const ChartReviewUiState* const review
) {
    panel(renderer, inspector_panel, cyan);
    text(renderer, 858.0F, 111.0F, "CHART CONFIGURATION", cyan, 1.35F);
    const std::array labels{
        std::string{"Title: "} + editor.metadata().title,
        std::string{"Artist: "} + editor.metadata().artist,
        std::string{"Charter: "} + editor.metadata().charter,
        std::string{"Difficulty: "} + editor.metadata().difficulty,
        std::string{"Stage: "} + editor.metadata().stage_id,
        std::string{"Player: "} + editor.metadata().player_character,
        std::string{"Opponent: "} + editor.metadata().opponent_character,
        std::string{"Girlfriend: "} + editor.metadata().girlfriend_character,
        std::string{"Note style: "} + editor.metadata().note_style,
    };
    float y = 142.0F;
    for (const auto& label : labels) {
        const SDL_FRect row{854.0F, y - 8.0F, 382.0F, 27.0F};
        const bool hovered = contains(row, mouse_x, mouse_y);
        fill(renderer, row, hovered ? SDL_Color{52, 44, 88, 245}
                                    : SDL_Color{34, 30, 65, 230});
        text(renderer, 864.0F, y, label, hovered ? yellow : white, 1.0F, 49U);
        y += 30.0F;
    }

    const auto tempo = tempo_at(tempos, view.playhead_ms);
    text(
        renderer,
        858.0F,
        424.0F,
        "Time " + format_time(view.playhead_ms) + "   BPM "
            + format_number(tempo.bpm, 2),
        yellow,
        1.12F
    );
    text(
        renderer,
        858.0F,
        448.0F,
        "Scroll " + format_number(editor.scroll_speed(), 2)
            + "   Quant 1/" + std::to_string(view.subdivisions),
        muted,
        1.05F
    );
    text(
        renderer,
        858.0F,
        470.0F,
        "Type " + view.note_kind + "   Sustain "
            + std::to_string(view.sustain_steps) + " step(s)",
        muted,
        1.05F,
        52U
    );
    text(
        renderer,
        858.0F,
        492.0F,
        "Notes " + std::to_string(editor.notes().size()) + "   Events "
            + std::to_string(editor.events().size()) + "   BPMs "
            + std::to_string(editor.tempos().size()),
        muted,
        1.0F
    );

    const std::array<std::pair<SDL_FRect, std::string_view>, 5U> buttons{{
        {{854.0F, 522.0F, 184.0F, 36.0F}, "B  BPM AT CURSOR"},
        {{1'052.0F, 522.0F, 184.0F, 36.0F}, "V  ADD EVENT"},
        {{854.0F, 566.0F, 118.0F, 36.0F}, "N  TYPE"},
        {{980.0F, 566.0F, 128.0F, 36.0F}, "K  NOTE SKIN"},
        {{1'116.0F, 566.0F, 120.0F, 36.0F}, "L  SCRIPTS"},
    }};
    for (const auto& [rectangle, label] : buttons) {
        static_cast<void>(button(
            renderer,
            rectangle,
            label,
            contains(rectangle, mouse_x, mouse_y)
        ));
    }
    static_cast<void>(button(
        renderer,
        {854.0F, 610.0F, 382.0F, 36.0F},
        "CTRL+S  SAVE PROJECT + PSYCH",
        contains({854.0F, 610.0F, 382.0F, 36.0F}, mouse_x, mouse_y),
        can_save,
        success
    ));

    if (review != nullptr && (review->active() || !review->error.empty())) {
        fill(renderer, {838.0F, 648.0F, 414.0F, 24.0F}, {20, 17, 44, 245});
        if (review->active()) {
            std::size_t high = 0U;
            std::size_t medium = 0U;
            for (const auto& item : review->entries) {
                if (item.queue.review_priority >= review->high_threshold) {
                    ++high;
                } else if (item.queue.review_priority >= review->medium_threshold) {
                    ++medium;
                }
            }
            text(
                renderer,
                850.0F,
                656.0F,
                "AUTOCHART REVIEW  F6 next  SHIFT+F6 prev  F7 overlay  H "
                    + std::to_string(high) + "  M " + std::to_string(medium),
                review->visible ? cyan : muted,
                0.82F,
                64U
            );
        } else {
            text(
                renderer,
                850.0F,
                656.0F,
                "AUTOCHART REVIEW unavailable: " + review->error,
                danger,
                0.82F,
                64U
            );
        }
    }

    if (!status.empty()) {
        fill(renderer, {0.0F, 676.0F, canvas_width, 44.0F}, {18, 14, 37, 250});
        text(
            renderer,
            28.0F,
            692.0F,
            status,
            status_error ? danger : success,
            1.12F,
            145U
        );
    }
}

[[nodiscard]] bool manifest_has_audio(const AudioManifest& manifest) noexcept {
    if (!manifest.instrumental.empty()) {
        return true;
    }
    return std::any_of(
        manifest.vocals.begin(),
        manifest.vocals.end(),
        [](const std::filesystem::path& path) { return !path.empty(); }
    );
}

[[nodiscard]] std::string ascii_lower(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        }
    );
    return value;
}

[[nodiscard]] bool regular_file(
    const std::filesystem::path& path
) noexcept {
    std::error_code error;
    return !path.empty() && std::filesystem::is_regular_file(path, error);
}

[[nodiscard]] std::filesystem::path direct_audio_candidate(
    const std::filesystem::path& requested,
    const std::vector<std::filesystem::path>& roots
) {
    if (requested.is_absolute()) {
        return regular_file(requested) ? requested : std::filesystem::path{};
    }
    for (const auto& supplied_root : roots) {
        std::error_code error;
        auto root = supplied_root;
        if (std::filesystem::is_regular_file(root, error)) {
            root = root.parent_path();
        }
        const auto relative_candidate = (root / requested).lexically_normal();
        if (regular_file(relative_candidate)) {
            return relative_candidate;
        }
        if (requested.has_parent_path()) {
            const auto filename_candidate = root / requested.filename();
            if (regular_file(filename_candidate)) {
                return filename_candidate;
            }
        }
    }
    return regular_file(requested) ? requested : std::filesystem::path{};
}

struct AudioPathRequest {
    std::filesystem::path requested;
    std::filesystem::path resolved;
};

[[nodiscard]] bool resolve_editor_audio_manifest(
    const AudioManifest& requested,
    const std::vector<std::filesystem::path>& roots,
    AudioManifest& resolved,
    std::string& error
) {
    std::vector<AudioPathRequest> paths;
    paths.reserve(1U + requested.vocals.size());
    if (!requested.instrumental.empty()) {
        paths.push_back({requested.instrumental, {}});
    }
    for (const auto& vocal : requested.vocals) {
        if (!vocal.empty()) {
            paths.push_back({vocal, {}});
        }
    }

    for (auto& path : paths) {
        path.resolved = direct_audio_candidate(path.requested, roots);
    }

    const auto all_resolved = [&]() {
        return std::all_of(
            paths.begin(),
            paths.end(),
            [](const AudioPathRequest& path) {
                return !path.resolved.empty();
            }
        );
    };

    // Some Psych/FNF packs put the chart several directories above Inst and
    // Voices. Scan each explicitly supplied content root once and compare only
    // the requested filenames. This remains bounded even for enormous packs.
    constexpr std::size_t maximum_discovery_entries = 500'000U;
    std::size_t visited_entries = 0U;
    bool discovery_limit_reached = false;
    for (const auto& supplied_root : roots) {
        if (all_resolved()) {
            break;
        }
        std::error_code error_code;
        auto root = supplied_root;
        if (std::filesystem::is_regular_file(root, error_code)) {
            root = root.parent_path();
        }
        error_code.clear();
        if (!std::filesystem::is_directory(root, error_code)) {
            continue;
        }
        std::filesystem::recursive_directory_iterator iterator(
            root,
            std::filesystem::directory_options::skip_permission_denied,
            error_code
        );
        const std::filesystem::recursive_directory_iterator end;
        while (iterator != end) {
            if (visited_entries >= maximum_discovery_entries) {
                discovery_limit_reached = true;
                break;
            }
            ++visited_entries;
            if (!error_code && iterator->is_regular_file(error_code)) {
                const auto candidate_name = ascii_lower(
                    iterator->path().filename().string()
                );
                for (auto& path : paths) {
                    if (path.resolved.empty()
                        && candidate_name == ascii_lower(
                            path.requested.filename().string()
                        )) {
                        path.resolved = iterator->path();
                    }
                }
            }
            error_code.clear();
            iterator.increment(error_code);
            if (error_code) {
                error_code.clear();
            }
            if (all_resolved()) {
                break;
            }
        }
        if (discovery_limit_reached) {
            break;
        }
    }

    const auto missing = std::find_if(
        paths.begin(),
        paths.end(),
        [](const AudioPathRequest& path) { return path.resolved.empty(); }
    );
    if (missing != paths.end()) {
        error = "Song audio was not found: " + missing->requested.string();
        if (discovery_limit_reached) {
            error += " (search stopped after 500000 content entries)";
        }
        return false;
    }

    std::size_t index = 0U;
    if (!requested.instrumental.empty()) {
        resolved.instrumental = paths[index++].resolved;
    }
    resolved.vocals.reserve(requested.vocals.size());
    for (const auto& vocal : requested.vocals) {
        if (!vocal.empty()) {
            resolved.vocals.push_back(paths[index++].resolved);
        }
    }
    return true;
}

}  // namespace

detail::ChartEditorAudioSession::ChartEditorAudioSession(
    const ChartEditorUiOptions& options,
    const AudioManifest& model_audio,
    const double fallback_duration_ms
) {
    if (options.audio != nullptr) {
        audio_requested_ = true;
        if (!options.audio->loaded()) {
            error_ = "The supplied Chart Editor audio transport is not loaded";
            return;
        }
        transport_ = options.audio;
        external_playback_rate_ = transport_->playback_rate();
        external_looping_ = transport_->looping();
        transport_->set_looping(false);
        transport_->set_playback_rate(1.0);
        return;
    }

    const auto& requested = manifest_has_audio(options.audio_manifest)
        ? options.audio_manifest
        : model_audio;
    audio_requested_ = manifest_has_audio(requested);
    if (!audio_requested_) {
        return;
    }

    AudioManifest resolved;
    if (!resolve_editor_audio_manifest(
            requested,
            options.audio_search_roots,
            resolved,
            error_
        )) {
        return;
    }

    owned_ = std::make_unique<AudioTransport>();
    if (!owned_->initialize(options.audio_settings, options.audio_backend, &error_)) {
        owned_.reset();
        return;
    }
    if (!owned_->load(resolved, fallback_duration_ms, 120.0, &error_)) {
        owned_.reset();
        return;
    }
    owned_->set_looping(false);
    // BPM controls grid/note timing only. Editor preview always advances at
    // the media's native one-second-per-second rate.
    owned_->set_playback_rate(1.0);
    transport_ = owned_.get();
}

detail::ChartEditorAudioSession::~ChartEditorAudioSession() {
    if (transport_ != nullptr
        && transport_->state() == AudioTransportState::playing) {
        transport_->pause();
    }
    if (transport_ != nullptr && owned_ == nullptr) {
        transport_->set_looping(external_looping_);
        transport_->set_playback_rate(external_playback_rate_);
    }
}

AudioTransport* detail::ChartEditorAudioSession::transport() const noexcept {
    return transport_;
}

const std::string& detail::ChartEditorAudioSession::error() const noexcept {
    return error_;
}

bool detail::ChartEditorAudioSession::audio_requested() const noexcept {
    return audio_requested_;
}

EditorUiOutcome run_chart_editor_ui(
    SDL_Window* window,
    SDL_Renderer* renderer,
    ChartEditor& editor,
    const ChartEditorUiOptions& options
) {
    EditorUiOutcome outcome;
    if (window == nullptr || renderer == nullptr) {
        outcome.exit = EditorUiExit::invalid_context;
        outcome.message = "Chart Editor requires an existing SDL window and renderer";
        return outcome;
    }

    EditorSessionGuard guard(window, renderer, "PulseForge // Chart Editor");
    auto note_index = build_note_index(editor);
    auto event_index = build_event_index(editor);
    auto tempo_index = build_tempo_index(editor);
    double structural_duration_ms = 1'000.0;
    if (!note_index.empty()) {
        structural_duration_ms = std::max(
            structural_duration_ms,
            note_index.back().time_ms + 2'000.0
        );
    }
    if (!event_index.empty()) {
        structural_duration_ms = std::max(
            structural_duration_ms,
            event_index.back().time_ms + 2'000.0
        );
    }
    if (!tempo_index.empty()) {
        structural_duration_ms = std::max(
            structural_duration_ms,
            tempo_index.back().first + 2'000.0
        );
    }
    detail::ChartEditorAudioSession audio_session(
        options,
        editor.metadata().audio,
        structural_duration_ms
    );
    auto* const editor_audio = audio_session.transport();
    ChartViewState view;
    if (editor_audio != nullptr) {
        view.playhead_ms = editor_audio->position_ms();
    }
    auto review = load_chart_review_state(options, editor, note_index);
    auto available_choices = build_chart_editor_choices(editor, options);
    InlineTextEditor inline_editor;
    SearchableOptionPicker option_picker;
    bool running = true;
    std::string status = audio_session.error().empty()
        ? "LMB add/select   RMB delete   V event   SHIFT+V delete event   ESC back"
        : "Audio unavailable: " + audio_session.error();
    bool status_error = !audio_session.error().empty();
    if (audio_session.error().empty() && review.active()) {
        status = "AutoChart review loaded: " + std::to_string(review.entries.size())
            + " priority note(s)   F6 next   SHIFT+F6 previous   F7 overlay";
    } else if (audio_session.error().empty() && !review.error.empty()
               && !options.autochart_review_index_path.empty()) {
        status = "AutoChart review unavailable: " + review.error;
        status_error = true;
    }

    if (options.storage != nullptr && !options.autosave_path.empty()) {
        try {
            auto settings = editor.autosave_settings();
            settings.enabled = true;
            settings.relative_path = options.autosave_path;
            settings.interval = options.autosave_interval;
            editor.configure_autosave(std::move(settings));
        } catch (const std::exception& exception) {
            status = "Autosave disabled: " + std::string(exception.what());
            status_error = true;
        }
    }

    const auto rebuild_indices = [&]() {
        note_index = build_note_index(editor);
        event_index = build_event_index(editor);
        tempo_index = build_tempo_index(editor);
        if (view.selected_note.has_value()
            && !editor.notes().contains(*view.selected_note)) {
            view.selected_note.reset();
        }
    };

    const auto active_step_ms = [&]() {
        const auto tempo = tempo_at(tempo_index, view.playhead_ms);
        return 60'000.0 / tempo.bpm
            / static_cast<double>(std::max(view.subdivisions, 1));
    };

    const auto seek_to = [&](const double requested) {
        view.playhead_ms = std::clamp(
            requested,
            0.0,
            12.0 * 60.0 * 60.0 * 1'000.0
        );
        if (editor_audio != nullptr) {
            editor_audio->seek_ms(view.playhead_ms);
        }
    };

    const auto navigate_review = [&](const int direction) {
        if (!review.active()) {
            status = review.error.empty()
                ? "No AutoChart review queue is attached to this editor"
                : "AutoChart review unavailable: " + review.error;
            status_error = true;
            return;
        }
        const auto count = review.entries.size();
        std::size_t candidate = 0U;
        if (review.cursor.has_value()) {
            const auto current = *review.cursor;
            candidate = direction >= 0
                ? (current + 1U) % count
                : (current + count - 1U) % count;
        } else {
            candidate = direction >= 0 ? 0U : count - 1U;
        }
        for (std::size_t attempts = 0U; attempts < count; ++attempts) {
            const auto& item = review.entries[candidate];
            const auto found = editor.notes().find(item.entity_id);
            if (found != editor.notes().end()) {
                std::string detail_error;
                if (!load_review_detail(review, candidate, detail_error)) {
                    status = "AutoChart review record failed: " + detail_error;
                    status_error = true;
                    return;
                }
                view.selected_note = item.entity_id;
                view.note_kind = found->second.kind.empty()
                    ? "normal"
                    : found->second.kind;
                seek_to(found->second.time_ms);
                status = review_status_text(review, editor);
                status_error = false;
                return;
            }
            candidate = direction >= 0
                ? (candidate + 1U) % count
                : (candidate + count - 1U) % count;
        }
        status = "Every note in the AutoChart review queue was deleted";
        status_error = true;
    };

    const auto chart_duration = [&]() {
        double duration = 1'000.0;
        if (!note_index.empty()) {
            duration = std::max(duration, note_index.back().time_ms + 2'000.0);
        }
        if (!event_index.empty()) {
            duration = std::max(duration, event_index.back().time_ms + 2'000.0);
        }
        if (!tempo_index.empty()) {
            duration = std::max(duration, tempo_index.back().first + 2'000.0);
        }
        if (editor_audio != nullptr) {
            duration = std::max(duration, editor_audio->duration_ms());
        }
        return duration;
    };

    const auto save_documents = [&]() {
        if (options.storage == nullptr) {
            status = "No EditorStorage was supplied; save is unavailable";
            status_error = true;
            return;
        }
        if (options.project_path.empty() && options.psych_chart_path.empty()) {
            status = "No project or Psych save path was configured";
            status_error = true;
            return;
        }
        bool succeeded = true;
        std::string saved_items;
        if (!options.project_path.empty()) {
            const auto result = editor.save_project(
                *options.storage,
                options.project_path
            );
            if (!result) {
                status = "Project save failed: " + result.message;
                status_error = true;
                succeeded = false;
            } else {
                outcome.project_saved = true;
                saved_items = "project";
            }
        }
        if (succeeded && !options.psych_chart_path.empty()) {
            const auto result = editor.save_psych_json(
                *options.storage,
                options.psych_chart_path
            );
            if (!result) {
                status = "Psych export failed: " + result.message;
                status_error = true;
                succeeded = false;
            } else {
                outcome.compatible_json_saved = true;
                if (saved_items.empty()) {
                    editor.mark_saved();
                    saved_items = "Psych JSON";
                } else {
                    saved_items += " + Psych JSON";
                }
            }
        }
        if (succeeded) {
            status = "Saved " + saved_items + " atomically";
            status_error = false;
        }
    };

    const auto begin_metadata_edit = [&](const std::size_t field) {
        auto current = editor.metadata();
        std::string label;
        std::string initial;
        switch (field) {
        case 0U:
            label = "Song title";
            initial = current.title;
            break;
        case 1U:
            label = "Artist";
            initial = current.artist;
            break;
        case 2U:
            label = "Charter";
            initial = current.charter;
            break;
        case 3U:
            label = "Difficulty";
            initial = current.difficulty;
            break;
        case 4U:
            label = "Stage id";
            initial = current.stage_id;
            break;
        case 5U:
            label = "Player character";
            initial = current.player_character;
            break;
        case 6U:
            label = "Opponent character";
            initial = current.opponent_character;
            break;
        case 7U:
            label = "Girlfriend character";
            initial = current.girlfriend_character;
            break;
        case 8U:
            label = "Note style";
            initial = current.note_style;
            break;
        default:
            return;
        }
        auto apply_value = [&, field, current](
            const std::string_view value,
            std::string& error
        ) mutable {
            const auto edited = std::string(value);
            switch (field) {
            case 0U:
                current.title = edited;
                break;
            case 1U:
                current.artist = edited;
                break;
            case 2U:
                current.charter = edited;
                break;
            case 3U:
                current.difficulty = edited;
                break;
            case 4U:
                current.stage_id = edited;
                break;
            case 5U:
                current.player_character = edited;
                break;
            case 6U:
                current.opponent_character = edited;
                break;
            case 7U:
                current.girlfriend_character = edited;
                break;
            case 8U:
                current.note_style = edited;
                break;
            default:
                return false;
            }
            if (!editor.set_metadata(current, &error)) {
                return false;
            }
            switch (field) {
            case 3U:
                available_choices.difficulties.push_back(edited);
                break;
            case 4U:
                available_choices.stages.push_back(edited);
                break;
            case 5U:
            case 6U:
            case 7U:
                available_choices.characters.push_back(edited);
                break;
            case 8U:
                available_choices.note_styles.push_back(edited);
                break;
            default:
                break;
            }
            normalize_chart_editor_choices(available_choices);
            status = "Chart metadata updated: " + edited;
            status_error = false;
            return true;
        };

        const std::vector<std::string>* suggested = nullptr;
        switch (field) {
        case 3U:
            suggested = &available_choices.difficulties;
            break;
        case 4U:
            suggested = &available_choices.stages;
            break;
        case 5U:
        case 6U:
        case 7U:
            suggested = &available_choices.characters;
            break;
        case 8U:
            suggested = &available_choices.note_styles;
            break;
        default:
            break;
        }
        if (suggested != nullptr) {
            option_picker.begin(
                window,
                std::move(label),
                std::move(initial),
                1'024U,
                *suggested,
                true,
                [&, apply_value](const std::string_view value) mutable {
                    std::string error;
                    if (!apply_value(value, error)) {
                        status = error.empty() ? "Metadata value was rejected" : error;
                        status_error = true;
                    }
                },
                field == 8U
            );
            return;
        }
        inline_editor.begin(
            window,
            std::move(label),
            std::move(initial),
            1'024U,
            apply_value
        );
    };

    const auto begin_bpm_edit = [&]() {
        const auto quantized = quantize_time(
            view.playhead_ms,
            tempo_at(tempo_index, view.playhead_ms),
            view.subdivisions
        );
        const auto initial_bpm = tempo_at(tempo_index, quantized).bpm;
        inline_editor.begin(
            window,
            "BPM at " + format_time(quantized),
            format_number(initial_bpm, 3),
            32U,
            [&, quantized](const std::string_view value, std::string& error) {
                double bpm = 0.0;
                if (!parse_double(value, bpm)) {
                    error = "Enter a finite number";
                    return false;
                }
                std::optional<EditorEntityId> existing_id;
                TempoChange existing;
                for (const auto& [id, tempo] : editor.tempos()) {
                    if (std::abs(tempo.time_ms - quantized) < 0.0001) {
                        existing_id = id;
                        existing = tempo;
                        break;
                    }
                }
                bool changed = false;
                if (existing_id.has_value()) {
                    existing.bpm = bpm;
                    changed = editor.update_tempo(*existing_id, existing, &error);
                } else {
                    changed = editor.add_tempo(
                        TempoChange{quantized, bpm, 4U, 4U},
                        &error
                    ).has_value();
                }
                if (!changed) {
                    return false;
                }
                tempo_index = build_tempo_index(editor);
                status = "BPM change updated";
                status_error = false;
                return true;
            }
        );
    };

    const auto begin_event_values_edit = [&](const std::string_view event_name) {
        const auto quantized = quantize_time(
            view.playhead_ms,
            tempo_at(tempo_index, view.playhead_ms),
            view.subdivisions
        );
        inline_editor.begin(
            window,
            "Event: name | value1 | value2",
            std::string(event_name) + "||",
            4'096U,
            [&, quantized](const std::string_view value, std::string& error) {
                const auto parts = split(value, '|', true);
                if (parts.empty() || parts[0].empty()) {
                    error = "Event name cannot be empty";
                    return false;
                }
                ChartEvent event;
                event.time_ms = quantized;
                event.name = parts[0];
                if (parts.size() > 1U) {
                    event.value1 = parts[1];
                }
                if (parts.size() > 2U) {
                    event.value2 = parts[2];
                }
                const auto id = editor.add_event(std::move(event), &error);
                if (!id.has_value()) {
                    return false;
                }
                available_choices.event_names.push_back(parts[0]);
                normalize_chart_editor_choices(available_choices);
                insert_note_index(event_index, {quantized, *id});
                status = "Event added at " + format_time(quantized);
                status_error = false;
                return true;
            }
        );
    };

    const auto begin_event_edit = [&]() {
        option_picker.begin(
            window,
            "Choose event type",
            {},
            maximum_chart_event_name_bytes,
            available_choices.event_names,
            true,
            [&](const std::string_view value) {
                begin_event_values_edit(value);
            }
        );
    };

    const auto begin_note_kind_edit = [&]() {
        option_picker.begin(
            window,
            view.selected_note.has_value()
                ? "Choose note type for selected and new notes"
                : "Choose note type for new notes",
            view.note_kind,
            maximum_chart_note_kind_bytes,
            available_choices.note_types,
            true,
            [&](const std::string_view value) {
                if (value.empty()) {
                    status = "Use normal or a custom note type";
                    status_error = true;
                    return;
                }
                view.note_kind = std::string(value);
                available_choices.note_types.push_back(view.note_kind);
                normalize_chart_editor_choices(available_choices);
                if (view.selected_note.has_value()) {
                    const auto found = editor.notes().find(*view.selected_note);
                    if (found != editor.notes().end()) {
                        auto changed = found->second;
                        changed.kind = view.note_kind == "normal"
                            ? std::string{}
                            : view.note_kind;
                        std::string error;
                        if (!editor.update_note(
                                *view.selected_note,
                                std::move(changed),
                                &error
                            )) {
                            status = std::move(error);
                            status_error = true;
                            return;
                        }
                        rebuild_indices();
                        status = "Selected note type: " + view.note_kind;
                        status_error = false;
                        return;
                    }
                    view.selected_note.reset();
                }
                status = "New note type: " + view.note_kind;
                status_error = false;
            }
        );
    };

    const auto begin_scripts_edit = [&]() {
        option_picker.begin(
            window,
            "Attach or detach chart script",
            {},
            32U * 1'024U,
            available_choices.scripts,
            true,
            [&](const std::string_view value) {
                auto scripts = editor.scripts();
                const auto found = std::find(scripts.begin(), scripts.end(), value);
                const bool removing = found != scripts.end();
                if (removing) {
                    scripts.erase(found);
                } else {
                    scripts.emplace_back(value);
                }
                std::string error;
                if (!editor.set_scripts(std::move(scripts), &error)) {
                    status = error.empty() ? "Script selection was rejected" : error;
                    status_error = true;
                    return;
                }
                available_choices.scripts.emplace_back(value);
                normalize_chart_editor_choices(available_choices);
                status = std::string(removing ? "Detached script: " : "Attached script: ")
                    + std::string(value);
                status_error = false;
            }
        );
    };

    const auto begin_scripts_text_edit = [&]() {
        inline_editor.begin(
            window,
            "Scripts (comma-separated relative paths)",
            scripts_as_csv(editor.scripts()),
            32U * 1'024U,
            [&](const std::string_view value, std::string& error) {
                if (!editor.set_scripts(scripts_from_csv(value), &error)) {
                    return false;
                }
                available_choices.scripts.insert(
                    available_choices.scripts.end(),
                    editor.scripts().begin(),
                    editor.scripts().end()
                );
                normalize_chart_editor_choices(available_choices);
                status = "Chart script list updated";
                status_error = false;
                return true;
            }
        );
    };

    const auto begin_scroll_edit = [&]() {
        inline_editor.begin(
            window,
            "Chart scroll speed",
            format_number(editor.scroll_speed(), 3),
            32U,
            [&](const std::string_view value, std::string& error) {
                double speed = 0.0;
                if (!parse_double(value, speed)) {
                    error = "Enter a finite number";
                    return false;
                }
                if (!editor.set_scroll_speed(speed, &error)) {
                    return false;
                }
                status = "Scroll speed updated";
                status_error = false;
                return true;
            }
        );
    };

    while (running) {
        if (editor_audio != nullptr
            && editor_audio->state() == AudioTransportState::playing) {
            view.playhead_ms = editor_audio->position_ms();
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            static_cast<void>(SDL_ConvertEventToRenderCoordinates(renderer, &event));
            if (editor_audio != nullptr
                && event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                editor_audio->set_focused(true);
            } else if (editor_audio != nullptr
                       && event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                editor_audio->set_focused(false);
            }
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                outcome.exit = EditorUiExit::quit_requested;
                running = false;
                break;
            }
            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                view.mouse_x = event.motion.x;
                view.mouse_y = event.motion.y;
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                       || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                view.mouse_x = event.button.x;
                view.mouse_y = event.button.y;
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                view.mouse_x = event.wheel.mouse_x;
                view.mouse_y = event.wheel.mouse_y;
            }

            if (option_picker.active()) {
                static_cast<void>(option_picker.handle(window, event));
                continue;
            }

            if (inline_editor.active()) {
                static_cast<void>(inline_editor.handle(window, event));
                continue;
            }

            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                const bool control = (event.key.mod & SDL_KMOD_CTRL) != 0;
                const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
                if (control && event.key.scancode == SDL_SCANCODE_S) {
                    save_documents();
                    continue;
                }
                if (control && event.key.scancode == SDL_SCANCODE_Z) {
                    std::string label;
                    if (editor.undo(&label)) {
                        rebuild_indices();
                        status = "Undo: " + label;
                        status_error = false;
                    }
                    continue;
                }
                if (control && event.key.scancode == SDL_SCANCODE_Y) {
                    std::string label;
                    if (editor.redo(&label)) {
                        rebuild_indices();
                        status = "Redo: " + label;
                        status_error = false;
                    }
                    continue;
                }

                switch (event.key.scancode) {
                case SDL_SCANCODE_F6:
                    navigate_review(shift ? -1 : 1);
                    break;
                case SDL_SCANCODE_F7:
                    if (review.active()) {
                        review.visible = !review.visible;
                        status = std::string{"AutoChart confidence overlay "}
                            + (review.visible ? "enabled" : "hidden");
                        status_error = false;
                    } else {
                        status = "No AutoChart confidence overlay is available";
                        status_error = true;
                    }
                    break;
                case SDL_SCANCODE_ESCAPE: {
                    const auto now = SDL_GetTicks();
                    if (editor.dirty() && now > view.escape_armed_until) {
                        view.escape_armed_until = now + 2'000U;
                        status = "Unsaved changes - press ESC again within 2 seconds to leave";
                        status_error = true;
                    } else {
                        running = false;
                    }
                    break;
                }
                case SDL_SCANCODE_SPACE:
                    if (editor_audio != nullptr) {
                        if (editor_audio->state() == AudioTransportState::playing) {
                            editor_audio->pause();
                            status = "Audio paused";
                        } else if (editor_audio->state()
                                   == AudioTransportState::paused) {
                            editor_audio->resume();
                            status = "Audio resumed";
                        } else {
                            if (editor_audio->state() == AudioTransportState::ended) {
                                view.playhead_ms = 0.0;
                            }
                            editor_audio->seek_ms(view.playhead_ms);
                            editor_audio->play();
                            status = "Audio playing";
                        }
                        status_error = false;
                    } else {
                        status = audio_session.audio_requested()
                            ? "Audio unavailable: " + audio_session.error()
                            : "No song audio was supplied to the Chart Editor";
                        status_error = true;
                    }
                    break;
                case SDL_SCANCODE_HOME:
                    seek_to(0.0);
                    break;
                case SDL_SCANCODE_LEFT:
                case SDL_SCANCODE_UP:
                case SDL_SCANCODE_W:
                    seek_to(view.playhead_ms - active_step_ms());
                    break;
                case SDL_SCANCODE_RIGHT:
                case SDL_SCANCODE_DOWN:
                case SDL_SCANCODE_S:
                    seek_to(view.playhead_ms + active_step_ms());
                    break;
                case SDL_SCANCODE_PAGEUP:
                    seek_to(view.playhead_ms - active_step_ms() * 16.0);
                    break;
                case SDL_SCANCODE_PAGEDOWN:
                    seek_to(view.playhead_ms + active_step_ms() * 16.0);
                    break;
                case SDL_SCANCODE_MINUS:
                case SDL_SCANCODE_KP_MINUS:
                    view.milliseconds_per_pixel = std::min(
                        500.0,
                        view.milliseconds_per_pixel * 1.25
                    );
                    break;
                case SDL_SCANCODE_EQUALS:
                case SDL_SCANCODE_KP_PLUS:
                    view.milliseconds_per_pixel = std::max(
                        0.05,
                        view.milliseconds_per_pixel / 1.25
                    );
                    break;
                case SDL_SCANCODE_LEFTBRACKET: {
                    constexpr std::array values{1, 2, 3, 4, 6, 8, 12, 16, 24, 32};
                    const auto found = std::lower_bound(
                        values.begin(),
                        values.end(),
                        view.subdivisions
                    );
                    if (found != values.begin()) {
                        view.subdivisions = *std::prev(found);
                    }
                    break;
                }
                case SDL_SCANCODE_RIGHTBRACKET: {
                    constexpr std::array values{1, 2, 3, 4, 6, 8, 12, 16, 24, 32};
                    const auto found = std::upper_bound(
                        values.begin(),
                        values.end(),
                        view.subdivisions
                    );
                    if (found != values.end()) {
                        view.subdivisions = *found;
                    }
                    break;
                }
                case SDL_SCANCODE_Q:
                    view.sustain_steps = std::max(0, view.sustain_steps - 1);
                    break;
                case SDL_SCANCODE_E:
                    if (shift) {
                        begin_event_edit();
                    } else {
                        view.sustain_steps = std::min(
                            view.sustain_steps + 1,
                            1'024
                        );
                    }
                    break;
                case SDL_SCANCODE_B:
                    if (shift) {
                        const auto quantized = quantize_time(
                            view.playhead_ms,
                            tempo_at(tempo_index, view.playhead_ms),
                            view.subdivisions
                        );
                        std::optional<EditorEntityId> id;
                        for (const auto& [tempo_id, tempo] : editor.tempos()) {
                            if (std::abs(tempo.time_ms - quantized) < 0.0001) {
                                id = tempo_id;
                                break;
                            }
                        }
                        std::string error;
                        if (id.has_value() && editor.remove_tempo(*id, &error)) {
                            tempo_index = build_tempo_index(editor);
                            status = "BPM change removed";
                            status_error = false;
                        } else {
                            status = error.empty()
                                ? "No BPM change at cursor"
                                : error;
                            status_error = true;
                        }
                    } else {
                        begin_bpm_edit();
                    }
                    break;
                case SDL_SCANCODE_V:
                    if (shift) {
                        const double radius = active_step_ms() * 0.5;
                        const auto first = std::lower_bound(
                            event_index.begin(),
                            event_index.end(),
                            view.playhead_ms - radius,
                            [](const IndexedNote& indexed, const double time) {
                                return indexed.time_ms < time;
                            }
                        );
                        std::optional<EditorEntityId> closest;
                        double closest_distance = radius;
                        for (auto iterator = first;
                             iterator != event_index.end()
                                 && iterator->time_ms
                                     <= view.playhead_ms + radius;
                             ++iterator) {
                            const double distance = std::abs(
                                iterator->time_ms - view.playhead_ms
                            );
                            if (distance <= closest_distance) {
                                closest = iterator->id;
                                closest_distance = distance;
                            }
                        }
                        std::string error;
                        if (closest.has_value()
                            && editor.remove_event(*closest, &error)) {
                            event_index = build_event_index(editor);
                            status = "Nearest event removed";
                            status_error = false;
                        } else {
                            status = error.empty()
                                ? "No event close to the cursor"
                                : error;
                            status_error = true;
                        }
                    } else {
                        begin_event_edit();
                    }
                    break;
                case SDL_SCANCODE_N:
                    begin_note_kind_edit();
                    break;
                case SDL_SCANCODE_K:
                    begin_metadata_edit(8U);
                    break;
                case SDL_SCANCODE_L:
                    if (shift) {
                        begin_scripts_text_edit();
                    } else {
                        begin_scripts_edit();
                    }
                    break;
                case SDL_SCANCODE_O:
                    begin_scroll_edit();
                    break;
                case SDL_SCANCODE_DELETE:
                case SDL_SCANCODE_BACKSPACE:
                    if (view.selected_note.has_value()) {
                        std::string error;
                        if (editor.remove_note(*view.selected_note, &error)) {
                            rebuild_indices();
                            view.selected_note.reset();
                            status = "Note removed";
                            status_error = false;
                        } else {
                            status = error;
                            status_error = true;
                        }
                    }
                    break;
                default:
                    break;
                }
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL
                       && contains(chart_grid, view.mouse_x, view.mouse_y)) {
                const bool control = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
                if (control) {
                    const double factor = event.wheel.y > 0.0F ? 0.82 : 1.22;
                    view.milliseconds_per_pixel = std::clamp(
                        view.milliseconds_per_pixel * factor,
                        0.05,
                        500.0
                    );
                } else {
                    seek_to(
                        view.playhead_ms
                        - static_cast<double>(event.wheel.y)
                            * active_step_ms() * 4.0
                    );
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                const SDL_FRect timeline{28.0F, 642.0F, 790.0F, 27.0F};
                if (contains(timeline, event.button.x, event.button.y)
                    && event.button.button == SDL_BUTTON_LEFT) {
                    const double ratio = std::clamp(
                        static_cast<double>((event.button.x - timeline.x) / timeline.w),
                        0.0,
                        1.0
                    );
                    seek_to(chart_duration() * ratio);
                    continue;
                }
                if (contains(chart_grid, event.button.x, event.button.y)) {
                    const auto nearby = note_near(
                        editor,
                        note_index,
                        view,
                        event.button.x,
                        event.button.y
                    );
                    if (event.button.button == SDL_BUTTON_RIGHT) {
                        if (nearby.has_value()) {
                            std::string error;
                            if (editor.remove_note(*nearby, &error)) {
                                rebuild_indices();
                                view.selected_note.reset();
                                status = "Note removed";
                                status_error = false;
                            } else {
                                status = error;
                                status_error = true;
                            }
                        }
                        continue;
                    }
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        if (nearby.has_value()) {
                            view.selected_note = nearby;
                            const auto selected = editor.notes().find(*nearby);
                            if (selected != editor.notes().end()) {
                                view.note_kind = selected->second.kind.empty()
                                    ? "normal"
                                    : selected->second.kind;
                            }
                            const auto review_item = review.by_entity.find(*nearby);
                            if (review_item != review.by_entity.end()) {
                                std::string detail_error;
                                if (load_review_detail(
                                        review,
                                        review_item->second,
                                        detail_error
                                    )) {
                                    status = review_status_text(review, editor);
                                    status_error = false;
                                } else {
                                    status = "Note selected; review detail failed: "
                                        + detail_error;
                                    status_error = true;
                                }
                            } else {
                                status = "Note selected; N changes type, DELETE removes it";
                                status_error = false;
                            }
                            continue;
                        }
                        const auto column_width = chart_grid.w
                            / static_cast<float>(editor.key_count() * 2U);
                        const auto raw_column = std::clamp(
                            static_cast<int>((event.button.x - chart_grid.x)
                                / column_width),
                            0,
                            static_cast<int>(editor.key_count() * 2U - 1U)
                        );
                        const double raw_time = view.playhead_ms
                            + static_cast<double>(
                                event.button.y
                                - (chart_grid.y + chart_grid.h * 0.5F)
                            ) * view.milliseconds_per_pixel;
                        const auto active_tempo = tempo_at(tempo_index, raw_time);
                        const double note_time = std::max(
                            -60'000.0,
                            quantize_time(raw_time, active_tempo, view.subdivisions)
                        );
                        Note note;
                        note.time_ms = note_time;
                        note.duration_ms = 60'000.0 / active_tempo.bpm
                            / static_cast<double>(view.subdivisions)
                            * static_cast<double>(view.sustain_steps);
                        note.owner = raw_column < editor.key_count()
                            ? NoteOwner::opponent
                            : NoteOwner::player;
                        note.lane = static_cast<std::uint16_t>(
                            raw_column % editor.key_count()
                        );
                        note.kind = view.note_kind;
                        std::string error;
                        const auto id = editor.add_note(note, &error);
                        if (id.has_value()) {
                            insert_note_index(note_index, {note_time, *id});
                            view.selected_note = *id;
                            status = "Note added at " + format_time(note_time);
                            status_error = false;
                        } else {
                            status = error;
                            status_error = true;
                        }
                        continue;
                    }
                }

                if (event.button.button == SDL_BUTTON_LEFT
                    && event.button.x >= 854.0F
                    && event.button.x <= 1'236.0F
                    && event.button.y >= 134.0F
                    && event.button.y < 404.0F) {
                    const auto field = static_cast<std::size_t>(
                        (event.button.y - 134.0F) / 30.0F
                    );
                    if (field < 9U) {
                        begin_metadata_edit(field);
                    }
                } else if (event.button.button == SDL_BUTTON_LEFT
                           && contains(
                               {854.0F, 522.0F, 184.0F, 36.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    begin_bpm_edit();
                } else if (event.button.button == SDL_BUTTON_LEFT
                           && contains(
                               {1'052.0F, 522.0F, 184.0F, 36.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    begin_event_edit();
                } else if (event.button.button == SDL_BUTTON_LEFT
                           && contains(
                               {854.0F, 566.0F, 118.0F, 36.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    begin_note_kind_edit();
                } else if (event.button.button == SDL_BUTTON_LEFT
                           && contains(
                               {980.0F, 566.0F, 128.0F, 36.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    begin_metadata_edit(8U);
                } else if (event.button.button == SDL_BUTTON_LEFT
                           && contains(
                               {1'116.0F, 566.0F, 120.0F, 36.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    begin_scripts_edit();
                } else if (event.button.button == SDL_BUTTON_LEFT
                           && contains(
                               {854.0F, 610.0F, 382.0F, 36.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    save_documents();
                }
            }
        }

        if (!running) {
            break;
        }
        if (options.storage != nullptr) {
            std::string autosave_error;
            const auto autosave = editor.autosave_if_due(
                *options.storage,
                std::chrono::steady_clock::now(),
                &autosave_error
            );
            if (autosave == EditorAutosaveStatus::saved) {
                status = "Autosave recovery project updated";
                status_error = false;
            } else if (autosave == EditorAutosaveStatus::failed) {
                status = "Autosave failed: " + autosave_error;
                status_error = true;
            }
        }

        const auto ticks = SDL_GetTicks();
        draw_background(renderer, ticks, cyan);
        text(renderer, 28.0F, 22.0F, "PULSEFORGE  //  CHART EDITOR", cyan, 1.8F);
        text(
            renderer,
            28.0F,
            55.0F,
            review.active()
                ? "SPACE audio   arrows seek   +/- zoom   Q/E sustain   N type   F6 review   F7 overlay"
                : "SPACE audio   arrows/W/S seek   +/- zoom   [ ] quantize   Q/E sustain   N type   K note skin",
            muted,
            1.02F,
            120U
        );
        draw_chart_grid(
            renderer,
            editor,
            note_index,
            event_index,
            tempo_index,
            view,
            &review
        );

        const SDL_FRect timeline{28.0F, 642.0F, 790.0F, 27.0F};
        fill(renderer, timeline, {22, 20, 46, 250});
        outline(renderer, timeline, panel_border);
        const auto duration = chart_duration();
        const float progress = static_cast<float>(std::clamp(
            view.playhead_ms / duration,
            0.0,
            1.0
        ));
        fill(
            renderer,
            {timeline.x, timeline.y, timeline.w * progress, timeline.h},
            {cyan.r, cyan.g, cyan.b, 90}
        );
        fill(
            renderer,
            {timeline.x + timeline.w * progress - 1.5F,
             timeline.y,
             3.0F,
             timeline.h},
            yellow
        );
        text(
            renderer,
            timeline.x + 8.0F,
            timeline.y + 8.0F,
            format_time(view.playhead_ms) + " / " + format_time(duration),
            white,
            0.95F
        );
        draw_chart_inspector(
            renderer,
            editor,
            view,
            tempo_index,
            status,
            status_error,
            options.storage != nullptr
                && (!options.project_path.empty()
                    || !options.psych_chart_path.empty()),
            view.mouse_x,
            view.mouse_y,
            &review
        );
        inline_editor.draw(renderer, ticks);
        option_picker.draw(renderer, ticks);
        static_cast<void>(SDL_RenderPresent(renderer));
        SDL_Delay(1U);
    }

    option_picker.cancel(window);
    inline_editor.cancel(window);
    if (outcome.message.empty()) {
        outcome.message = status;
    }
    return outcome;
}

namespace {

[[nodiscard]] bool parse_double_pair(
    const std::string_view value,
    double& first,
    double& second
) {
    const auto parts = split(value, ',', false);
    return parts.size() == 2U
        && parse_double(parts[0], first)
        && parse_double(parts[1], second);
}

[[nodiscard]] bool parse_rgb(
    const std::string_view value,
    DescriptorRgb& color
) {
    const auto parts = split(value, ',', false);
    int red = 0;
    int green = 0;
    int blue = 0;
    if (parts.size() != 3U || !parse_int(parts[0], red)
        || !parse_int(parts[1], green) || !parse_int(parts[2], blue)
        || red < 0 || red > 255 || green < 0 || green > 255 || blue < 0
        || blue > 255) {
        return false;
    }
    color = {red, green, blue};
    return true;
}

[[nodiscard]] std::string rgb_text(const DescriptorRgb& color) {
    return std::to_string(color.red) + "," + std::to_string(color.green)
        + "," + std::to_string(color.blue);
}

void draw_character_preview(
    SDL_Renderer* renderer,
    const CharacterDescriptor& character,
    const std::size_t selected_animation
) {
    const SDL_FRect preview{28.0F, 104.0F, 520.0F, 326.0F};
    panel(renderer, preview, purple);
    text(renderer, 46.0F, 122.0F, "LIVE CHARACTER PREVIEW", purple, 1.25F);
    const auto color = SDL_Color{
        static_cast<std::uint8_t>(std::clamp(character.healthbar_color.red, 0, 255)),
        static_cast<std::uint8_t>(std::clamp(character.healthbar_color.green, 0, 255)),
        static_cast<std::uint8_t>(std::clamp(character.healthbar_color.blue, 0, 255)),
        255,
    };
    const float scale = static_cast<float>(std::clamp(character.scale, 0.25, 3.0));
    const float center_x = preview.x + preview.w * 0.5F
        + static_cast<float>(character.position.x) * 0.08F;
    const float floor_y = preview.y + preview.h - 38.0F
        + static_cast<float>(character.position.y) * 0.04F;
    fill(renderer, {preview.x + 20.0F, floor_y, preview.w - 40.0F, 2.0F}, {91, 79, 126, 210});
    fill(
        renderer,
        {center_x - 34.0F * scale,
         floor_y - 116.0F * scale,
         68.0F * scale,
         92.0F * scale},
        {color.r, color.g, color.b, 220}
    );
    fill(
        renderer,
        {center_x - 25.0F * scale,
         floor_y - 164.0F * scale,
         50.0F * scale,
         50.0F * scale},
        {static_cast<std::uint8_t>(std::min(255, color.r + 25)),
         static_cast<std::uint8_t>(std::min(255, color.g + 25)),
         static_cast<std::uint8_t>(std::min(255, color.b + 25)),
         240}
    );
    line(
        renderer,
        center_x - 48.0F * scale,
        floor_y - 100.0F * scale,
        center_x + 48.0F * scale,
        floor_y - 72.0F * scale,
        white
    );
    const float camera_x = preview.x + preview.w * 0.5F
        + static_cast<float>(character.camera_position.x) * 0.08F;
    const float camera_y = preview.y + preview.h * 0.45F
        + static_cast<float>(character.camera_position.y) * 0.08F;
    line(renderer, camera_x - 9.0F, camera_y, camera_x + 9.0F, camera_y, yellow);
    line(renderer, camera_x, camera_y - 9.0F, camera_x, camera_y + 9.0F, yellow);
    text(
        renderer,
        46.0F,
        392.0F,
        "Image " + character.image,
        muted,
        0.98F,
        61U
    );
    if (!character.animations.empty()) {
        const auto index = std::min(
            selected_animation,
            character.animations.size() - 1U
        );
        text(
            renderer,
            46.0F,
            410.0F,
            "Animation " + character.animations[index].id + " / "
                + character.animations[index].name,
            cyan,
            0.98F,
            61U
        );
    }
}

}  // namespace

EditorUiOutcome run_character_editor_ui(
    SDL_Window* window,
    SDL_Renderer* renderer,
    CharacterEditor& editor,
    const DescriptorEditorUiOptions& options
) {
    EditorUiOutcome outcome;
    if (window == nullptr || renderer == nullptr) {
        outcome.exit = EditorUiExit::invalid_context;
        outcome.message = "Character Editor requires an existing SDL context";
        return outcome;
    }

    EditorSessionGuard guard(window, renderer, "PulseForge // Character Editor");
    InlineTextEditor inline_editor;
    std::size_t selected_animation = 0U;
    float mouse_x = 0.0F;
    float mouse_y = 0.0F;
    bool running = true;
    std::uint64_t escape_armed_until = 0U;
    std::string status =
        "Click fields   A add   DELETE remove   R loop   I frame indices   CTRL+S save";
    bool status_error = false;

    const auto apply = [&](CharacterDescriptor document, std::string label) {
        std::string error;
        if (!editor.replace(std::move(document), std::move(label), &error)) {
            status = error;
            status_error = true;
            return false;
        }
        if (!editor.document().animations.empty()) {
            selected_animation = std::min(
                selected_animation,
                editor.document().animations.size() - 1U
            );
        } else {
            selected_animation = 0U;
        }
        status_error = false;
        return true;
    };

    const auto save = [&]() {
        if (options.storage == nullptr || options.psych_json_path.empty()) {
            status = "Character save path/storage is not configured";
            status_error = true;
            return;
        }
        const auto result = editor.save_psych_json(
            *options.storage,
            options.psych_json_path
        );
        if (!result) {
            status = "Character save failed: " + result.message;
            status_error = true;
            return;
        }
        outcome.compatible_json_saved = true;
        status = "Character JSON saved atomically";
        status_error = false;
    };

    const auto begin_root_field = [&](const std::size_t field) {
        auto current = editor.document();
        std::string label;
        std::string initial;
        switch (field) {
        case 0U:
            label = "Character image asset";
            initial = current.image;
            break;
        case 1U:
            label = "Scale";
            initial = format_number(current.scale, 3);
            break;
        case 2U:
            label = "Sing duration";
            initial = format_number(current.sing_duration, 3);
            break;
        case 3U:
            label = "Health icon";
            initial = current.health_icon;
            break;
        case 4U:
            label = "Position x,y";
            initial = format_number(current.position.x) + ","
                + format_number(current.position.y);
            break;
        case 5U:
            label = "Camera position x,y";
            initial = format_number(current.camera_position.x) + ","
                + format_number(current.camera_position.y);
            break;
        case 6U:
            label = "Healthbar RGB r,g,b";
            initial = rgb_text(current.healthbar_color);
            break;
        case 7U:
            label = "Vocals file";
            initial = current.vocals_file;
            break;
        default:
            return;
        }
        inline_editor.begin(
            window,
            std::move(label),
            std::move(initial),
            16U * 1'024U,
            [&, field, current](const std::string_view value, std::string& error) mutable {
                double first = 0.0;
                double second = 0.0;
                switch (field) {
                case 0U:
                    current.image = std::string(value);
                    break;
                case 1U:
                    if (!parse_double(value, first)) {
                        error = "Enter a finite scale";
                        return false;
                    }
                    current.scale = first;
                    break;
                case 2U:
                    if (!parse_double(value, first)) {
                        error = "Enter a finite duration";
                        return false;
                    }
                    current.sing_duration = first;
                    break;
                case 3U:
                    current.health_icon = std::string(value);
                    break;
                case 4U:
                    if (!parse_double_pair(value, first, second)) {
                        error = "Use x,y";
                        return false;
                    }
                    current.position = {first, second};
                    break;
                case 5U:
                    if (!parse_double_pair(value, first, second)) {
                        error = "Use x,y";
                        return false;
                    }
                    current.camera_position = {first, second};
                    break;
                case 6U:
                    if (!parse_rgb(value, current.healthbar_color)) {
                        error = "Use r,g,b with components from 0 to 255";
                        return false;
                    }
                    break;
                case 7U:
                    current.vocals_file = std::string(value);
                    break;
                default:
                    return false;
                }
                if (!apply(current, "Edit character field")) {
                    error = status;
                    return false;
                }
                status = "Character field updated";
                return true;
            }
        );
    };

    const auto begin_animation_field = [&](const std::size_t field) {
        auto current = editor.document();
        if (current.animations.empty()) {
            status = "Add an animation first with A";
            status_error = true;
            return;
        }
        const auto animation_index = std::min(
            selected_animation,
            current.animations.size() - 1U
        );
        const auto& animation = current.animations[animation_index];
        std::string label;
        std::string initial;
        switch (field) {
        case 0U:
            label = "Animation id";
            initial = animation.id;
            break;
        case 1U:
            label = "Atlas prefix/name";
            initial = animation.name;
            break;
        case 2U:
            label = "Animation FPS";
            initial = std::to_string(animation.fps);
            break;
        case 3U:
            label = "Animation offsets x,y";
            initial = format_number(animation.offsets.x) + ","
                + format_number(animation.offsets.y);
            break;
        default:
            return;
        }
        inline_editor.begin(
            window,
            std::move(label),
            std::move(initial),
            16U * 1'024U,
            [&, field, current, animation_index](
                const std::string_view value,
                std::string& error
            ) mutable {
                auto& target = current.animations[animation_index];
                double first = 0.0;
                double second = 0.0;
                int integer = 0;
                switch (field) {
                case 0U:
                    target.id = std::string(value);
                    break;
                case 1U:
                    target.name = std::string(value);
                    break;
                case 2U:
                    if (!parse_int(value, integer)) {
                        error = "Enter an integer FPS";
                        return false;
                    }
                    target.fps = integer;
                    break;
                case 3U:
                    if (!parse_double_pair(value, first, second)) {
                        error = "Use x,y";
                        return false;
                    }
                    target.offsets = {first, second};
                    break;
                default:
                    return false;
                }
                if (!apply(current, "Edit character animation")) {
                    error = status;
                    return false;
                }
                status = "Animation updated";
                return true;
            }
        );
    };

    const auto begin_animation_indices = [&]() {
        auto current = editor.document();
        if (current.animations.empty()) {
            status = "Add an animation first with A";
            status_error = true;
            return;
        }
        const auto animation_index = std::min(
            selected_animation,
            current.animations.size() - 1U
        );
        std::string initial;
        const auto& indices = current.animations[animation_index].indices;
        for (std::size_t index = 0U; index < indices.size(); ++index) {
            if (index != 0U) {
                initial += ',';
            }
            initial += std::to_string(indices[index]);
        }
        inline_editor.begin(
            window,
            "Animation frame indices (CSV; empty uses atlas prefix)",
            std::move(initial),
            256U * 1'024U,
            [&, current, animation_index](
                const std::string_view value,
                std::string& error
            ) mutable {
                std::vector<std::int32_t> parsed_indices;
                if (!trim(value).empty()) {
                    const auto values = split(value, ',', false);
                    if (values.size() > 1'000'000U) {
                        error = "Too many frame indices";
                        return false;
                    }
                    parsed_indices.reserve(values.size());
                    for (const auto& item : values) {
                        int frame = 0;
                        if (!parse_int(item, frame) || frame < 0) {
                            error = "Frame indices must be non-negative integers";
                            return false;
                        }
                        parsed_indices.push_back(frame);
                    }
                }
                current.animations[animation_index].indices =
                    std::move(parsed_indices);
                if (!apply(current, "Edit animation frame indices")) {
                    error = status;
                    return false;
                }
                status = "Animation frame indices updated";
                return true;
            }
        );
    };

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            static_cast<void>(SDL_ConvertEventToRenderCoordinates(renderer, &event));
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                outcome.exit = EditorUiExit::quit_requested;
                running = false;
                break;
            }
            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                mouse_x = event.motion.x;
                mouse_y = event.motion.y;
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                mouse_x = event.button.x;
                mouse_y = event.button.y;
            }
            if (inline_editor.active()) {
                static_cast<void>(inline_editor.handle(window, event));
                continue;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                const bool control = (event.key.mod & SDL_KMOD_CTRL) != 0;
                if (control && event.key.scancode == SDL_SCANCODE_S) {
                    save();
                    continue;
                }
                if (control && event.key.scancode == SDL_SCANCODE_Z) {
                    std::string label;
                    if (editor.undo(&label)) {
                        if (editor.document().animations.empty()) {
                            selected_animation = 0U;
                        } else {
                            selected_animation = std::min(
                                selected_animation,
                                editor.document().animations.size() - 1U
                            );
                        }
                        status = "Undo: " + label;
                        status_error = false;
                    }
                    continue;
                }
                if (control && event.key.scancode == SDL_SCANCODE_Y) {
                    std::string label;
                    if (editor.redo(&label)) {
                        if (editor.document().animations.empty()) {
                            selected_animation = 0U;
                        } else {
                            selected_animation = std::min(
                                selected_animation,
                                editor.document().animations.size() - 1U
                            );
                        }
                        status = "Redo: " + label;
                        status_error = false;
                    }
                    continue;
                }
                switch (event.key.scancode) {
                case SDL_SCANCODE_ESCAPE: {
                    const auto now = SDL_GetTicks();
                    if (editor.dirty() && now > escape_armed_until) {
                        escape_armed_until = now + 2'000U;
                        status = "Unsaved character - press ESC again to leave";
                        status_error = true;
                    } else {
                        running = false;
                    }
                    break;
                }
                case SDL_SCANCODE_A: {
                    auto document = editor.document();
                    AnimationDescriptor animation;
                    animation.id = "animation-"
                        + std::to_string(document.animations.size() + 1U);
                    animation.name = animation.id;
                    document.animations.push_back(std::move(animation));
                    selected_animation = document.animations.size() - 1U;
                    if (apply(std::move(document), "Add character animation")) {
                        status = "Animation added";
                    }
                    break;
                }
                case SDL_SCANCODE_R:
                    if (!editor.document().animations.empty()) {
                        auto document = editor.document();
                        document.animations[selected_animation].loop =
                            !document.animations[selected_animation].loop;
                        if (apply(std::move(document), "Toggle animation loop")) {
                            status = "Animation loop toggled";
                        }
                    }
                    break;
                case SDL_SCANCODE_I:
                    begin_animation_indices();
                    break;
                case SDL_SCANCODE_DELETE:
                    if (!editor.document().animations.empty()) {
                        auto document = editor.document();
                        document.animations.erase(
                            document.animations.begin()
                            + static_cast<std::ptrdiff_t>(selected_animation)
                        );
                        if (apply(std::move(document), "Remove character animation")) {
                            status = "Animation removed";
                        }
                    }
                    break;
                default:
                    break;
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                       && event.button.button == SDL_BUTTON_LEFT) {
                if (event.button.x >= 584.0F && event.button.x <= 1'236.0F
                    && event.button.y >= 132.0F
                    && event.button.y < 404.0F) {
                    const auto field = static_cast<std::size_t>(
                        (event.button.y - 132.0F) / 34.0F
                    );
                    if (field < 8U) {
                        begin_root_field(field);
                    }
                } else if (contains(
                               {584.0F, 412.0F, 310.0F, 36.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    auto document = editor.document();
                    document.flip_x = !document.flip_x;
                    if (apply(std::move(document), "Toggle character flip")) {
                        status = "Flip X toggled";
                    }
                } else if (contains(
                               {910.0F, 412.0F, 326.0F, 36.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    auto document = editor.document();
                    document.no_antialiasing = !document.no_antialiasing;
                    if (apply(std::move(document), "Toggle character antialiasing")) {
                        status = "Antialiasing toggled";
                    }
                } else if (event.button.x >= 584.0F
                           && event.button.x <= 1'236.0F
                           && event.button.y >= 470.0F
                           && event.button.y < 606.0F) {
                    const auto field = static_cast<std::size_t>(
                        (event.button.y - 470.0F) / 34.0F
                    );
                    if (field < 4U) {
                        begin_animation_field(field);
                    }
                } else if (event.button.x >= 44.0F && event.button.x <= 532.0F
                           && event.button.y >= 462.0F
                           && event.button.y < 630.0F) {
                    const auto visible = std::min<std::size_t>(
                        editor.document().animations.size(),
                        7U
                    );
                    const auto first = selected_animation >= visible
                        ? selected_animation - visible + 1U
                        : 0U;
                    const auto row = static_cast<std::size_t>(
                        (event.button.y - 462.0F) / 24.0F
                    );
                    if (row < visible
                        && first + row < editor.document().animations.size()) {
                        selected_animation = first + row;
                    }
                } else if (contains(
                               {584.0F, 608.0F, 652.0F, 36.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    save();
                }
            }
        }

        if (!running) {
            break;
        }
        const auto ticks = SDL_GetTicks();
        draw_background(renderer, ticks, purple);
        text(renderer, 28.0F, 22.0F, "PULSEFORGE  //  CHARACTER EDITOR", purple, 1.8F);
        text(
            renderer,
            28.0F,
            55.0F,
            "In-engine modal editor - no window reload   CTRL+Z/Y undo/redo",
            muted,
            1.05F
        );
        draw_character_preview(
            renderer,
            editor.document(),
            selected_animation
        );

        const SDL_FRect animation_list{28.0F, 444.0F, 520.0F, 198.0F};
        panel(renderer, animation_list, panel_border);
        text(renderer, 44.0F, 454.0F, "ANIMATIONS", cyan, 1.1F);
        const auto visible = std::min<std::size_t>(
            editor.document().animations.size(),
            7U
        );
        const auto first = selected_animation >= visible && visible != 0U
            ? selected_animation - visible + 1U
            : 0U;
        for (std::size_t row = 0U; row < visible; ++row) {
            const auto index = first + row;
            const auto y = 474.0F + static_cast<float>(row) * 24.0F;
            const bool selected = index == selected_animation;
            if (selected) {
                fill(renderer, {40.0F, y - 7.0F, 496.0F, 22.0F}, {66, 49, 109, 245});
            }
            const auto& animation = editor.document().animations[index];
            text(
                renderer,
                48.0F,
                y,
                std::string(selected ? "> " : "  ") + animation.id + "  // "
                    + animation.name + "  @" + std::to_string(animation.fps),
                selected ? yellow : white,
                0.96F,
                62U
            );
        }

        const SDL_FRect fields{570.0F, 104.0F, 682.0F, 548.0F};
        panel(renderer, fields, cyan);
        text(renderer, 586.0F, 119.0F, "CHARACTER DATA", cyan, 1.25F);
        const std::array root_labels{
            std::string{"Image: "} + editor.document().image,
            std::string{"Scale: "} + format_number(editor.document().scale, 3),
            std::string{"Sing duration: "} + format_number(editor.document().sing_duration, 3),
            std::string{"Health icon: "} + editor.document().health_icon,
            std::string{"Position: "} + format_number(editor.document().position.x)
                + "," + format_number(editor.document().position.y),
            std::string{"Camera: "} + format_number(editor.document().camera_position.x)
                + "," + format_number(editor.document().camera_position.y),
            std::string{"Health RGB: "} + rgb_text(editor.document().healthbar_color),
            std::string{"Vocals: "} + editor.document().vocals_file,
        };
        float field_y = 140.0F;
        for (const auto& label : root_labels) {
            const SDL_FRect row{584.0F, field_y - 8.0F, 652.0F, 29.0F};
            const bool hovered = contains(row, mouse_x, mouse_y);
            fill(renderer, row, hovered ? SDL_Color{54, 46, 91, 245}
                                        : SDL_Color{35, 31, 66, 230});
            text(renderer, 594.0F, field_y, label, hovered ? yellow : white, 1.0F, 80U);
            field_y += 34.0F;
        }
        static_cast<void>(button(
            renderer,
            {584.0F, 412.0F, 310.0F, 36.0F},
            std::string{"FLIP X: "} + (editor.document().flip_x ? "ON" : "OFF"),
            contains({584.0F, 412.0F, 310.0F, 36.0F}, mouse_x, mouse_y),
            true,
            purple
        ));
        static_cast<void>(button(
            renderer,
            {910.0F, 412.0F, 326.0F, 36.0F},
            std::string{"ANTIALIASING: "}
                + (editor.document().no_antialiasing ? "OFF" : "ON"),
            contains({910.0F, 412.0F, 326.0F, 36.0F}, mouse_x, mouse_y),
            true,
            purple
        ));
        text(renderer, 586.0F, 456.0F, "SELECTED ANIMATION", purple, 1.1F);
        if (!editor.document().animations.empty()) {
            const auto& animation = editor.document().animations[selected_animation];
            const std::array animation_labels{
                std::string{"Id: "} + animation.id,
                std::string{"Atlas prefix: "} + animation.name,
                std::string{"FPS: "} + std::to_string(animation.fps)
                    + (animation.loop ? "  [loop]" : ""),
                std::string{"Offsets: "} + format_number(animation.offsets.x)
                    + "," + format_number(animation.offsets.y),
            };
            float animation_y = 478.0F;
            for (const auto& label : animation_labels) {
                const SDL_FRect row{584.0F, animation_y - 8.0F, 652.0F, 29.0F};
                const bool hovered = contains(row, mouse_x, mouse_y);
                fill(renderer, row, hovered ? SDL_Color{54, 46, 91, 245}
                                            : SDL_Color{35, 31, 66, 230});
                text(renderer, 594.0F, animation_y, label, hovered ? yellow : white, 1.0F, 80U);
                animation_y += 34.0F;
            }
        } else {
            text(renderer, 594.0F, 486.0F, "No animations - press A to add one", muted, 1.0F);
        }
        static_cast<void>(button(
            renderer,
            {584.0F, 608.0F, 652.0F, 36.0F},
            "CTRL+S  SAVE PSYCH CHARACTER JSON",
            contains({584.0F, 608.0F, 652.0F, 36.0F}, mouse_x, mouse_y),
            options.storage != nullptr && !options.psych_json_path.empty(),
            success
        ));
        fill(renderer, {0.0F, 676.0F, canvas_width, 44.0F}, {18, 14, 37, 250});
        text(
            renderer,
            28.0F,
            692.0F,
            status,
            status_error ? danger : success,
            1.1F,
            145U
        );
        inline_editor.draw(renderer, ticks);
        static_cast<void>(SDL_RenderPresent(renderer));
        SDL_Delay(1U);
    }

    inline_editor.cancel(window);
    outcome.message = status;
    return outcome;
}

EditorUiOutcome run_week_editor_ui(
    SDL_Window* window,
    SDL_Renderer* renderer,
    WeekEditor& editor,
    const DescriptorEditorUiOptions& options
) {
    EditorUiOutcome outcome;
    if (window == nullptr || renderer == nullptr) {
        outcome.exit = EditorUiExit::invalid_context;
        outcome.message = "Week Editor requires an existing SDL context";
        return outcome;
    }

    EditorSessionGuard guard(window, renderer, "PulseForge // Week Editor");
    InlineTextEditor inline_editor;
    std::size_t selected_song = 0U;
    float mouse_x = 0.0F;
    float mouse_y = 0.0F;
    bool running = true;
    std::uint64_t escape_armed_until = 0U;
    std::string status =
        "Click fields/songs to edit   A add song   DELETE remove   CTRL+S save";
    bool status_error = false;

    const auto clamp_selection = [&]() {
        if (editor.document().songs.empty()) {
            selected_song = 0U;
        } else {
            selected_song = std::min(
                selected_song,
                editor.document().songs.size() - 1U
            );
        }
    };

    const auto apply = [&](WeekDescriptor document, std::string label) {
        std::string error;
        if (!editor.replace(std::move(document), std::move(label), &error)) {
            status = error;
            status_error = true;
            return false;
        }
        clamp_selection();
        status_error = false;
        return true;
    };

    const auto save = [&]() {
        if (options.storage == nullptr || options.psych_json_path.empty()) {
            status = "Week save path/storage is not configured";
            status_error = true;
            return;
        }
        const auto result = editor.save_psych_json(
            *options.storage,
            options.psych_json_path
        );
        if (!result) {
            status = "Week save failed: " + result.message;
            status_error = true;
            return;
        }
        outcome.compatible_json_saved = true;
        status = "Week JSON saved atomically";
        status_error = false;
    };

    const auto begin_root_field = [&](const std::size_t field) {
        auto current = editor.document();
        std::string label;
        std::string initial;
        switch (field) {
        case 0U:
            label = "Week display name";
            initial = current.display_name;
            break;
        case 1U:
            label = "Story heading";
            initial = current.story_name;
            break;
        case 2U:
            label = "Week background id";
            initial = current.background;
            break;
        case 3U:
            label = "Previous week id";
            initial = current.previous_week;
            break;
        case 4U:
            label = "Menu characters: opponent,player,girlfriend";
            initial = current.characters[0] + "," + current.characters[1]
                + "," + current.characters[2];
            break;
        case 5U: {
            label = "Difficulties (comma-separated)";
            for (std::size_t index = 0U; index < current.difficulties.size(); ++index) {
                if (index != 0U) {
                    initial += ", ";
                }
                initial += current.difficulties[index];
            }
            break;
        }
        default:
            return;
        }
        inline_editor.begin(
            window,
            std::move(label),
            std::move(initial),
            32U * 1'024U,
            [&, field, current](const std::string_view value, std::string& error) mutable {
                switch (field) {
                case 0U:
                    current.display_name = std::string(value);
                    break;
                case 1U:
                    current.story_name = std::string(value);
                    break;
                case 2U:
                    current.background = std::string(value);
                    break;
                case 3U:
                    current.previous_week = std::string(value);
                    break;
                case 4U: {
                    const auto values = split(value, ',', false);
                    if (values.size() != 3U) {
                        error = "Provide exactly three character ids";
                        return false;
                    }
                    current.characters = {values[0], values[1], values[2]};
                    break;
                }
                case 5U:
                    current.difficulties = split(value, ',', false);
                    break;
                default:
                    return false;
                }
                if (!apply(current, "Edit week field")) {
                    error = status;
                    return false;
                }
                status = "Week field updated";
                return true;
            }
        );
    };

    const auto begin_song_edit = [&]() {
        auto current = editor.document();
        if (current.songs.empty()) {
            status = "Add a song first with A";
            status_error = true;
            return;
        }
        const auto index = std::min(selected_song, current.songs.size() - 1U);
        const auto& song = current.songs[index];
        inline_editor.begin(
            window,
            "Song: name | menu character | r,g,b",
            song.name + "|" + song.character + "|" + rgb_text(song.color),
            32U * 1'024U,
            [&, current, index](const std::string_view value, std::string& error) mutable {
                const auto parts = split(value, '|', true);
                if (parts.size() != 3U || parts[0].empty() || parts[1].empty()) {
                    error = "Use name|character|r,g,b";
                    return false;
                }
                auto& target = current.songs[index];
                target.name = parts[0];
                target.character = parts[1];
                if (!parse_rgb(parts[2], target.color)) {
                    error = "RGB components must be between 0 and 255";
                    return false;
                }
                if (!apply(current, "Edit week song")) {
                    error = status;
                    return false;
                }
                status = "Week song updated";
                return true;
            }
        );
    };

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            static_cast<void>(SDL_ConvertEventToRenderCoordinates(renderer, &event));
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                outcome.exit = EditorUiExit::quit_requested;
                running = false;
                break;
            }
            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                mouse_x = event.motion.x;
                mouse_y = event.motion.y;
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                mouse_x = event.button.x;
                mouse_y = event.button.y;
            }
            if (inline_editor.active()) {
                static_cast<void>(inline_editor.handle(window, event));
                continue;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                const bool control = (event.key.mod & SDL_KMOD_CTRL) != 0;
                if (control && event.key.scancode == SDL_SCANCODE_S) {
                    save();
                    continue;
                }
                if (control && event.key.scancode == SDL_SCANCODE_Z) {
                    std::string label;
                    if (editor.undo(&label)) {
                        clamp_selection();
                        status = "Undo: " + label;
                        status_error = false;
                    }
                    continue;
                }
                if (control && event.key.scancode == SDL_SCANCODE_Y) {
                    std::string label;
                    if (editor.redo(&label)) {
                        clamp_selection();
                        status = "Redo: " + label;
                        status_error = false;
                    }
                    continue;
                }
                if (control && event.key.scancode == SDL_SCANCODE_UP
                    && selected_song > 0U
                    && selected_song < editor.document().songs.size()) {
                    auto document = editor.document();
                    std::swap(
                        document.songs[selected_song],
                        document.songs[selected_song - 1U]
                    );
                    --selected_song;
                    if (apply(std::move(document), "Move week song up")) {
                        status = "Song moved up";
                    }
                    continue;
                }
                if (control && event.key.scancode == SDL_SCANCODE_DOWN
                    && selected_song + 1U < editor.document().songs.size()) {
                    auto document = editor.document();
                    std::swap(
                        document.songs[selected_song],
                        document.songs[selected_song + 1U]
                    );
                    ++selected_song;
                    if (apply(std::move(document), "Move week song down")) {
                        status = "Song moved down";
                    }
                    continue;
                }
                switch (event.key.scancode) {
                case SDL_SCANCODE_ESCAPE: {
                    const auto now = SDL_GetTicks();
                    if (editor.dirty() && now > escape_armed_until) {
                        escape_armed_until = now + 2'000U;
                        status = "Unsaved week - press ESC again to leave";
                        status_error = true;
                    } else {
                        running = false;
                    }
                    break;
                }
                case SDL_SCANCODE_UP:
                    if (!editor.document().songs.empty()) {
                        selected_song = selected_song == 0U
                            ? editor.document().songs.size() - 1U
                            : selected_song - 1U;
                    }
                    break;
                case SDL_SCANCODE_DOWN:
                    if (!editor.document().songs.empty()) {
                        selected_song = (selected_song + 1U)
                            % editor.document().songs.size();
                    }
                    break;
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER:
                    begin_song_edit();
                    break;
                case SDL_SCANCODE_A: {
                    auto document = editor.document();
                    WeekSongDescriptor song;
                    song.name = "new-song-"
                        + std::to_string(document.songs.size() + 1U);
                    song.character = "dad";
                    document.songs.push_back(std::move(song));
                    selected_song = document.songs.size() - 1U;
                    if (apply(std::move(document), "Add week song")) {
                        status = "Song added";
                    }
                    break;
                }
                case SDL_SCANCODE_DELETE:
                    if (!editor.document().songs.empty()) {
                        auto document = editor.document();
                        document.songs.erase(
                            document.songs.begin()
                            + static_cast<std::ptrdiff_t>(selected_song)
                        );
                        if (apply(std::move(document), "Remove week song")) {
                            status = "Song removed";
                        }
                    }
                    break;
                default:
                    break;
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                       && event.button.button == SDL_BUTTON_LEFT) {
                if (event.button.x >= 570.0F && event.button.x <= 1'236.0F
                    && event.button.y >= 144.0F
                    && event.button.y < 360.0F) {
                    const auto field = static_cast<std::size_t>(
                        (event.button.y - 144.0F) / 36.0F
                    );
                    if (field < 6U) {
                        begin_root_field(field);
                    }
                } else if (contains(
                               {570.0F, 384.0F, 320.0F, 36.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    auto document = editor.document();
                    document.start_unlocked = !document.start_unlocked;
                    if (apply(std::move(document), "Toggle week unlocked")) {
                        status = "Start-unlocked toggled";
                    }
                } else if (contains(
                               {906.0F, 384.0F, 330.0F, 36.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    auto document = editor.document();
                    document.hidden_until_unlocked = !document.hidden_until_unlocked;
                    if (apply(std::move(document), "Toggle hidden-until-unlocked")) {
                        status = "Hidden-until-unlocked toggled";
                    }
                } else if (contains(
                               {570.0F, 430.0F, 320.0F, 36.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    auto document = editor.document();
                    document.hide_story = !document.hide_story;
                    if (apply(std::move(document), "Toggle Story visibility")) {
                        status = "Story visibility toggled";
                    }
                } else if (contains(
                               {906.0F, 430.0F, 330.0F, 36.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    auto document = editor.document();
                    document.hide_freeplay = !document.hide_freeplay;
                    if (apply(std::move(document), "Toggle Freeplay visibility")) {
                        status = "Freeplay visibility toggled";
                    }
                } else if (event.button.x >= 44.0F && event.button.x <= 534.0F
                           && event.button.y >= 152.0F
                           && event.button.y < 582.0F) {
                    constexpr std::size_t visible_rows = 15U;
                    const auto first = selected_song >= visible_rows
                        ? selected_song - visible_rows + 1U
                        : 0U;
                    const auto row = static_cast<std::size_t>(
                        (event.button.y - 152.0F) / 28.0F
                    );
                    if (row < visible_rows
                        && first + row < editor.document().songs.size()) {
                        selected_song = first + row;
                        begin_song_edit();
                    }
                } else if (contains(
                               {570.0F, 586.0F, 666.0F, 42.0F},
                               event.button.x,
                               event.button.y
                           )) {
                    save();
                }
            }
        }

        if (!running) {
            break;
        }
        const auto ticks = SDL_GetTicks();
        draw_background(renderer, ticks, yellow);
        text(renderer, 28.0F, 22.0F, "PULSEFORGE  //  WEEK EDITOR", yellow, 1.8F);
        text(
            renderer,
            28.0F,
            55.0F,
            "ENTER/click edit song   CTRL+UP/DOWN reorder   CTRL+Z/Y undo/redo",
            muted,
            1.05F
        );

        const SDL_FRect song_panel{28.0F, 104.0F, 520.0F, 548.0F};
        panel(renderer, song_panel, purple);
        text(
            renderer,
            44.0F,
            122.0F,
            "PLAYLIST  //  " + editor.document().id,
            purple,
            1.25F
        );
        constexpr std::size_t visible_rows = 15U;
        const auto first = selected_song >= visible_rows
            ? selected_song - visible_rows + 1U
            : 0U;
        const auto last = std::min(
            first + visible_rows,
            editor.document().songs.size()
        );
        float song_y = 160.0F;
        for (std::size_t index = first; index < last; ++index) {
            const auto& song = editor.document().songs[index];
            const bool selected = index == selected_song;
            if (selected) {
                fill(renderer, {40.0F, song_y - 10.0F, 496.0F, 27.0F}, {68, 51, 108, 245});
                fill(
                    renderer,
                    {40.0F, song_y - 10.0F, 4.0F, 27.0F},
                    {static_cast<std::uint8_t>(std::clamp(song.color.red, 0, 255)),
                     static_cast<std::uint8_t>(std::clamp(song.color.green, 0, 255)),
                     static_cast<std::uint8_t>(std::clamp(song.color.blue, 0, 255)),
                     255}
                );
            }
            text(
                renderer,
                52.0F,
                song_y,
                std::string(selected ? "> " : "  ") + song.name + "  // "
                    + song.character + "  [" + rgb_text(song.color) + "]",
                selected ? yellow : white,
                1.02F,
                61U
            );
            song_y += 28.0F;
        }
        if (editor.document().songs.empty()) {
            text(renderer, 52.0F, 174.0F, "No songs - press A to add one", muted, 1.05F);
        }
        text(
            renderer,
            44.0F,
            622.0F,
            "A add   DELETE remove   " + std::to_string(editor.document().songs.size())
                + " song(s)",
            muted,
            1.0F
        );

        const SDL_FRect fields{558.0F, 104.0F, 694.0F, 548.0F};
        panel(renderer, fields, cyan);
        text(renderer, 574.0F, 122.0F, "WEEK DATA", cyan, 1.25F);
        std::string characters = editor.document().characters[0] + ","
            + editor.document().characters[1] + ","
            + editor.document().characters[2];
        std::string difficulties;
        for (std::size_t index = 0U;
             index < editor.document().difficulties.size();
             ++index) {
            if (index != 0U) {
                difficulties += ", ";
            }
            difficulties += editor.document().difficulties[index];
        }
        const std::array labels{
            std::string{"Display name: "} + editor.document().display_name,
            std::string{"Story heading: "} + editor.document().story_name,
            std::string{"Background: "} + editor.document().background,
            std::string{"Previous week: "} + editor.document().previous_week,
            std::string{"Menu characters: "} + characters,
            std::string{"Difficulties: "} + difficulties,
        };
        float field_y = 152.0F;
        for (const auto& label : labels) {
            const SDL_FRect row{570.0F, field_y - 8.0F, 666.0F, 31.0F};
            const bool hovered = contains(row, mouse_x, mouse_y);
            fill(renderer, row, hovered ? SDL_Color{55, 47, 92, 245}
                                        : SDL_Color{35, 31, 66, 230});
            text(renderer, 580.0F, field_y, label, hovered ? yellow : white, 1.02F, 82U);
            field_y += 36.0F;
        }
        static_cast<void>(button(
            renderer,
            {570.0F, 384.0F, 320.0F, 36.0F},
            std::string{"START UNLOCKED: "}
                + (editor.document().start_unlocked ? "YES" : "NO"),
            contains({570.0F, 384.0F, 320.0F, 36.0F}, mouse_x, mouse_y),
            true,
            purple
        ));
        static_cast<void>(button(
            renderer,
            {906.0F, 384.0F, 330.0F, 36.0F},
            std::string{"HIDDEN UNTIL UNLOCKED: "}
                + (editor.document().hidden_until_unlocked ? "YES" : "NO"),
            contains({906.0F, 384.0F, 330.0F, 36.0F}, mouse_x, mouse_y),
            true,
            purple
        ));
        static_cast<void>(button(
            renderer,
            {570.0F, 430.0F, 320.0F, 36.0F},
            std::string{"STORY: "}
                + (editor.document().hide_story ? "HIDDEN" : "VISIBLE"),
            contains({570.0F, 430.0F, 320.0F, 36.0F}, mouse_x, mouse_y),
            true,
            purple
        ));
        static_cast<void>(button(
            renderer,
            {906.0F, 430.0F, 330.0F, 36.0F},
            std::string{"FREEPLAY: "}
                + (editor.document().hide_freeplay ? "HIDDEN" : "VISIBLE"),
            contains({906.0F, 430.0F, 330.0F, 36.0F}, mouse_x, mouse_y),
            true,
            purple
        ));

        if (!editor.document().songs.empty()) {
            const auto& selected = editor.document().songs[selected_song];
            fill(renderer, {570.0F, 486.0F, 666.0F, 78.0F}, {31, 27, 60, 240});
            outline(renderer, {570.0F, 486.0F, 666.0F, 78.0F}, panel_border);
            text(renderer, 584.0F, 502.0F, "SELECTED SONG", purple, 1.0F);
            text(
                renderer,
                584.0F,
                528.0F,
                selected.name + "  // character " + selected.character
                    + "  // RGB " + rgb_text(selected.color),
                yellow,
                1.08F,
                78U
            );
            text(renderer, 584.0F, 549.0F, "Click playlist row or press ENTER to edit", muted, 0.96F);
        }
        static_cast<void>(button(
            renderer,
            {570.0F, 586.0F, 666.0F, 42.0F},
            "CTRL+S  SAVE PSYCH WEEK JSON",
            contains({570.0F, 586.0F, 666.0F, 42.0F}, mouse_x, mouse_y),
            options.storage != nullptr && !options.psych_json_path.empty(),
            success
        ));

        fill(renderer, {0.0F, 676.0F, canvas_width, 44.0F}, {18, 14, 37, 250});
        text(
            renderer,
            28.0F,
            692.0F,
            status,
            status_error ? danger : success,
            1.1F,
            145U
        );
        inline_editor.draw(renderer, ticks);
        static_cast<void>(SDL_RenderPresent(renderer));
        SDL_Delay(1U);
    }

    inline_editor.cancel(window);
    outcome.message = status;
    return outcome;
}

}  // namespace pulseforge
