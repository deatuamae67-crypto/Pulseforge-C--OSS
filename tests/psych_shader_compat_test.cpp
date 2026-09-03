#include "psych_shader_compat.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_classification() {
    using namespace pulseforge::detail;
    require(
        psych_shader_kind("Chromatic") == PsychShaderCompatKind::chromatic,
        "Chromatic should use the chromatic compatibility path"
    );
    require(
        psych_shader_kind("post/CHROMATIC-ABERRATION")
            == PsychShaderCompatKind::chromatic,
        "nested/case-insensitive Chromatic classification failed"
    );
    require(
        psych_shader_kind("pulseEffect")
            == PsychShaderCompatKind::pulse_effect,
        "pulseEffect should use the pulse compatibility path"
    );
    require(
        psych_shader_kind("shared/WIGGLE")
            == PsychShaderCompatKind::wiggle,
        "Wiggle should use the UV-mesh compatibility path"
    );
    require(
        psych_shader_kind("adjustColor")
            == PsychShaderCompatKind::adjust_color,
        "adjustColor should use the exact CPU colour path"
    );
    require(
        psych_shader_kind("shared/GRAYSCALE")
            == PsychShaderCompatKind::grayscale,
        "grayscale should use the exact CPU colour path"
    );
    require(
        psych_shader_kind("HSV")
            == PsychShaderCompatKind::hsv,
        "HSV should use the exact CPU colour path"
    );
    require(
        psych_shader_kind("mosaic")
            == PsychShaderCompatKind::mosaic,
        "mosaic should use the CPU texture compatibility path"
    );
    require(
        psych_shader_kind("GAUSSIAN-BLUR")
            == PsychShaderCompatKind::gaussian_blur,
        "gaussianBlur should use the bounded CPU blur path"
    );
    require(
        psych_shader_kind("Snowstorm")
            == PsychShaderCompatKind::snowstorm,
        "Snowstorm should use the native particle compatibility path"
    );
}

void test_scalar_uniforms() {
    using namespace pulseforge::detail;
    const PsychShaderScalarUniformView uniforms[]{
        {"alpha", 0.5},
        {"opacity", 0.5},
        {"brightness", 2.0},
        {"red", 0.5},
        {"green", 1.0},
        {"blue", 1.5},
        {"exposure", 1.0},
    };
    const auto state = psych_shader_compat_state(uniforms);
    require(std::abs(state.alpha_multiplier - 0.25F) < 0.0001F,
            "alpha/opacity multiplication failed");
    require(std::abs(state.red_multiplier - 2.0F) < 0.0001F,
            "red scalar modulation failed");
    require(std::abs(state.green_multiplier - 4.0F) < 0.0001F,
            "green scalar modulation failed");
    require(std::abs(state.blue_multiplier - 6.0F) < 0.0001F,
            "blue scalar modulation failed");
}

void test_pulse_contract() {
    using namespace pulseforge::detail;
    const PsychShaderScalarUniformView uniforms[]{
        {"uEnabled", 1.0},
        {"uSpeed", 2.0},
        {"uFrequency", 4.0},
        {"uWaveAmplitude", 0.4},
        {"uAmplitude", 123.0},
    };
    const auto state = psych_shader_compat_state(uniforms);
    require(state.enabled, "pulse uEnabled should be true");
    require(std::abs(state.wave_amplitude - 0.4F) < 0.0001F,
            "pulse uWaveAmplitude parsing failed");
    require(!state.time_explicit,
            "uTime should be automatic when the script does not set it");

    const auto a = psych_pulse_color_multipliers(state, 0.0);
    const auto b = psych_pulse_color_multipliers(state, 500.0);
    require(
        std::abs(a.red - b.red) > 0.0001F
            || std::abs(a.green - b.green) > 0.0001F
            || std::abs(a.blue - b.blue) > 0.0001F,
        "pulse colour approximation must animate with song time"
    );

    const PsychShaderScalarUniformView disabled_uniforms[]{
        {"uEnabled", 0.0},
        {"uWaveAmplitude", 0.5},
    };
    const auto disabled_state = psych_shader_compat_state(disabled_uniforms);
    const auto disabled = psych_pulse_color_multipliers(disabled_state, 999.0);
    require(
        std::abs(disabled.red - 1.0F) < 0.0001F
            && std::abs(disabled.green - 1.0F) < 0.0001F
            && std::abs(disabled.blue - 1.0F) < 0.0001F,
        "disabled pulseEffect must be visually neutral"
    );
}

