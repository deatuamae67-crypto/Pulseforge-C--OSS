#include "runtime_texture_policy.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void test_known_large_character_sheets() {
    using namespace pulseforge::detail;

    const auto girlfriend =
        evaluate_runtime_texture_admission(5'905U, 5'766U, 0U);
    require(girlfriend.accepted(), "5905x5766 should be admitted");

    const auto gfstyle = evaluate_runtime_texture_admission(
        5'737U, 5'315U, girlfriend.decoded_bytes
    );
    require(gfstyle.accepted(), "5737x5315 should be admitted");

    const auto boyfriend = evaluate_runtime_texture_admission(
        8'192U,
        8'192U,
        girlfriend.decoded_bytes + gfstyle.decoded_bytes
    );
    require(boyfriend.accepted(), "8192x8192 should be admitted");
    require(
        boyfriend.decoded_bytes == 256ULL * 1024ULL * 1024ULL,
        "8192x8192 RGBA32 must equal 256 MiB"
    );
}

void test_boundaries() {
    using namespace pulseforge::detail;

    require(
        evaluate_runtime_texture_admission(8'192U, 8'192U, 0U).accepted(),
        "8192 boundary should be accepted"
    );
    const auto too_wide =
        evaluate_runtime_texture_admission(8'193U, 1U, 0U);
    require(
        too_wide.failure == RuntimeTextureAdmissionFailure::dimension_limit,
        "8193 width should hit dimension limit"
    );
}

void test_scene_budget() {
    using namespace pulseforge::detail;

    constexpr std::uint64_t almost_full =
        1ULL * 1024ULL * 1024ULL * 1024ULL
        - 64ULL * 1024ULL * 1024ULL;

    require(
        evaluate_runtime_texture_admission(
            4'096U, 4'096U, almost_full
        ).accepted(),
        "last 64 MiB should fit exactly"
    );

    const auto rejected = evaluate_runtime_texture_admission(
        4'096U, 4'096U, almost_full + 1U
    );
    require(
        rejected.failure
            == RuntimeTextureAdmissionFailure::scene_decoded_budget,
        "scene total must stay bounded"
    );
}

void test_legacy_16_mpx_ceiling_is_gone() {
    using namespace pulseforge::detail;

    const auto result =
        evaluate_runtime_texture_admission(5'737U, 5'315U, 0U);
    require(
        result.pixels > 16ULL * 1024ULL * 1024ULL,
        "fixture must exceed legacy 16 Mpx"
    );
    require(result.accepted(), "legacy 16 Mpx ceiling still active");
}

}  // namespace

int main() {
    try {
        test_known_large_character_sheets();
        test_boundaries();
        test_scene_budget();
        test_legacy_16_mpx_ceiling_is_gone();
        std::cout << "[PASS] Runtime large-texture admission policy\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
