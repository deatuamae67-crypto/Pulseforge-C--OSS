#include "application_runner.hpp"
#include "discord_presence.hpp"
#include "offline_encoder.hpp"
#include "ps2_theme.hpp"
#include "psych_content_roots.hpp"
#include "psych_camera_target.hpp"
#include "psych_text_layout.hpp"
#include "runtime_post_effects.hpp"
#include "runtime_scene.hpp"
#include "sdl_input_actions.hpp"

#include "pulseforge/audio_controls.hpp"
#include "pulseforge/audio_transport.hpp"
#include "pulseforge/gameplay.hpp"
#include "pulseforge/musical_chart.hpp"
#include "pulseforge/note_render_lod.hpp"
#include "pulseforge/note_skin_catalog.hpp"
#include "pulseforge/note_types.hpp"
#include "pulseforge/replay.hpp"
#include "pulseforge/shader_catalog.hpp"
#include "pulseforge/streaming_chart_importer.hpp"
#include "pulseforge/streaming_gameplay.hpp"
#include "pulseforge/visual_density_index.hpp"

#ifndef PULSEFORGE_PATCH_BUILD
#define PULSEFORGE_PATCH_BUILD "unversioned-runtime"
#endif

#if defined(PULSEFORGE_HAS_LUA)
#include "pulseforge/script_manager.hpp"
#endif

#include <SDL3/SDL.h>
#include <stb_image.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulseforge::detail {

TransferredPlatform::TransferredPlatform(
    SDL_Window* const window_value,
    SDL_Renderer* const renderer_value,
    const bool owns_sdl_value
) noexcept
    : window(window_value),
      renderer(renderer_value),
      owns_sdl(owns_sdl_value) {}

TransferredPlatform::~TransferredPlatform() noexcept {
    reset();
}

TransferredPlatform::TransferredPlatform(TransferredPlatform&& other) noexcept
    : window(std::exchange(other.window, nullptr)),
      renderer(std::exchange(other.renderer, nullptr)),
      owns_sdl(std::exchange(other.owns_sdl, false)) {}
