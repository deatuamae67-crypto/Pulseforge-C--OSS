#pragma once

#include "pulseforge/gameplay.hpp"
#include "pulseforge/input_bindings.hpp"
#include "pulseforge/runtime_telemetry.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge {

enum class ScrollSpeedMode : std::uint8_t {
    multiplicative,
    constant,
};

enum class PresentationTheme : std::uint8_t {
    pulseforge,
    watch_dogs,
    ps2,
    beverly_hills_90210,
    just_cause_3,
    just_cause_4,
    xbox_original,
    xbox_360,
};

enum class AudioVisualizerImage : std::uint8_t {
    star_of_david,
    circular_symbol,
    hammer_sickle_star,
    custom,
};

// SDL_Renderer-native post effects. These are intentionally distinct from
// ShaderCatalog: the latter discovers compatibility GLSL sources, while this
// enum names effects that the current renderer can execute today.
enum class PostEffect : std::uint8_t {
    off,
    watch_dogs_scanlines,
    rgb_split,
};

struct AudioSettings {
    // The public value remains normalized for DSP, but UI/persistence quantize
    // it to the eleven user-facing levels 0, 10, ... 100 percent.
    float master_volume{1.0F};
    float instrumental_volume{1.0F};
    float vocals_volume{1.0F};
    double audio_offset_ms{};
    double output_latency_compensation_ms{};
    double playback_rate{1.0};
    std::uint32_t sample_rate{48'000};
    std::uint32_t buffer_frames{256};
    // `randomized` selects the complete discovered menu playlist. Any other
    // value is a filename only (never a path) and identifies the user's
    // preferred menu/pause track.
    std::string menu_music_selection{"randomized"};
    // Explicitly user-selected external audio. It is never treated as an
    // executable/content root and is validated as media immediately before use.
    std::string custom_menu_music_path;
    // False is the default shuffled playlist. True repeats the selected track;
    // an unavailable/unsafe selection falls back to randomized playback.
    bool menu_music_loop_selected{false};
    // Menu/pause BGM mute is independent from the master mute so gameplay
    // audio remains audible when the launcher soundtrack is disabled.
    bool menu_music_muted{false};
    bool muted{false};
    bool mute_when_unfocused{true};
};

struct VisualSettings {
    std::int32_t width{1280};
    std::int32_t height{720};
    std::int32_t fps_cap{240};
    float background_dim{0.25F};
    float lane_underlay_opacity{0.45F};
    // Background opacity for the gameplay audio visualizer panel.
    // The icon/bars remain readable while the black backdrop is adjustable.
    float audio_visualizer_background_opacity{0.50F};
    AudioVisualizerImage audio_visualizer_image{
        AudioVisualizerImage::star_of_david
    };
    // User-selected image used only when audio_visualizer_image == custom.
    std::string audio_visualizer_custom_image_path;
    // Overrides only the visual note skin. Note-type gameplay semantics such as
    // Hurt Note / GF Sing remain controlled by the chart itself.
    // "chart-default", or an auto-discovered "atlas:<style>|<root>" /
    // "pixel:<style>|<root>" selection from the installed note-skin catalogue.
    std::string note_skin_selection{"chart-default"};
    ScrollSpeedMode scroll_speed_mode{ScrollSpeedMode::multiplicative};
    bool vsync{false};
    bool fullscreen{false};
    bool low_quality{false};
    bool reduced_motion{false};
    bool note_splashes{true};
    bool flashing_lights{true};
    bool show_fps{true};
    bool show_timing_graph{true};
    PresentationTheme theme{PresentationTheme::pulseforge};
    PostEffect post_effect{PostEffect::off};
    // The movie can be dismissed with Enter or Space while it is playing.
    bool skip_intro{false};
};

struct NormalPerformanceProfile {
    std::int32_t fps_cap{240};
    std::uint32_t audio_buffer_frames{256};
    std::uint32_t max_cosmetic_bursts_per_frame{32};
    bool vsync{false};
    bool low_quality{false};
    bool reduced_motion{false};
    bool note_splashes{true};
    bool flashing_lights{true};
    bool show_timing_graph{true};
    bool hot_reload_scripts{true};
};

struct PerformanceSettings {
    std::uint32_t max_visible_notes{4096};
    std::uint32_t max_cosmetic_bursts_per_frame{32};
    std::uint32_t script_memory_mb{16};
    std::uint32_t script_instruction_budget{1'000'000};
    bool hot_reload_scripts{true};
    bool auto_pause_on_focus_loss{true};
    bool pause_on_controller_disconnect{true};
    // A reversible parallel runtime profile.  normal_profile is persisted so
    // switching it off after a restart restores the user's exact prior state.
    bool maximum_performance_mode{false};
    // Requests the lowest practical scheduler/input/audio latency without
    // switching the whole visual profile to maximum-performance mode.
    bool ultra_low_latency{false};
    std::optional<NormalPerformanceProfile> normal_profile;
};

// Platform-neutral persistence for the Android virtual controller. Desktop
// runtimes retain these values but never activate the overlay unless the
// explicit developer test hook is set.
struct TouchSettings {
    bool gameplay_enabled{true};
    bool show_labels{true};
    bool editor_direct_touch{true};
    float opacity{0.42F};
    float scale{1.0F};
    float horizontal_offset{};
    float vertical_offset{};
    float sensitivity{1.0F};
    float deadzone{0.02F};
    float gameplay_coverage{0.80F};
};

struct KeyBinding {
    std::string key;
    std::uint16_t lane{};

