#pragma once

namespace pulseforge::detail {

enum class PsychCameraRole {
    girlfriend,
    opponent,
    player,
};

struct PsychCameraPoint {
    double x{};
    double y{};
};

struct PsychCameraTargetInput {
    PsychCameraRole role{PsychCameraRole::player};
    double midpoint_x{};
    double midpoint_y{};
    double character_camera_x{};
    double character_camera_y{};
    double stage_camera_x{};
    double stage_camera_y{};
};


class PsychCameraTurnTracker final {
public:
    // PULSEFORGE_P1_1_19_AUTOMATIC_CAMERA_TURN_TRACKER_V1
    [[nodiscard]] constexpr bool needs_focus(
        const PsychCameraRole next
    ) const noexcept {
        return !initialized_ || current_ != next;
    }

    constexpr void commit(const PsychCameraRole role) noexcept {
        current_ = role;
        initialized_ = true;
    }

    constexpr void reset() noexcept {
        initialized_ = false;
        current_ = PsychCameraRole::player;
    }

private:
    PsychCameraRole current_{PsychCameraRole::player};
    bool initialized_{};
};

[[nodiscard]] constexpr PsychCameraPoint psych_camera_target(
    const PsychCameraTargetInput input
) noexcept {
    switch (input.role) {
    case PsychCameraRole::girlfriend:
        return {
            input.midpoint_x
                + input.character_camera_x
                + input.stage_camera_x,
            input.midpoint_y
                + input.character_camera_y
                + input.stage_camera_y,
        };
    case PsychCameraRole::opponent:
        return {
            input.midpoint_x
                + 150.0
                + input.character_camera_x
                + input.stage_camera_x,
            input.midpoint_y
                - 100.0
                + input.character_camera_y
                + input.stage_camera_y,
        };
    case PsychCameraRole::player:
        return {
            input.midpoint_x
                - 100.0
                - input.character_camera_x
                + input.stage_camera_x,
            input.midpoint_y
                - 100.0
                + input.character_camera_y
                + input.stage_camera_y,
        };
    }
    return {input.midpoint_x, input.midpoint_y};
}

}  // namespace pulseforge::detail
