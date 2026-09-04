#include "editor_ui.hpp"
#include "menu_layout.hpp"
#include "sdl_input_actions.hpp"

#include "pulseforge/packed_chart.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void push_key(const SDL_Scancode scancode) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = scancode;
    event.key.repeat = false;
    require(SDL_PushEvent(&event), "key event is queued");
}

void push_text(const char* value) {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = value;
    require(SDL_PushEvent(&event), "text input event is queued");
}

void push_mouse_button(
    const float x,
    const float y,
    const Uint8 button
) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = button;
    event.button.x = x;
    event.button.y = y;
    require(SDL_PushEvent(&event), "mouse button event is queued");
}

void write_u16(std::ofstream& output, const std::uint16_t value) {
    output.put(static_cast<char>(value & 0xFFU));
    output.put(static_cast<char>((value >> 8U) & 0xFFU));
}

void write_u32(std::ofstream& output, const std::uint32_t value) {
    output.put(static_cast<char>(value & 0xFFU));
    output.put(static_cast<char>((value >> 8U) & 0xFFU));
    output.put(static_cast<char>((value >> 16U) & 0xFFU));
    output.put(static_cast<char>((value >> 24U) & 0xFFU));
}

void write_silent_wav(
    const std::filesystem::path& path,
    const std::uint32_t sample_rate,
    const std::uint32_t frames
) {
    constexpr std::uint16_t channels = 2U;
    constexpr std::uint16_t bits_per_sample = 16U;
    constexpr std::uint16_t block_align = channels * (bits_per_sample / 8U);
    const auto data_bytes = frames * block_align;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write("RIFF", 4);
    write_u32(output, 36U + data_bytes);
    output.write("WAVEfmt ", 8);
    write_u32(output, 16U);
    write_u16(output, 1U);
    write_u16(output, channels);
    write_u32(output, sample_rate);
    write_u32(output, sample_rate * block_align);
    write_u16(output, block_align);
    write_u16(output, bits_per_sample);
    output.write("data", 4);
    write_u32(output, data_bytes);
    for (std::uint32_t byte = 0U; byte < data_bytes; ++byte) {
        output.put('\0');
    }
    require(static_cast<bool>(output), "editor WAV fixture can be written");
}

[[nodiscard]] std::thread queue_escape_after(
    const std::chrono::milliseconds delay
) {
    return std::thread([delay]() {
        std::this_thread::sleep_for(delay);
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.scancode = SDL_SCANCODE_ESCAPE;
        event.key.repeat = false;
        static_cast<void>(SDL_PushEvent(&event));
    });
}

}  // namespace

