#include "pulseforge/audio_controls.hpp"
#include "pulseforge/input_bindings.hpp"
#include "pulseforge/settings.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] std::string read_all(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

void write_all(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    require(static_cast<bool>(output), "write test fixture");
}

}  // namespace

int main() {
    using namespace pulseforge;

    auto bindings = default_input_bindings();
    require(static_cast<bool>(validate_input_bindings(bindings)), "defaults validate");
    const auto* note_zero = find_action_binding(bindings, "note_0");
    require(note_zero != nullptr, "default note action exists");
    require(note_zero->inputs.size() == 4U, "multiple defaults per lane");
    require(lane_for_input_action("note_17") == 17U, "18-key action mapping");
    require(!lane_for_input_action("note_18").has_value(), "lane upper bound");
    const auto* mute_binding = find_action_binding(bindings, "volume_mute");
    require(
        mute_binding != nullptr && mute_binding->inputs.size() == 1U
            && mute_binding->inputs.front().name == "m",
        "mute defaults to M and remains remappable"
    );
    const auto* volume_up_binding = find_action_binding(bindings, "volume_up");
    require(
        volume_up_binding != nullptr && volume_up_binding->inputs.size() == 1U
            && volume_up_binding->inputs.front().name == "shift+equals",
        "volume up defaults to the literal plus chord"
    );
    require(
        canonicalize_input_name("Shift+=") == "shift+equals",
        "captured plus chord canonicalizes to its persisted alias"
    );

    AudioSettings stepped_audio;
    require(master_volume_percent(stepped_audio) == 100, "master volume starts at 100");
    for (int step = 0; step < 10; ++step) {
        require(adjust_master_volume(stepped_audio, -1), "volume decrements by 10");
    }
    require(master_volume_percent(stepped_audio) == 0, "master volume reaches zero");
    require(!adjust_master_volume(stepped_audio, -1), "zero volume clamps safely");
    require(adjust_master_volume(stepped_audio, 1), "volume increments by 10");
    require(master_volume_percent(stepped_audio) == 10, "volume step is exactly ten percent");
    toggle_master_mute(stepped_audio);
    require(stepped_audio.muted, "mute toggles independently of stored volume");

    const auto duplicate = add_input_binding(
        bindings,
        "note_0",
        {InputDevice::keyboard, "  D  "}
    );
    require(
        duplicate.status == BindingEditStatus::duplicate,
        "case and surrounding whitespace deduplicate"
    );

    const auto conflict = add_input_binding(
        bindings,
        "note_1",
        {InputDevice::keyboard, "d"}
    );
    require(conflict.status == BindingEditStatus::conflict, "same-context conflict");
    require(
        conflict.conflict.has_value()
            && conflict.conflict->conflicting_action == "note_0",
        "conflict identifies existing action"
    );
    const auto replaced = add_input_binding(
        bindings,
        "note_1",
        {InputDevice::keyboard, "D"},
        BindingConflictPolicy::replace_existing
    );
    require(static_cast<bool>(replaced), "explicit conflict replacement");
    note_zero = find_action_binding(bindings, "note_0");
    require(note_zero != nullptr, "note zero remains after replacement");
    require(
        std::ranges::none_of(note_zero->inputs, [](const PhysicalInput& input) {
            return input.device == InputDevice::keyboard && input.name == "d";
        }),
        "replacement removes old binding"
    );

    // UI and gameplay are separate active contexts, so arrows can be reused.
    require(
        static_cast<bool>(validate_input_bindings(bindings)),
        "cross-context defaults do not conflict"
    );
    const auto global_conflict = add_input_binding(
        bindings,
        "editor_save",
        {InputDevice::keyboard, "f12"}
    );
    require(
        global_conflict.status == BindingEditStatus::conflict,
        "global action conflicts with every active context"
    );
    require(
        add_input_binding(
            bindings,
            "does_not_exist",
            {InputDevice::keyboard, "q"}
        ).status == BindingEditStatus::invalid_action,
        "unknown action rejected"
    );
    require(
        add_input_binding(
            bindings,
            "note_4",
            {InputDevice::keyboard, std::string(65U, 'x')}
        ).status == BindingEditStatus::invalid_input,
        "oversized input name rejected"
    );
    for (std::size_t index = 0; index < maximum_inputs_per_action; ++index) {
        require(
            static_cast<bool>(add_input_binding(
                bindings,
                "note_4",
                {InputDevice::keyboard, "unused" + std::to_string(index)}
            )),
            "binding limit accepts bounded entries"
        );
    }
    require(
        add_input_binding(
            bindings,
            "note_4",
            {InputDevice::keyboard, "one-too-many"}
        ).status == BindingEditStatus::limit_exceeded,
        "per-action binding limit enforced"
    );

    auto removal = default_input_bindings();
    const auto* original_note = find_action_binding(removal, "note_0");
    require(original_note != nullptr, "required action for removal test");
    const auto original_inputs = original_note->inputs;
    for (std::size_t index = 0; index + 1U < original_inputs.size(); ++index) {
        require(
            static_cast<bool>(remove_input_binding(
                removal,
                "note_0",
                original_inputs[index]
            )),
            "remove non-final required binding"
        );
    }
    require(
        remove_input_binding(removal, "note_0", original_inputs.back()).status
            == BindingEditStatus::required_action,
        "final required binding is protected"
    );

    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    const auto test_directory = std::filesystem::temp_directory_path()
        / ("pulseforge-input-bindings-" + unique);
    std::filesystem::create_directories(test_directory);
    const auto settings_path = test_directory / "settings.json";

    EngineSettings settings;
    settings.visual.theme = PresentationTheme::watch_dogs;
    settings.visual.post_effect = PostEffect::rgb_split;
    settings.visual.skip_intro = true;
    settings.visual.fps_cap = 144;
    settings.visual.vsync = true;
    settings.audio.buffer_frames = 512U;
    settings.audio.playback_rate = 1.25;
    settings.audio.master_volume = 0.6F;
    settings.audio.menu_music_selection = "grants-opus.mp3";
    settings.audio.menu_music_loop_selected = true;
    settings.audio.muted = true;
    settings.touch.gameplay_enabled = false;
    settings.touch.show_labels = false;
    settings.touch.editor_direct_touch = false;
    settings.touch.opacity = 0.70F;
    settings.touch.scale = 1.20F;
    settings.touch.horizontal_offset = -0.15F;
    settings.touch.vertical_offset = 0.075F;
    settings.touch.sensitivity = 1.50F;
    settings.touch.deadzone = 0.04F;
    settings.touch.gameplay_coverage = 0.90F;
    settings.performance.max_cosmetic_bursts_per_frame = 23U;
    set_maximum_performance_mode(settings, true);
    require(
        settings.performance.maximum_performance_mode
            && settings.performance.normal_profile.has_value()
            && settings.visual.fps_cap == 0
            && !settings.visual.vsync
            && settings.visual.low_quality
            && settings.visual.reduced_motion
            && !settings.visual.note_splashes
            && !settings.visual.flashing_lights
            && !settings.visual.show_timing_graph
            && settings.audio.buffer_frames == 64U
            && settings.performance.max_cosmetic_bursts_per_frame == 0U
            && !settings.performance.hot_reload_scripts,
        "maximum performance mode applies its complete parallel profile"
    );
    require(
        settings.audio.playback_rate == 1.25,
        "maximum performance mode never changes playback rate"
    );
    require(
        static_cast<bool>(add_input_binding(
            settings.controls,
            "note_4",
            {InputDevice::keyboard, "Left Shift"}
        )),
        "add fifth-lane key"
    );
    rebuild_legacy_lane_bindings(settings);
    std::string error;
    require(save_settings(settings_path, settings, &error), "save schema v3: " + error);
    const auto first_save = read_all(settings_path);
    require(
        first_save.find("\"schemaVersion\": 3") != std::string::npos,
        "schema v3 persisted"
    );
    require(
        first_save.find("\"controls\"") != std::string::npos,
        "action controls persisted"
    );
    require(
        first_save.find("\"theme\": \"watch-dogs\"") != std::string::npos,
        "presentation theme persisted"
    );
    require(
        first_save.find("\"postEffect\": \"rgb-split\"")
            != std::string::npos,
        "runtime post effect persisted"
    );
    require(
        first_save.find("\"skipIntro\": true") != std::string::npos,
        "intro preference persisted"
    );
    require(
        first_save.find("\"muted\": true") != std::string::npos,
        "master mute preference persisted"
    );
    require(
        first_save.find("\"menuMusicSelection\": \"grants-opus.mp3\"")
            != std::string::npos
            && first_save.find("\"menuMusicLoopSelected\": true")
                != std::string::npos,
        "menu music choice and playback mode persisted"
    );

    auto loaded = load_settings(settings_path);
    require(static_cast<bool>(loaded), "load action controls: " + loaded.error);
    require(
        loaded.settings->visual.theme == PresentationTheme::watch_dogs,
        "presentation theme round trips"
    );
    require(
        loaded.settings->visual.post_effect == PostEffect::rgb_split,
        "runtime post effect round trips"
    );
    require(
        loaded.settings->visual.skip_intro,
        "intro preference round trips"
    );
    require(
        loaded.settings->audio.muted
            && master_volume_percent(loaded.settings->audio) == 60,
        "stepped master volume and mute round trip"
    );
    require(
        loaded.settings->audio.menu_music_selection == "grants-opus.mp3"
            && loaded.settings->audio.menu_music_loop_selected,
        "menu music choice and playback mode round trip"
    );
    require(
        !loaded.settings->touch.gameplay_enabled
            && !loaded.settings->touch.show_labels
            && !loaded.settings->touch.editor_direct_touch
            && std::abs(loaded.settings->touch.opacity - 0.70F) < 0.001F
            && std::abs(loaded.settings->touch.scale - 1.20F) < 0.001F
            && std::abs(loaded.settings->touch.horizontal_offset + 0.15F) < 0.001F
            && std::abs(loaded.settings->touch.vertical_offset - 0.075F) < 0.001F
            && std::abs(loaded.settings->touch.sensitivity - 1.50F) < 0.001F
            && std::abs(loaded.settings->touch.deadzone - 0.04F) < 0.001F
            && std::abs(loaded.settings->touch.gameplay_coverage - 0.90F) < 0.001F,
        "Android touch layout and behavior settings round trip"
    );
    const auto unsafe_music_path = test_directory / "unsafe-music.json";
    EngineSettings unsafe_music;
    unsafe_music.audio.menu_music_selection = "../outside.mp3";
    unsafe_music.audio.menu_music_loop_selected = true;
    error.clear();
    require(
        save_settings(unsafe_music_path, unsafe_music, &error),
        "unsafe menu music selection sanitizes on save: " + error
    );
    const auto sanitized_music = load_settings(unsafe_music_path);
    require(
        sanitized_music
            && sanitized_music.settings->audio.menu_music_selection
                == "randomized"
            && !sanitized_music.settings->audio.menu_music_loop_selected,
        "menu music setting cannot escape the discovered music directory"
    );
    require(
        loaded.settings->performance.maximum_performance_mode
            && loaded.settings->performance.normal_profile.has_value()
            && loaded.settings->visual.fps_cap == 0
            && loaded.settings->audio.buffer_frames == 64U,
        "maximum performance profile round trips and is re-enforced"
    );
    set_maximum_performance_mode(*loaded.settings, false);
    require(
        !loaded.settings->performance.maximum_performance_mode
            && !loaded.settings->performance.normal_profile.has_value()
            && loaded.settings->visual.fps_cap == 144
            && loaded.settings->visual.vsync
            && loaded.settings->audio.buffer_frames == 512U
            && loaded.settings->performance.max_cosmetic_bursts_per_frame == 23U
            && loaded.settings->performance.hot_reload_scripts,
        "disabling maximum performance restores the persisted normal profile"
    );
    require(
        loaded.settings->audio.playback_rate == 1.25,
        "restoring the normal profile leaves playback rate untouched"
    );
    const auto* loaded_note = find_action_binding(loaded.settings->controls, "note_4");
    require(loaded_note != nullptr, "fifth lane round trips");
    require(
        std::ranges::any_of(loaded_note->inputs, [](const PhysicalInput& input) {
            return input.device == InputDevice::keyboard
                && input.name == "left shift";
        }),
        "input names are canonicalized and round trip"
    );
    require(
        std::ranges::any_of(
            loaded.settings->keyboard,
            [](const KeyBinding& binding) {
                return binding.lane == 4U && binding.key == "left shift";
            }
        ),
        "legacy runtime view rebuilt"
    );

    // A rejected save must leave the previously valid file untouched.
    settings.controls.actions.push_back(settings.controls.actions.front());
    error.clear();
    require(!save_settings(settings_path, settings, &error), "invalid save rejected");
    require(read_all(settings_path) == first_save, "failed save preserves old file");

    const auto legacy_path = test_directory / "legacy.json";
    write_all(
        legacy_path,
        R"({
            "schemaVersion": 1,
            "keyboard": [
                {"key": "Q", "lane": 0},
                {"key": "q", "lane": 0}
            ],
            "gamepad": []
        })"
    );
    const auto migrated = load_settings(legacy_path);
    require(static_cast<bool>(migrated), "legacy controls migrate: " + migrated.error);
    require(
        migrated.settings->visual.theme == PresentationTheme::pulseforge
            && !migrated.settings->visual.skip_intro,
        "legacy settings receive safe presentation defaults"
    );
    require(
        migrated.settings->audio.menu_music_selection == "randomized"
            && !migrated.settings->audio.menu_music_loop_selected,
        "legacy settings receive randomized menu music defaults"
    );
    const auto* migrated_note = find_action_binding(
        migrated.settings->controls,
        "note_0"
    );
    require(migrated_note != nullptr, "migrated action exists");
    require(migrated_note->inputs.size() == 1U, "legacy duplicates deduplicate");
    require(migrated_note->inputs.front().name == "q", "legacy input canonicalized");

    std::error_code cleanup_error;
    std::filesystem::remove_all(test_directory, cleanup_error);
    require(!cleanup_error, "clean test directory");
    std::cout << "PulseForge input binding tests passed\n";
    return EXIT_SUCCESS;
}
