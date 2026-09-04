#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>

namespace pulseforge::detail {

// PULSEFORGE_P1_1_13_PULSE_WIGGLE_COMPAT_V1
// PULSEFORGE_P1_1_14_COLOR_SHADER_COMPAT_V1
// PULSEFORGE_P1_1_15_SHADER_EFFECTS_BATCH_V1
enum class PsychShaderCompatKind : std::uint8_t {
    generic_scalar,
    chromatic,
    pulse_effect,
    wiggle,
    adjust_color,
    grayscale,
    hsv,
    mosaic,
    gaussian_blur,
    snowstorm,
};

enum class PsychWiggleEffectType : std::int32_t {
    dreamy = 0,
    wavy = 1,
    heat_wave_horizontal = 2,
    heat_wave_vertical = 3,
    flag = 4,
};

struct PsychShaderScalarUniformView {
    std::string_view name;
    double value{};
};

struct PsychShaderCompatState {
    float alpha_multiplier{1.0F};
    float red_multiplier{1.0F};
    float green_multiplier{1.0F};
    float blue_multiplier{1.0F};
    float aberration{};
    float effect_time{};
    float speed{};
    float frequency{};
    float wave_amplitude{};
    float amplitude{};
    std::int32_t effect_type{};
    bool enabled{true};
    bool time_explicit{};

    // Renderer-independent scalar contracts for exact CPU colour fallbacks.
    float adjust_hue_degrees{};
    float adjust_saturation_percent{};
    float adjust_brightness{};
    float adjust_contrast{};
    float grayscale_amount{};
    float hsv_hue{1.0F};
    float hsv_saturation{1.0F};
    float hsv_value{1.0F};

    // P1.1.15 remaining shader-effect contracts.
    float mosaic_block_x{1.0F};
    float mosaic_block_y{1.0F};
    float gaussian_blur_amount{};
    float snowstorm_speed{3.5F};
};

struct PsychShaderColorMultipliers {
    float red{1.0F};
    float green{1.0F};
    float blue{1.0F};
};

struct PsychShaderRgb {
    float red{};
    float green{};
    float blue{};
};

struct PsychCpuShaderKey {
    PsychShaderCompatKind kind{PsychShaderCompatKind::generic_scalar};
    float a{};
    float b{};
    float c{};
    float d{};

