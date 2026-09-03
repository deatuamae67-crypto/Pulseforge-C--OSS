#pragma once

#include "pulseforge/settings.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

struct SDL_Renderer;
struct SDL_Window;

namespace pulseforge::detail {

enum class StartupIntroStatus {
    played,
    skipped,
    fallback_played,
    quit_requested,
};

struct StartupIntroResult {
    StartupIntroStatus status{StartupIntroStatus::fallback_played};
    std::string diagnostic;
};

using StartupIntroOverlay = void (*)(
    SDL_Renderer* renderer,
    std::uint64_t elapsed_ms,
    void* userdata
);

struct StartupIntroPlaybackOptions {
    bool allow_decoded_derivative{true};
    bool allow_native_movie{true};
    bool allow_procedural_fallback{true};
    bool loop_until_skip{false};
    bool show_skip_hint{true};
    StartupIntroOverlay overlay{};
    void* overlay_userdata{};
};

[[nodiscard]] std::uint64_t native_intro_timeout_ns(
    std::uint64_t media_duration_100ns
) noexcept;

[[nodiscard]] StartupIntroResult play_startup_intro(
    SDL_Window* window,
    SDL_Renderer* renderer,
    const std::filesystem::path& movie_path,
    const AudioSettings& audio_settings = {},
    bool allow_decoded_derivative = true
);

[[nodiscard]] StartupIntroResult play_startup_intro_ex(
    SDL_Window* window,
    SDL_Renderer* renderer,
    const std::filesystem::path& movie_path,
    const AudioSettings& audio_settings,
    const StartupIntroPlaybackOptions& playback
);

}  // namespace pulseforge::detail
