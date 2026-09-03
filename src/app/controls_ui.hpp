#pragma once

#include "pulseforge/settings.hpp"

#include <filesystem>

struct SDL_Renderer;
struct SDL_Window;

namespace pulseforge::detail {

// Runs inside the launcher's existing SDL window. No SDL subsystem, window or
// renderer is recreated while controls are inspected or changed.
void show_controls_editor(
    SDL_Window* window,
    SDL_Renderer* renderer,
    EngineSettings& settings,
    const std::filesystem::path& settings_path
);

}  // namespace pulseforge::detail
