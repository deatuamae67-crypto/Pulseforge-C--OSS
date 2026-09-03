#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pulseforge::detail {

// Generic, renderer-independent semantics for the legacy Psych-compatible
// wavyEffect(tag, amplitude, frequency, speed) helper.
//
// amplitude is expressed as a fraction of the rendered sprite width (0.1 = 10%).
// frequency is the number of horizontal wave cycles over the sprite height.
// speed is the number of temporal cycles per second. Negative frequency/speed
// reverse their respective direction. Values are bounded before entering the
// real-time renderer so malformed mods cannot create unbounded geometry.
struct PsychWavyEffect final {
    float amplitude{};
    float frequency{};
    float speed{};
    bool enabled{};
};

inline constexpr float psych_wavy_max_amplitude = 0.25F;
inline constexpr float psych_wavy_max_frequency = 64.0F;
inline constexpr float psych_wavy_max_speed = 32.0F;
inline constexpr std::size_t psych_wavy_min_segments = 8U;
inline constexpr std::size_t psych_wavy_max_segments = 64U;

[[nodiscard]] inline PsychWavyEffect psych_wavy_effect(
    const double amplitude,
    const double frequency,
    const double speed
) noexcept {
    if (!std::isfinite(amplitude) || !std::isfinite(frequency)
        || !std::isfinite(speed)) {
        return {};
    }

    PsychWavyEffect result{
        static_cast<float>(std::clamp(
            amplitude,
            -static_cast<double>(psych_wavy_max_amplitude),
            static_cast<double>(psych_wavy_max_amplitude)
        )),
        static_cast<float>(std::clamp(
            frequency,
            -static_cast<double>(psych_wavy_max_frequency),
            static_cast<double>(psych_wavy_max_frequency)
        )),
        static_cast<float>(std::clamp(
            speed,
            -static_cast<double>(psych_wavy_max_speed),
            static_cast<double>(psych_wavy_max_speed)
        )),
        false,
    };
    result.enabled = std::abs(result.amplitude) > 0.000001F;
    return result;
}

[[nodiscard]] inline float psych_wavy_offset(
    const PsychWavyEffect effect,
    const float normalized_y,
    const double song_time_ms,
    const float rendered_width
) noexcept {
    if (!effect.enabled || !std::isfinite(normalized_y)
        || !std::isfinite(song_time_ms) || !std::isfinite(rendered_width)
        || rendered_width <= 0.0F) {
        return 0.0F;
    }

    constexpr double tau = 6.283185307179586476925286766559;
    const double spatial_cycles = static_cast<double>(normalized_y)
        * static_cast<double>(effect.frequency);
    const double temporal_cycles = (song_time_ms / 1'000.0)
        * static_cast<double>(effect.speed);
    // Keep very long songs numerically stable without changing the periodic
    // result. remainder() bounds the argument before the trigonometric call.
    const double phase = std::remainder(
        (spatial_cycles + temporal_cycles) * tau,
        tau
    );
    return static_cast<float>(
        static_cast<double>(effect.amplitude)
        * static_cast<double>(rendered_width)
        * std::sin(phase)
    );
}

[[nodiscard]] inline std::size_t psych_wavy_segment_count(
    const PsychWavyEffect effect,
    const float rendered_height
) noexcept {
    if (!effect.enabled || !std::isfinite(rendered_height)
        || rendered_height <= 0.0F) {
        return 0U;
    }

    // Eight samples per spatial cycle prevents the common 5-cycle stage
    // effect from becoming visibly angular. The pixel-density term keeps
    // low-frequency waves smooth on very tall backgrounds. Both paths are
    // hard-capped so the renderer never allocates or submits unbounded meshes.
    const auto frequency_segments = static_cast<std::size_t>(std::ceil(
        std::abs(effect.frequency) * 8.0F
    ));
    const auto height_segments = static_cast<std::size_t>(std::ceil(
        rendered_height / 32.0F
    ));
    return std::clamp(
        std::max(frequency_segments, height_segments),
        psych_wavy_min_segments,
        psych_wavy_max_segments
    );
}

}  // namespace pulseforge::detail
