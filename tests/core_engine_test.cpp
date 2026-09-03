#include "pulseforge/CoreEngine.h"
#include "application_runner.hpp"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

}  // namespace

static_assert(!std::is_copy_constructible_v<pulseforge::CoreEngine>);
static_assert(!std::is_copy_assignable_v<pulseforge::CoreEngine>);
static_assert(!std::is_move_constructible_v<pulseforge::CoreEngine>);
static_assert(!std::is_move_assignable_v<pulseforge::CoreEngine>);
static_assert(!std::is_copy_constructible_v<
    pulseforge::detail::TransferredPlatform>);
static_assert(std::is_nothrow_move_constructible_v<
    pulseforge::detail::TransferredPlatform>);

void test_platform_round_trip() {
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    require(
        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD),
        SDL_GetError()
    );
    auto* const window = SDL_CreateWindow(
        "PulseForge platform round-trip test",
        320,
        180,
        SDL_WINDOW_HIDDEN
    );
    require(window != nullptr, SDL_GetError());
    auto* const renderer = SDL_CreateRenderer(window, "software");
    if (renderer == nullptr) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        require(false, SDL_GetError());
    }

    pulseforge::AppLaunchOptions options;
    options.chart_path = std::filesystem::path(PULSEFORGE_TEST_SOURCE_DIR)
        / "assets/demo/chart.json";
    options.enable_lua = false;
    options.safe_mode = true;
    options.smoke_test = true;
    options.smoke_test_chart_path = options.chart_path;
    options.return_to_launcher = true;
    options.settings.visual.vsync = false;
    // A persisted theme must not make the intentionally silent smoke chart
    // fail before the platform round-trip can be exercised.
    options.settings.visual.theme = pulseforge::PresentationTheme::ps2;
    options.settings.performance.maximum_performance_mode = true;

    pulseforge::detail::TransferredPlatform sent(window, renderer, true);
    pulseforge::detail::TransferredPlatform returned;
    auto runner = pulseforge::detail::make_gameplay_application(
        std::move(options),
        std::move(sent),
        &returned
    );
    require(!sent.complete(), "moving the platform must empty its source");
    require(runner->run() == EXIT_SUCCESS, "transferred smoke gameplay failed");
    runner.reset();
    require(returned.complete(), "gameplay did not return the SDL platform");
    require(
        returned.window == window && returned.renderer == renderer,
        "gameplay recreated the transferred window or renderer"
    );

    pulseforge::AppLaunchOptions ps2_audio_policy_options;
    ps2_audio_policy_options.chart_path = std::filesystem::path(
        PULSEFORGE_TEST_SOURCE_DIR
    ) / "assets/demo/chart.json";
    ps2_audio_policy_options.enable_lua = false;
    ps2_audio_policy_options.safe_mode = true;
    ps2_audio_policy_options.return_to_launcher = true;
    ps2_audio_policy_options.suppress_load_error_acknowledgement = true;
    ps2_audio_policy_options.settings.visual.vsync = false;
    ps2_audio_policy_options.settings.visual.theme =
        pulseforge::PresentationTheme::ps2;

    pulseforge::detail::TransferredPlatform ps2_audio_policy_returned;
    auto ps2_audio_policy_runner =
        pulseforge::detail::make_gameplay_application(
            std::move(ps2_audio_policy_options),
            std::move(returned),
            &ps2_audio_policy_returned
        );
    require(
        ps2_audio_policy_runner->run()
            == pulseforge::detail::runner_chart_load_failed,
        "PS2 gameplay without an Inst stem must remain a load failure"
    );
    ps2_audio_policy_runner.reset();
    require(
        ps2_audio_policy_returned.complete(),
        "failed PS2 gameplay did not return the SDL platform"
    );
    require(
        ps2_audio_policy_returned.window == window
            && ps2_audio_policy_returned.renderer == renderer,
        "failed PS2 gameplay recreated the transferred window or renderer"
    );
    returned = std::move(ps2_audio_policy_returned);

    const auto render_root = std::filesystem::temp_directory_path()
        / ("pulseforge-transferred-render-"
            + std::to_string(SDL_GetTicksNS()));
    std::error_code filesystem_error;
    require(
        std::filesystem::create_directory(render_root, filesystem_error),
        filesystem_error.message()
    );
    pulseforge::AppLaunchOptions render_options;
    render_options.chart_path = std::filesystem::path(
        PULSEFORGE_TEST_SOURCE_DIR
    ) / "assets/demo/chart.json";
    render_options.enable_lua = false;
    render_options.safe_mode = true;
    render_options.smoke_test = true;
    render_options.smoke_test_chart_path = render_options.chart_path;
    render_options.return_to_launcher = true;
    render_options.settings.visual.vsync = false;
    render_options.settings.visual.theme = pulseforge::PresentationTheme::ps2;
    render_options.settings.performance.maximum_performance_mode = true;
    render_options.offline_render.enabled = true;
    render_options.offline_render.output_directory = render_root;
    render_options.offline_render.output_name = "round-trip.mp4";
    render_options.offline_render.ffmpeg_executable =
        std::filesystem::path(PULSEFORGE_TEST_FFMPEG_PATH);
    render_options.offline_render.width = 642U;
    render_options.offline_render.height = 362U;
    render_options.offline_render.fps = 1U;
    render_options.offline_render.overwrite = true;

    pulseforge::detail::TransferredPlatform render_returned;
    auto render_runner = pulseforge::detail::make_gameplay_application(
        std::move(render_options),
        std::move(returned),
        &render_returned
    );
    require(
        render_runner->run() == pulseforge::detail::runner_return_to_launcher,
        "transferred offline render failed"
    );
    render_runner.reset();
    require(
        render_returned.complete(),
        "offline render did not return the SDL platform"
    );
    require(
        render_returned.window == window && render_returned.renderer == renderer,
        "offline render recreated the transferred window or renderer"
    );
    require(
        std::filesystem::is_regular_file(
            render_root / "round-trip.mp4",
            filesystem_error
        ) && !filesystem_error,
        "transferred offline render did not publish its output"
    );
    std::filesystem::remove_all(render_root, filesystem_error);
    require(!filesystem_error, filesystem_error.message());
}

int main() {
    try {
        pulseforge::AppLaunchOptions options;
        options.chart_path = std::filesystem::path(
            "__pulseforge_intentionally_missing_core_engine_test__.json"
        );
        options.enable_lua = false;
        options.safe_mode = true;

        pulseforge::CoreEngine engine(std::move(options));
        require(
            engine.state() == pulseforge::CoreEngineState::ready,
            "new engine must be ready"
        );
        require(engine.run() != EXIT_SUCCESS, "missing chart must fail");
        require(
            engine.state() == pulseforge::CoreEngineState::failed,
            "failed runtime must publish failed state"
        );

        bool rejected_second_run = false;
        try {
            static_cast<void>(engine.run());
        } catch (const std::logic_error&) {
            rejected_second_run = true;
        }
        require(rejected_second_run, "CoreEngine must reject a second run");

        test_platform_round_trip();

        std::cout << "CoreEngine lifecycle test passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "CoreEngine lifecycle test failed: "
                  << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