void test_wiggle_contract() {
    using namespace pulseforge::detail;
    const PsychShaderScalarUniformView uniforms[]{
        {"uSpeed", 2.0},
        {"uFrequency", 8.0},
        {"uWaveAmplitude", 0.08},
        {"effectType", 1.0},
    };
    const auto state = psych_shader_compat_state(uniforms);
    require(state.effect_type == 1, "Wiggle effectType parsing failed");
    require(
        psych_wiggle_segment_count(state, 1280.0F, 720.0F)
            >= psych_wiggle_min_segments,
        "Wiggle should allocate a bounded non-zero mesh"
    );
    const auto uv_a = psych_wiggle_uv(state, 0.0, 0.25F, 0.5F);
    const auto uv_b = psych_wiggle_uv(state, 500.0, 0.25F, 0.5F);
    require(std::abs(uv_a.v - uv_b.v) > 0.0001F,
            "Wiggle uTime fallback must animate UVs");

    for (int type = 0; type <= 4; ++type) {
        const PsychShaderScalarUniformView typed[]{
            {"uSpeed", 1.5},
            {"uFrequency", 10.0},
            {"uWaveAmplitude", 0.12},
            {"effectType", static_cast<double>(type)},
        };
        const auto typed_state = psych_shader_compat_state(typed);
        const auto uv = psych_wiggle_uv(
            typed_state, 250.0, 0.37F, 0.61F
        );
        require(std::isfinite(uv.u) && std::isfinite(uv.v),
                "all five Wiggle modes must stay finite");
        require(uv.u >= 0.0F && uv.u <= 1.0F
                && uv.v >= 0.0F && uv.v <= 1.0F,
                "Wiggle UVs must remain texture-safe");
    }

    const PsychShaderScalarUniformView explicit_time_uniforms[]{
        {"uTime", 42.0},
        {"uSpeed", 2.0},
        {"uFrequency", 8.0},
        {"uWaveAmplitude", 0.08},
        {"effectType", 1.0},
    };
    const auto explicit_state =
        psych_shader_compat_state(explicit_time_uniforms);
    require(explicit_state.time_explicit,
            "explicit uTime should override automatic song time");
    require(
        std::abs(psych_shader_effective_time(explicit_state, 999999.0)
            - 42.0F) < 0.0001F,
        "explicit uTime override failed"
    );
}


void test_adjust_color_contract() {
    using namespace pulseforge::detail;
    const PsychShaderScalarUniformView neutral_uniforms[]{
        {"hue", 0.0},
        {"saturation", 0.0},
        {"brightness", 0.0},
        {"contrast", 0.0},
    };
    const auto neutral_state = psych_shader_compat_state(
        neutral_uniforms,
        PsychShaderCompatKind::adjust_color
    );
    const PsychShaderRgb input{0.2F, 0.4F, 0.8F};
    const auto neutral = psych_cpu_color_shader_rgb(
        PsychShaderCompatKind::adjust_color,
        neutral_state,
        input
    );
    require(std::abs(neutral.red - input.red) < 0.0002F
            && std::abs(neutral.green - input.green) < 0.0002F
            && std::abs(neutral.blue - input.blue) < 0.0002F,
            "neutral adjustColor uniforms must preserve RGB");

    const PsychShaderScalarUniformView bright_uniforms[]{
        {"brightness", 255.0},
    };
    const auto bright_state = psych_shader_compat_state(
        bright_uniforms,
        PsychShaderCompatKind::adjust_color
    );
    const auto bright = psych_cpu_color_shader_rgb(
        PsychShaderCompatKind::adjust_color,
        bright_state,
        PsychShaderRgb{0.0F, 0.0F, 0.0F}
    );
    require(bright.red > 0.999F && bright.green > 0.999F
            && bright.blue > 0.999F,
            "adjustColor brightness=255 should clamp black to white");

    const auto key = psych_cpu_shader_key(
        PsychShaderCompatKind::adjust_color,
        bright_state
    );
    require(key.kind == PsychShaderCompatKind::adjust_color
            && std::abs(key.c - 255.0F) < 0.0001F,
            "adjustColor cache key must include brightness");
}

