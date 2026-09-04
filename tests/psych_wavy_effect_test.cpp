#include "psych_wavy_effect.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& label) {
    if (!condition) {
        throw std::runtime_error(label);
    }
}

void require_near(
    const double actual,
    const double expected,
    const std::string& label,
    const double tolerance = 0.00001
) {
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(
            label + ": expected " + std::to_string(expected)
            + ", got " + std::to_string(actual)
        );
    }
}

void test_unknown_black_hole_parameters_are_preserved() {
    using namespace pulseforge::detail;
    const auto effect = psych_wavy_effect(0.1, 5.0, 2.0);
    require(effect.enabled, "Unknown Black Hole effect must be enabled");
    require_near(effect.amplitude, 0.1, "amplitude");
    require_near(effect.frequency, 5.0, "frequency");
    require_near(effect.speed, 2.0, "speed");
    require(psych_wavy_segment_count(effect, 720.0F) == 40U,
            "5-cycle effect should use 40 bounded segments");
}

void test_amplitude_is_normalized_to_sprite_width() {
    using namespace pulseforge::detail;
    const auto effect = psych_wavy_effect(0.1, 1.0, 0.0);
    const auto quarter_cycle = psych_wavy_offset(effect, 0.25F, 0.0, 800.0F);
    require_near(quarter_cycle, 80.0, "10 percent width amplitude", 0.001);
}

void test_temporal_phase_advances_deterministically() {
    using namespace pulseforge::detail;
    const auto effect = psych_wavy_effect(0.1, 0.0, 2.0);
    const auto at_zero = psych_wavy_offset(effect, 0.0F, 0.0, 1'000.0F);
    const auto at_eighth_second = psych_wavy_offset(
        effect, 0.0F, 125.0, 1'000.0F
    );
    require_near(at_zero, 0.0, "phase at t=0", 0.001);
    require_near(at_eighth_second, 100.0, "phase at t=125ms", 0.001);
}

void test_invalid_and_extreme_values_are_bounded() {
    using namespace pulseforge::detail;
    const auto bounded = psych_wavy_effect(9.0, -900.0, 900.0);
    require_near(bounded.amplitude, psych_wavy_max_amplitude, "amplitude clamp");
    require_near(bounded.frequency, -psych_wavy_max_frequency, "frequency clamp");
    require_near(bounded.speed, psych_wavy_max_speed, "speed clamp");
    require(psych_wavy_segment_count(bounded, 100'000.0F)
                == psych_wavy_max_segments,
            "segment count must remain bounded");

    const auto disabled = psych_wavy_effect(0.0, 5.0, 2.0);
    require(!disabled.enabled, "zero amplitude should disable the mesh path");
}

}  // namespace

int main() {
    try {
        test_unknown_black_hole_parameters_are_preserved();
        test_amplitude_is_normalized_to_sprite_width();
        test_temporal_phase_advances_deterministically();
        test_invalid_and_extreme_values_are_bounded();
        std::cout << "[PASS] Generic Psych wavyEffect semantics\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
