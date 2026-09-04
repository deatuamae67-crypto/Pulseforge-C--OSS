#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge {

// Actions live in separate contexts so a physical key can legitimately be
// reused by the menus, gameplay and editors without becoming a conflict.
enum class InputContext : std::uint8_t {
    global,
    menu,
    gameplay,
    editor,
};

enum class InputDevice : std::uint8_t {
    keyboard,
    gamepad,
};

struct PhysicalInput {
    InputDevice device{InputDevice::keyboard};
    std::string name;

    [[nodiscard]] bool operator==(const PhysicalInput&) const = default;
};

struct ActionBinding {
    std::string action;
    std::vector<PhysicalInput> inputs;
};

struct InputBindings {
    std::vector<ActionBinding> actions;
};

struct ActionDefinition {
    std::string_view id;
    std::string_view label;
    InputContext context{};
    bool required{};
};

struct BindingConflict {
    std::string action;
    std::string conflicting_action;
    PhysicalInput input;
};

enum class BindingConflictPolicy : std::uint8_t {
    reject,
    replace_existing,
};

enum class BindingEditStatus : std::uint8_t {
    applied,
    duplicate,
    not_found,
    invalid_action,
    invalid_input,
    conflict,
    limit_exceeded,
    required_action,
};

struct BindingEditResult {
    BindingEditStatus status{BindingEditStatus::applied};
    std::optional<BindingConflict> conflict;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == BindingEditStatus::applied
            || status == BindingEditStatus::duplicate;
    }
};

struct BindingValidationResult {
    std::vector<std::string> errors;
    std::vector<BindingConflict> conflicts;

    [[nodiscard]] explicit operator bool() const noexcept {
        return errors.empty() && conflicts.empty();
    }
};

inline constexpr std::size_t maximum_action_count = 96U;
inline constexpr std::size_t maximum_inputs_per_action = 8U;
inline constexpr std::size_t maximum_input_name_bytes = 64U;

[[nodiscard]] std::span<const ActionDefinition> input_action_definitions() noexcept;
[[nodiscard]] const ActionDefinition* find_input_action(
    std::string_view action
) noexcept;
[[nodiscard]] std::optional<std::uint16_t> lane_for_input_action(
    std::string_view action
) noexcept;
[[nodiscard]] std::string canonicalize_input_name(std::string_view name);
[[nodiscard]] bool is_valid_input_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view to_string(InputDevice device) noexcept;
[[nodiscard]] std::optional<InputDevice> input_device_from_string(
    std::string_view value
) noexcept;

[[nodiscard]] InputBindings default_input_bindings();
[[nodiscard]] const ActionBinding* find_action_binding(
    const InputBindings& bindings,
    std::string_view action
) noexcept;
[[nodiscard]] ActionBinding* find_action_binding(
    InputBindings& bindings,
    std::string_view action
) noexcept;

[[nodiscard]] BindingEditResult add_input_binding(
    InputBindings& bindings,
    std::string_view action,
    PhysicalInput input,
    BindingConflictPolicy policy = BindingConflictPolicy::reject
);
[[nodiscard]] BindingEditResult remove_input_binding(
    InputBindings& bindings,
    std::string_view action,
    const PhysicalInput& input
);
[[nodiscard]] BindingEditResult reset_input_action(
    InputBindings& bindings,
    std::string_view action
);
void reset_all_input_bindings(InputBindings& bindings);

// Strict validation is used before persistence. It catches duplicate action
// records, malformed names and ambiguous bindings in the same input context.
[[nodiscard]] BindingValidationResult validate_input_bindings(
    const InputBindings& bindings
);

}  // namespace pulseforge