void test_grayscale_contract() {
    using namespace pulseforge::detail;
    const PsychShaderScalarUniformView off_uniforms[]{
        {"_amount", 0.0},
    };
    const auto off_state = psych_shader_compat_state(
        off_uniforms,
        PsychShaderCompatKind::grayscale
    );
    const PsychShaderRgb red{1.0F, 0.0F, 0.0F};
    const auto off = psych_cpu_color_shader_rgb(
        PsychShaderCompatKind::grayscale,
        off_state,
        red
    );
    require(std::abs(off.red - 1.0F) < 0.0001F
            && std::abs(off.green) < 0.0001F
            && std::abs(off.blue) < 0.0001F,
            "grayscale amount 0 must preserve colour");

    const PsychShaderScalarUniformView full_uniforms[]{
        {"_amount", 1.0},
    };
    const auto full_state = psych_shader_compat_state(
        full_uniforms,
        PsychShaderCompatKind::grayscale
    );
    const auto full = psych_cpu_color_shader_rgb(
        PsychShaderCompatKind::grayscale,
        full_state,
        red
    );
    require(std::abs(full.red - 0.2126F) < 0.0002F
            && std::abs(full.green - 0.2126F) < 0.0002F
            && std::abs(full.blue - 0.2126F) < 0.0002F,
            "full grayscale must use the source shader luminance weights");
}

void test_hsv_contract() {
    using namespace pulseforge::detail;
    const PsychShaderScalarUniformView neutral_uniforms[]{
        {"_hue", 1.0},
        {"_sat", 1.0},
        {"_val", 1.0},
    };
    const auto neutral_state = psych_shader_compat_state(
        neutral_uniforms,
        PsychShaderCompatKind::hsv
    );
    const PsychShaderRgb input{0.25F, 0.6F, 0.9F};
    const auto neutral = psych_cpu_color_shader_rgb(
        PsychShaderCompatKind::hsv,
        neutral_state,
        input
    );
    require(std::abs(neutral.red - input.red) < 0.001F
            && std::abs(neutral.green - input.green) < 0.001F
            && std::abs(neutral.blue - input.blue) < 0.001F,
            "HSV multipliers 1/1/1 must be visually neutral");

    const PsychShaderScalarUniformView value_uniforms[]{
        {"_hue", 1.0},
        {"_sat", 1.0},
        {"_val", 0.5},
    };
    const auto value_state = psych_shader_compat_state(
        value_uniforms,
        PsychShaderCompatKind::hsv
    );
    const auto darker = psych_cpu_color_shader_rgb(
        PsychShaderCompatKind::hsv,
        value_state,
        input
    );
    require(darker.red < input.red && darker.green < input.green
            && darker.blue < input.blue,
            "HSV _val=0.5 should lower output value");
}


void test_mosaic_contract() {
    using namespace pulseforge::detail;
    const PsychShaderScalarUniformView uniforms[]{
        {"uBlocksize[0]", 8.0},
        {"uBlocksize[1]", 12.0},
    };
    const auto state = psych_shader_compat_state(
        uniforms,
        PsychShaderCompatKind::mosaic
    );
    require(std::abs(state.mosaic_block_x - 8.0F) < 0.0001F
            && std::abs(state.mosaic_block_y - 12.0F) < 0.0001F,
            "mosaic vector components must map to uBlocksize x/y");
    require(!psych_cpu_shader_neutral(PsychShaderCompatKind::mosaic, state),
            "mosaic block size above 1 must not be neutral");
    const auto key = psych_cpu_shader_key(
        PsychShaderCompatKind::mosaic,
        state
    );
    require(key.kind == PsychShaderCompatKind::mosaic
            && std::abs(key.a - 8.0F) < 0.0001F
            && std::abs(key.b - 12.0F) < 0.0001F,
            "mosaic cache key must retain both vector components");
}

