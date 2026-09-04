#include "pulseforge/settings.hpp"
#include "pulseforge/note_skin_catalog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace pulseforge {
namespace {

using Json = nlohmann::json;

constexpr std::uintmax_t maximum_settings_bytes = 1U * 1024U * 1024U;
constexpr std::size_t maximum_key_bindings = 64U;
constexpr std::size_t maximum_key_name_bytes = 64;
constexpr std::size_t maximum_gamepad_bindings = 64U;
constexpr std::size_t maximum_button_name_bytes = 64;
constexpr std::uint32_t maximum_cosmetic_bursts_per_frame = 32;
constexpr std::size_t maximum_discord_template_bytes = 512U;
constexpr std::size_t maximum_discord_button_label_template_bytes = 128U;
constexpr std::size_t maximum_discord_redirect_uri_bytes = 256U;
std::atomic<std::uint64_t> temporary_file_sequence{};

[[nodiscard]] NormalPerformanceProfile capture_normal_profile(
    const EngineSettings& settings
) noexcept {
    return {
        settings.visual.fps_cap,
        settings.audio.buffer_frames,
        settings.performance.max_cosmetic_bursts_per_frame,
        settings.visual.vsync,
        settings.visual.low_quality,
        settings.visual.reduced_motion,
        settings.visual.note_splashes,
        settings.visual.flashing_lights,
        settings.visual.show_timing_graph,
        settings.performance.hot_reload_scripts,
    };
}

void apply_maximum_performance_overrides(EngineSettings& settings) noexcept {
    settings.visual.vsync = false;
    settings.visual.fps_cap = 0;
    settings.visual.low_quality = true;
    settings.visual.reduced_motion = true;
    settings.visual.note_splashes = false;
    settings.visual.flashing_lights = false;
    settings.visual.show_timing_graph = false;
    // PULSEFORGE_P1_5_0E_MAXIMUM_PERFORMANCE_AUDIO_LATENCY_V1
    // Maximum-performance mode explicitly opts into the smallest supported
    // buffer. AudioTransport retains its normal device/open failure handling.
    settings.audio.buffer_frames = 64U;
    settings.performance.max_cosmetic_bursts_per_frame = 0U;
    settings.performance.hot_reload_scripts = false;
}

void restore_normal_profile(
    EngineSettings& settings,
    const NormalPerformanceProfile& profile
) noexcept {
    settings.visual.fps_cap = profile.fps_cap;
    settings.audio.buffer_frames = profile.audio_buffer_frames;
    settings.performance.max_cosmetic_bursts_per_frame =
        profile.max_cosmetic_bursts_per_frame;
    settings.visual.vsync = profile.vsync;
    settings.visual.low_quality = profile.low_quality;
    settings.visual.reduced_motion = profile.reduced_motion;
    settings.visual.note_splashes = profile.note_splashes;
    settings.visual.flashing_lights = profile.flashing_lights;
    settings.visual.show_timing_graph = profile.show_timing_graph;
    settings.performance.hot_reload_scripts = profile.hot_reload_scripts;
}

template <typename Type>
[[nodiscard]] Type value_or(
    const Json& object,
    const char* key,
    const Type fallback
) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || iterator->is_null()) {
        return fallback;
    }
    try {
        return iterator->get<Type>();
    } catch (...) {
        return fallback;
    }
}

[[nodiscard]] const Json& object_or_empty(const Json& root, const char* key) {
    static const Json empty = Json::object();
    const auto iterator = root.find(key);
    return iterator != root.end() && iterator->is_object() ? *iterator : empty;
}

[[nodiscard]] bool load_bounded_string(
    const Json& object,
    const char* key,
    std::string& target,
    const std::size_t maximum_bytes,
    std::string& error
) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || iterator->is_null()) return true;
    if (!iterator->is_string()) {
        error = std::string("discord.") + key + " must be a string";
        return false;
    }
    const auto value = iterator->get<std::string>();
    if (value.size() > maximum_bytes) {
        error = std::string("discord.") + key + " exceeds the bounded template limit";
        return false;
    }
    target = value;
    return true;
}

[[nodiscard]] bool load_discord_button(
    const Json& discord,
    const char* key,
    DiscordPresenceButtonSettings& target,
    std::string& error
) {
    const auto iterator = discord.find(key);
    if (iterator == discord.end() || iterator->is_null()) return true;
    if (!iterator->is_object()) {
        error = std::string("discord.") + key + " must be an object";
        return false;
    }
    target.enabled = value_or(*iterator, "enabled", target.enabled);
    if (!load_bounded_string(
            *iterator,
            "label",
            target.label,
            maximum_discord_button_label_template_bytes,
            error
        )) {
        error = std::string("discord.") + key + ".label exceeds or violates the bounded template schema";
        return false;
    }
    if (!load_bounded_string(
            *iterator,
            "url",
            target.url,
            maximum_discord_template_bytes,
            error
        )) {
        error = std::string("discord.") + key + ".url exceeds or violates the bounded template schema";
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<std::string> discord_settings_validation_error(
    const DiscordPresenceSettings& settings
) {
    if (settings.oauth_redirect_uri.size() > maximum_discord_redirect_uri_bytes) {
        return "discord.oauthRedirectUri exceeds the bounded URI limit";
    }
    if (!discord_oauth_redirect_uri_valid(settings.oauth_redirect_uri)) {
        return "discord.oauthRedirectUri must be empty, http://127.0.0.1/callback, or a mobile scheme ending in :/authorize/callback";
    }
    const std::array<std::pair<std::string_view, const std::string*>, 11U> templates{{
        {"activityNameTemplate", &settings.activity_name_template},
        {"detailsTemplate", &settings.details_template},
        {"stateTemplate", &settings.state_template},
        {"detailsUrlTemplate", &settings.details_url_template},
        {"stateUrlTemplate", &settings.state_url_template},
        {"largeImageTemplate", &settings.large_image_template},
        {"largeTextTemplate", &settings.large_text_template},
        {"largeUrlTemplate", &settings.large_url_template},
        {"smallImageTemplate", &settings.small_image_template},
        {"smallTextTemplate", &settings.small_text_template},
        {"smallUrlTemplate", &settings.small_url_template},
    }};
    for (const auto& [name, value] : templates) {
        if (value->size() > maximum_discord_template_bytes) {
            return std::string("discord.") + std::string(name)
                + " exceeds the bounded template limit";
        }
    }
    const auto validate_button = [](
        const DiscordPresenceButtonSettings& button,
        const std::string_view name
    ) -> std::optional<std::string> {
        if (button.label.size() > maximum_discord_button_label_template_bytes) {
            return std::string("discord.") + std::string(name)
                + ".label exceeds the bounded template limit";
        }
        if (button.url.size() > maximum_discord_template_bytes) {
            return std::string("discord.") + std::string(name)
                + ".url exceeds the bounded template limit";
        }
        return std::nullopt;
    };
    if (auto error = validate_button(settings.button1, "button1")) return error;
    if (auto error = validate_button(settings.button2, "button2")) return error;
    return std::nullopt;
}

[[nodiscard]] std::string_view audio_visualizer_image_storage_name(
    const AudioVisualizerImage image
) noexcept {
    switch (image) {
    case AudioVisualizerImage::star_of_david:
        return "star-of-david";
    case AudioVisualizerImage::circular_symbol:
        return "circular-symbol";
    case AudioVisualizerImage::hammer_sickle_star:
        return "hammer-sickle-star";
    case AudioVisualizerImage::custom:
        return "custom";
    }
    return "star-of-david";
}

[[nodiscard]] std::string_view presentation_theme_storage_name(
    const PresentationTheme theme
) noexcept {
    switch (theme) {
    case PresentationTheme::pulseforge: return "pulseforge";
    case PresentationTheme::watch_dogs: return "watch-dogs";
    case PresentationTheme::ps2: return "ps2";
    case PresentationTheme::beverly_hills_90210: return "beverly-hills-90210";
    case PresentationTheme::just_cause_3: return "just-cause-3";
    case PresentationTheme::just_cause_4: return "just-cause-4";
    case PresentationTheme::xbox_original: return "xbox-original";
    case PresentationTheme::xbox_360: return "xbox-360";
    }
    return "pulseforge";
}

[[nodiscard]] std::string_view discord_presence_privacy_storage_name(
    const DiscordPresencePrivacy privacy
) noexcept {
    switch (privacy) {
    case DiscordPresencePrivacy::full: return "full";
    case DiscordPresencePrivacy::reduced: return "reduced";
    case DiscordPresencePrivacy::minimal: return "minimal";
    }
    return "full";
}

[[nodiscard]] std::string binding_description(const PhysicalInput& input) {
    return std::string(to_string(input.device)) + ":" + input.name;
}

void clear_legacy_device_note_bindings(
    InputBindings& bindings,
    const InputDevice device
) {
    for (auto& entry : bindings.actions) {
        if (!lane_for_input_action(entry.action).has_value()) {
            continue;
        }
        std::erase_if(entry.inputs, [device](const PhysicalInput& input) {
            return input.device == device;
        });
    }
}

void migrate_legacy_lane_bindings(
    EngineSettings& settings,
    const bool keyboard_was_present,
    const bool gamepad_was_present
) {
    if (keyboard_was_present) {
        clear_legacy_device_note_bindings(settings.controls, InputDevice::keyboard);
        for (const auto& binding : settings.keyboard) {
            static_cast<void>(add_input_binding(
                settings.controls,
                "note_" + std::to_string(binding.lane),
                {InputDevice::keyboard, binding.key},
                BindingConflictPolicy::reject
            ));
        }
    }
    if (gamepad_was_present) {
        clear_legacy_device_note_bindings(settings.controls, InputDevice::gamepad);
        for (const auto& binding : settings.gamepad) {
            static_cast<void>(add_input_binding(
                settings.controls,
                "note_" + std::to_string(binding.lane),
                {InputDevice::gamepad, binding.button},
                BindingConflictPolicy::reject
            ));
        }
    }
}

[[nodiscard]] bool write_text_atomically(
    const std::filesystem::path& path,
    const std::string& text,
    std::string* error
) {
    if (path.empty() || path.filename().empty()) {
        if (error != nullptr) {
            *error = "settings path must name a file";
        }
        return false;
    }
    std::error_code filesystem_error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error) {
            if (error != nullptr) {
                *error = "cannot create settings directory: "
                    + filesystem_error.message();
            }
            return false;
        }
    }

    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    const auto sequence = temporary_file_sequence.fetch_add(
        1U,
        std::memory_order_relaxed
    );
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(stamp) + "-" + std::to_string(sequence);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (error != nullptr) {
                *error = "cannot create temporary settings file: "
                    + temporary.string();
            }
            return false;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, filesystem_error);
            if (error != nullptr) {
                *error = "failed while writing temporary settings file";
            }
            return false;
        }
    }