int main() {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    try {
        const auto first_page = pulseforge::detail::menu_visible_range(
            30U,
            0U,
            14U
        );
        require(
            first_page.first == 0U && first_page.last == 14U,
            "row zero remains in the first visible menu page"
        );
        const auto last_page = pulseforge::detail::menu_visible_range(
            30U,
            29U,
            14U
        );
        require(
            last_page.first == 16U && last_page.last == 30U,
            "menu window remains full and bounded at the last row"
        );
        const auto bindings = pulseforge::default_input_bindings();
        SDL_KeyboardEvent volume_up{};
        volume_up.scancode = SDL_SCANCODE_EQUALS;
        volume_up.mod = SDL_KMOD_SHIFT;
        require(
            pulseforge::detail::keyboard_action_matches(
                bindings,
                "volume_up",
                volume_up
            ),
            "the physical + key resolves through the remappable volume action"
        );
        SDL_KeyboardEvent mute{};
        mute.scancode = SDL_SCANCODE_M;
        require(
            pulseforge::detail::keyboard_action_matches(
                bindings,
                "volume_mute",
                mute
            ),
            "M resolves through the remappable mute action"
        );
        static_cast<void>(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
        require(
            SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS),
            std::string("SDL initializes: ") + SDL_GetError()
        );
        require(
            SDL_CreateWindowAndRenderer(
                "PulseForge editor UI test",
                1'280,
                720,
                0U,
                &window,
                &renderer
            ),
            std::string("dummy window/renderer initialize: ") + SDL_GetError()
        );
        static_cast<void>(SDL_SetRenderLogicalPresentation(
            renderer,
            1'280,
            720,
            SDL_LOGICAL_PRESENTATION_LETTERBOX
        ));

        const auto audio_root = std::filesystem::temp_directory_path()
            / ("pulseforge-editor-audio-"
               + std::to_string(static_cast<std::uint64_t>(
                   std::chrono::steady_clock::now().time_since_epoch().count()
               )));
        const auto nested_audio = audio_root / "songs" / "deep" / "Inst.wav";
        std::filesystem::create_directories(nested_audio.parent_path());
        write_silent_wav(nested_audio, 48'000U, 48'000U);

        pulseforge::ChartEditorUiOptions discovered_audio_options;
        discovered_audio_options.audio_manifest.instrumental = "Inst.wav";
        discovered_audio_options.audio_search_roots = {audio_root};
        discovered_audio_options.audio_backend =
            pulseforge::AudioTransportBackend::null_device;
        {
            pulseforge::detail::ChartEditorAudioSession discovered_audio(
                discovered_audio_options,
                {},
                2'000.0
            );
            require(
                discovered_audio.transport() != nullptr,
                "editor discovers song audio below nested content roots"
            );
            require(
                discovered_audio.error().empty(),
                "nested editor audio discovery has no error"
            );
            require(
                !discovered_audio.transport()->using_silent_audio(),
                "editor uses the decoded song instead of a silent/demo clock"
            );
        }
        auto missing_audio_options = discovered_audio_options;
        missing_audio_options.audio_manifest.instrumental = "Missing-Inst.wav";
        {
            pulseforge::detail::ChartEditorAudioSession missing_audio(
                missing_audio_options,
                {},
                2'000.0
            );
            require(
                missing_audio.transport() == nullptr,
                "missing editor audio leaves editing available without a transport"
            );
            require(
                missing_audio.error().find("Missing-Inst.wav")
                    != std::string::npos,
                "missing editor audio error identifies the requested stem"
            );
        }

        pulseforge::Chart chart;
        chart.title = "Editor UI Smoke";
        chart.player_character = "bf";
        chart.opponent_character = "dad";
        chart.girlfriend_character = "gf";
        chart.tempos = {{0.0, 120.0, 4U, 4U}};
        chart.normalize();
        pulseforge::ChartEditor chart_editor(chart);
        pulseforge::ChartEditorUiOptions chart_options;
        chart_options.choices.characters = {
            "picker-alpha",
            "picker-beta",
            "picker-gamma",
        };
        // The Player row opens the searchable picker. Filtering and End select
        // the last deterministic result without recreating the SDL renderer.
        push_mouse_button(900.0F, 294.0F, SDL_BUTTON_LEFT);
        push_text("PICKER");
        push_key(SDL_SCANCODE_END);
        push_key(SDL_SCANCODE_RETURN);
        push_mouse_button(100.0F, 372.0F, SDL_BUTTON_LEFT);
        push_key(SDL_SCANCODE_ESCAPE);
        push_key(SDL_SCANCODE_ESCAPE);
        const auto chart_result = pulseforge::run_chart_editor_ui(
            window,
            renderer,
            chart_editor,
            chart_options
        );
        require(
            chart_result.exit == pulseforge::EditorUiExit::closed,
            "chart editor exits back to caller"
        );
        require(chart_editor.notes().size() == 1U, "chart editor adds a note");
        require(
            chart_editor.metadata().player_character == "picker-gamma",
            "searchable picker applies the keyboard-selected character"
        );
        require(SDL_GetRenderer(window) == renderer, "chart editor preserves renderer");

        pulseforge::AudioTransport chart_audio;
        pulseforge::AudioSettings chart_audio_settings;
        chart_audio_settings.buffer_frames = 128U;
        std::string chart_audio_error;
        require(
            chart_audio.initialize(
                chart_audio_settings,
                pulseforge::AudioTransportBackend::null_device,
                &chart_audio_error
            ),
            "editor null audio transport initializes"
        );
        pulseforge::AudioManifest chart_audio_manifest;
        chart_audio_manifest.instrumental = nested_audio;
        require(
            chart_audio.load(
                chart_audio_manifest,
                2'000.0,
                120.0,
                &chart_audio_error
            ),
            "editor test song loads"
        );
        pulseforge::ChartEditor audio_chart_editor(chart);
        pulseforge::ChartEditorUiOptions audio_chart_options;
        audio_chart_options.audio = &chart_audio;
        push_key(SDL_SCANCODE_RIGHT);
        push_key(SDL_SCANCODE_SPACE);
        auto stop_audio_editor = queue_escape_after(std::chrono::milliseconds(45));
        const auto audio_chart_result = pulseforge::run_chart_editor_ui(
            window,
            renderer,
            audio_chart_editor,
            audio_chart_options
        );
        stop_audio_editor.join();
        require(
            audio_chart_result.exit == pulseforge::EditorUiExit::closed,
            "audio chart editor returns to its caller"
        );
        require(
            chart_audio.position_ms() > 110.0,
            "moving the chart cursor seeks real audio before Space playback"
        );
        require(
            chart_audio.state() == pulseforge::AudioTransportState::paused,
            "leaving the editor pauses its dedicated song transport"
        );

        const auto streaming_fixture = std::filesystem::temp_directory_path()
            / ("pulseforge-streaming-editor-ui-"
               + std::to_string(static_cast<std::uint64_t>(
                   std::chrono::steady_clock::now().time_since_epoch().count()
               )) + ".pfc");
        pulseforge::PackedChartData packed;
        packed.key_count = 4U;
        packed.kinds = {"normal"};
        packed.notes.push_back({
            0,
            0U,
            0U,
            pulseforge::PackedNoteOwner::opponent,
            0U,
            0U,
        });
        std::string packed_error;
        require(
            pulseforge::write_packed_chart(
                streaming_fixture,
                packed,
                {},
                &packed_error
            ),
            packed_error
        );
        auto packed_reader = pulseforge::PackedChartReader::open(
            streaming_fixture,
            &packed_error
        );
        require(packed_reader.has_value(), packed_error);
        pulseforge::Chart streaming_metadata;
        streaming_metadata.title = "Streaming UI Smoke";
        streaming_metadata.tempos.push_back({0.0, 120.0, 4U, 4U});
        pulseforge::StreamingChartEditor streaming_editor(
            std::move(*packed_reader),
            std::move(streaming_metadata),
            streaming_fixture,
            1'000'000U
        );
        push_mouse_button(150.0F, 372.0F, SDL_BUTTON_LEFT);
        push_key(SDL_SCANCODE_ESCAPE);
        const auto streaming_result =
            pulseforge::run_streaming_chart_editor_ui(
                window,
                renderer,
                streaming_editor
            );
        require(
            streaming_result.exit == pulseforge::EditorUiExit::closed,
            "streaming chart editor exits back to caller"
        );
        require(
            streaming_editor.note_count() == 2U,
            "streaming chart editor adds an overlay note"
        );
        require(
            SDL_GetRenderer(window) == renderer,
            "streaming chart editor preserves renderer"
        );
        chart_audio.seek_ms(0.0);
        pulseforge::ChartEditorUiOptions streaming_audio_options;
        streaming_audio_options.audio = &chart_audio;
        push_key(SDL_SCANCODE_RIGHT);
        push_key(SDL_SCANCODE_SPACE);
        auto stop_streaming_audio = queue_escape_after(
            std::chrono::milliseconds(45)
        );
        const auto streaming_audio_result =
            pulseforge::run_streaming_chart_editor_ui(
                window,
                renderer,
                streaming_editor,
                streaming_audio_options
            );
        stop_streaming_audio.join();
        require(
            streaming_audio_result.exit == pulseforge::EditorUiExit::closed,
            "streaming audio editor returns to its caller"
        );
        require(
            chart_audio.position_ms() > 110.0,
            "PFC1 editor seeks and plays the same real song transport"
        );
        require(
            chart_audio.state() == pulseforge::AudioTransportState::paused,
            "PFC1 editor pauses song audio before returning to menus"
        );

        auto high_bpm_reader = pulseforge::PackedChartReader::open(
            streaming_fixture,
            &packed_error
        );
        require(high_bpm_reader.has_value(), packed_error);
        pulseforge::Chart high_bpm_metadata;
        high_bpm_metadata.title = "Streaming High BPM UI";
        high_bpm_metadata.tempos.push_back({0.0, 1'044.0, 4U, 4U});
        pulseforge::StreamingChartEditor high_bpm_editor(
            std::move(*high_bpm_reader),
            std::move(high_bpm_metadata),
            streaming_fixture,
            1'000'000U
        );
        chart_audio.seek_ms(0.0);
        push_key(SDL_SCANCODE_RIGHT);
        push_key(SDL_SCANCODE_ESCAPE);
        const auto high_bpm_result = pulseforge::run_streaming_chart_editor_ui(
            window,
            renderer,
            high_bpm_editor,
            streaming_audio_options
        );
        require(
            high_bpm_result.exit == pulseforge::EditorUiExit::closed,
            "high-BPM streaming editor returns to its caller"
        );
        const double expected_high_bpm_step = 60'000.0 / 1'044.0 / 4.0;
        require(
            std::abs(chart_audio.position_ms() - expected_high_bpm_step) < 0.01,
            "streaming editor navigation preserves 1044 BPM without clamping"
        );
        chart_audio.shutdown();
        std::error_code remove_error;
        std::filesystem::remove(streaming_fixture, remove_error);
        std::filesystem::remove_all(audio_root, remove_error);

        pulseforge::CharacterDescriptor character;
        character.id = "ui-smoke";
        character.image = "characters/ui-smoke";
        character.animations.push_back({
            "idle",
            "Idle",
            24,
            true,
            {},
            {},
            {},
        });
        pulseforge::CharacterEditor character_editor(character);
        push_key(SDL_SCANCODE_A);
        push_key(SDL_SCANCODE_ESCAPE);
        push_key(SDL_SCANCODE_ESCAPE);
        const auto character_result = pulseforge::run_character_editor_ui(
            window,
            renderer,
            character_editor
        );
        require(
            character_result.exit == pulseforge::EditorUiExit::closed,
            "character editor exits back to caller"
        );
        require(
            character_editor.document().animations.size() == 2U,
            "character editor adds an animation"
        );
        require(SDL_GetRenderer(window) == renderer, "character editor preserves renderer");

        pulseforge::WeekDescriptor week;
        week.id = "ui-week";
        week.story_name = "UI WEEK";
        week.display_name = "UI Week";
        week.songs.push_back({"Editor UI Smoke", "dad", {}, {}});
        pulseforge::WeekEditor week_editor(week);
        push_key(SDL_SCANCODE_A);
        push_key(SDL_SCANCODE_ESCAPE);
        push_key(SDL_SCANCODE_ESCAPE);
        const auto week_result = pulseforge::run_week_editor_ui(
            window,
            renderer,
            week_editor
        );
        require(
            week_result.exit == pulseforge::EditorUiExit::closed,
            "week editor exits back to caller"
        );
        require(
            week_editor.document().songs.size() == 2U,
            "week editor adds a song"
        );
        require(SDL_GetRenderer(window) == renderer, "week editor preserves renderer");

        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
        SDL_DestroyWindow(window);
        window = nullptr;
        SDL_Quit();
        std::cout << "Editor UI smoke tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        if (renderer != nullptr) {
            SDL_DestroyRenderer(renderer);
        }
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
        std::cerr << "Editor UI smoke tests failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
