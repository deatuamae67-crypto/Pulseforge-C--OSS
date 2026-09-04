#include "offline_encoder.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef PULSEFORGE_FAKE_FFMPEG
#error PULSEFORGE_FAKE_FFMPEG must name the fake process fixture
#endif

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(value.data()),
        value.size(),
    };
}

class SdlGuard final {
public:
    SdlGuard() {
        require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
    }
    ~SdlGuard() {
        SDL_Quit();
    }
};

class TemporaryRender final {
public:
    TemporaryRender() {
        path_ = std::filesystem::temp_directory_path()
            / ("pulseforge-encoder-test-" + std::to_string(SDL_GetTicksNS()));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryRender() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
private:
    std::filesystem::path path_;
};

[[nodiscard]] std::vector<char> capture_rgba(
    SDL_Renderer* const renderer,
    const std::uint32_t width,
    const std::uint32_t height
) {
    SDL_Surface* const surface = SDL_RenderReadPixels(renderer, nullptr);
    require(surface != nullptr, SDL_GetError());
    const auto row_bytes = static_cast<std::size_t>(width) * 4U;
    std::vector<char> result(
        row_bytes * static_cast<std::size_t>(height)
    );
    const bool converted = SDL_ConvertPixels(
        surface->w,
        surface->h,
        surface->format,
        surface->pixels,
        surface->pitch,
        SDL_PIXELFORMAT_RGBA32,
        result.data(),
        static_cast<int>(row_bytes)
    );
    SDL_DestroySurface(surface);
    require(converted, SDL_GetError());
    return result;
}

void process_pipe_and_atomic_publish_work() {
    SdlGuard sdl;
    TemporaryRender temporary;
    SDL_Window* window = SDL_CreateWindow(
        "offline encoder test",
        320,
        180,
        SDL_WINDOW_HIDDEN
    );
    require(window != nullptr, SDL_GetError());
    SDL_Renderer* renderer = SDL_CreateRenderer(window, "software");
    if (renderer == nullptr) {
        SDL_DestroyWindow(window);
        throw std::runtime_error(SDL_GetError());
    }

    pulseforge::OfflineRenderPlan plan;
    plan.ffmpeg_executable = PULSEFORGE_FAKE_FFMPEG;
    plan.final_output_path = temporary.path() / "renders" / "result.mp4";
    plan.temporary_output_path = temporary.path() / "renders" / ".partial.mp4";
    plan.diagnostic_log_path = temporary.path() / "renders" / ".ffmpeg.log";
    plan.arguments = {
        PULSEFORGE_FAKE_FFMPEG,
        path_utf8(plan.temporary_output_path),
    };
    plan.width = 320U;
    plan.height = 180U;
    plan.fps = 60U;
    plan.frame_count = 2U;
    plan.duration_ms = 1'000.0 / 30.0;

    pulseforge::detail::OfflineEncoder encoder;
    std::string error;
    require(encoder.start(std::move(plan), &error), error);
    require(encoder.active(), "encoder must report active after start");
    require(encoder.total_frames() == 2U, "encoder exposes planned frame count");
    require(encoder.progress_fraction() == 0.0, "initial progress must be zero");
    SDL_SetRenderDrawColor(renderer, 20U, 40U, 80U, 255U);
    require(SDL_RenderClear(renderer), SDL_GetError());
    require(encoder.write_frame(renderer, &error), error);
    require(
        encoder.frames_written() == 1U
            && encoder.progress_fraction() == 0.5,
        "progress must track committed raw frames"
    );
    SDL_SetRenderDrawColor(renderer, 90U, 30U, 10U, 255U);
    require(SDL_RenderClear(renderer), SDL_GetError());
    require(encoder.write_frame(renderer, &error), error);
    require(encoder.finish(&error), error);
    require(
        !encoder.active() && encoder.progress_fraction() == 1.0,
        "finished encoder must expose complete, inactive progress"
    );

    const auto output = temporary.path() / "renders" / "result.mp4";
    require(std::filesystem::is_regular_file(output), "final output is missing");
    require(std::filesystem::file_size(output) > 0U, "final output is empty");
    require(
        !std::filesystem::exists(temporary.path() / "renders" / ".partial.mp4")
            && !std::filesystem::exists(
                temporary.path() / "renders" / ".ffmpeg.log"
            ),
        "private files were not cleaned after success"
    );

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

void cancellation_removes_private_output() {
    SdlGuard sdl;
    TemporaryRender temporary;
    SDL_Window* window = SDL_CreateWindow(
        "offline encoder cancellation test",
        320,
        180,
        SDL_WINDOW_HIDDEN
    );
    require(window != nullptr, SDL_GetError());
    SDL_Renderer* renderer = SDL_CreateRenderer(window, "software");
    if (renderer == nullptr) {
        SDL_DestroyWindow(window);
        throw std::runtime_error(SDL_GetError());
    }

    pulseforge::OfflineRenderPlan plan;
    plan.ffmpeg_executable = PULSEFORGE_FAKE_FFMPEG;
    plan.final_output_path = temporary.path() / "renders" / "cancelled.mp4";
    plan.temporary_output_path = temporary.path() / "renders" / ".cancelled.mp4";
    plan.diagnostic_log_path = temporary.path() / "renders" / ".cancelled.log";
    plan.arguments = {
        PULSEFORGE_FAKE_FFMPEG,
        path_utf8(plan.temporary_output_path),
    };
    plan.width = 320U;
    plan.height = 180U;
    plan.fps = 60U;
    plan.frame_count = 10U;
    plan.duration_ms = 1'000.0 / 6.0;

    pulseforge::detail::OfflineEncoder encoder;
    std::string error;
    require(encoder.start(std::move(plan), &error), error);
    SDL_SetRenderDrawColor(renderer, 1U, 2U, 3U, 255U);
    require(SDL_RenderClear(renderer), SDL_GetError());
    require(encoder.write_frame(renderer, &error), error);
    require(encoder.progress_fraction() == 0.1, "partial progress is exposed");
    encoder.cancel();
    encoder.cancel();
    require(!encoder.active(), "cancelled encoder must be inactive");
    require(
        !std::filesystem::exists(
            temporary.path() / "renders" / "cancelled.mp4"
        )
            && !std::filesystem::exists(
                temporary.path() / "renders" / ".cancelled.mp4"
            )
            && !std::filesystem::exists(
                temporary.path() / "renders" / ".cancelled.log"
            ),
        "cancellation must leave neither final nor private output"
    );

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

void nonzero_exit_never_publishes_partial_output() {
    SdlGuard sdl;
    TemporaryRender temporary;
    SDL_Window* window = SDL_CreateWindow(
        "offline encoder failure test",
        320,
        180,
        SDL_WINDOW_HIDDEN
    );
    require(window != nullptr, SDL_GetError());
    SDL_Renderer* renderer = SDL_CreateRenderer(window, "software");
    if (renderer == nullptr) {
        SDL_DestroyWindow(window);
        throw std::runtime_error(SDL_GetError());
    }

    const auto render_directory = temporary.path()
        / "renders with spaces & brackets (test)";
    pulseforge::OfflineRenderPlan plan;
    plan.ffmpeg_executable = PULSEFORGE_FAKE_FFMPEG;
    plan.final_output_path = render_directory / "must-not-exist.mp4";
    plan.temporary_output_path = render_directory / ".failed partial.mp4";
    plan.diagnostic_log_path = render_directory / ".failed encoder.log";
    plan.arguments = {
        PULSEFORGE_FAKE_FFMPEG,
        "--fail-after-partial",
        path_utf8(plan.temporary_output_path),
    };
    plan.width = 320U;
    plan.height = 180U;
    plan.fps = 60U;
    plan.frame_count = 1U;
    plan.duration_ms = 1'000.0 / 60.0;

    pulseforge::detail::OfflineEncoder encoder;
    std::string error;
    require(encoder.start(std::move(plan), &error), error);
    SDL_SetRenderDrawColor(renderer, 7U, 11U, 13U, 255U);
    require(SDL_RenderClear(renderer), SDL_GetError());
    require(encoder.write_frame(renderer, &error), error);
    require(!encoder.finish(&error), "non-zero FFmpeg exit must fail finish()");
    require(
        error.find("exit code 17") != std::string::npos,
        "real FFmpeg exit code was not preserved: " + error
    );
    require(
        error.find("intentional encoder failure") != std::string::npos,
        "FFmpeg diagnostics were not retained in the error: " + error
    );
    require(
        !std::filesystem::exists(render_directory / "must-not-exist.mp4")
            && !std::filesystem::exists(render_directory / ".failed partial.mp4")
            && !std::filesystem::exists(render_directory / ".failed encoder.log"),
        "failed FFmpeg output or its private files were published/retained"
    );

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

void slow_consumer_preserves_every_byte_in_frame_order() {
    SdlGuard sdl;
    TemporaryRender temporary;
    // 640x480 RGBA is larger than the 1 MiB synchronous fast-path threshold,
    // so this exercises the bounded asynchronous writer.
    constexpr std::uint32_t width = 640U;
    constexpr std::uint32_t height = 480U;
    constexpr std::uint64_t frame_count = 8U;
    SDL_Window* window = SDL_CreateWindow(
        "offline encoder ordered pipeline test",
        static_cast<int>(width),
        static_cast<int>(height),
        SDL_WINDOW_HIDDEN
    );
    require(window != nullptr, SDL_GetError());
    SDL_Renderer* renderer = SDL_CreateRenderer(window, "software");
    if (renderer == nullptr) {
        SDL_DestroyWindow(window);
        throw std::runtime_error(SDL_GetError());
    }

    pulseforge::OfflineRenderPlan plan;
    plan.ffmpeg_executable = PULSEFORGE_FAKE_FFMPEG;
    plan.final_output_path = temporary.path() / "renders" / "ordered.mp4";
    plan.temporary_output_path = temporary.path() / "renders" / ".ordered.mp4";
    plan.diagnostic_log_path = temporary.path() / "renders" / ".ordered.log";
    plan.arguments = {
        PULSEFORGE_FAKE_FFMPEG,
        "--dump-input",
        "--slow-read-ms=2",
        path_utf8(plan.temporary_output_path),
    };
    plan.width = width;
    plan.height = height;
    plan.fps = 60U;
    plan.frame_count = frame_count;
    plan.duration_ms = static_cast<double>(frame_count) * 1'000.0 / 60.0;

    pulseforge::detail::OfflineEncoder encoder;
    std::string error;
    require(encoder.start(std::move(plan), &error), error);
    std::vector<char> expected;
    expected.reserve(
        static_cast<std::size_t>(width) * height * 4U
            * static_cast<std::size_t>(frame_count)
    );
    for (std::uint64_t frame = 0U; frame < frame_count; ++frame) {
        SDL_SetRenderDrawColor(
            renderer,
            static_cast<std::uint8_t>(17U * frame),
            static_cast<std::uint8_t>(251U - 23U * frame),
            static_cast<std::uint8_t>(31U * frame),
            255U
        );
        require(SDL_RenderClear(renderer), SDL_GetError());
        const auto snapshot = capture_rgba(renderer, width, height);
        expected.insert(expected.end(), snapshot.begin(), snapshot.end());
        require(encoder.write_frame(renderer, &error), error);
    }
    require(encoder.finish(&error), error);

    std::ifstream input(
        temporary.path() / "renders" / "ordered.mp4",
        std::ios::binary
    );
    require(static_cast<bool>(input), "ordered output is missing");
    std::vector<char> actual;
    actual.assign(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );
    require(actual == expected, "async writer lost, duplicated, or reordered bytes");

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

void worker_pipe_error_propagates_without_publication() {
    SdlGuard sdl;
    TemporaryRender temporary;
    constexpr std::uint32_t width = 800U;
    constexpr std::uint32_t height = 600U;
    SDL_Window* window = SDL_CreateWindow(
        "offline encoder worker failure test",
        static_cast<int>(width),
        static_cast<int>(height),
        SDL_WINDOW_HIDDEN
    );
    require(window != nullptr, SDL_GetError());
    SDL_Renderer* renderer = SDL_CreateRenderer(window, "software");
    if (renderer == nullptr) {
        SDL_DestroyWindow(window);
        throw std::runtime_error(SDL_GetError());
    }

    pulseforge::OfflineRenderPlan plan;
    plan.ffmpeg_executable = PULSEFORGE_FAKE_FFMPEG;
    plan.final_output_path = temporary.path() / "renders" / "pipe-error.mp4";
    plan.temporary_output_path = temporary.path() / "renders" / ".pipe-error.mp4";
    plan.diagnostic_log_path = temporary.path() / "renders" / ".pipe-error.log";
    plan.arguments = {
        PULSEFORGE_FAKE_FFMPEG,
        "--fail-after-bytes=1024",
        path_utf8(plan.temporary_output_path),
    };
    plan.width = width;
    plan.height = height;
    plan.fps = 60U;
    plan.frame_count = 2U;
    plan.duration_ms = 1'000.0 / 30.0;

    pulseforge::detail::OfflineEncoder encoder;
    std::string error;
    require(encoder.start(std::move(plan), &error), error);
    SDL_SetRenderDrawColor(renderer, 55U, 89U, 144U, 255U);
    require(SDL_RenderClear(renderer), SDL_GetError());
    require(encoder.write_frame(renderer, &error), error);
    SDL_SetRenderDrawColor(renderer, 233U, 21U, 8U, 255U);
    require(SDL_RenderClear(renderer), SDL_GetError());
    require(encoder.write_frame(renderer, &error), error);
    require(!encoder.finish(&error), "closed FFmpeg pipe must fail finish()");
    require(
        error.find("FFmpeg stopped accepting raw video") != std::string::npos
            || error.find("exit code 19") != std::string::npos,
        "worker/process failure was not propagated: " + error
    );
    require(
        !std::filesystem::exists(temporary.path() / "renders" / "pipe-error.mp4")
            && !std::filesystem::exists(
                temporary.path() / "renders" / ".pipe-error.mp4"
            ),
        "pipe failure published output"
    );

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

void cancellation_unblocks_a_stalled_writer() {
    SdlGuard sdl;
    TemporaryRender temporary;
    constexpr std::uint32_t width = 800U;
    constexpr std::uint32_t height = 600U;
    SDL_Window* window = SDL_CreateWindow(
        "offline encoder stalled cancellation test",
        static_cast<int>(width),
        static_cast<int>(height),
        SDL_WINDOW_HIDDEN
    );
    require(window != nullptr, SDL_GetError());
    SDL_Renderer* renderer = SDL_CreateRenderer(window, "software");
    if (renderer == nullptr) {
        SDL_DestroyWindow(window);
        throw std::runtime_error(SDL_GetError());
    }

    pulseforge::OfflineRenderPlan plan;
    plan.ffmpeg_executable = PULSEFORGE_FAKE_FFMPEG;
    plan.final_output_path = temporary.path() / "renders" / "stalled.mp4";
    plan.temporary_output_path = temporary.path() / "renders" / ".stalled.mp4";
    plan.diagnostic_log_path = temporary.path() / "renders" / ".stalled.log";
    plan.arguments = {
        PULSEFORGE_FAKE_FFMPEG,
        "--initial-delay-ms=10000",
        path_utf8(plan.temporary_output_path),
    };
    plan.width = width;
    plan.height = height;
    plan.fps = 60U;
    plan.frame_count = 5U;
    plan.duration_ms = 1'000.0 / 12.0;

    pulseforge::detail::OfflineEncoder encoder;
    std::string error;
    require(encoder.start(std::move(plan), &error), error);
    SDL_SetRenderDrawColor(renderer, 3U, 5U, 8U, 255U);
    require(SDL_RenderClear(renderer), SDL_GetError());
    require(encoder.write_frame(renderer, &error), error);
    require(encoder.write_frame(renderer, &error), error);
    const auto cancel_started = std::chrono::steady_clock::now();
    encoder.cancel();
    const auto cancel_elapsed = std::chrono::steady_clock::now() - cancel_started;
    require(
        cancel_elapsed < std::chrono::seconds(5),
        "cancel deadlocked behind a blocked pipe write"
    );
    require(!encoder.active(), "cancelled stalled encoder remained active");
    require(
        !std::filesystem::exists(temporary.path() / "renders" / "stalled.mp4")
            && !std::filesystem::exists(
                temporary.path() / "renders" / ".stalled.mp4"
            ),
        "cancelled stalled render left output"
    );

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

void maximum_performance_queue_remains_bounded() {
    // PULSEFORGE_P1_5_0E_BOUNDED_ENCODER_QUEUE_TEST_V1
    TemporaryRender temporary;

    const auto make_plan = [&](const bool maximum_performance,
                               const std::string_view name) {
        pulseforge::OfflineRenderPlan plan;
        plan.ffmpeg_executable = PULSEFORGE_FAKE_FFMPEG;
        plan.final_output_path = temporary.path() / "renders" / std::string(name);
        plan.temporary_output_path = temporary.path() / "renders"
            / ("." + std::string(name));
        plan.diagnostic_log_path = temporary.path() / "renders"
            / ("." + std::string(name) + ".log");
        plan.arguments = {
            PULSEFORGE_FAKE_FFMPEG,
            "--initial-delay-ms=10000",
            path_utf8(plan.temporary_output_path),
        };
        plan.width = 1920U;
        plan.height = 1080U;
        plan.fps = 60U;
        plan.frame_count = 1U;
        plan.duration_ms = 1000.0 / 60.0;
        plan.maximum_performance = maximum_performance;
        return plan;
    };

    std::string error;
    {
        pulseforge::detail::OfflineEncoder normal;
        require(normal.start(make_plan(false, "normal.mp4"), &error), error);
        const auto telemetry = normal.telemetry();
        require(
            telemetry.maximum_frames_in_flight >= 2U
                && telemetry.maximum_frames_in_flight <= 6U,
            "normal encoder queue escaped the 2-6 frame bound"
        );
        normal.cancel();
    }
    {
        pulseforge::detail::OfflineEncoder maximum;
        require(maximum.start(make_plan(true, "maximum.mp4"), &error), error);
        const auto telemetry = maximum.telemetry();
        require(
            telemetry.maximum_frames_in_flight >= 2U
                && telemetry.maximum_frames_in_flight <= 12U,
            "maximum-performance encoder queue escaped the 2-12 frame bound"
        );
        require(
            telemetry.maximum_frames_in_flight > 6U,
            "1080p maximum-performance queue did not expose extra bounded pipelining"
        );
        maximum.cancel();
    }
}

}  // namespace

int main() {
    try {
        process_pipe_and_atomic_publish_work();
        cancellation_removes_private_output();
        nonzero_exit_never_publishes_partial_output();
        slow_consumer_preserves_every_byte_in_frame_order();
        worker_pipe_error_propagates_without_publication();
        cancellation_unblocks_a_stalled_writer();
        maximum_performance_queue_remains_bounded();
        std::cout << "offline encoder process test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "offline encoder process test failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
