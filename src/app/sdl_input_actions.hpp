#pragma once

#include "pulseforge/input_bindings.hpp"

#include <SDL3/SDL.h>

#include <string>
#include <string_view>

namespace pulseforge::detail {

[[nodiscard]] inline bool keyboard_binding_matches(
    const std::string_view stored_name,
    const SDL_KeyboardEvent& event
) {
    auto name = canonicalize_input_name(stored_name);
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool gui = false;
    bool consumed = true;
    while (consumed) {
        consumed = false;
        const auto consume = [&](const std::string_view prefix, bool& flag) {
            if (name.starts_with(prefix)) {
                flag = true;
                name.erase(0U, prefix.size());
                consumed = true;
            }
        };
        consume("ctrl+", ctrl);
        consume("alt+", alt);
        consume("shift+", shift);
        consume("gui+", gui);
    }

    const auto modifiers = event.mod;
    const bool event_ctrl = (modifiers & SDL_KMOD_CTRL) != 0U;
    const bool event_alt = (modifiers & SDL_KMOD_ALT) != 0U;
    const bool event_shift = (modifiers & SDL_KMOD_SHIFT) != 0U;
    const bool event_gui = (modifiers & SDL_KMOD_GUI) != 0U;
    if (ctrl != event_ctrl || alt != event_alt || gui != event_gui
        || (shift && !event_shift)) {
        return false;
    }
    // An unmodified binding is allowed with Shift. This makes the physical
    // =/+ key behave as the familiar '+' volume shortcut on common layouts.
    if (!shift && event_shift && name != "equals") {
        return false;
    }

    if (name == "equals" || name == "=") {
        return event.scancode == SDL_SCANCODE_EQUALS;
    }
    if (name == "minus" || name == "-") {
        return event.scancode == SDL_SCANCODE_MINUS;
    }
    const auto configured = SDL_GetScancodeFromName(name.c_str());
    if (configured != SDL_SCANCODE_UNKNOWN) {
        return configured == event.scancode;
    }
    const char* raw = SDL_GetScancodeName(event.scancode);
    return raw != nullptr && canonicalize_input_name(raw) == name;
}

[[nodiscard]] inline bool keyboard_action_matches(
    const InputBindings& bindings,
    const std::string_view action,
    const SDL_KeyboardEvent& event
) {
    const auto* binding = find_action_binding(bindings, action);
    if (binding == nullptr) {
        return false;
    }
    for (const auto& input : binding->inputs) {
        if (input.device == InputDevice::keyboard
            && keyboard_binding_matches(input.name, event)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool gamepad_action_matches(
    const InputBindings& bindings,
    const std::string_view action,
    const SDL_GamepadButtonEvent& event
) {
    const auto* binding = find_action_binding(bindings, action);
    if (binding == nullptr) {
        return false;
    }
    const auto button = static_cast<SDL_GamepadButton>(event.button);
    const char* raw = SDL_GetGamepadStringForButton(button);
    const auto name = canonicalize_input_name(raw == nullptr ? "" : raw);
    for (const auto& input : binding->inputs) {
        if (input.device == InputDevice::gamepad
            && canonicalize_input_name(input.name) == name) {
            return true;
        }
    }
    return false;
}

}  // namespace pulseforge::detail
