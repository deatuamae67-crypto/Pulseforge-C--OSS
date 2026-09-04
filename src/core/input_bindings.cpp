#include "pulseforge/input_bindings.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <utility>

namespace pulseforge {
namespace {

constexpr std::array action_definitions{
    ActionDefinition{"note_0", "Note Left", InputContext::gameplay, true},
    ActionDefinition{"note_1", "Note Down", InputContext::gameplay, true},
    ActionDefinition{"note_2", "Note Up", InputContext::gameplay, true},
    ActionDefinition{"note_3", "Note Right", InputContext::gameplay, true},
    ActionDefinition{"note_4", "Note 5", InputContext::gameplay, false},
    ActionDefinition{"note_5", "Note 6", InputContext::gameplay, false},
    ActionDefinition{"note_6", "Note 7", InputContext::gameplay, false},
    ActionDefinition{"note_7", "Note 8", InputContext::gameplay, false},
    ActionDefinition{"note_8", "Note 9", InputContext::gameplay, false},
    ActionDefinition{"note_9", "Note 10", InputContext::gameplay, false},
    ActionDefinition{"note_10", "Note 11", InputContext::gameplay, false},
    ActionDefinition{"note_11", "Note 12", InputContext::gameplay, false},
    ActionDefinition{"note_12", "Note 13", InputContext::gameplay, false},
    ActionDefinition{"note_13", "Note 14", InputContext::gameplay, false},
    ActionDefinition{"note_14", "Note 15", InputContext::gameplay, false},
    ActionDefinition{"note_15", "Note 16", InputContext::gameplay, false},
    ActionDefinition{"note_16", "Note 17", InputContext::gameplay, false},
    ActionDefinition{"note_17", "Note 18", InputContext::gameplay, false},
    ActionDefinition{"pause", "Pause", InputContext::gameplay, true},
    ActionDefinition{"reset_song", "Reset song", InputContext::gameplay, false},
    ActionDefinition{"ui_left", "UI Left", InputContext::menu, true},
    ActionDefinition{"ui_down", "UI Down", InputContext::menu, true},
    ActionDefinition{"ui_up", "UI Up", InputContext::menu, true},
    ActionDefinition{"ui_right", "UI Right", InputContext::menu, true},
    ActionDefinition{"ui_accept", "Accept", InputContext::menu, true},
    ActionDefinition{"ui_back", "Back", InputContext::menu, true},
    ActionDefinition{"fullscreen", "Fullscreen", InputContext::global, false},
    ActionDefinition{"screenshot", "Screenshot", InputContext::global, false},
    ActionDefinition{"volume_mute", "Mute", InputContext::global, false},
    ActionDefinition{"volume_down", "Volume down", InputContext::global, false},
    ActionDefinition{"volume_up", "Volume up", InputContext::global, false},
    ActionDefinition{"editor_play_pause", "Editor play/pause", InputContext::editor, true},
    ActionDefinition{"editor_save", "Editor save", InputContext::editor, false},
    ActionDefinition{"editor_undo", "Editor undo", InputContext::editor, false},
    ActionDefinition{"editor_redo", "Editor redo", InputContext::editor, false},
    ActionDefinition{"editor_copy", "Editor copy", InputContext::editor, false},
    ActionDefinition{"editor_paste", "Editor paste", InputContext::editor, false},
    ActionDefinition{"editor_delete", "Editor delete", InputContext::editor, false},
    ActionDefinition{"editor_zoom_in", "Editor zoom in", InputContext::editor, false},
    ActionDefinition{"editor_zoom_out", "Editor zoom out", InputContext::editor, false},
    ActionDefinition{"editor_grid_finer", "Editor finer grid", InputContext::editor, false},
    ActionDefinition{"editor_grid_coarser", "Editor coarser grid", InputContext::editor, false},
};

[[nodiscard]] bool same_input(
    const PhysicalInput& left,
    const PhysicalInput& right
) noexcept {
    return left.device == right.device && left.name == right.name;
}

[[nodiscard]] bool contexts_overlap(
    const InputContext left,
    const InputContext right
) noexcept {
    return left == right
        || left == InputContext::global
        || right == InputContext::global;
}

void append_default(
    InputBindings& result,
    const std::string_view action,
    const InputDevice device,
    const std::string_view input
) {
    auto* entry = find_action_binding(result, action);
    if (entry == nullptr) {
        result.actions.push_back({std::string(action), {}});
        entry = &result.actions.back();
    }
    entry->inputs.push_back({device, std::string(input)});
}

}  // namespace

std::span<const ActionDefinition> input_action_definitions() noexcept {
    return action_definitions;
}

const ActionDefinition* find_input_action(const std::string_view action) noexcept {
    const auto iterator = std::ranges::find(
        action_definitions,
        action,
        &ActionDefinition::id
    );
    return iterator == action_definitions.end() ? nullptr : &*iterator;
}

std::optional<std::uint16_t> lane_for_input_action(
    const std::string_view action
) noexcept {
    constexpr std::string_view prefix = "note_";
    if (!action.starts_with(prefix)) {
        return std::nullopt;
    }
    const auto number = action.substr(prefix.size());
    std::uint16_t lane = 0;
    const auto parsed = std::from_chars(
        number.data(),
        number.data() + number.size(),
        lane
    );
    if (parsed.ec != std::errc{}
        || parsed.ptr != number.data() + number.size()
        || lane > 17U) {
        return std::nullopt;
    }
    return lane;
}

std::string canonicalize_input_name(const std::string_view name) {
    std::string result;
    result.reserve(name.size());
    bool pending_space = false;
    for (const unsigned char value : name) {
        if (value == ' ' || value == '\t' || value == '\r' || value == '\n') {
            pending_space = !result.empty();
            continue;
        }
        if (pending_space) {
            result.push_back(' ');
            pending_space = false;
        }
        result.push_back(value >= 'A' && value <= 'Z'
            ? static_cast<char>(value - 'A' + 'a')
            : static_cast<char>(value));
    }
    const auto base = result.rfind('+');
    const auto base_start = base == std::string::npos ? 0U : base + 1U;
    if (result.substr(base_start) == "=") {
        result.replace(base_start, 1U, "equals");
    } else if (result.substr(base_start) == "-") {
        result.replace(base_start, 1U, "minus");
    }
    return result;
}

bool is_valid_input_name(const std::string_view name) noexcept {
    if (name.empty() || name.size() > maximum_input_name_bytes) {
        return false;
    }
    // SDL input names and our modifier chord syntax are deliberately ASCII.
    // Rejecting controls and non-ASCII bytes keeps comparisons deterministic.
    return name.front() != ' '
        && name.back() != ' '
        && std::ranges::all_of(name, [](const unsigned char value) {
        return value >= 0x20U && value <= 0x7EU;
    });
}

std::string_view to_string(const InputDevice device) noexcept {
    switch (device) {
    case InputDevice::keyboard: return "keyboard";
    case InputDevice::gamepad: return "gamepad";
    }
    return "keyboard";
}

std::optional<InputDevice> input_device_from_string(
    const std::string_view value
) noexcept {
    if (value == "keyboard") {
        return InputDevice::keyboard;
    }
    if (value == "gamepad") {
        return InputDevice::gamepad;
    }
    return std::nullopt;
}

InputBindings default_input_bindings() {
    InputBindings result;
    result.actions.reserve(action_definitions.size());
    for (const auto& definition : action_definitions) {
        result.actions.push_back({std::string(definition.id), {}});
    }

    constexpr std::array keyboard_lanes{
        std::array<std::string_view, 2>{"d", "left"},
        std::array<std::string_view, 2>{"f", "down"},
        std::array<std::string_view, 2>{"j", "up"},
        std::array<std::string_view, 2>{"k", "right"},
    };
    constexpr std::array gamepad_lanes{
        std::array<std::string_view, 2>{"dpleft", "x"},
        std::array<std::string_view, 2>{"dpdown", "a"},
        std::array<std::string_view, 2>{"dpup", "y"},
        std::array<std::string_view, 2>{"dpright", "b"},
    };
    for (std::size_t lane = 0; lane < keyboard_lanes.size(); ++lane) {
        const auto action = "note_" + std::to_string(lane);
        for (const auto input : keyboard_lanes[lane]) {
            append_default(result, action, InputDevice::keyboard, input);
        }
        for (const auto input : gamepad_lanes[lane]) {
            append_default(result, action, InputDevice::gamepad, input);
        }
    }

    append_default(result, "pause", InputDevice::keyboard, "escape");
    append_default(result, "pause", InputDevice::keyboard, "return");
    append_default(result, "pause", InputDevice::gamepad, "start");
    append_default(result, "reset_song", InputDevice::keyboard, "r");
    append_default(result, "ui_left", InputDevice::keyboard, "left");
    append_default(result, "ui_left", InputDevice::keyboard, "a");
    append_default(result, "ui_left", InputDevice::gamepad, "dpleft");
    append_default(result, "ui_down", InputDevice::keyboard, "down");
    append_default(result, "ui_down", InputDevice::keyboard, "s");
    append_default(result, "ui_down", InputDevice::gamepad, "dpdown");
    append_default(result, "ui_up", InputDevice::keyboard, "up");
    append_default(result, "ui_up", InputDevice::keyboard, "w");
    append_default(result, "ui_up", InputDevice::gamepad, "dpup");
    append_default(result, "ui_right", InputDevice::keyboard, "right");
    append_default(result, "ui_right", InputDevice::keyboard, "d");
    append_default(result, "ui_right", InputDevice::gamepad, "dpright");
    append_default(result, "ui_accept", InputDevice::keyboard, "return");
    append_default(result, "ui_accept", InputDevice::keyboard, "space");
    append_default(result, "ui_accept", InputDevice::gamepad, "a");
    append_default(result, "ui_back", InputDevice::keyboard, "escape");
    append_default(result, "ui_back", InputDevice::keyboard, "backspace");
    append_default(result, "ui_back", InputDevice::gamepad, "b");
    append_default(result, "fullscreen", InputDevice::keyboard, "f11");
    append_default(result, "screenshot", InputDevice::keyboard, "f12");
    append_default(result, "volume_mute", InputDevice::keyboard, "m");
    append_default(result, "volume_down", InputDevice::keyboard, "minus");
    append_default(result, "volume_up", InputDevice::keyboard, "shift+equals");
    append_default(result, "editor_play_pause", InputDevice::keyboard, "space");
    append_default(result, "editor_save", InputDevice::keyboard, "ctrl+s");
    append_default(result, "editor_undo", InputDevice::keyboard, "ctrl+z");
    append_default(result, "editor_redo", InputDevice::keyboard, "ctrl+y");
    append_default(result, "editor_copy", InputDevice::keyboard, "ctrl+c");
    append_default(result, "editor_paste", InputDevice::keyboard, "ctrl+v");
    append_default(result, "editor_delete", InputDevice::keyboard, "delete");
    append_default(result, "editor_zoom_in", InputDevice::keyboard, "ctrl+equals");
    append_default(result, "editor_zoom_out", InputDevice::keyboard, "ctrl+minus");
    append_default(result, "editor_grid_finer", InputDevice::keyboard, "rightbracket");
    append_default(result, "editor_grid_coarser", InputDevice::keyboard, "leftbracket");
    return result;
}

const ActionBinding* find_action_binding(
    const InputBindings& bindings,
    const std::string_view action
) noexcept {
    const auto iterator = std::ranges::find(
        bindings.actions,
        action,
        &ActionBinding::action
    );
    return iterator == bindings.actions.end() ? nullptr : &*iterator;
}

ActionBinding* find_action_binding(
    InputBindings& bindings,
    const std::string_view action
) noexcept {
    const auto iterator = std::ranges::find(
        bindings.actions,
        action,
        &ActionBinding::action
    );
    return iterator == bindings.actions.end() ? nullptr : &*iterator;
}

BindingEditResult add_input_binding(
    InputBindings& bindings,
    const std::string_view action,
    PhysicalInput input,
    const BindingConflictPolicy policy
) {
    const auto* definition = find_input_action(action);
    if (definition == nullptr) {
        return {BindingEditStatus::invalid_action, std::nullopt, "unknown input action"};
    }
    input.name = canonicalize_input_name(input.name);
    if (!is_valid_input_name(input.name)) {
        return {BindingEditStatus::invalid_input, std::nullopt, "invalid input name"};
    }

    auto* destination = find_action_binding(bindings, action);
    if (destination != nullptr
        && std::ranges::any_of(destination->inputs, [&](const PhysicalInput& item) {
            return same_input(item, input);
        })) {
        return {BindingEditStatus::duplicate, std::nullopt, {}};
    }

    if (destination != nullptr
        && destination->inputs.size() >= maximum_inputs_per_action) {
        return {
            BindingEditStatus::limit_exceeded,
            std::nullopt,
            "too many bindings for this action",
        };
    }
    if (destination == nullptr && bindings.actions.size() >= maximum_action_count) {
        return {BindingEditStatus::limit_exceeded, std::nullopt, "too many actions"};
    }

    std::vector<ActionBinding*> conflicting_entries;
    for (auto& entry : bindings.actions) {
        if (entry.action == action) {
            continue;
        }
        const auto* other_definition = find_input_action(entry.action);
        if (other_definition == nullptr
            || !contexts_overlap(other_definition->context, definition->context)) {
            continue;
        }
        const auto iterator = std::ranges::find_if(
            entry.inputs,
            [&](const PhysicalInput& item) { return same_input(item, input); }
        );
        if (iterator == entry.inputs.end()) {
            continue;
        }
        BindingConflict conflict{
            std::string(action),
            entry.action,
            input,
        };
        if (policy == BindingConflictPolicy::reject) {
            return {
                BindingEditStatus::conflict,
                std::move(conflict),
                "input is already bound in this context",
            };
        }
        if (other_definition->required && entry.inputs.size() == 1U) {
            return {
                BindingEditStatus::required_action,
                std::move(conflict),
                "cannot remove the final binding from a required action",
            };
        }
        conflicting_entries.push_back(&entry);
    }
    for (auto* entry : conflicting_entries) {
        const auto iterator = std::ranges::find_if(
            entry->inputs,
            [&](const PhysicalInput& item) { return same_input(item, input); }
        );
        if (iterator != entry->inputs.end()) {
            entry->inputs.erase(iterator);
        }
    }

    if (destination == nullptr) {
        bindings.actions.push_back({std::string(action), {}});
        destination = &bindings.actions.back();
    }
    destination->inputs.push_back(std::move(input));
    return {};
}

BindingEditResult remove_input_binding(
    InputBindings& bindings,
    const std::string_view action,
    const PhysicalInput& raw_input
) {
    const auto* definition = find_input_action(action);
    if (definition == nullptr) {
        return {BindingEditStatus::invalid_action, std::nullopt, "unknown input action"};
    }
    PhysicalInput input = raw_input;
    input.name = canonicalize_input_name(input.name);
    if (!is_valid_input_name(input.name)) {
        return {BindingEditStatus::invalid_input, std::nullopt, "invalid input name"};
    }
    auto* entry = find_action_binding(bindings, action);
    if (entry == nullptr) {
        return {BindingEditStatus::not_found, std::nullopt, "action has no bindings"};
    }
    const auto iterator = std::ranges::find_if(
        entry->inputs,
        [&](const PhysicalInput& item) { return same_input(item, input); }
    );
    if (iterator == entry->inputs.end()) {
        return {BindingEditStatus::not_found, std::nullopt, "binding was not found"};
    }
    if (definition->required && entry->inputs.size() == 1U) {
        return {
            BindingEditStatus::required_action,
            std::nullopt,
            "cannot remove the final binding from a required action",
        };
    }
    entry->inputs.erase(iterator);
    return {};
}

BindingEditResult reset_input_action(
    InputBindings& bindings,
    const std::string_view action
) {
    if (find_input_action(action) == nullptr) {
        return {BindingEditStatus::invalid_action, std::nullopt, "unknown input action"};
    }
    const auto defaults = default_input_bindings();
    const auto* default_entry = find_action_binding(defaults, action);
    InputBindings candidate = bindings;
    auto* current = find_action_binding(candidate, action);
    if (current != nullptr) {
        current->inputs.clear();
    } else if (candidate.actions.size() < maximum_action_count) {
        candidate.actions.push_back({std::string(action), {}});
    } else {
        return {BindingEditStatus::limit_exceeded, std::nullopt, "too many actions"};
    }
    if (default_entry != nullptr) {
        for (const auto& input : default_entry->inputs) {
            const auto added = add_input_binding(
                candidate,
                action,
                input,
                BindingConflictPolicy::replace_existing
            );
            if (!added) {
                return added;
            }
        }
    }
    bindings = std::move(candidate);
    return {};
}

void reset_all_input_bindings(InputBindings& bindings) {
    bindings = default_input_bindings();
}

BindingValidationResult validate_input_bindings(const InputBindings& bindings) {
    BindingValidationResult result;
    if (bindings.actions.size() > maximum_action_count) {
        result.errors.emplace_back("input settings contain too many actions");
    }
    for (std::size_t index = 0; index < bindings.actions.size(); ++index) {
        const auto& entry = bindings.actions[index];
        const auto* definition = find_input_action(entry.action);
        if (definition == nullptr) {
            result.errors.push_back("unknown input action: " + entry.action);
            continue;
        }
        if (entry.inputs.size() > maximum_inputs_per_action) {
            result.errors.push_back("too many inputs for action: " + entry.action);
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (bindings.actions[previous].action == entry.action) {
                result.errors.push_back("duplicate input action: " + entry.action);
                break;
            }
        }
        for (std::size_t input_index = 0; input_index < entry.inputs.size(); ++input_index) {
            const auto& input = entry.inputs[input_index];
            if (!is_valid_input_name(input.name)
                || canonicalize_input_name(input.name) != input.name) {
                result.errors.push_back("invalid or non-canonical input for action: " + entry.action);
                continue;
            }
            for (std::size_t previous_input = 0;
                 previous_input < input_index;
                 ++previous_input) {
                if (same_input(entry.inputs[previous_input], input)) {
                    result.errors.push_back("duplicate input for action: " + entry.action);
                    break;
                }
            }
            for (std::size_t previous_action = 0;
                 previous_action < index;
                 ++previous_action) {
                const auto& other = bindings.actions[previous_action];
                const auto* other_definition = find_input_action(other.action);
                if (other_definition == nullptr
                    || !contexts_overlap(
                        other_definition->context,
                        definition->context
                    )) {
                    continue;
                }
                if (std::ranges::any_of(other.inputs, [&](const PhysicalInput& item) {
                    return same_input(item, input);
                })) {
                    result.conflicts.push_back({entry.action, other.action, input});
                }
            }
        }
        if (definition->required && entry.inputs.empty()) {
            result.errors.push_back("required input action has no binding: " + entry.action);
        }
    }
    for (const auto& definition : action_definitions) {
        if (definition.required
            && find_action_binding(bindings, definition.id) == nullptr) {
            result.errors.push_back("required input action is missing: "
                + std::string(definition.id));
        }
    }
    return result;
}

}  // namespace pulseforge
