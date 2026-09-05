#include "intro_player.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

}  // namespace

int main(const int argument_count, char** arguments) {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    try {
        constexpr std::uint64_t ctos_duration_100ns = 1'208'366'440ULL;
        constexpr std::uint64_t ctos_duration_ns =
            ctos_duration_100ns * 100ULL;
        const auto ctos_timeout =
            pulseforge::detail::native_intro_timeout_ns(
                ctos_duration_100ns
            );
        require(
            ctos_timeout == ctos_duration_ns + 30'000'000'000ULL,
            "native timeout includes a 30 second completion margin"
        );
        require(
            ctos_timeout > 120'836'644'000ULL,
            "the 120.84 second ctOS intro cannot hit the former 90 second cap"
        );
        require(
            pulseforge::detail::native_intro_timeout_ns(0U)
                >= 600'000'000'000ULL,
            "unknown media duration has a conservative bounded fallback"
        );
        require(
            pulseforge::detail::native_intro_timeout_ns(
                std::numeric_limits<std::uint64_t>::max()
            ) == 21'600'000'000'000ULL,
            "maliciously large duration is capped at six hours"
        );

        const bool decoded_probe = argument_count == 2
            && std::filesystem::is_regular_file(arguments[1]);
        // Both paths are deterministic and headless.  The argument form points
        // at the source movie so play_startup_intro can locate the bounded
        // decoded PNG/OGG sequence beside it; it does not exercise MFPlay.
        static_cast<void>(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
        require(
            SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS),
            std::string("SDL initializes: ") + SDL_GetError()
        );
        require(
            SDL_CreateWindowAndRenderer(
                "PulseForge intro test",
                1'280,
                720,
                0U,
                &window,
                &renderer
            ),
            std::string("dummy window/renderer initialize: ") + SDL_GetError()
        );
        static_cast<void>(SDL_SetRenderLogicalPresentation(
            renderer,
            1'280,
            720,
            SDL_LOGICAL_PRESENTATION_LETTERBOX
        ));

        SDL_Event accidental_click{};
        accidental_click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        accidental_click.button.button = SDL_BUTTON_LEFT;
        require(SDL_PushEvent(&accidental_click), "accidental click is queued");
        const auto begin = SDL_GetTicks();
        std::jthread delayed_skip([decoded_probe] {
            SDL_Delay(decoded_probe ? 750U : 150U);
            SDL_Event skip{};
            skip.type = SDL_EVENT_KEY_DOWN;
            skip.key.scancode = SDL_SCANCODE_SPACE;
            static_cast<void>(SDL_PushEvent(&skip));
        });

        const auto result = pulseforge::detail::play_startup_intro(
            window,
            renderer,
            decoded_probe
                ? std::filesystem::path(arguments[1])
                : std::filesystem::path("missing-intro-for-test.mp4")
        );
        if (delayed_skip.joinable()) {
            delayed_skip.join();
        }
        require(
            SDL_GetTicks() - begin >= 100U,
            "mouse clicks cannot dismiss the intro"
        );
        require(
            result.status == pulseforge::detail::StartupIntroStatus::skipped,
            "fallback intro can be skipped immediately"
        );
        if (decoded_probe) {
            require(
                result.diagnostic.empty(),
                "the bounded decoded intro sequence opens without fallback: "
                    + result.diagnostic
            );
        } else {
            require(
                !result.diagnostic.empty(),
                "missing native movie reports why the fallback was selected"
            );
        }

        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
        SDL_DestroyWindow(window);
        window = nullptr;
        SDL_Quit();
        std::cout << "PulseForge startup intro tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        if (renderer != nullptr) {
            SDL_DestroyRenderer(renderer);
        }
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
        std::cerr << "PulseForge startup intro test failed: "
                  << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
