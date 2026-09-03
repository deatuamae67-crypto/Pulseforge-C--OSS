#include "controls_ui.hpp"
#include "mobile_touch_controls.hpp"

#include "pulseforge/input_bindings.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge::detail {
namespace {

constexpr float logical_width = 1280.0F;
constexpr float logical_height = 720.0F;
constexpr std::size_t visible_rows = 13U;

struct ControlsPalette {
    SDL_Color top;
    SDL_Color bottom;
    SDL_Color header;
    SDL_Color panel;
    SDL_Color accent;
    SDL_Color accent_secondary;
    SDL_Color selected;
    SDL_Color title;
    SDL_Color text;
    SDL_Color muted;
};

[[nodiscard]] constexpr ControlsPalette palette_for(
    const PresentationTheme theme
) noexcept {
    if (theme == PresentationTheme::watch_dogs) {
        return {
            {2, 5, 7, 255},
            {12, 15, 17, 255},
            {4, 8, 10, 255},
            {6, 12, 14, 238},
            {47, 233, 241, 255},
            {214, 246, 247, 255},
            {17, 39, 42, 245},
            {225, 254, 255, 255},
            {214, 224, 224, 255},
            {120, 159, 162, 255},
        };
    }
    return {
        {7, 7, 24, 255},
        {23, 13, 51, 255},
        {31, 16, 67, 255},
        {15, 10, 35, 238},
        {96, 231, 239, 255},
        {132, 91, 239, 255},
        {56, 35, 119, 245},
        {123, 244, 255, 255},
        {224, 216, 239, 255},
        {190, 182, 224, 255},
    };
}

[[nodiscard]] std::string printable(
    const std::string_view text,
    const std::size_t limit
) {
    std::string result;
    result.reserve(std::min(text.size(), limit));
    for (const unsigned char value : text) {
        if (result.size() >= limit) {
            break;
        }
        result.push_back(value >= 32U && value <= 126U
            ? static_cast<char>(value)
            : '?');
    }
    if (text.size() > limit && limit >= 3U) {
        result.resize(limit - 3U);
        result += "...";
    }
    return result;
}

void fill(
    SDL_Renderer* renderer,
    const SDL_FRect rectangle,
    const SDL_Color color
) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &rectangle);
}

void outline(
    SDL_Renderer* renderer,
    const SDL_FRect rectangle,
    const SDL_Color color
) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    static_cast<void>(SDL_RenderRect(renderer, &rectangle));
}

void text(
    SDL_Renderer* renderer,
    const float x,
    const float y,
    const std::string_view value,
    const SDL_Color color,
    const float scale = 1.35F
) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    float old_x = 1.0F;
    float old_y = 1.0F;
    SDL_GetRenderScale(renderer, &old_x, &old_y);
    SDL_SetRenderScale(renderer, scale, scale);
    const auto safe = printable(value, 150U);
    SDL_RenderDebugText(renderer, x / scale, y / scale, safe.c_str());
    SDL_SetRenderScale(renderer, old_x, old_y);
}

