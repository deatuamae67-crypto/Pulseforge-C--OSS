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

[[nodiscard]] inline bool default_volume_up_layout_fallback(
    const ActionBinding& binding,
    const SDL_KeyboardEvent& event
) {
    // The historical default was Shift+=, which assumes a US-like layout.
    // On layouts such as Portuguese, '+" can be a different physical key.
    // Keep remapping authoritative: this compatibility path only activates
    // while the action still contains that historical default binding.
    bool historical_default = false;
    for (const auto& input : binding.inputs) {
        if (input.device == InputDevice::keyboard
            && canonicalize_input_name(input.name) == "shift+equals") {
            historical_default = true;
            break;
        }
    }
    if (!historical_default) {
        return false;
    }

    const auto modifiers = event.mod;
    if ((modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0U) {
        return false;
    }
    if (event.scancode == SDL_SCANCODE_EQUALS
        || event.scancode == SDL_SCANCODE_KP_PLUS) {
        return true;
    }

    // SDL key names are layout-aware, unlike scancodes. This catches the
    // dedicated '+' position used by several non-US keyboard layouts.
    const char* logical = SDL_GetKeyName(event.key);
    if (logical == nullptr) {
        return false;
    }
    const auto logical_name = canonicalize_input_name(logical);
    return logical_name == "+" || logical_name == "plus";
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
    return action == "volume_up"
        && default_volume_up_layout_fallback(*binding, event);
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