void test_gaussian_blur_contract() {
    using namespace pulseforge::detail;
    const PsychShaderScalarUniformView uniforms[]{
        {"_amount", 2.5},
    };
    const auto state = psych_shader_compat_state(
        uniforms,
        PsychShaderCompatKind::gaussian_blur
    );
    require(std::abs(state.gaussian_blur_amount - 2.5F) < 0.0001F,
            "gaussian blur _amount parsing failed");
    require(!psych_cpu_shader_neutral(
                PsychShaderCompatKind::gaussian_blur,
                state
            ),
            "positive blur amount must not be neutral");
    const auto key = psych_cpu_shader_key(
        PsychShaderCompatKind::gaussian_blur,
        state
    );
    require(key.kind == PsychShaderCompatKind::gaussian_blur
            && key.a > 2.4F && key.a < 2.6F,
            "gaussian blur cache key must retain quantized amount");
}

void test_snowstorm_contract() {
    using namespace pulseforge::detail;
    const PsychShaderScalarUniformView uniforms[]{
        {"speed", 4.0},
    };
    const auto state = psych_shader_compat_state(
        uniforms,
        PsychShaderCompatKind::snowstorm
    );
    require(std::abs(state.snowstorm_speed - 4.0F) < 0.0001F,
            "Snowstorm speed parsing failed");

    const auto a = psych_snowstorm_particle(3U, 17U, state, 0.0);
    const auto b = psych_snowstorm_particle(3U, 17U, state, 500.0);
    require(a.x >= 0.0F && a.x < 1.0F
            && a.y >= 0.0F && a.y < 1.0F
            && b.x >= 0.0F && b.x < 1.0F
            && b.y >= 0.0F && b.y < 1.0F,
            "Snowstorm particles must remain normalized");
    require(std::abs(a.x - b.x) > 0.00001F
            || std::abs(a.y - b.y) > 0.00001F,
            "Snowstorm particles must animate with song time");
    const auto repeat = psych_snowstorm_particle(3U, 17U, state, 500.0);
    require(std::abs(repeat.x - b.x) < 0.000001F
            && std::abs(repeat.y - b.y) < 0.000001F,
            "Snowstorm particle field must be deterministic");
}

void test_chromatic_scale() {
    using namespace pulseforge::detail;
    const PsychShaderScalarUniformView uniforms[]{
        {"aberration", 0.1},
        {"effectTime", 0.0},
    };
    const auto state = psych_shader_compat_state(uniforms);
    const float offset = psych_chromatic_offset_pixels(state, 1000.0F);
    require(offset > 0.0F && offset <= 12.5F,
            "chromatic offset is outside the source shader's natural scale");

    const PsychShaderScalarUniformView hostile[]{
        {"aberration", 999.0},
        {"effectTime", 1.0e300},
    };
    const auto bounded = psych_shader_compat_state(hostile);
    require(std::abs(bounded.aberration) <= 0.25F,
            "aberration must remain bounded");
    require(std::abs(psych_chromatic_offset_pixels(bounded, 100000.0F))
                <= 64.0F,
            "chromatic pixel offset must remain bounded");
}

}  // namespace

int main() {
    try {
        test_classification();
        test_scalar_uniforms();
        test_pulse_contract();
        test_wiggle_contract();
        test_adjust_color_contract();
        test_grayscale_contract();
        test_hsv_contract();
        test_mosaic_contract();
        test_gaussian_blur_contract();
        test_snowstorm_contract();
        test_chromatic_scale();
        std::cout << "[PASS] Generic Psych shader compatibility semantics\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
