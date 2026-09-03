#pragma once

#include "pulseforge/chart_loader.hpp"
#include "pulseforge/offline_render.hpp"
#include "pulseforge/settings.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pulseforge {

struct AppLaunchOptions {
    std::filesystem::path chart_path;
    ChartLoadOptions chart_options;
    EngineSettings settings;
    std::filesystem::path settings_path;
    std::optional<std::filesystem::path> instrumental_override;
    std::vector<std::filesystem::path> vocal_overrides;
    std::vector<std::filesystem::path> script_paths;
    // Legacy single-script entry point retained for embedders using 0.1 APIs.
    std::optional<std::filesystem::path> script_path;
    std::optional<std::filesystem::path> replay_path;
    std::optional<std::filesystem::path> save_replay_path;
    std::vector<std::filesystem::path> content_roots;
    std::filesystem::path selected_content_root;
    std::filesystem::path selected_mod_root;
    // Empty selects out/cache/large-charts. JSON sources are never modified;
    // this private cache is used automatically when the materialized loader's
    // byte/note limits would otherwise reject a supported chart.
    std::filesystem::path large_chart_cache_root;
    std::optional<std::string> catalog_song;
    OfflineRenderConfig offline_render;
    bool enable_lua{true};
    bool safe_mode{false};
    bool smoke_test{false};
    // Non-empty only for the engine-owned, intentionally silent diagnostic
    // chart. Runtime code requires an exact normalized path match before it
    // relaxes presentation-theme policies for missing audio.
    std::filesystem::path smoke_test_chart_path;
    bool show_launcher{false};
    bool return_to_launcher{false};
    // Story playlists need to distinguish a clear, a failure and an explicit
    // return to the menu.  Single-song/embedding callers retain the original
    // EXIT_SUCCESS contract when this flag is false.
    bool campaign_mode{false};
    // Story Mode sets this only after the player explicitly chooses to seek
    // the next loadable song. Individual rejected candidates still return a
    // typed failure result, but do not stop the search for acknowledgement.
    bool suppress_load_error_acknowledgement{false};
    bool enable_large_chart_streaming{true};
};

[[nodiscard]] int run_application(const AppLaunchOptions& options);

}  // namespace pulseforge