#if defined(_WIN32)
    if (MoveFileExW(
            temporary.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
        ) == 0) {
        const auto native_error = static_cast<unsigned long>(GetLastError());
        std::filesystem::remove(temporary, filesystem_error);
        if (error != nullptr) {
            *error = "cannot atomically replace settings file (Windows error "
                + std::to_string(native_error) + ")";
        }
        return false;
    }
#else
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary, filesystem_error);
        if (error != nullptr) {
            *error = "cannot atomically replace settings file: "
                + filesystem_error.message();
        }
        return false;
    }
#endif
    return true;
}

void sanitize(EngineSettings& settings) {
    sanitize_gameplay_settings(settings.gameplay);
    auto& windows = settings.gameplay.windows;
    windows.marvelous_ms = std::clamp(windows.marvelous_ms, 1.0, 500.0);
    windows.sick_ms = std::clamp(windows.sick_ms, windows.marvelous_ms, 500.0);
    windows.good_ms = std::clamp(windows.good_ms, windows.sick_ms, 500.0);
    windows.bad_ms = std::clamp(windows.bad_ms, windows.good_ms, 500.0);
    windows.miss_ms = std::clamp(windows.miss_ms, windows.bad_ms, 1'000.0);
    settings.gameplay.scroll_speed =
        std::clamp(settings.gameplay.scroll_speed, 0.1, 10.0);
    settings.gameplay.input_offset_ms =
        std::clamp(settings.gameplay.input_offset_ms, -1'000.0, 1'000.0);
    settings.gameplay.visual_offset_ms =
        std::clamp(settings.gameplay.visual_offset_ms, -1'000.0, 1'000.0);
    settings.gameplay.health_gain =
        std::clamp(settings.gameplay.health_gain, 0.0, 2.0);
    settings.gameplay.health_loss =
        std::clamp(settings.gameplay.health_loss, 0.0, 2.0);

    settings.audio.master_volume = std::clamp(
        std::round(settings.audio.master_volume * 10.0F) / 10.0F,
        0.0F,
        1.0F
    );
    settings.audio.instrumental_volume =
        std::clamp(settings.audio.instrumental_volume, 0.0F, 2.0F);
    settings.audio.vocals_volume =
        std::clamp(settings.audio.vocals_volume, 0.0F, 2.0F);
    settings.audio.audio_offset_ms = std::clamp(
        settings.audio.audio_offset_ms,
        -10'000.0,
        10'000.0
    );
    settings.audio.output_latency_compensation_ms = std::clamp(
        settings.audio.output_latency_compensation_ms,
        -10'000.0,
        10'000.0
    );
    settings.audio.playback_rate =
        std::clamp(settings.audio.playback_rate, 0.25, 4.0);
    settings.audio.sample_rate =
        std::clamp<std::uint32_t>(settings.audio.sample_rate, 8'000, 192'000);
    settings.audio.buffer_frames =
        std::clamp<std::uint32_t>(settings.audio.buffer_frames, 64, 8'192);
    constexpr std::size_t maximum_menu_music_name_bytes = 255U;
    const auto& menu_music = settings.audio.menu_music_selection;
    const bool safe_menu_music = !menu_music.empty()
        && menu_music.size() <= maximum_menu_music_name_bytes
        && menu_music != "."
        && menu_music != ".."
        && menu_music.find_first_of("/\\") == std::string::npos
        && menu_music.find('\0') == std::string::npos
        && std::filesystem::path(menu_music).filename() == menu_music;
    if (!safe_menu_music) {
        settings.audio.menu_music_selection = "randomized";
        settings.audio.menu_music_loop_selected = false;
    } else if (settings.audio.menu_music_selection == "randomized") {
        settings.audio.menu_music_loop_selected = false;
    }
    // PULSEFORGE_P1_5_0E_USER_MEDIA_SETTINGS_V1
    constexpr std::size_t maximum_user_media_path_bytes = 4'096U;
    const auto sanitize_user_media_path = [](std::string& value) {
        if (value.size() > maximum_user_media_path_bytes
            || value.find('\0') != std::string::npos) {
            value.clear();
        }
    };
    sanitize_user_media_path(settings.audio.custom_menu_music_path);
    sanitize_user_media_path(
        settings.visual.audio_visualizer_custom_image_path
    );

    settings.visual.width = std::clamp(settings.visual.width, 640, 7'680);
    settings.visual.height = std::clamp(settings.visual.height, 360, 4'320);
    settings.visual.fps_cap = std::clamp(settings.visual.fps_cap, 0, 1'000);
    settings.visual.background_dim =
        std::clamp(settings.visual.background_dim, 0.0F, 1.0F);
    settings.visual.lane_underlay_opacity =
        std::clamp(settings.visual.lane_underlay_opacity, 0.0F, 1.0F);
    settings.visual.audio_visualizer_background_opacity = std::clamp(
        settings.visual.audio_visualizer_background_opacity,
        0.0F,
        1.0F
    );

    settings.touch.opacity = std::clamp(settings.touch.opacity, 0.05F, 1.0F);
    settings.touch.scale = std::clamp(settings.touch.scale, 0.65F, 1.60F);
    settings.touch.horizontal_offset = std::clamp(
        settings.touch.horizontal_offset,
        -0.25F,
        0.25F
    );
    settings.touch.vertical_offset = std::clamp(
        settings.touch.vertical_offset,
        -0.25F,
        0.25F
    );
    settings.touch.sensitivity = std::clamp(
        settings.touch.sensitivity,
        0.50F,
        2.0F
    );
    settings.touch.deadzone = std::clamp(settings.touch.deadzone, 0.0F, 0.20F);
    settings.touch.gameplay_coverage = std::clamp(
        settings.touch.gameplay_coverage,
        0.35F,
        1.0F
    );

    settings.performance.max_visible_notes =
        std::clamp<std::uint32_t>(settings.performance.max_visible_notes, 64, 1'000'000);
    settings.performance.max_cosmetic_bursts_per_frame =
        std::min(
            settings.performance.max_cosmetic_bursts_per_frame,
            maximum_cosmetic_bursts_per_frame
        );
    settings.performance.script_memory_mb =
        std::clamp<std::uint32_t>(settings.performance.script_memory_mb, 1, 1'024);
    settings.performance.script_instruction_budget =
        std::clamp<std::uint32_t>(
            settings.performance.script_instruction_budget,
            10'000,
            100'000'000
        );
    if (settings.performance.normal_profile.has_value()) {
        auto& profile = *settings.performance.normal_profile;
        profile.fps_cap = std::clamp(profile.fps_cap, 0, 1'000);
        profile.audio_buffer_frames = std::clamp<std::uint32_t>(
            profile.audio_buffer_frames,
            64U,
            8'192U
        );
        profile.max_cosmetic_bursts_per_frame = std::min(
            profile.max_cosmetic_bursts_per_frame,
            maximum_cosmetic_bursts_per_frame
        );
    }
    if (settings.performance.maximum_performance_mode) {
        if (!settings.performance.normal_profile.has_value()) {
            const EngineSettings defaults;
            settings.performance.normal_profile = capture_normal_profile(defaults);
        }
        apply_maximum_performance_overrides(settings);
    } else {
        settings.performance.normal_profile.reset();
    }
}

}  // namespace

std::string_view post_effect_storage_name(const PostEffect effect) noexcept {
    switch (effect) {
    case PostEffect::off: return "off";
    case PostEffect::watch_dogs_scanlines: return "watch-dogs-scanlines";
    case PostEffect::rgb_split: return "rgb-split";
    }
    return "off";
}

std::string_view post_effect_display_name(const PostEffect effect) noexcept {
    switch (effect) {
    case PostEffect::off: return "Off";
    case PostEffect::watch_dogs_scanlines: return "Watch Dogs Scanlines";
    case PostEffect::rgb_split: return "RGB Split";
    }
    return "Off";
}

PostEffect next_post_effect(const PostEffect effect) noexcept {
    switch (effect) {
    case PostEffect::off: return PostEffect::watch_dogs_scanlines;
    case PostEffect::watch_dogs_scanlines: return PostEffect::rgb_split;
    case PostEffect::rgb_split: return PostEffect::off;
    }
    return PostEffect::off;
}

void set_maximum_performance_mode(
    EngineSettings& settings,
    const bool enabled
) {
    if (enabled) {
        if (!settings.performance.maximum_performance_mode) {
            settings.performance.normal_profile = capture_normal_profile(settings);
        } else if (!settings.performance.normal_profile.has_value()) {
            const EngineSettings defaults;
            settings.performance.normal_profile = capture_normal_profile(defaults);
        }
        settings.performance.maximum_performance_mode = true;
        apply_maximum_performance_overrides(settings);
        return;
    }

    if (settings.performance.maximum_performance_mode
        && settings.performance.normal_profile.has_value()) {
        restore_normal_profile(
            settings,
            *settings.performance.normal_profile
        );
    }
    settings.performance.maximum_performance_mode = false;
    settings.performance.normal_profile.reset();
}

void rebuild_legacy_lane_bindings(EngineSettings& settings) {
    settings.keyboard.clear();
    settings.gamepad.clear();
    for (const auto& entry : settings.controls.actions) {
        const auto lane = lane_for_input_action(entry.action);
        if (!lane.has_value()) {
            continue;
        }
        for (const auto& input : entry.inputs) {
            if (input.device == InputDevice::keyboard
                && settings.keyboard.size() < maximum_key_bindings) {
                settings.keyboard.push_back({input.name, *lane});
            } else if (input.device == InputDevice::gamepad
                && settings.gamepad.size() < maximum_gamepad_bindings) {
                settings.gamepad.push_back({input.name, *lane});
            }
        }
    }
}

SettingsLoadResult load_settings(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return {std::nullopt, "cannot open settings file: " + path.string()};
    }
    const auto end = input.tellg();
    if (end < std::streampos{0}
        || static_cast<std::uintmax_t>(end) > maximum_settings_bytes) {
        return {
            std::nullopt,
            "settings file exceeds the 1 MiB safety limit",
        };
    }
    std::string json_text(static_cast<std::size_t>(end), '\0');
    input.seekg(0, std::ios::beg);
    if (!json_text.empty()) {
        input.read(json_text.data(), static_cast<std::streamsize>(json_text.size()));
    }
    if (!input) {
        return {std::nullopt, "failed while reading settings file"};
    }

    try {
        const auto root = Json::parse(json_text, nullptr, true, true);
        if (!root.is_object()) {
            return {std::nullopt, "settings root must be a JSON object"};
        }

        EngineSettings settings;
        const auto& gameplay = object_or_empty(root, "gameplay");
        const auto& judgments = object_or_empty(gameplay, "judgments");
        settings.gameplay.windows.marvelous_ms = value_or(
            judgments,
            "marvelousMs",
            settings.gameplay.windows.marvelous_ms
        );
        settings.gameplay.windows.sick_ms = value_or(
            judgments,
            "sickMs",
            settings.gameplay.windows.sick_ms
        );
        settings.gameplay.windows.good_ms = value_or(
            judgments,
            "goodMs",
            settings.gameplay.windows.good_ms
        );
        settings.gameplay.windows.bad_ms = value_or(
            judgments,
            "badMs",
            settings.gameplay.windows.bad_ms
        );
        settings.gameplay.windows.miss_ms = value_or(
            judgments,
            "missMs",
            settings.gameplay.windows.miss_ms
        );
        settings.gameplay.input_offset_ms = value_or(
            gameplay,
            "inputOffsetMs",
            settings.gameplay.input_offset_ms
        );
        settings.gameplay.visual_offset_ms = value_or(
            gameplay,
            "visualOffsetMs",
            settings.gameplay.visual_offset_ms
        );
        settings.gameplay.release_grace_ms = value_or(
            gameplay,
            "releaseGraceMs",
            settings.gameplay.release_grace_ms
        );
        settings.gameplay.stacked_note_tolerance_ms = value_or(
            gameplay,
            "stackedNoteToleranceMs",
            settings.gameplay.stacked_note_tolerance_ms
        );
        settings.gameplay.health_gain = value_or(
            gameplay,
            "healthGain",
            settings.gameplay.health_gain
        );
        settings.gameplay.health_loss = value_or(
            gameplay,
            "healthLoss",
            settings.gameplay.health_loss
        );
        settings.gameplay.ghost_tap_health_loss = value_or(
            gameplay,
            "ghostTapHealthLoss",
            settings.gameplay.ghost_tap_health_loss
        );
        settings.gameplay.scroll_speed = value_or(
            gameplay,
            "scrollSpeed",
            settings.gameplay.scroll_speed
        );
        settings.gameplay.random_seed = value_or(
            gameplay,
            "randomSeed",
            settings.gameplay.random_seed
        );
        settings.gameplay.ghost_tapping = value_or(
            gameplay,
            "ghostTapping",
            settings.gameplay.ghost_tapping
        );
        settings.gameplay.autoplay = value_or(
            gameplay,
            "autoplay",
            settings.gameplay.autoplay
        );
        settings.gameplay.practice = value_or(
            gameplay,
            "practice",
            settings.gameplay.practice
        );
        settings.gameplay.no_fail = value_or(
            gameplay,
            "noFail",
            settings.gameplay.no_fail
        );
        settings.gameplay.mirror = value_or(
            gameplay,
            "mirror",
            settings.gameplay.mirror
        );
        settings.gameplay.randomize_lanes = value_or(
            gameplay,
            "randomizeLanes",
            settings.gameplay.randomize_lanes
        );
        settings.gameplay.downscroll = value_or(
            gameplay,
            "downscroll",
            settings.gameplay.downscroll
        );
        settings.gameplay.middle_scroll = value_or(
            gameplay,
            "middleScroll",
            settings.gameplay.middle_scroll
        );
        settings.gameplay.hide_opponent_notes = value_or(
            gameplay,
            "hideOpponentNotes",
            settings.gameplay.hide_opponent_notes
        );

        const auto& audio = object_or_empty(root, "audio");
        settings.audio.master_volume =
            value_or(audio, "masterVolume", settings.audio.master_volume);
        settings.audio.instrumental_volume =
            value_or(audio, "instrumentalVolume", settings.audio.instrumental_volume);
        settings.audio.vocals_volume =
            value_or(audio, "vocalsVolume", settings.audio.vocals_volume);
        settings.audio.audio_offset_ms =
            value_or(audio, "audioOffsetMs", settings.audio.audio_offset_ms);
        settings.audio.output_latency_compensation_ms = value_or(
            audio,
            "outputLatencyCompensationMs",
            settings.audio.output_latency_compensation_ms
        );
        settings.audio.playback_rate =
            value_or(audio, "playbackRate", settings.audio.playback_rate);
        settings.audio.sample_rate =
            value_or(audio, "sampleRate", settings.audio.sample_rate);
        settings.audio.buffer_frames =
            value_or(audio, "bufferFrames", settings.audio.buffer_frames);
        settings.audio.menu_music_selection = value_or(
            audio,
            "menuMusicSelection",
            settings.audio.menu_music_selection
        );
        settings.audio.custom_menu_music_path = value_or(
            audio,
            "customMenuMusicPath",
            settings.audio.custom_menu_music_path
        );
        settings.audio.menu_music_loop_selected = value_or(
            audio,
            "menuMusicLoopSelected",
            settings.audio.menu_music_loop_selected
        );
        settings.audio.menu_music_muted = value_or(
            audio,
            "menuMusicMuted",
            settings.audio.menu_music_muted
        );
        settings.audio.muted =
            value_or(audio, "muted", settings.audio.muted);
        settings.audio.mute_when_unfocused =
            value_or(audio, "muteWhenUnfocused", settings.audio.mute_when_unfocused);

        const auto& visual = object_or_empty(root, "visual");
        settings.visual.width = value_or(visual, "width", settings.visual.width);
        settings.visual.height = value_or(visual, "height", settings.visual.height);
        settings.visual.fps_cap = value_or(visual, "fpsCap", settings.visual.fps_cap);
        settings.visual.background_dim =
            value_or(visual, "backgroundDim", settings.visual.background_dim);
        settings.visual.lane_underlay_opacity = value_or(
            visual,
            "laneUnderlayOpacity",
            settings.visual.lane_underlay_opacity
        );
        settings.visual.audio_visualizer_background_opacity = value_or(
            visual,
            "audioVisualizerBackgroundOpacity",
            settings.visual.audio_visualizer_background_opacity
        );
        if (const auto visualizer_image = visual.find("audioVisualizerImage");
            visualizer_image != visual.end() && !visualizer_image->is_null()) {
            if (!visualizer_image->is_string()) {
                return {
                    std::nullopt,
                    "visual.audioVisualizerImage must be a string",
                };
            }
            const auto name = visualizer_image->get<std::string>();
            if (name == "star-of-david" || name == "star_of_david") {
                settings.visual.audio_visualizer_image =
                    AudioVisualizerImage::star_of_david;
            } else if (name == "circular-symbol"
                       || name == "circular_symbol") {
                settings.visual.audio_visualizer_image =
                    AudioVisualizerImage::circular_symbol;
            } else if (name == "hammer-sickle-star"
                       || name == "hammer_sickle_star") {
                settings.visual.audio_visualizer_image =
                    AudioVisualizerImage::hammer_sickle_star;
            } else if (name == "custom") {
                settings.visual.audio_visualizer_image =
                    AudioVisualizerImage::custom;
            } else {
                return {
                    std::nullopt,
                    "visual.audioVisualizerImage must be star-of-david, "
                    "circular-symbol, hammer-sickle-star, or custom",
                };
            }
        }
        settings.visual.audio_visualizer_custom_image_path = value_or(
            visual,
            "audioVisualizerCustomImagePath",
            settings.visual.audio_visualizer_custom_image_path
        );
        if (const auto note_skin = visual.find("noteSkinSelection");
            note_skin != visual.end() && !note_skin->is_null()) {
            if (!note_skin->is_string()) {
                return {
                    std::nullopt,
                    "visual.noteSkinSelection must be a string",
                };
            }
            const auto name = note_skin->get<std::string>();
            if (!valid_note_skin_selection(name)) {
                return {
                    std::nullopt,
                    "visual.noteSkinSelection is not a valid note-skin "
                    "catalogue selection",
                };
            }
            settings.visual.note_skin_selection =
                normalize_note_skin_selection(name);
        }
        if (const auto scroll_speed_mode = visual.find("scrollSpeedMode");
            scroll_speed_mode != visual.end() && !scroll_speed_mode->is_null()) {
            if (!scroll_speed_mode->is_string()) {
                return {
                    std::nullopt,
                    "visual.scrollSpeedMode must be a string",
                };
            }
            const auto mode = scroll_speed_mode->get<std::string>();
            if (mode == "constant") {
                settings.visual.scroll_speed_mode = ScrollSpeedMode::constant;
            } else if (mode == "multiplicative") {
                settings.visual.scroll_speed_mode =
                    ScrollSpeedMode::multiplicative;
            } else {
                return {
                    std::nullopt,
                    "visual.scrollSpeedMode must be constant or multiplicative",
                };
            }
        }
        settings.visual.vsync = value_or(visual, "vsync", settings.visual.vsync);
        settings.visual.fullscreen =
            value_or(visual, "fullscreen", settings.visual.fullscreen);
        settings.visual.low_quality =
            value_or(visual, "lowQuality", settings.visual.low_quality);
        settings.visual.reduced_motion =
            value_or(visual, "reducedMotion", settings.visual.reduced_motion);
        settings.visual.note_splashes =
            value_or(visual, "noteSplashes", settings.visual.note_splashes);
        settings.visual.flashing_lights =
            value_or(visual, "flashingLights", settings.visual.flashing_lights);
        settings.visual.show_fps =
            value_or(visual, "showFps", settings.visual.show_fps);
        settings.visual.show_timing_graph =
            value_or(visual, "showTimingGraph", settings.visual.show_timing_graph);
        settings.visual.skip_intro =
            value_or(visual, "skipIntro", settings.visual.skip_intro);
        if (const auto post_effect = visual.find("postEffect");
            post_effect != visual.end() && !post_effect->is_null()) {
            if (!post_effect->is_string()) {
                return {std::nullopt, "visual.postEffect must be a string"};
            }
            const auto name = post_effect->get<std::string>();
            if (name == "off") {
                settings.visual.post_effect = PostEffect::off;
            } else if (name == "watch-dogs-scanlines"
                       || name == "watch_dogs_scanlines") {
                settings.visual.post_effect = PostEffect::watch_dogs_scanlines;
            } else if (name == "rgb-split" || name == "rgb_split") {
                settings.visual.post_effect = PostEffect::rgb_split;
            } else {
                return {
                    std::nullopt,
                    "visual.postEffect must be off, watch-dogs-scanlines, or rgb-split",
                };
            }
        }
        if (const auto theme = visual.find("theme");
            theme != visual.end() && !theme->is_null()) {
            if (!theme->is_string()) {
                return {std::nullopt, "visual.theme must be a string"};
            }
            const auto name = theme->get<std::string>();
            if (name == "pulseforge" || name == "default") {
                settings.visual.theme = PresentationTheme::pulseforge;
            } else if (name == "watch-dogs" || name == "watch_dogs") {
                settings.visual.theme = PresentationTheme::watch_dogs;
            } else if (name == "ps2" || name == "playstation-2"
                       || name == "playstation2") {
                settings.visual.theme = PresentationTheme::ps2;
            } else if (name == "beverly-hills-90210"
                       || name == "beverly_hills_90210"
                       || name == "90210") {
                settings.visual.theme = PresentationTheme::beverly_hills_90210;
            } else if (name == "just-cause-3" || name == "just_cause_3") {
                settings.visual.theme = PresentationTheme::just_cause_3;
            } else if (name == "just-cause-4" || name == "just_cause_4") {
                settings.visual.theme = PresentationTheme::just_cause_4;
            } else if (name == "xbox-original" || name == "xbox_original") {
                settings.visual.theme = PresentationTheme::xbox_original;
            } else if (name == "xbox-360" || name == "xbox_360") {
                settings.visual.theme = PresentationTheme::xbox_360;
            } else {
                return {
                    std::nullopt,
                    "visual.theme must be pulseforge, watch-dogs, ps2, "
                    "beverly-hills-90210, just-cause-3, just-cause-4, "
                    "xbox-original, or xbox-360",
                };
            }
        }

        const auto& performance = object_or_empty(root, "performance");
        settings.performance.max_visible_notes = value_or(
            performance,
            "maxVisibleNotes",
            settings.performance.max_visible_notes
        );
        settings.performance.max_cosmetic_bursts_per_frame = value_or(
            performance,
            "maxCosmeticBurstsPerFrame",
            settings.performance.max_cosmetic_bursts_per_frame
        );
        settings.performance.script_memory_mb = value_or(
            performance,
            "scriptMemoryMb",
            settings.performance.script_memory_mb
        );
        settings.performance.script_instruction_budget = value_or(
            performance,
            "scriptInstructionBudget",
            settings.performance.script_instruction_budget
        );
        settings.performance.hot_reload_scripts = value_or(
            performance,
            "hotReloadScripts",
            settings.performance.hot_reload_scripts
        );
        settings.performance.auto_pause_on_focus_loss = value_or(
            performance,
            "autoPauseOnFocusLoss",
            settings.performance.auto_pause_on_focus_loss
        );
        settings.performance.pause_on_controller_disconnect = value_or(
            performance,
            "pauseOnControllerDisconnect",
            settings.performance.pause_on_controller_disconnect
        );
        settings.performance.maximum_performance_mode = value_or(
            performance,
            "maximumPerformanceMode",
            settings.performance.maximum_performance_mode
        );
        settings.performance.ultra_low_latency = value_or(
            performance,
            "ultraLowLatency",
            settings.performance.ultra_low_latency
        );
        // PULSEFORGE_P1_5_0F_DISCORD_PRESENCE_SETTINGS_LOAD_V1
        const auto& discord = object_or_empty(root, "discord");
        settings.discord.enabled = value_or(
            discord,
            "enabled",
            settings.discord.enabled
        );
        settings.discord.application_id = value_or(
            discord,
            "applicationId",
            settings.discord.application_id
        );
        std::string discord_template_error;
        if (!load_bounded_string(
                discord,
                "oauthRedirectUri",
                settings.discord.oauth_redirect_uri,
                maximum_discord_redirect_uri_bytes,
                discord_template_error
            )) {
            return {std::nullopt, discord_template_error};
        }
        settings.discord.show_chart_name = value_or(
            discord,
            "showChartName",
            settings.discord.show_chart_name
        );
        settings.discord.show_difficulty_mania = value_or(
            discord,
            "showDifficultyMania",
            settings.discord.show_difficulty_mania
        );
        settings.discord.show_progress = value_or(
            discord,
            "showProgress",
            settings.discord.show_progress
        );
        settings.discord.show_note_counter = value_or(
            discord,
            "showNoteCounter",
            settings.discord.show_note_counter
        );
        settings.discord.show_gameplay_stats = value_or(
            discord,
            "showGameplayStats",
            settings.discord.show_gameplay_stats
        );
        settings.discord.show_botplay = value_or(
            discord,
            "showBotplay",
            settings.discord.show_botplay
        );
        settings.discord.show_remaining_time = value_or(
            discord,
            "showRemainingTime",
            settings.discord.show_remaining_time
        );
        settings.discord.show_mod_name = value_or(
            discord,
            "showModName",
            settings.discord.show_mod_name
        );
        settings.discord.advanced_customization = value_or(
            discord,
            "advancedCustomization",
            settings.discord.advanced_customization
        );
        settings.discord.retry_failed_updates = value_or(
            discord,
            "retryFailedUpdates",
            settings.discord.retry_failed_updates
        );
        const std::array template_fields{
            std::tuple{"activityNameTemplate", &settings.discord.activity_name_template},
            std::tuple{"detailsTemplate", &settings.discord.details_template},
            std::tuple{"stateTemplate", &settings.discord.state_template},
            std::tuple{"detailsUrlTemplate", &settings.discord.details_url_template},
            std::tuple{"stateUrlTemplate", &settings.discord.state_url_template},
            std::tuple{"largeImageTemplate", &settings.discord.large_image_template},
            std::tuple{"largeTextTemplate", &settings.discord.large_text_template},
            std::tuple{"largeUrlTemplate", &settings.discord.large_url_template},
            std::tuple{"smallImageTemplate", &settings.discord.small_image_template},
            std::tuple{"smallTextTemplate", &settings.discord.small_text_template},
            std::tuple{"smallUrlTemplate", &settings.discord.small_url_template},
        };
        for (const auto& [key, target] : template_fields) {
            if (!load_bounded_string(
                    discord,
                    key,
                    *target,
                    maximum_discord_template_bytes,
                    discord_template_error
                )) {
                return {std::nullopt, discord_template_error};
            }
        }
        if (!load_discord_button(
                discord,
                "button1",
                settings.discord.button1,
                discord_template_error
            )
            || !load_discord_button(
                discord,
                "button2",
                settings.discord.button2,
                discord_template_error
            )) {
            return {std::nullopt, discord_template_error};
        }
        settings.discord.publish_interval_ms = std::clamp(
            value_or(
                discord,
                "publishIntervalMs",
                settings.discord.publish_interval_ms
            ),
            2'000U,
            5'000U
        );
        if (const auto privacy = discord.find("privacy");
            privacy != discord.end() && !privacy->is_null()) {
            if (!privacy->is_string()) {
                return {std::nullopt, "discord.privacy must be full, reduced, or minimal"};
            }
            const auto name = privacy->get<std::string>();
            if (name == "full") {
                settings.discord.privacy = DiscordPresencePrivacy::full;
            } else if (name == "reduced") {
                settings.discord.privacy = DiscordPresencePrivacy::reduced;
            } else if (name == "minimal") {
                settings.discord.privacy = DiscordPresencePrivacy::minimal;
            } else {
                return {std::nullopt, "discord.privacy must be full, reduced, or minimal"};
            }
        }

        if (const auto profile = performance.find("normalProfile");
            profile != performance.end() && !profile->is_null()) {
            if (!profile->is_object()) {
                return {
                    std::nullopt,
                    "performance.normalProfile must be an object or null",
                };
            }
            auto normal = capture_normal_profile(settings);
            normal.fps_cap = value_or(*profile, "fpsCap", normal.fps_cap);
            normal.audio_buffer_frames = value_or(
                *profile,
                "audioBufferFrames",
                normal.audio_buffer_frames
            );
            normal.max_cosmetic_bursts_per_frame = value_or(
                *profile,
                "maxCosmeticBurstsPerFrame",
                normal.max_cosmetic_bursts_per_frame
            );
            normal.vsync = value_or(*profile, "vsync", normal.vsync);
            normal.low_quality = value_or(
                *profile,
                "lowQuality",
                normal.low_quality
            );
            normal.reduced_motion = value_or(
                *profile,
                "reducedMotion",
                normal.reduced_motion
            );
            normal.note_splashes = value_or(
                *profile,
                "noteSplashes",
                normal.note_splashes
            );
            normal.flashing_lights = value_or(
                *profile,
                "flashingLights",
                normal.flashing_lights
            );
            normal.show_timing_graph = value_or(
                *profile,
                "showTimingGraph",
                normal.show_timing_graph
            );
            normal.hot_reload_scripts = value_or(
                *profile,
                "hotReloadScripts",
                normal.hot_reload_scripts
            );
            settings.performance.normal_profile = normal;
        }

        const auto& touch = object_or_empty(root, "touch");
        settings.touch.gameplay_enabled = value_or(
            touch,
            "gameplayEnabled",
            settings.touch.gameplay_enabled
        );
        settings.touch.show_labels = value_or(
            touch,
            "showLabels",
            settings.touch.show_labels
        );
        settings.touch.editor_direct_touch = value_or(
            touch,
            "editorDirectTouch",
            settings.touch.editor_direct_touch
        );
        settings.touch.opacity = value_or(touch, "opacity", settings.touch.opacity);
        settings.touch.scale = value_or(touch, "scale", settings.touch.scale);
        settings.touch.horizontal_offset = value_or(
            touch,
            "horizontalOffset",
            settings.touch.horizontal_offset
        );
        settings.touch.vertical_offset = value_or(
            touch,
            "verticalOffset",
            settings.touch.vertical_offset
        );
        settings.touch.sensitivity = value_or(
            touch,
            "sensitivity",
            settings.touch.sensitivity
        );
        settings.touch.deadzone = value_or(
            touch,
            "deadzone",
            settings.touch.deadzone
        );
        settings.touch.gameplay_coverage = value_or(
            touch,
            "gameplayCoverage",
            settings.touch.gameplay_coverage
        );

        const bool legacy_keyboard_was_present = root.contains("keyboard");
        const bool legacy_gamepad_was_present = root.contains("gamepad");
        if (const auto controls = root.find("keyboard");
            controls != root.end()) {
            if (!controls->is_array()) {
                return {std::nullopt, "settings keyboard must be an array"};
            }
            if (controls->size() > maximum_key_bindings) {
                return {
                    std::nullopt,
                    "settings contain too many legacy key bindings",
                };
            }
            settings.keyboard.clear();
            for (const auto& binding : *controls) {
                if (!binding.is_object()
                    || !binding.contains("key")
                    || !binding["key"].is_string()
                    || !binding.contains("lane")
                    || (!binding["lane"].is_number_integer()
                        && !binding["lane"].is_number_unsigned())) {
                    return {
                        std::nullopt,
                        "each key binding must contain a string key and integer lane",
                    };
                }
                const auto key = binding["key"].get<std::string>();
                std::uint64_t lane = 0;
                if (binding["lane"].is_number_unsigned()) {
                    lane = binding["lane"].get<std::uint64_t>();
                } else {
                    const auto signed_lane = binding["lane"].get<std::int64_t>();
                    if (signed_lane < 0) {
                        return {std::nullopt, "key binding lane cannot be negative"};
                    }
                    lane = static_cast<std::uint64_t>(signed_lane);
                }
                if (key.empty()
                    || key.size() > maximum_key_name_bytes
                    || lane > 17U) {
                    return {
                        std::nullopt,
                        "key binding key/lane exceeds the supported limit",
                    };
                }
                settings.keyboard.push_back({
                    key,
                    static_cast<std::uint16_t>(lane),
                });
            }
        }

        if (const auto controls = root.find("gamepad");
            controls != root.end()) {
            if (!controls->is_array()) {
                return {std::nullopt, "settings gamepad must be an array"};
            }
            if (controls->size() > maximum_gamepad_bindings) {
                return {
                    std::nullopt,
                    "settings contain too many legacy gamepad bindings",
                };
            }
            settings.gamepad.clear();
            for (const auto& binding : *controls) {
                if (!binding.is_object()
                    || !binding.contains("button")
                    || !binding["button"].is_string()
                    || !binding.contains("lane")
                    || (!binding["lane"].is_number_integer()
                        && !binding["lane"].is_number_unsigned())) {
                    return {
                        std::nullopt,
                        "each gamepad binding must contain a string button "
                        "and integer lane",
                    };
                }
                const auto button = binding["button"].get<std::string>();
                std::uint64_t lane = 0;
                if (binding["lane"].is_number_unsigned()) {
                    lane = binding["lane"].get<std::uint64_t>();
                } else {
                    const auto signed_lane = binding["lane"].get<std::int64_t>();
                    if (signed_lane < 0) {
                        return {
                            std::nullopt,
                            "gamepad binding lane cannot be negative",
                        };
                    }
                    lane = static_cast<std::uint64_t>(signed_lane);
                }
                if (button.empty()
                    || button.size() > maximum_button_name_bytes
                    || lane > 17U) {
                    return {
                        std::nullopt,
                        "gamepad binding button/lane exceeds the supported "
                        "limit",
                    };
                }
                settings.gamepad.push_back({
                    button,
                    static_cast<std::uint16_t>(lane),
                });
            }
        }

        const auto action_controls = root.find("controls");
        if (action_controls != root.end()) {
            if (!action_controls->is_object()) {
                return {std::nullopt, "settings controls must be an object"};
            }
            const auto actions = action_controls->find("actions");
            if (actions == action_controls->end() || !actions->is_array()) {
                return {
                    std::nullopt,
                    "settings controls.actions must be an array",
                };
            }
            if (actions->size() > maximum_action_count) {
                return {std::nullopt, "settings contain too many input actions"};
            }
            settings.controls.actions.clear();
            for (const auto& action_value : *actions) {
                if (!action_value.is_object()
                    || !action_value.contains("action")
                    || !action_value["action"].is_string()
                    || !action_value.contains("inputs")
                    || !action_value["inputs"].is_array()) {
                    return {
                        std::nullopt,
                        "each input action must contain an action string and inputs array",
                    };
                }
                const auto action = action_value["action"].get<std::string>();
                if (find_input_action(action) == nullptr) {
                    return {std::nullopt, "unknown input action: " + action};
                }
                const auto& inputs = action_value["inputs"];
                if (inputs.size() > maximum_inputs_per_action) {
                    return {
                        std::nullopt,
                        "too many inputs for action: " + action,
                    };
                }
                if (find_action_binding(settings.controls, action) == nullptr) {
                    settings.controls.actions.push_back({action, {}});
                }
                for (const auto& input_value : inputs) {
                    if (!input_value.is_object()
                        || !input_value.contains("device")
                        || !input_value["device"].is_string()
                        || !input_value.contains("name")
                        || !input_value["name"].is_string()) {
                        return {
                            std::nullopt,
                            "each action input must contain string device and name fields",
                        };
                    }
                    const auto device_name =
                        input_value["device"].get<std::string>();
                    const auto device = input_device_from_string(device_name);
                    if (!device.has_value()) {
                        return {
                            std::nullopt,
                            "input device must be keyboard or gamepad",
                        };
                    }
                    PhysicalInput physical_input{
                        *device,
                        input_value["name"].get<std::string>(),
                    };
                    const auto added = add_input_binding(
                        settings.controls,
                        action,
                        std::move(physical_input),
                        BindingConflictPolicy::reject
                    );
                    if (!added) {
                        std::string message = added.message;
                        if (added.conflict.has_value()) {
                            message += ": "
                                + binding_description(added.conflict->input)
                                + " conflicts with "
                                + added.conflict->conflicting_action;
                        }
                        return {std::nullopt, std::move(message)};
                    }
                }
            }
            const auto validation = validate_input_bindings(settings.controls);
            if (!validation.errors.empty()) {
                return {std::nullopt, validation.errors.front()};
            }
            if (!validation.conflicts.empty()) {
                const auto& conflict = validation.conflicts.front();
                return {
                    std::nullopt,
                    binding_description(conflict.input) + " conflicts between "
                        + conflict.action + " and " + conflict.conflicting_action,
                };
            }
        } else {
            migrate_legacy_lane_bindings(
                settings,
                legacy_keyboard_was_present,
                legacy_gamepad_was_present
            );
        }

        if (const auto discord_error =
                discord_settings_validation_error(settings.discord);
            discord_error.has_value()) {
            return {std::nullopt, *discord_error};
        }
        sanitize(settings);
        rebuild_legacy_lane_bindings(settings);
        return {std::move(settings), {}};
    } catch (const std::exception& exception) {
        return {
            std::nullopt,
            std::string("invalid settings JSON: ") + exception.what(),
        };
    }
}

bool save_settings(
    const std::filesystem::path& path,
    const EngineSettings& settings,
    std::string* error
) {
    try {
        EngineSettings normalized = settings;
        normalized.discord.publish_interval_ms = std::clamp(
            normalized.discord.publish_interval_ms,
            2'000U,
            5'000U
        );
        if (const auto discord_error = discord_settings_validation_error(normalized.discord);
            discord_error.has_value()) {
            if (error != nullptr) *error = *discord_error;
            return false;
        }
        EngineSettings action_view = settings;
        rebuild_legacy_lane_bindings(action_view);
        const bool legacy_keyboard_changed =
            settings.keyboard != action_view.keyboard;
        const bool legacy_gamepad_changed =
            settings.gamepad != action_view.gamepad;
        if (legacy_keyboard_changed || legacy_gamepad_changed) {
            // Compatibility for callers built against schema v1. New UI code
            // edits controls and calls rebuild_legacy_lane_bindings immediately.
            migrate_legacy_lane_bindings(
                normalized,
                legacy_keyboard_changed,
                legacy_gamepad_changed
            );
        }
        const auto validation = validate_input_bindings(normalized.controls);
        if (!validation.errors.empty()) {
            if (error != nullptr) {
                *error = "cannot save input bindings: " + validation.errors.front();
            }
            return false;
        }
        if (!validation.conflicts.empty()) {
            const auto& conflict = validation.conflicts.front();
            if (error != nullptr) {
                *error = "cannot save conflicting input binding "
                    + binding_description(conflict.input) + " for "
                    + conflict.action + " and " + conflict.conflicting_action;
            }
            return false;
        }
        rebuild_legacy_lane_bindings(normalized);
        if (normalized.keyboard.size() > maximum_key_bindings) {
            if (error != nullptr) {
                *error = "cannot save too many legacy key bindings";
            }
            return false;
        }
        if (normalized.gamepad.size() > maximum_gamepad_bindings) {
            if (error != nullptr) {
                *error = "cannot save too many legacy gamepad bindings";
            }
            return false;
        }
        Json keyboard = Json::array();
        for (const auto& binding : normalized.keyboard) {
            if (binding.key.empty()
                || binding.key.size() > maximum_key_name_bytes
                || binding.lane > 17U) {
                if (error != nullptr) {
                    *error = "cannot save an invalid key binding";
                }
                return false;
            }
            keyboard.push_back({
                {"key", binding.key},
                {"lane", binding.lane},
            });
        }
        Json gamepad = Json::array();
        for (const auto& binding : normalized.gamepad) {
            if (binding.button.empty()
                || binding.button.size() > maximum_button_name_bytes
                || binding.lane > 17U) {
                if (error != nullptr) {
                    *error = "cannot save an invalid gamepad binding";
                }
                return false;
            }
            gamepad.push_back({
                {"button", binding.button},
                {"lane", binding.lane},
            });
        }
        Json actions = Json::array();
        for (const auto& action : normalized.controls.actions) {
            Json inputs = Json::array();
            for (const auto& input : action.inputs) {
                inputs.push_back({
                    {"device", to_string(input.device)},
                    {"name", input.name},
                });
            }
            actions.push_back({
                {"action", action.action},
                {"inputs", std::move(inputs)},
            });
        }

        Json normal_profile = nullptr;
        if (settings.performance.normal_profile.has_value()) {
            const auto& profile = *settings.performance.normal_profile;
            normal_profile = {
                {"fpsCap", profile.fps_cap},
                {"audioBufferFrames", profile.audio_buffer_frames},
                {"maxCosmeticBurstsPerFrame",
                 profile.max_cosmetic_bursts_per_frame},
                {"vsync", profile.vsync},
                {"lowQuality", profile.low_quality},
                {"reducedMotion", profile.reduced_motion},
                {"noteSplashes", profile.note_splashes},
                {"flashingLights", profile.flashing_lights},
                {"showTimingGraph", profile.show_timing_graph},
                {"hotReloadScripts", profile.hot_reload_scripts},
            };
        }

        const Json root{
            {"schemaVersion", 3},
            {"gameplay", {
                {"judgments", {
                    {"marvelousMs", settings.gameplay.windows.marvelous_ms},
                    {"sickMs", settings.gameplay.windows.sick_ms},
                    {"goodMs", settings.gameplay.windows.good_ms},
                    {"badMs", settings.gameplay.windows.bad_ms},
                    {"missMs", settings.gameplay.windows.miss_ms},
                }},
                {"inputOffsetMs", settings.gameplay.input_offset_ms},
                {"visualOffsetMs", settings.gameplay.visual_offset_ms},
                {"releaseGraceMs", settings.gameplay.release_grace_ms},
                {"stackedNoteToleranceMs", settings.gameplay.stacked_note_tolerance_ms},
                {"healthGain", settings.gameplay.health_gain},
                {"healthLoss", settings.gameplay.health_loss},
                {"ghostTapHealthLoss", settings.gameplay.ghost_tap_health_loss},
                {"scrollSpeed", settings.gameplay.scroll_speed},
                {"randomSeed", settings.gameplay.random_seed},
                {"ghostTapping", settings.gameplay.ghost_tapping},
                {"autoplay", settings.gameplay.autoplay},
                {"practice", settings.gameplay.practice},
                {"noFail", settings.gameplay.no_fail},
                {"mirror", settings.gameplay.mirror},
                {"randomizeLanes", settings.gameplay.randomize_lanes},
                {"downscroll", settings.gameplay.downscroll},
                {"middleScroll", settings.gameplay.middle_scroll},
                {"hideOpponentNotes", settings.gameplay.hide_opponent_notes},
            }},
            {"audio", {
                {"masterVolume", settings.audio.master_volume},
                {"instrumentalVolume", settings.audio.instrumental_volume},
                {"vocalsVolume", settings.audio.vocals_volume},
                {"audioOffsetMs", settings.audio.audio_offset_ms},
                {"outputLatencyCompensationMs",
                 settings.audio.output_latency_compensation_ms},
                {"playbackRate", settings.audio.playback_rate},
                {"sampleRate", settings.audio.sample_rate},
                {"bufferFrames", settings.audio.buffer_frames},
                {"menuMusicSelection", settings.audio.menu_music_selection},
                {"customMenuMusicPath", settings.audio.custom_menu_music_path},
                {"menuMusicLoopSelected",
                 settings.audio.menu_music_loop_selected},
                {"menuMusicMuted", settings.audio.menu_music_muted},
                {"muted", settings.audio.muted},
                {"muteWhenUnfocused", settings.audio.mute_when_unfocused},
            }},
            {"visual", {
                {"width", settings.visual.width},
                {"height", settings.visual.height},
                {"fpsCap", settings.visual.fps_cap},
                {"backgroundDim", settings.visual.background_dim},
                {"laneUnderlayOpacity", settings.visual.lane_underlay_opacity},
                {"audioVisualizerBackgroundOpacity",
                 settings.visual.audio_visualizer_background_opacity},
                {"audioVisualizerImage",
                 audio_visualizer_image_storage_name(
                     settings.visual.audio_visualizer_image
                 )},
                {"audioVisualizerCustomImagePath",
                 settings.visual.audio_visualizer_custom_image_path},
                {"noteSkinSelection",
                 settings.visual.note_skin_selection},
                {"scrollSpeedMode",
                 settings.visual.scroll_speed_mode == ScrollSpeedMode::constant
                     ? "constant"
                     : "multiplicative"},
                {"vsync", settings.visual.vsync},
                {"fullscreen", settings.visual.fullscreen},
                {"lowQuality", settings.visual.low_quality},
                {"reducedMotion", settings.visual.reduced_motion},
                {"noteSplashes", settings.visual.note_splashes},
                {"flashingLights", settings.visual.flashing_lights},
                {"showFps", settings.visual.show_fps},
                {"showTimingGraph", settings.visual.show_timing_graph},
                {"postEffect", post_effect_storage_name(
                    settings.visual.post_effect
                )},
                {"theme", presentation_theme_storage_name(
                    settings.visual.theme
                )},
                {"skipIntro", settings.visual.skip_intro},
            }},
            {"performance", {
                {"maxVisibleNotes", settings.performance.max_visible_notes},
                {"maxCosmeticBurstsPerFrame",
                 settings.performance.max_cosmetic_bursts_per_frame},
                {"scriptMemoryMb", settings.performance.script_memory_mb},
                {"scriptInstructionBudget",
                 settings.performance.script_instruction_budget},
                {"hotReloadScripts", settings.performance.hot_reload_scripts},
                {"autoPauseOnFocusLoss",
                 settings.performance.auto_pause_on_focus_loss},
                {"pauseOnControllerDisconnect",
                 settings.performance.pause_on_controller_disconnect},
                {"maximumPerformanceMode",
                 settings.performance.maximum_performance_mode},
                {"ultraLowLatency",
                 settings.performance.ultra_low_latency},
                {"normalProfile", std::move(normal_profile)},
            }},
            {"discord", {
                {"enabled", normalized.discord.enabled},
                {"applicationId", normalized.discord.application_id},
                {"oauthRedirectUri", normalized.discord.oauth_redirect_uri},
                {"privacy", discord_presence_privacy_storage_name(normalized.discord.privacy)},
                {"showChartName", normalized.discord.show_chart_name},
                {"showDifficultyMania", normalized.discord.show_difficulty_mania},
                {"showProgress", normalized.discord.show_progress},
                {"showNoteCounter", normalized.discord.show_note_counter},
                {"showGameplayStats", normalized.discord.show_gameplay_stats},
                {"showBotplay", normalized.discord.show_botplay},
                {"showRemainingTime", normalized.discord.show_remaining_time},
                {"showModName", normalized.discord.show_mod_name},
                {"advancedCustomization", normalized.discord.advanced_customization},
                {"activityNameTemplate", normalized.discord.activity_name_template},
                {"detailsTemplate", normalized.discord.details_template},
                {"stateTemplate", normalized.discord.state_template},
                {"detailsUrlTemplate", normalized.discord.details_url_template},
                {"stateUrlTemplate", normalized.discord.state_url_template},
                {"largeImageTemplate", normalized.discord.large_image_template},
                {"largeTextTemplate", normalized.discord.large_text_template},
                {"largeUrlTemplate", normalized.discord.large_url_template},
                {"smallImageTemplate", normalized.discord.small_image_template},
                {"smallTextTemplate", normalized.discord.small_text_template},
                {"smallUrlTemplate", normalized.discord.small_url_template},
                {"button1", {
                    {"enabled", normalized.discord.button1.enabled},
                    {"label", normalized.discord.button1.label},
                    {"url", normalized.discord.button1.url},
                }},
                {"button2", {
                    {"enabled", normalized.discord.button2.enabled},
                    {"label", normalized.discord.button2.label},
                    {"url", normalized.discord.button2.url},
                }},
                {"retryFailedUpdates", normalized.discord.retry_failed_updates},
                {"publishIntervalMs", normalized.discord.publish_interval_ms},
            }},
            {"touch", {
                {"gameplayEnabled", normalized.touch.gameplay_enabled},
                {"showLabels", normalized.touch.show_labels},
                {"editorDirectTouch", normalized.touch.editor_direct_touch},
                {"opacity", normalized.touch.opacity},
                {"scale", normalized.touch.scale},
                {"horizontalOffset", normalized.touch.horizontal_offset},
                {"verticalOffset", normalized.touch.vertical_offset},
                {"sensitivity", normalized.touch.sensitivity},
                {"deadzone", normalized.touch.deadzone},
                {"gameplayCoverage", normalized.touch.gameplay_coverage},
            }},
            {"controls", {
                {"actions", std::move(actions)},
            }},
            {"keyboard", std::move(keyboard)},
            {"gamepad", std::move(gamepad)},
        };

        auto serialized = root.dump(2);
        serialized.push_back('\n');
        if (serialized.size() > maximum_settings_bytes) {
            if (error != nullptr) {
                *error = "serialized settings exceed the 1 MiB safety limit";
            }
            return false;
        }
        return write_text_atomically(path, serialized, error);
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = exception.what();
        }
        return false;
    }
}

}  // namespace pulseforge
