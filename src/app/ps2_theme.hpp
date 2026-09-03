#pragma once

#include "pulseforge/settings.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>

struct SDL_Renderer;
struct SDL_Window;

namespace pulseforge::detail {

void draw_ps2_startup_overlay(
    SDL_Renderer* renderer,
    std::uint64_t elapsed_ms,
    void* userdata
);

void show_ps2_error_screen(
    SDL_Window* window,
    SDL_Renderer* renderer,
    std::string_view title,
    std::string_view detail,
    std::string_view hint = "PRESS ENTER / SPACE / ESC TO RETURN",
    const std::filesystem::path& background_movie = {},
    const AudioSettings& audio_settings = {}
);

void show_ps2_error_screen_standalone(
    std::string_view title,
    std::string_view detail,
    const std::filesystem::path& background_movie = {},
    const AudioSettings& audio_settings = {}
);

}  // namespace pulseforge::detail
