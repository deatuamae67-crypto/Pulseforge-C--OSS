#include "psych_camera_target.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require_near(
    const double actual,
    const double expected,
    const std::string& label
) {
    if (std::abs(actual - expected) > 0.000001) {
        throw std::runtime_error(
            label + ": expected " + std::to_string(expected)
            + ", got " + std::to_string(actual)
        );
    }
}

void test_girlfriend_formula() {
    using namespace pulseforge::detail;
    const auto target = psych_camera_target({
        PsychCameraRole::girlfriend,
        500.0, 400.0,
        20.0, -10.0,
        5.0, 7.0,
    });
    require_near(target.x, 525.0, "girlfriend x");
    require_near(target.y, 397.0, "girlfriend y");
}

void test_opponent_formula() {
    using namespace pulseforge::detail;
    const auto target = psych_camera_target({
        PsychCameraRole::opponent,
        500.0, 400.0,
        20.0, -10.0,
        5.0, 7.0,
    });
    require_near(target.x, 675.0, "opponent x");
    require_near(target.y, 297.0, "opponent y");
}

void test_player_formula_has_psych_x_sign() {
    using namespace pulseforge::detail;
    const auto target = psych_camera_target({
        PsychCameraRole::player,
        500.0, 400.0,
        20.0, -10.0,
        5.0, 7.0,
    });
    require_near(target.x, 385.0, "player x");
    require_near(target.y, 297.0, "player y");
}

void test_zero_metadata_keeps_base_offsets() {
    using namespace pulseforge::detail;
    const auto dad = psych_camera_target({
        PsychCameraRole::opponent, 500.0, 400.0, 0.0, 0.0, 0.0, 0.0,
    });
    require_near(dad.x, 650.0, "dad base x");
    require_near(dad.y, 300.0, "dad base y");

    const auto bf = psych_camera_target({
        PsychCameraRole::player, 500.0, 400.0, 0.0, 0.0, 0.0, 0.0,
    });
    require_near(bf.x, 400.0, "bf base x");
    require_near(bf.y, 300.0, "bf base y");
}


void test_automatic_turn_tracker() {
    using namespace pulseforge::detail;
    PsychCameraTurnTracker tracker;

    if (!tracker.needs_focus(PsychCameraRole::opponent)) {
        throw std::runtime_error("first opponent turn must request camera focus");
    }
    tracker.commit(PsychCameraRole::opponent);
    if (tracker.needs_focus(PsychCameraRole::opponent)) {
        throw std::runtime_error("same opponent turn must not fight script camera overrides");
    }
    if (!tracker.needs_focus(PsychCameraRole::player)) {
        throw std::runtime_error("opponent-to-player turn must restore boyfriend focus");
    }
    tracker.commit(PsychCameraRole::player);
    if (!tracker.needs_focus(PsychCameraRole::opponent)) {
        throw std::runtime_error("player-to-opponent turn must restore dad focus");
    }
    tracker.reset();
    if (!tracker.needs_focus(PsychCameraRole::player)) {
        throw std::runtime_error("reset must make the next gameplay owner authoritative");
    }
}

}  // namespace

int main() {
    try {
        test_girlfriend_formula();
        test_opponent_formula();
        test_player_formula_has_psych_x_sign();
        test_zero_metadata_keeps_base_offsets();
        test_automatic_turn_tracker();
        std::cout << "[PASS] Generic Psych camera target + automatic turn semantics\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