    [[nodiscard]] bool operator==(const KeyBinding&) const = default;
};

struct GamepadBinding {
    std::string button;
    std::uint16_t lane{};

    [[nodiscard]] bool operator==(const GamepadBinding&) const = default;
};

struct EngineSettings {
    GameplaySettings gameplay;
    AudioSettings audio;
    VisualSettings visual;
    PerformanceSettings performance;
    TouchSettings touch;
    // PULSEFORGE_P1_5_0F_DISCORD_PRESENCE_SETTINGS_V1
    DiscordPresenceSettings discord;
    // Authoritative action-based controls. keyboard/gamepad below are a
    // compatibility view consumed by the current gameplay hot path.
    InputBindings controls{default_input_bindings()};
    std::vector<KeyBinding> keyboard{
        {"d", 0}, {"left", 0},
        {"f", 1}, {"down", 1},
        {"j", 2}, {"up", 2},
        {"k", 3}, {"right", 3},
    };
    std::vector<GamepadBinding> gamepad{
        {"dpleft", 0}, {"x", 0},
        {"dpdown", 1}, {"a", 1},
        {"dpup", 2}, {"y", 2},
        {"dpright", 3}, {"b", 3},
    };
};

struct SettingsLoadResult {
    std::optional<EngineSettings> settings;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return settings.has_value();
    }
};

[[nodiscard]] SettingsLoadResult load_settings(const std::filesystem::path& path);
[[nodiscard]] std::string_view post_effect_storage_name(PostEffect effect) noexcept;
[[nodiscard]] std::string_view post_effect_display_name(PostEffect effect) noexcept;
[[nodiscard]] PostEffect next_post_effect(PostEffect effect) noexcept;
// Enables/disables the reversible maximum-performance profile. Enabling
// snapshots all settings it overrides; disabling restores that snapshot.
// Audio playback_rate is deliberately never modified.
void set_maximum_performance_mode(EngineSettings& settings, bool enabled);
// Rebuilds the legacy lane-only view after an action binding edit. The runtime
// will consume this precompiled view until its input router is fully action based.
void rebuild_legacy_lane_bindings(EngineSettings& settings);
[[nodiscard]] bool save_settings(
    const std::filesystem::path& path,
    const EngineSettings& settings,
    std::string* error = nullptr
);

}  // namespace pulseforge
