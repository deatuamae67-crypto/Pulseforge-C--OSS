#pragma once

#include <cstdint>

namespace pulseforge::detail {

enum class RuntimeTextureAdmissionFailure : std::uint8_t {
    none,
    zero_dimension,
    dimension_limit,
    pixel_limit,
    per_texture_decoded_budget,
    scene_decoded_budget,
};

struct RuntimeTextureAdmissionPolicy {
    std::uint32_t maximum_dimension{8'192U};
    std::uint64_t maximum_pixels{64ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_decoded_bytes_per_texture{
        256ULL * 1024ULL * 1024ULL
    };
    std::uint64_t maximum_scene_decoded_bytes{
        1ULL * 1024ULL * 1024ULL * 1024ULL
    };
};

struct RuntimeTextureAdmission {
    RuntimeTextureAdmissionFailure failure{
        RuntimeTextureAdmissionFailure::none
    };
    std::uint64_t pixels{};
    std::uint64_t decoded_bytes{};

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return failure == RuntimeTextureAdmissionFailure::none;
    }
};

[[nodiscard]] constexpr RuntimeTextureAdmission
evaluate_runtime_texture_admission(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint64_t current_scene_decoded_bytes,
    const RuntimeTextureAdmissionPolicy policy = {}
) noexcept {
    RuntimeTextureAdmission result;

    if (width == 0U || height == 0U) {
        result.failure = RuntimeTextureAdmissionFailure::zero_dimension;
        return result;
    }
    if (width > policy.maximum_dimension || height > policy.maximum_dimension) {
        result.failure = RuntimeTextureAdmissionFailure::dimension_limit;
        return result;
    }

    result.pixels =
        static_cast<std::uint64_t>(width)
        * static_cast<std::uint64_t>(height);

    if (result.pixels > policy.maximum_pixels) {
        result.failure = RuntimeTextureAdmissionFailure::pixel_limit;
        return result;
    }
    if (result.pixels > UINT64_MAX / 4ULL) {
        result.failure =
            RuntimeTextureAdmissionFailure::per_texture_decoded_budget;
        return result;
    }

    result.decoded_bytes = result.pixels * 4ULL;

    if (result.decoded_bytes > policy.maximum_decoded_bytes_per_texture) {
        result.failure =
            RuntimeTextureAdmissionFailure::per_texture_decoded_budget;
        return result;
    }

    if (current_scene_decoded_bytes > policy.maximum_scene_decoded_bytes
        || result.decoded_bytes
            > policy.maximum_scene_decoded_bytes
                - current_scene_decoded_bytes) {
        result.failure = RuntimeTextureAdmissionFailure::scene_decoded_budget;
        return result;
    }

    return result;
}

[[nodiscard]] constexpr const char*
runtime_texture_admission_failure_name(
    const RuntimeTextureAdmissionFailure failure
) noexcept {
    switch (failure) {
    case RuntimeTextureAdmissionFailure::none:
        return "none";
    case RuntimeTextureAdmissionFailure::zero_dimension:
        return "zero dimension";
    case RuntimeTextureAdmissionFailure::dimension_limit:
        return "dimension limit";
    case RuntimeTextureAdmissionFailure::pixel_limit:
        return "pixel limit";
    case RuntimeTextureAdmissionFailure::per_texture_decoded_budget:
        return "per-texture decoded budget";
    case RuntimeTextureAdmissionFailure::scene_decoded_budget:
        return "scene decoded budget";
    }
    return "unknown";
}

}  // namespace pulseforge::detail