    [[nodiscard]] friend constexpr bool operator==(
        const PsychCpuShaderKey& left,
        const PsychCpuShaderKey& right
    ) noexcept = default;
};

struct PsychShaderUv {
    float u{};
    float v{};
};

inline constexpr std::size_t psych_wiggle_min_segments = 8U;
inline constexpr std::size_t psych_wiggle_max_segments = 24U;

[[nodiscard]] constexpr char psych_shader_ascii_lower(const char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

[[nodiscard]] constexpr bool psych_shader_ascii_equal(
    const std::string_view left,
    const std::string_view right
) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (psych_shader_ascii_lower(left[index])
            != psych_shader_ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr std::string_view psych_shader_leaf_id(
    const std::string_view shader_id
) noexcept {
    const auto slash = shader_id.find_last_of("/\\");
    return slash == std::string_view::npos
        ? shader_id
        : shader_id.substr(slash + 1U);
}

[[nodiscard]] constexpr PsychShaderCompatKind psych_shader_kind(
    const std::string_view shader_id
) noexcept {
    const auto leaf = psych_shader_leaf_id(shader_id);
    if (psych_shader_ascii_equal(leaf, "chromatic")
        || psych_shader_ascii_equal(leaf, "chromaticaberration")
        || psych_shader_ascii_equal(leaf, "chromatic-aberration")
        || psych_shader_ascii_equal(leaf, "rgb-split")) {
        return PsychShaderCompatKind::chromatic;
    }
    if (psych_shader_ascii_equal(leaf, "pulseeffect")
        || psych_shader_ascii_equal(leaf, "pulse-effect")
        || psych_shader_ascii_equal(leaf, "pulse")) {
        return PsychShaderCompatKind::pulse_effect;
    }
    if (psych_shader_ascii_equal(leaf, "wiggle")
        || psych_shader_ascii_equal(leaf, "wiggleeffect")
        || psych_shader_ascii_equal(leaf, "wiggle-effect")) {
        return PsychShaderCompatKind::wiggle;
    }
    if (psych_shader_ascii_equal(leaf, "adjustcolor")
        || psych_shader_ascii_equal(leaf, "adjust-color")
        || psych_shader_ascii_equal(leaf, "coloradjust")
        || psych_shader_ascii_equal(leaf, "color-adjust")) {
        return PsychShaderCompatKind::adjust_color;
    }
    if (psych_shader_ascii_equal(leaf, "grayscale")
        || psych_shader_ascii_equal(leaf, "greyscale")
        || psych_shader_ascii_equal(leaf, "gray-scale")
        || psych_shader_ascii_equal(leaf, "grey-scale")) {
        return PsychShaderCompatKind::grayscale;
    }
    if (psych_shader_ascii_equal(leaf, "hsv")) {
        return PsychShaderCompatKind::hsv;
    }
    if (psych_shader_ascii_equal(leaf, "mosaic")
        || psych_shader_ascii_equal(leaf, "pixelate")
        || psych_shader_ascii_equal(leaf, "pixelation")) {
        return PsychShaderCompatKind::mosaic;
    }
    if (psych_shader_ascii_equal(leaf, "gaussianblur")
        || psych_shader_ascii_equal(leaf, "gaussian-blur")
        || psych_shader_ascii_equal(leaf, "blur")) {
        return PsychShaderCompatKind::gaussian_blur;
    }
    if (psych_shader_ascii_equal(leaf, "snowstorm")
        || psych_shader_ascii_equal(leaf, "snow-storm")) {
        return PsychShaderCompatKind::snowstorm;
    }
    return PsychShaderCompatKind::generic_scalar;
}

[[nodiscard]] constexpr bool psych_shader_uses_cpu_color_transform(
    const PsychShaderCompatKind kind
) noexcept {
    return kind == PsychShaderCompatKind::adjust_color
        || kind == PsychShaderCompatKind::grayscale
        || kind == PsychShaderCompatKind::hsv;
}

[[nodiscard]] constexpr bool psych_shader_uses_cpu_texture_transform(
    const PsychShaderCompatKind kind
) noexcept {
    return psych_shader_uses_cpu_color_transform(kind)
        || kind == PsychShaderCompatKind::mosaic
        || kind == PsychShaderCompatKind::gaussian_blur;
}

[[nodiscard]] inline PsychShaderCompatState psych_shader_compat_state(
    const std::span<const PsychShaderScalarUniformView> uniforms,
    const PsychShaderCompatKind shader_kind =
        PsychShaderCompatKind::generic_scalar
) noexcept {
    PsychShaderCompatState result{};
    float brightness = 1.0F;
    float intensity = 1.0F;
    float exposure = 0.0F;
    float alpha = 1.0F;
    float opacity = 1.0F;

    for (const auto& uniform : uniforms) {
        if (!std::isfinite(uniform.value)) {
            continue;
        }
        if (psych_shader_ascii_equal(uniform.name, "alpha")) {
            alpha = static_cast<float>(std::clamp(uniform.value, 0.0, 4.0));
        } else if (psych_shader_ascii_equal(uniform.name, "opacity")) {
            opacity = static_cast<float>(std::clamp(uniform.value, 0.0, 4.0));
        } else if (psych_shader_ascii_equal(uniform.name, "brightness")) {
            if (shader_kind == PsychShaderCompatKind::adjust_color) {
                result.adjust_brightness = static_cast<float>(
                    std::clamp(uniform.value, -255.0, 255.0)
                );
            } else {
                brightness = static_cast<float>(
                    std::clamp(uniform.value, 0.0, 8.0)
                );
            }
        } else if (psych_shader_ascii_equal(uniform.name, "hue")
            && shader_kind == PsychShaderCompatKind::adjust_color) {
            result.adjust_hue_degrees = static_cast<float>(
                std::clamp(uniform.value, -3'600.0, 3'600.0)
            );
        } else if (psych_shader_ascii_equal(uniform.name, "saturation")
            && shader_kind == PsychShaderCompatKind::adjust_color) {
            result.adjust_saturation_percent = static_cast<float>(
                std::clamp(uniform.value, -100.0, 1'000.0)
            );
        } else if (psych_shader_ascii_equal(uniform.name, "contrast")
            && shader_kind == PsychShaderCompatKind::adjust_color) {
            result.adjust_contrast = static_cast<float>(
                std::clamp(uniform.value, -255.0, 255.0)
            );
        } else if (psych_shader_ascii_equal(uniform.name, "_amount")
            && shader_kind == PsychShaderCompatKind::grayscale) {
            result.grayscale_amount = static_cast<float>(
                std::clamp(uniform.value, 0.0, 1.0)
            );
        } else if (psych_shader_ascii_equal(uniform.name, "_hue")
            && shader_kind == PsychShaderCompatKind::hsv) {
            result.hsv_hue = static_cast<float>(
                std::clamp(uniform.value, -8.0, 8.0)
            );
        } else if (psych_shader_ascii_equal(uniform.name, "_sat")
            && shader_kind == PsychShaderCompatKind::hsv) {
            result.hsv_saturation = static_cast<float>(
                std::clamp(uniform.value, 0.0, 8.0)
            );
        } else if (psych_shader_ascii_equal(uniform.name, "_val")
            && shader_kind == PsychShaderCompatKind::hsv) {
            result.hsv_value = static_cast<float>(
                std::clamp(uniform.value, 0.0, 8.0)
            );
        } else if (shader_kind == PsychShaderCompatKind::mosaic
            && (psych_shader_ascii_equal(uniform.name, "uBlocksize")
                || psych_shader_ascii_equal(uniform.name, "blocksize"))) {
            const float block = static_cast<float>(
                std::clamp(uniform.value, 1.0, 4'096.0)
            );
            result.mosaic_block_x = block;
            result.mosaic_block_y = block;
        } else if (shader_kind == PsychShaderCompatKind::mosaic
            && (psych_shader_ascii_equal(uniform.name, "uBlocksize[0]")
                || psych_shader_ascii_equal(uniform.name, "uBlocksize.x")
                || psych_shader_ascii_equal(uniform.name, "uBlocksizeX"))) {
            result.mosaic_block_x = static_cast<float>(
                std::clamp(uniform.value, 1.0, 4'096.0)
            );
        } else if (shader_kind == PsychShaderCompatKind::mosaic
            && (psych_shader_ascii_equal(uniform.name, "uBlocksize[1]")
                || psych_shader_ascii_equal(uniform.name, "uBlocksize.y")
                || psych_shader_ascii_equal(uniform.name, "uBlocksizeY"))) {
            result.mosaic_block_y = static_cast<float>(
                std::clamp(uniform.value, 1.0, 4'096.0)
            );
        } else if (shader_kind == PsychShaderCompatKind::gaussian_blur
            && psych_shader_ascii_equal(uniform.name, "_amount")) {
            result.gaussian_blur_amount = static_cast<float>(
                std::clamp(uniform.value, 0.0, 32.0)
            );
        } else if (shader_kind == PsychShaderCompatKind::snowstorm
            && psych_shader_ascii_equal(uniform.name, "speed")) {
            result.snowstorm_speed = static_cast<float>(
                std::clamp(uniform.value, -32.0, 32.0)
            );
        } else if (psych_shader_ascii_equal(uniform.name, "intensity")) {
            intensity = static_cast<float>(std::clamp(uniform.value, 0.0, 8.0));
        } else if (psych_shader_ascii_equal(uniform.name, "exposure")) {
            exposure = static_cast<float>(std::clamp(uniform.value, -8.0, 8.0));
        } else if (psych_shader_ascii_equal(uniform.name, "red")) {
            result.red_multiplier = static_cast<float>(
                std::clamp(uniform.value, 0.0, 8.0)
            );
        } else if (psych_shader_ascii_equal(uniform.name, "green")) {
            result.green_multiplier = static_cast<float>(
                std::clamp(uniform.value, 0.0, 8.0)
            );
        } else if (psych_shader_ascii_equal(uniform.name, "blue")) {
            result.blue_multiplier = static_cast<float>(
                std::clamp(uniform.value, 0.0, 8.0)
            );
        } else if (psych_shader_ascii_equal(uniform.name, "aberration")) {
            result.aberration = static_cast<float>(
                std::clamp(uniform.value, -0.25, 0.25)
            );
        } else if (psych_shader_ascii_equal(uniform.name, "effectTime")
            || psych_shader_ascii_equal(uniform.name, "time")
            || psych_shader_ascii_equal(uniform.name, "iTime")
            || psych_shader_ascii_equal(uniform.name, "uTime")) {
            result.effect_time = static_cast<float>(
                std::clamp(uniform.value, -1.0e9, 1.0e9)
            );
            result.time_explicit = true;
        } else if (psych_shader_ascii_equal(uniform.name, "uSpeed")
            || psych_shader_ascii_equal(uniform.name, "speed")) {
            result.speed = static_cast<float>(
                std::clamp(uniform.value, -128.0, 128.0)
            );
        } else if (psych_shader_ascii_equal(uniform.name, "uFrequency")
            || psych_shader_ascii_equal(uniform.name, "frequency")) {
            result.frequency = static_cast<float>(
                std::clamp(uniform.value, -256.0, 256.0)
            );
        } else if (psych_shader_ascii_equal(uniform.name, "uWaveAmplitude")
            || psych_shader_ascii_equal(uniform.name, "waveAmplitude")) {
            result.wave_amplitude = static_cast<float>(
                std::clamp(uniform.value, -0.5, 0.5)
            );
        } else if (psych_shader_ascii_equal(uniform.name, "uAmplitude")) {
            // pulseEffect.frag declares this uniform but does not consume it.
            // Retain it for contract compatibility without inventing semantics.
            result.amplitude = static_cast<float>(
                std::clamp(uniform.value, -8.0, 8.0)
            );
        } else if (psych_shader_ascii_equal(uniform.name, "uEnabled")
            || psych_shader_ascii_equal(uniform.name, "enabled")) {
            result.enabled = uniform.value != 0.0;
        } else if (psych_shader_ascii_equal(uniform.name, "effectType")) {
            const auto rounded = static_cast<std::int32_t>(
                std::clamp(std::llround(uniform.value), 0LL, 4LL)
            );
            result.effect_type = rounded;
        }
    }

    const float exposure_multiplier = std::exp2(exposure);
    const float common = std::clamp(
        brightness * intensity * exposure_multiplier,
        0.0F,
        8.0F
    );
    result.red_multiplier = std::clamp(
        result.red_multiplier * common, 0.0F, 8.0F
    );
    result.green_multiplier = std::clamp(
        result.green_multiplier * common, 0.0F, 8.0F
    );
    result.blue_multiplier = std::clamp(
        result.blue_multiplier * common, 0.0F, 8.0F
    );
    result.alpha_multiplier = std::clamp(alpha * opacity, 0.0F, 4.0F);
    return result;
}

[[nodiscard]] inline float psych_shader_effective_time(
    const PsychShaderCompatState& state,
    const double song_time_ms
) noexcept {
    if (state.time_explicit) {
        return state.effect_time;
    }
    if (!std::isfinite(song_time_ms)) {
        return 0.0F;
    }
    return static_cast<float>(std::clamp(
        song_time_ms / 1000.0,
        -1.0e9,
        1.0e9
    ));
}

[[nodiscard]] inline std::uint8_t psych_shader_modulated_channel(
    const std::uint8_t base,
    const float multiplier
) noexcept {
    const float value = static_cast<float>(base)
        * std::clamp(multiplier, 0.0F, 8.0F);
    return static_cast<std::uint8_t>(
        std::clamp(std::lround(value), 0L, 255L)
    );
}

[[nodiscard]] inline float psych_chromatic_offset_pixels(
    const PsychShaderCompatState& state,
    const float rendered_width
) noexcept {
    if (!std::isfinite(rendered_width) || rendered_width <= 0.0F
        || state.aberration == 0.0F) {
        return 0.0F;
    }

    // The source shader offsets red/blue by aberration*(uv-.5)^3.
    // |(uv-.5)^3| peaks at 1/8, so this preserves its natural pixel scale.
    const float temporal = 0.65F
        + 0.35F * std::abs(std::sin(state.effect_time * 2.0F));
    return std::clamp(
        state.aberration * rendered_width * 0.125F * temporal,
        -64.0F,
        64.0F
    );
}


[[nodiscard]] inline float psych_shader_quantize(
    const float value,
    const float step
) noexcept {
    if (!std::isfinite(value) || step <= 0.0F) {
        return 0.0F;
    }
    return std::round(value / step) * step;
}

[[nodiscard]] inline PsychCpuShaderKey psych_cpu_shader_key(
    const PsychShaderCompatKind kind,
    const PsychShaderCompatState& state
) noexcept {
    switch (kind) {
    case PsychShaderCompatKind::adjust_color:
        return {
            kind,
            state.adjust_hue_degrees,
            state.adjust_saturation_percent,
            state.adjust_brightness,
            state.adjust_contrast,
        };
    case PsychShaderCompatKind::grayscale:
        return {kind, state.grayscale_amount, 0.0F, 0.0F, 0.0F};
    case PsychShaderCompatKind::hsv:
        return {
            kind,
            state.hsv_hue,
            state.hsv_saturation,
            state.hsv_value,
            0.0F,
        };
    case PsychShaderCompatKind::mosaic:
        return {
            kind,
            psych_shader_quantize(state.mosaic_block_x, 0.25F),
            psych_shader_quantize(state.mosaic_block_y, 0.25F),
            0.0F,
            0.0F,
        };
    case PsychShaderCompatKind::gaussian_blur:
        return {
            kind,
            psych_shader_quantize(state.gaussian_blur_amount, 0.0625F),
            0.0F,
            0.0F,
            0.0F,
        };
    default:
        return {};
    }
}

[[nodiscard]] inline bool psych_cpu_shader_neutral(
    const PsychShaderCompatKind kind,
    const PsychShaderCompatState& state
) noexcept {
    switch (kind) {
    case PsychShaderCompatKind::adjust_color:
        return std::abs(state.adjust_hue_degrees) <= 0.00001F
            && std::abs(state.adjust_saturation_percent) <= 0.00001F
            && std::abs(state.adjust_brightness) <= 0.00001F
            && std::abs(state.adjust_contrast) <= 0.00001F;
    case PsychShaderCompatKind::grayscale:
        return state.grayscale_amount <= 0.00001F;
    case PsychShaderCompatKind::hsv:
        return std::abs(state.hsv_hue - 1.0F) <= 0.00001F
            && std::abs(state.hsv_saturation - 1.0F) <= 0.00001F
            && std::abs(state.hsv_value - 1.0F) <= 0.00001F;
    case PsychShaderCompatKind::mosaic:
        return state.mosaic_block_x <= 1.00001F
            && state.mosaic_block_y <= 1.00001F;
    case PsychShaderCompatKind::gaussian_blur:
        return state.gaussian_blur_amount <= 0.00001F;
    default:
        return true;
    }
}

[[nodiscard]] inline PsychShaderRgb psych_adjust_color_rgb(
    PsychShaderRgb color,
    const PsychShaderCompatState& state
) noexcept {
    color.red = std::clamp(
        color.red + state.adjust_brightness / 255.0F, 0.0F, 1.0F
    );
    color.green = std::clamp(
        color.green + state.adjust_brightness / 255.0F, 0.0F, 1.0F
    );
    color.blue = std::clamp(
        color.blue + state.adjust_brightness / 255.0F, 0.0F, 1.0F
    );

    constexpr float k = 0.57735F;
    constexpr float degrees_to_radians =
        0.01745329251994329576923690768489F;
    const float angle = state.adjust_hue_degrees * degrees_to_radians;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    const float dot = k * (color.red + color.green + color.blue);
    const PsychShaderRgb cross{
        k * color.blue - k * color.green,
        k * color.red - k * color.blue,
        k * color.green - k * color.red,
    };
    color = {
        color.red * cosine + cross.red * sine
            + k * dot * (1.0F - cosine),
        color.green * cosine + cross.green * sine
            + k * dot * (1.0F - cosine),
        color.blue * cosine + cross.blue * sine
            + k * dot * (1.0F - cosine),
    };

    const float contrast = 1.0F + state.adjust_contrast / 255.0F;
    color.red = std::clamp(
        (color.red - 0.5F) * contrast + 0.5F, 0.0F, 1.0F
    );
    color.green = std::clamp(
        (color.green - 0.5F) * contrast + 0.5F, 0.0F, 1.0F
    );
    color.blue = std::clamp(
        (color.blue - 0.5F) * contrast + 0.5F, 0.0F, 1.0F
    );

    const float intensity = color.red * 0.30980392156F
        + color.green * 0.60784313725F
        + color.blue * 0.08235294117F;
    const float saturation =
        1.0F + state.adjust_saturation_percent / 100.0F;
    color.red = std::clamp(
        std::lerp(intensity, color.red, saturation), 0.0F, 1.0F
    );
    color.green = std::clamp(
        std::lerp(intensity, color.green, saturation), 0.0F, 1.0F
    );
    color.blue = std::clamp(
        std::lerp(intensity, color.blue, saturation), 0.0F, 1.0F
    );
    return color;
}

[[nodiscard]] inline PsychShaderRgb psych_grayscale_rgb(
    const PsychShaderRgb input,
    const PsychShaderCompatState& state
) noexcept {
    const float amount = std::clamp(state.grayscale_amount, 0.0F, 1.0F);
    const float retained = 1.0F - amount;
    return {
        std::clamp(
            (0.2126F + 0.7874F * retained) * input.red
                + (0.7152F - 0.7152F * retained) * input.green
                + (0.0722F - 0.0722F * retained) * input.blue,
            0.0F,
            1.0F
        ),
        std::clamp(
            (0.2126F - 0.2126F * retained) * input.red
                + (0.7152F + 0.2848F * retained) * input.green
                + (0.0722F - 0.0722F * retained) * input.blue,
            0.0F,
            1.0F
        ),
        std::clamp(
            (0.2126F - 0.2126F * retained) * input.red
                + (0.7152F - 0.7152F * retained) * input.green
                + (0.0722F + 0.9278F * retained) * input.blue,
            0.0F,
            1.0F
        ),
    };
}

[[nodiscard]] inline PsychShaderRgb psych_rgb_to_hsv(
    const PsychShaderRgb input
) noexcept {
    const float maximum = (std::max)({input.red, input.green, input.blue});
    const float minimum = (std::min)({input.red, input.green, input.blue});
    const float delta = maximum - minimum;

    float hue = 0.0F;
    if (delta > 1.0e-10F) {
        if (maximum == input.red) {
            hue = std::fmod(
                (input.green - input.blue) / delta,
                6.0F
            );
        } else if (maximum == input.green) {
            hue = (input.blue - input.red) / delta + 2.0F;
        } else {
            hue = (input.red - input.green) / delta + 4.0F;
        }
        hue /= 6.0F;
        if (hue < 0.0F) {
            hue += 1.0F;
        }
    }

    const float saturation = maximum > 1.0e-10F
        ? delta / maximum
        : 0.0F;
    return {hue, saturation, maximum};
}

[[nodiscard]] inline PsychShaderRgb psych_hsv_to_rgb(
    const PsychShaderRgb hsv
) noexcept {
    const float hue = hsv.red - std::floor(hsv.red);
    const float saturation = hsv.green;
    const float value = hsv.blue;

    const float h = hue * 6.0F;
    const int sector = static_cast<int>(std::floor(h)) % 6;
    const float fraction = h - std::floor(h);
    const float p = value * (1.0F - saturation);
    const float q = value * (1.0F - fraction * saturation);
    const float t = value * (1.0F - (1.0F - fraction) * saturation);

    PsychShaderRgb rgb{};
    switch (sector) {
    case 0: rgb = {value, t, p}; break;
    case 1: rgb = {q, value, p}; break;
    case 2: rgb = {p, value, t}; break;
    case 3: rgb = {p, q, value}; break;
    case 4: rgb = {t, p, value}; break;
    default: rgb = {value, p, q}; break;
    }
    return {
        std::clamp(rgb.red, 0.0F, 1.0F),
        std::clamp(rgb.green, 0.0F, 1.0F),
        std::clamp(rgb.blue, 0.0F, 1.0F),
    };
}

[[nodiscard]] inline PsychShaderRgb psych_hsv_rgb(
    const PsychShaderRgb input,
    const PsychShaderCompatState& state
) noexcept {
    auto hsv = psych_rgb_to_hsv(input);
    hsv.red *= state.hsv_hue;
    hsv.green *= state.hsv_saturation;
    hsv.blue *= state.hsv_value;
    hsv.blue *= state.hsv_hue * 0.5F + 0.5F;
    return psych_hsv_to_rgb(hsv);
}

[[nodiscard]] inline PsychShaderRgb psych_cpu_color_shader_rgb(
    const PsychShaderCompatKind kind,
    const PsychShaderCompatState& state,
    const PsychShaderRgb input
) noexcept {
    switch (kind) {
    case PsychShaderCompatKind::adjust_color:
        return psych_adjust_color_rgb(input, state);
    case PsychShaderCompatKind::grayscale:
        return psych_grayscale_rgb(input, state);
    case PsychShaderCompatKind::hsv:
        return psych_hsv_rgb(input, state);
    default:
        return input;
    }
}

// pulseEffect.frag warps the sampled RGB components independently. SDL_Renderer
// cannot execute that per-pixel formula, so this is a bounded global colour
// approximation driven by the same time/speed/frequency/wave-amplitude inputs.
// uAmplitude is intentionally not used because the source shader does not use it.
[[nodiscard]] inline PsychShaderColorMultipliers psych_pulse_color_multipliers(
    const PsychShaderCompatState& state,
    const double song_time_ms
) noexcept {
    if (!state.enabled || std::abs(state.wave_amplitude) <= 0.00001F) {
        return {};
    }

    const float time = psych_shader_effective_time(state, song_time_ms);
    const float speed = state.speed == 0.0F ? 1.0F : state.speed;
    const float frequency = state.frequency == 0.0F ? 1.0F : state.frequency;
    const float strength = std::clamp(
        state.wave_amplitude * state.wave_amplitude,
        0.0F,
        0.25F
    );
    const float phase = time * speed;
    const float r = std::sin(phase * frequency);
    const float g = std::sin(phase * (frequency * 0.5F) + 2.09439510239F);
    const float b = std::sin(phase * (frequency * 0.33333333333F)
        + 4.18879020479F);

    return {
        std::clamp(1.0F + r * strength * 1.20F, 0.25F, 1.75F),
        std::clamp(1.0F + g * strength * 0.90F, 0.25F, 1.75F),
        std::clamp(1.0F + b * strength * 1.05F, 0.25F, 1.75F),
    };
}

[[nodiscard]] inline PsychShaderUv psych_wiggle_uv(
    const PsychShaderCompatState& state,
    const double song_time_ms,
    float u,
    float v
) noexcept {
    if (!state.enabled || std::abs(state.wave_amplitude) <= 0.000001F) {
        return {std::clamp(u, 0.0F, 1.0F), std::clamp(v, 0.0F, 1.0F)};
    }

    const float time = psych_shader_effective_time(state, song_time_ms);
    const float frequency = state.frequency;
    const float speed = state.speed;
    const float amplitude = state.wave_amplitude;
    const auto type = static_cast<PsychWiggleEffectType>(
        std::clamp(state.effect_type, 0, 4)
    );

    float x = 0.0F;
    float y = 0.0F;
    switch (type) {
    case PsychWiggleEffectType::dreamy: {
        const float offset_x = std::sin(
            u * frequency + time * speed
        ) * amplitude;
        v += offset_x;
        const float offset_y = std::sin(
            v * (frequency * 0.5F) + time * (speed * 0.5F)
        ) * (amplitude * 0.5F);
        u += offset_y;
        break;
    }
    case PsychWiggleEffectType::wavy:
        v += std::sin(u * frequency + time * speed) * amplitude;
        break;
    case PsychWiggleEffectType::heat_wave_horizontal:
        x = std::sin(u * frequency + time * speed) * amplitude;
        break;
    case PsychWiggleEffectType::heat_wave_vertical:
        y = std::sin(v * frequency + time * speed) * amplitude;
        break;
    case PsychWiggleEffectType::flag:
        y = std::sin(
            v * frequency + 10.0F * u + time * speed
        ) * amplitude;
        x = std::sin(
            u * frequency + 5.0F * v + time * speed
        ) * amplitude;
        break;
    }

    return {
        std::clamp(u + x, 0.0F, 1.0F),
        std::clamp(v + y, 0.0F, 1.0F),
    };
}

[[nodiscard]] inline std::size_t psych_wiggle_segment_count(
    const PsychShaderCompatState& state,
    const float rendered_width,
    const float rendered_height
) noexcept {
    if (!state.enabled || std::abs(state.wave_amplitude) <= 0.000001F
        || !std::isfinite(rendered_width) || !std::isfinite(rendered_height)
        || rendered_width <= 0.0F || rendered_height <= 0.0F) {
        return 0U;
    }
    const float complexity = std::clamp(
        std::abs(state.frequency) * 0.75F
            + std::abs(state.wave_amplitude) * 80.0F,
        static_cast<float>(psych_wiggle_min_segments),
        static_cast<float>(psych_wiggle_max_segments)
    );
    return static_cast<std::size_t>(std::lround(complexity));
}


struct PsychSnowstormParticle {
    float x{};
    float y{};
    float size{};
    float alpha{};
};

[[nodiscard]] inline float psych_shader_fract(const float value) noexcept {
    return value - std::floor(value);
}

[[nodiscard]] inline float psych_shader_hash01(
    const std::uint32_t value
) noexcept {
    std::uint32_t x = value;
    x ^= x >> 16U;
    x *= 0x7feb352dU;
    x ^= x >> 15U;
    x *= 0x846ca68bU;
    x ^= x >> 16U;
    return static_cast<float>(x & 0x00FFFFFFU)
        / static_cast<float>(0x01000000U);
}

[[nodiscard]] inline PsychSnowstormParticle psych_snowstorm_particle(
    const std::uint32_t layer,
    const std::uint32_t index,
    const PsychShaderCompatState& state,
    const double song_time_ms
) noexcept {
    constexpr float layer_motion[5]{
        0.10F, 0.20F, 0.40F, 0.60F, 1.00F
    };
    constexpr float layer_size[5]{
        1.0F, 1.3F, 1.8F, 2.5F, 3.5F
    };
    constexpr float layer_alpha[5]{
        0.30F, 0.36F, 0.44F, 0.55F, 0.70F
    };
    const std::uint32_t bounded_layer = std::min(layer, 4U);
    const float seed_x = psych_shader_hash01(
        index * 0x9e3779b9U + bounded_layer * 0x85ebca6bU + 17U
    );
    const float seed_y = psych_shader_hash01(
        index * 0xc2b2ae35U + bounded_layer * 0x27d4eb2fU + 53U
    );
    const float time = psych_shader_effective_time(state, song_time_ms);
    const float speed = state.snowstorm_speed;
    const float motion = layer_motion[bounded_layer];
    const float x = psych_shader_fract(
        seed_x - time * 0.045F * speed * motion
    );
    const float y = psych_shader_fract(
        seed_y + time * 0.0225F * speed * motion
    );
    return {
        x,
        y,
        layer_size[bounded_layer],
        layer_alpha[bounded_layer],
    };
}


}  // namespace pulseforge::detail