void background(
    SDL_Renderer* renderer,
    const std::uint64_t ticks,
    const PresentationTheme theme
) {
    const auto palette = palette_for(theme);
    constexpr int band_count = 36;
    for (int index = 0; index < band_count; ++index) {
        const float ratio = static_cast<float>(index)
            / static_cast<float>(band_count - 1);
        const auto mix = [ratio](
            const std::uint8_t first,
            const std::uint8_t last
        ) {
            return static_cast<std::uint8_t>(
                static_cast<float>(first)
                + (static_cast<float>(last) - static_cast<float>(first)) * ratio
            );
        };
        fill(
            renderer,
            {0.0F, ratio * logical_height, logical_width,
             logical_height / static_cast<float>(band_count) + 1.0F},
            {
                mix(palette.top.r, palette.bottom.r),
                mix(palette.top.g, palette.bottom.g),
                mix(palette.top.b, palette.bottom.b),
                255,
            }
        );
    }
    fill(renderer, {0.0F, 0.0F, logical_width, 108.0F}, palette.header);
    fill(renderer, {0.0F, 106.0F, logical_width, 2.0F}, palette.accent);

    if (theme == PresentationTheme::watch_dogs) {
        for (int index = 0; index < 56; ++index) {
            const float y = 116.0F + static_cast<float>(index) * 10.4F;
            fill(renderer, {0.0F, y, logical_width, 1.0F}, {38, 56, 58, 35});
        }
        for (int index = 0; index < 11; ++index) {
            const auto seed = static_cast<std::uint64_t>(index) * 9'973ULL;
            const float x = static_cast<float>((seed + ticks / 7ULL) % 1'390ULL)
                - 110.0F;
            const float y = 126.0F + static_cast<float>((seed * 19ULL) % 545ULL);
            const float width = 28.0F + static_cast<float>(index % 4) * 31.0F;
            fill(
                renderer,
                {x, y, width, 2.0F},
                {palette.accent.r, palette.accent.g, palette.accent.b, 100}
            );
        }

        fill(renderer, {18.0F, 132.0F, 108.0F, 532.0F}, {2, 8, 9, 230});
        outline(renderer, {18.0F, 132.0F, 108.0F, 532.0F}, palette.accent);
        text(renderer, 31.0F, 151.0F, "ctOS", palette.title, 1.35F);
        text(renderer, 31.0F, 181.0F, "INPUT", palette.accent, 0.90F);
        text(renderer, 31.0F, 203.0F, "MATRIX", palette.muted, 0.80F);
        for (int index = 0; index < 19; ++index) {
            const float y = 246.0F + static_cast<float>(index) * 20.0F;
            const auto phase = static_cast<std::uint64_t>(index) * 37ULL
                + ticks / 23ULL;
            const float width = 12.0F + static_cast<float>(phase % 70ULL);
            fill(
                renderer,
                {31.0F, y, width, index % 4 == 0 ? 3.0F : 1.0F},
                {
                    palette.accent.r,
                    palette.accent.g,
                    palette.accent.b,
                    static_cast<std::uint8_t>(index % 4 == 0 ? 190 : 90),
                }
            );
        }
        text(renderer, 31.0F, 626.0F, "LINKED", palette.muted, 0.85F);
        fill(renderer, {1'154.0F, 132.0F, 106.0F, 532.0F}, {2, 8, 9, 205});
        outline(renderer, {1'154.0F, 132.0F, 106.0F, 532.0F}, palette.accent);
        text(renderer, 1'171.0F, 154.0F, "CFG", palette.title, 1.05F);
        text(renderer, 1'171.0F, 181.0F, "LIVE", palette.accent, 0.85F);
        for (int index = 0; index < 15; ++index) {
            const float y = 232.0F + static_cast<float>(index) * 25.0F;
            const auto phase = static_cast<std::uint64_t>(index) * 17ULL
                + ticks / 31ULL;
            const float width = 14.0F + static_cast<float>(phase % 60ULL);
            fill(
                renderer,
                {1'171.0F, y, width, 2.0F},
                {palette.accent.r, palette.accent.g, palette.accent.b, 110}
            );
        }
        text(renderer, 1'052.0F, 39.0F, "ctOS // ONLINE", palette.accent, 1.15F);
        return;
    }

    for (int index = 0; index < 18; ++index) {
        const auto seed = static_cast<std::uint64_t>(index) * 7'919ULL;
        const float x = static_cast<float>((seed + ticks / 19ULL) % 1'340ULL)
            - 30.0F;
        const float y = 118.0F + static_cast<float>((seed * 13ULL) % 560ULL);
        const float size = 2.0F + static_cast<float>(index % 3);
        fill(renderer, {x, y, size, size}, {130, 108, 230, 70});
    }
    const float drift = static_cast<float>(
        std::sin(static_cast<double>(ticks) * 0.0008)
    );
    fill(
        renderer,
        {875.0F + drift * 95.0F, -55.0F, 435.0F, 190.0F},
        {81, 221, 229, 25}
    );
    fill(renderer, {20.0F, 132.0F, 106.0F, 532.0F}, {24, 13, 53, 225});
    outline(renderer, {20.0F, 132.0F, 106.0F, 532.0F}, palette.accent_secondary);
    fill(renderer, {31.0F, 151.0F, 84.0F, 5.0F}, palette.accent);
    text(renderer, 31.0F, 177.0F, "PF//CORE", palette.title, 1.0F);
    text(renderer, 31.0F, 207.0F, "INPUT", palette.muted, 0.82F);
    for (int index = 0; index < 12; ++index) {
        const float y = 264.0F + static_cast<float>(index) * 29.0F;
        const float wave = 0.5F + 0.5F * static_cast<float>(std::sin(
            static_cast<double>(ticks) * 0.003
            + static_cast<double>(index) * 0.72
        ));
        fill(
            renderer,
            {31.0F, y, 22.0F + 51.0F * wave, 3.0F},
            {palette.accent_secondary.r, palette.accent_secondary.g,
             palette.accent_secondary.b, 170}
        );
    }
    fill(renderer, {1'154.0F, 132.0F, 106.0F, 532.0F}, {18, 10, 43, 205});
    outline(renderer, {1'154.0F, 132.0F, 106.0F, 532.0F}, palette.accent_secondary);
    text(renderer, 1'171.0F, 157.0F, "FORGE", palette.title, 0.95F);
    for (int index = 0; index < 8; ++index) {
        const float inset = static_cast<float>(index) * 5.0F;
        outline(
            renderer,
            {1'174.0F + inset, 236.0F + inset, 66.0F - inset * 2.0F,
             66.0F - inset * 2.0F},
            {
                palette.accent_secondary.r,
                palette.accent_secondary.g,
                palette.accent_secondary.b,
                static_cast<std::uint8_t>(150 - index * 13),
            }
        );
    }
    text(renderer, 1'171.0F, 626.0F, "READY", palette.accent, 0.85F);
}

[[nodiscard]] std::string context_name(const InputContext context) {
    switch (context) {
    case InputContext::global: return "GLOBAL";
    case InputContext::menu: return "MENU";
    case InputContext::gameplay: return "GAMEPLAY";
    case InputContext::editor: return "EDITOR";
    }
    return "UNKNOWN";
}

[[nodiscard]] std::string input_label(const PhysicalInput& input) {
    return std::string(input.device == InputDevice::keyboard ? "KB " : "PAD ")
        + input.name;
}

[[nodiscard]] std::string action_label(
    const ActionDefinition& definition,
    const InputBindings& bindings
) {
    std::string result = "[" + context_name(definition.context) + "] ";
    result += definition.label;
    result += "  :  ";
    const auto* binding = find_action_binding(bindings, definition.id);
    if (binding == nullptr || binding->inputs.empty()) {
        result += "UNBOUND";
        return result;
    }
    for (std::size_t index = 0; index < binding->inputs.size(); ++index) {
        if (index != 0U) {
            result += " | ";
        }
        result += input_label(binding->inputs[index]);
    }
    return result;
}

[[nodiscard]] std::string themed_title(
    const std::string_view title,
    const PresentationTheme theme
) {
    if (theme != PresentationTheme::watch_dogs) {
        return std::string(title);
    }
    constexpr std::string_view product_prefix{"PULSEFORGE  //  "};
    constexpr std::string_view controls_prefix{"CONTROLS  //  "};
    if (title.starts_with(product_prefix)) {
        return std::string{"ctOS  //  "}
            + std::string(title.substr(product_prefix.size()));
    }
    if (title.starts_with(controls_prefix)) {
        return std::string{"ctOS  //  INPUT  //  "}
            + std::string(title.substr(controls_prefix.size()));
    }
    if (title == "BINDING CONFLICT") {
        return "ctOS  //  BINDING COLLISION";
    }
    if (title == "RESET ALL CONTROLS?") {
        return "ctOS  //  RESET INPUT MATRIX?";
    }
    return std::string{"ctOS  //  "} + std::string(title);
}

void draw_list(
    SDL_Window* window,
    SDL_Renderer* renderer,
    const std::string_view title,
    const std::span<const std::string> rows,
    const std::size_t selected,
    const std::string_view footer,
    const PresentationTheme theme
) {
    SDL_SetWindowTitle(
        window,
        theme == PresentationTheme::watch_dogs
            ? "PulseForge - ctOS Controls"
            : "PulseForge - Controls"
    );
    const auto ticks = SDL_GetTicks();
    const auto palette = palette_for(theme);
    background(renderer, ticks, theme);
    text(
        renderer,
        43.0F,
        29.0F,
        themed_title(title, theme),
        palette.title,
        2.05F
    );
    text(
        renderer,
        43.0F,
        70.0F,
        printable(footer, 108U),
        palette.muted,
        1.15F
    );

    fill(renderer, {144.0F, 116.0F, 992.0F, 558.0F}, palette.panel);
    outline(
        renderer,
        {144.0F, 116.0F, 992.0F, 558.0F},
        {
            palette.accent_secondary.r,
            palette.accent_secondary.g,
            palette.accent_secondary.b,
            145,
        }
    );

    const auto visible = std::min(visible_rows, rows.size());
    const auto half = visible / 2U;
    auto first = selected > half ? selected - half : 0U;
    if (first + visible > rows.size()) {
        first = rows.size() - visible;
    }
    const auto last = first + visible;
    float y = 134.0F;
    for (std::size_t index = first; index < last; ++index) {
        if (index == selected) {
            fill(renderer, {156.0F, y - 13.0F, 968.0F, 40.0F}, palette.selected);
            fill(renderer, {156.0F, y - 13.0F, 6.0F, 40.0F}, palette.accent);
            if (theme == PresentationTheme::watch_dogs) {
                fill(renderer, {1'109.0F, y - 9.0F, 8.0F, 2.0F}, palette.accent);
                fill(renderer, {1'109.0F, y + 18.0F, 8.0F, 2.0F}, palette.accent);
            }
        }
        text(
            renderer,
            174.0F,
            y,
            printable(rows[index], 91U),
            index == selected
                ? palette.title
                : palette.text,
            1.27F
        );
        y += 41.5F;
    }
    if (rows.size() > visible) {
        text(
            renderer,
            1'171.0F,
            635.0F,
            std::to_string(selected + 1U) + "/" + std::to_string(rows.size()),
            palette.accent,
            0.90F
        );
        text(renderer, 1'197.0F, 607.0F, "^", palette.muted, 0.90F);
        text(renderer, 1'197.0F, 655.0F, "v", palette.muted, 0.90F);
    }
    static_cast<void>(present_with_mobile_touch(renderer));
}

[[nodiscard]] std::optional<std::size_t> choose(
    SDL_Window* window,
    SDL_Renderer* renderer,
    const std::string_view title,
    const std::span<const std::string> rows,
    const std::string_view footer,
    const std::size_t initial,
    const PresentationTheme theme
) {
    if (rows.empty()) {
        return std::nullopt;
    }
    std::size_t selected = std::min(initial, rows.size() - 1U);
    while (true) {
        SDL_Event event;
        while (poll_mobile_event(&event)) {
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                SDL_Event quit{};
                quit.type = SDL_EVENT_QUIT;
                static_cast<void>(SDL_PushEvent(&quit));
                return std::nullopt;
            }
            if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
                continue;
            }
            switch (event.key.scancode) {
            case SDL_SCANCODE_ESCAPE:
                return std::nullopt;
            case SDL_SCANCODE_UP:
                selected = selected == 0U ? rows.size() - 1U : selected - 1U;
                break;
            case SDL_SCANCODE_DOWN:
                selected = (selected + 1U) % rows.size();
                break;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER:
                return selected;
            default:
                break;
            }
        }
        draw_list(window, renderer, title, rows, selected, footer, theme);
        SDL_Delay(1U);
    }
}

[[nodiscard]] bool is_modifier(const SDL_Scancode scancode) noexcept {
    return scancode == SDL_SCANCODE_LCTRL || scancode == SDL_SCANCODE_RCTRL
        || scancode == SDL_SCANCODE_LSHIFT || scancode == SDL_SCANCODE_RSHIFT
        || scancode == SDL_SCANCODE_LALT || scancode == SDL_SCANCODE_RALT
        || scancode == SDL_SCANCODE_LGUI || scancode == SDL_SCANCODE_RGUI;
}

[[nodiscard]] std::string keyboard_input_name(const SDL_KeyboardEvent& event) {
    const char* raw = SDL_GetScancodeName(event.scancode);
    if (raw == nullptr || raw[0] == '\0') {
        return {};
    }
    auto name = canonicalize_input_name(raw);
    std::string chord;
    if ((event.mod & SDL_KMOD_CTRL) != 0U) {
        chord += "ctrl+";
    }
    if ((event.mod & SDL_KMOD_ALT) != 0U) {
        chord += "alt+";
    }
    if ((event.mod & SDL_KMOD_SHIFT) != 0U) {
        chord += "shift+";
    }
    if ((event.mod & SDL_KMOD_GUI) != 0U) {
        chord += "gui+";
    }
    return chord + name;
}

[[nodiscard]] std::optional<PhysicalInput> capture_input(
    SDL_Window* window,
    SDL_Renderer* renderer,
    const InputDevice requested_device,
    const PresentationTheme theme
) {
    SDL_SetWindowTitle(
        window,
        theme == PresentationTheme::watch_dogs
            ? "PulseForge - ctOS Input Capture"
            : "PulseForge - Input Capture"
    );
    while (true) {
        SDL_Event event;
        while (poll_mobile_event(&event)) {
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                SDL_Event quit{};
                quit.type = SDL_EVENT_QUIT;
                static_cast<void>(SDL_PushEvent(&quit));
                return std::nullopt;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    return std::nullopt;
                }
                if (requested_device == InputDevice::keyboard
                    && !is_modifier(event.key.scancode)) {
                    auto name = keyboard_input_name(event.key);
                    if (is_valid_input_name(name)) {
                        return PhysicalInput{InputDevice::keyboard, std::move(name)};
                    }
                }
            }
            if (requested_device == InputDevice::gamepad
                && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                const auto button = static_cast<SDL_GamepadButton>(event.gbutton.button);
                const char* raw = SDL_GetGamepadStringForButton(button);
                auto name = canonicalize_input_name(raw == nullptr ? "" : raw);
                if (is_valid_input_name(name)) {
                    return PhysicalInput{InputDevice::gamepad, std::move(name)};
                }
            }
        }

        const auto ticks = SDL_GetTicks();
        const auto palette = palette_for(theme);
        background(renderer, ticks, theme);
        fill(renderer, {158.0F, 176.0F, 964.0F, 282.0F}, palette.panel);
        outline(renderer, {158.0F, 176.0F, 964.0F, 282.0F}, palette.accent);
        fill(renderer, {158.0F, 176.0F, 9.0F, 282.0F}, palette.accent_secondary);
        text(
            renderer,
            194.0F,
            207.0F,
            theme == PresentationTheme::watch_dogs
                ? "ctOS  //  AWAITING INPUT"
                : "PF//CORE  //  AWAITING INPUT",
            palette.title,
            2.05F
        );
        text(
            renderer,
            194.0F,
            257.0F,
            requested_device == InputDevice::keyboard
                ? "CHANNEL: KEYBOARD"
                : "CHANNEL: GAMEPAD",
            palette.accent,
            1.15F
        );
        text(
            renderer,
            194.0F,
            307.0F,
            requested_device == InputDevice::keyboard
                ? "Press a keyboard key or chord (Ctrl/Alt/Shift/GUI + key)."
                : "Press a connected gamepad button.",
            palette.text,
            1.35F
        );
        const float activity = 180.0F + static_cast<float>(
            (ticks / 5ULL) % 680ULL
        );
        fill(
            renderer,
            {194.0F, 356.0F, 842.0F, 3.0F},
            {
                palette.accent.r,
                palette.accent.g,
                palette.accent.b,
                75,
            }
        );
        fill(renderer, {194.0F, 356.0F, activity, 3.0F}, palette.accent);
        text(
            renderer,
            194.0F,
            399.0F,
            "ESC cancels without changing anything.",
            palette.muted
        );
        static_cast<void>(present_with_mobile_touch(renderer));
        SDL_Delay(1U);
    }
}

[[nodiscard]] std::string persist(
    EngineSettings& settings,
    const std::filesystem::path& settings_path
) {
    rebuild_legacy_lane_bindings(settings);
    if (settings_path.empty()) {
        return "Changed for this session (no settings path was configured).";
    }
    std::string error;
    if (!save_settings(settings_path, settings, &error)) {
        return "SAVE FAILED: " + error;
    }
    return "Saved atomically to " + settings_path.filename().string();
}

void edit_action(
    SDL_Window* window,
    SDL_Renderer* renderer,
    EngineSettings& settings,
    const std::filesystem::path& settings_path,
    const ActionDefinition& definition,
    std::string& status,
    const PresentationTheme theme
) {
    std::size_t selected_row = 0U;
    while (true) {
        auto* binding = find_action_binding(settings.controls, definition.id);
        std::vector<std::string> rows{
            "Add keyboard key/chord...",
            "Add gamepad button...",
        };
        const auto input_count = binding == nullptr ? 0U : binding->inputs.size();
        for (std::size_t index = 0; index < input_count; ++index) {
            rows.push_back("Remove  " + input_label(binding->inputs[index]));
        }
        rows.emplace_back("Reset this action to defaults");
        rows.emplace_back("Back to all actions");
        const auto selected = choose(
            window,
            renderer,
            std::string("CONTROLS  //  ") + std::string(definition.label),
            rows,
            status.empty()
                ? "ENTER edit   ESC back   required actions keep at least one binding"
                : status,
            selected_row,
            theme
        );
        if (!selected.has_value() || *selected == rows.size() - 1U) {
            return;
        }
        selected_row = std::min(*selected, rows.size() - 1U);
        if (*selected < 2U) {
            const auto device = *selected == 0U
                ? InputDevice::keyboard
                : InputDevice::gamepad;
            const auto captured = capture_input(window, renderer, device, theme);
            if (!captured.has_value()) {
                status = "Input capture cancelled.";
                continue;
            }
            auto result = add_input_binding(
                settings.controls,
                definition.id,
                *captured,
                BindingConflictPolicy::reject
            );
            if (result.status == BindingEditStatus::conflict
                && result.conflict.has_value()) {
                const std::array conflict_rows{
                    std::string("Replace binding on ")
                        + result.conflict->conflicting_action,
                    std::string("Keep the existing binding"),
                };
                const auto replace = choose(
                    window,
                    renderer,
                    "BINDING CONFLICT",
                    conflict_rows,
                    input_label(*captured) + " is already used in this context",
                    0U,
                    theme
                );
                if (replace.has_value() && *replace == 0U) {
                    result = add_input_binding(
                        settings.controls,
                        definition.id,
                        *captured,
                        BindingConflictPolicy::replace_existing
                    );
                }
            }
            status = result
                ? persist(settings, settings_path)
                : "NOT CHANGED: " + result.message;
            continue;
        }
        if (*selected < 2U + input_count) {
            binding = find_action_binding(settings.controls, definition.id);
            if (binding == nullptr) {
                status = "NOT CHANGED: action has no bindings.";
                continue;
            }
            const auto input = binding->inputs[*selected - 2U];
            const auto result = remove_input_binding(
                settings.controls,
                definition.id,
                input
            );
            status = result
                ? persist(settings, settings_path)
                : "NOT CHANGED: " + result.message;
            continue;
        }
        const auto result = reset_input_action(settings.controls, definition.id);
        status = result
            ? persist(settings, settings_path)
            : "NOT CHANGED: " + result.message;
    }
}

}  // namespace

void show_controls_editor(
    SDL_Window* window,
    SDL_Renderer* renderer,
    EngineSettings& settings,
    const std::filesystem::path& settings_path
) {
    if (window == nullptr || renderer == nullptr) {
        return;
    }
    ScopedMobileTouchContext touch_context{MobileTouchContext::editor};
    std::size_t selected_row = 0U;
    std::string status;
    while (true) {
        const auto theme = settings.visual.theme;
        std::vector<std::string> rows;
        const auto definitions = input_action_definitions();
        rows.reserve(definitions.size() + 2U);
        for (const auto& definition : definitions) {
            rows.push_back(action_label(definition, settings.controls));
        }
        rows.emplace_back("Reset every action to defaults");
        rows.emplace_back("Back to options");
        const auto selected = choose(
            window,
            renderer,
            "PULSEFORGE  //  CONTROLS",
            rows,
            status.empty()
                ? "UP/DOWN select   ENTER edit   keyboard + gamepad supported"
                : status,
            selected_row,
            theme
        );
        if (!selected.has_value() || *selected == rows.size() - 1U) {
            return;
        }
        selected_row = *selected;
        if (*selected == definitions.size()) {
            const std::array confirmation{
                std::string("Reset all controls"),
                std::string("Cancel"),
            };
            const auto confirmed = choose(
                window,
                renderer,
                "RESET ALL CONTROLS?",
                confirmation,
                "This replaces every custom keyboard and gamepad binding",
                0U,
                theme
            );
            if (confirmed.has_value() && *confirmed == 0U) {
                reset_all_input_bindings(settings.controls);
                status = persist(settings, settings_path);
            }
            continue;
        }
        edit_action(
            window,
            renderer,
            settings,
            settings_path,
            definitions[*selected],
            status,
            theme
        );
    }
}

}  // namespace pulseforge::detail
