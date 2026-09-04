#include "pulseforge/audio_transport.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
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
    constexpr std::uint16_t block_align =
        channels * (bits_per_sample / 8U);
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
    require(static_cast<bool>(output), "WAV fixture can be written");
}

void write_tone_wav(
    const std::filesystem::path& path,
    const std::uint32_t sample_rate,
    const std::uint32_t frames
) {
    constexpr std::uint16_t channels = 2U;
    constexpr std::uint16_t bits_per_sample = 16U;
    constexpr std::uint16_t block_align =
        channels * (bits_per_sample / 8U);
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

    constexpr double pi = 3.14159265358979323846;
    for (std::uint32_t frame = 0U; frame < frames; ++frame) {
        const double phase = 2.0 * pi * 440.0
            * static_cast<double>(frame)
            / static_cast<double>(sample_rate);
        const auto sample = static_cast<std::int16_t>(
            std::sin(phase) * 8'000.0
        );
        const auto raw = static_cast<std::uint16_t>(sample);
        for (std::uint16_t channel = 0U; channel < channels; ++channel) {
            write_u16(output, raw);
        }
    }
    require(static_cast<bool>(output), "tone WAV fixture can be written");
}

[[nodiscard]] pulseforge::Chart chart_at_bpm(const double bpm) {
    pulseforge::Chart chart;
    chart.title = "Timing invariant";
    chart.tempos = {{0.0, bpm, 4U, 4U}};
    chart.notes = {{2'000.0, 0.0, 0U, pulseforge::NoteOwner::player}};
    return chart;
}

}  // namespace

int main(const int argument_count, char** arguments) {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    const auto root = std::filesystem::temp_directory_path()
        / ("pulseforge-audio-transport-" + unique);
    std::filesystem::create_directories(root);
    try {
        pulseforge::AudioSettings settings;
        settings.playback_rate = 1.25;
        settings.buffer_frames = 512U;
        pulseforge::AudioTransport transport;
        std::string error;
        require(
            transport.initialize(
                settings,
                pulseforge::AudioTransportBackend::null_device,
                &error
            ),
            "null audio device initializes for transport regression test"
        );
        require(
            std::abs(transport.playback_rate() - 1.25) < 0.000001,
            "explicit playback rate is applied"
        );
        require(!transport.muted(), "transport starts unmuted");
        transport.set_muted(true);
        require(transport.muted(), "runtime mute is observable");
        transport.set_muted(false);
        transport.set_looping(true);
        require(transport.looping(), "looping can be enabled for menu music");
        transport.set_looping(false);

        auto slow_chart = chart_at_bpm(60.0);
        require(transport.load(slow_chart, &error), "60 BPM silent chart loads");
        require(
            transport.using_silent_audio(),
            "missing stems use silence instead of demo synthesis"
        );
        require(
            !transport.using_procedural_audio(),
            "procedural demo substitution stays disabled"
        );
        require(
            std::abs(transport.playback_rate() - 1.25) < 0.000001,
            "60 BPM does not alter playback rate"
        );
        transport.play();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        require(
            transport.position_ms() > 0.0,
            "silent fallback advances the authoritative audio clock"
        );
        transport.pause();

        auto fast_chart = chart_at_bpm(900.0);
        require(transport.load(fast_chart, &error), "900 BPM silent chart loads");
        require(
            std::abs(transport.playback_rate() - 1.25) < 0.000001,
            "900 BPM does not alter playback rate"
        );

        const auto wav = root / "Inst.wav";
        write_silent_wav(wav, settings.sample_rate, settings.sample_rate);
        fast_chart.audio.instrumental = wav;
        require(transport.load(fast_chart, &error), "explicit WAV loads");
        require(
            !transport.using_silent_audio(),
            "decoded audio is distinguishable from a silent clock"
        );
        require(
            std::abs(transport.duration_ms() - 1'000.0) < 2.0,
            "decoded audio duration is independent of chart BPM"
        );

        // PULSEFORGE_P1_1_18_PSYCH_SOUND_BANK_TEST_V1
        const auto sfx = root / "blip.wav";
        write_tone_wav(
            sfx,
            settings.sample_rate,
            settings.sample_rate * 120U / 1'000U
        );
        require(
            transport.precache_sound(sfx, &error),
            "Psych SFX can be decoded into the bounded cache: " + error
        );
        require(
            transport.play_sound(sfx, 0.5F, "blip", false, &error),
            "tagged Psych SFX starts: " + error
        );
        require(transport.sound_playing("blip"), "tagged SFX reports playing");

        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        double sound_time_before_pause = 0.0;
        require(
            transport.sound_time_ms("blip", sound_time_before_pause)
                && sound_time_before_pause > 0.0,
            "tagged SFX exposes an advancing playhead"
        );
        require(transport.pause_sound("blip"), "tagged SFX can pause");
        double paused_time = 0.0;
        require(
            transport.sound_time_ms("blip", paused_time),
            "paused SFX time can be queried"
        );
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        double paused_time_after = 0.0;
        require(
            transport.sound_time_ms("blip", paused_time_after)
                && std::abs(paused_time_after - paused_time) < 15.0,
            "paused SFX playhead remains stable"
        );

        require(
            transport.set_sound_volume("blip", 0.3F),
            "tagged SFX volume can be changed"
        );
        float queried_volume = 0.0F;
        require(
            transport.sound_volume("blip", queried_volume)
                && std::abs(queried_volume - 0.3F) < 0.01F,
            "tagged SFX volume is observable"
        );
        require(
            transport.set_sound_time_ms("blip", 0.0),
            "tagged SFX can seek"
        );
        require(
            transport.fade_sound("blip", 0.04, 0.3F, 0.8F),
            "tagged SFX fade can start"
        );
        require(transport.resume_sound("blip"), "tagged SFX can resume");
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        require(
            transport.sound_volume("blip", queried_volume)
                && queried_volume >= 0.3F && queried_volume <= 0.81F,
            "audio callback advances tagged SFX fade"
        );
        require(
            transport.cancel_sound_fade("blip"),
            "tagged SFX fade can be cancelled"
        );

        bool completion_seen = false;
        const auto completion_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < completion_deadline
            && !completion_seen) {
            for (const auto& tag : transport.consume_sound_completions()) {
                if (tag == "blip") {
                    completion_seen = true;
                }
            }
            if (!completion_seen) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        require(completion_seen, "natural tagged SFX completion is reported");
        require(
            !transport.sound_playing("blip"),
            "completed tagged SFX no longer reports playing"
        );

        require(
            transport.play_sound(sfx, 0.25F, "loop", true, &error),
            "looping auxiliary sound starts"
        );
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        require(
            transport.sound_playing("loop"),
            "looping auxiliary sound survives its source EOF"
        );
        require(transport.stop_sound("loop"), "looping SFX can be stopped");
        require(
            !transport.sound_playing("loop"),
            "stopped looping SFX reports not playing"
        );

        if (argument_count == 2) {
            pulseforge::AudioManifest menu_music;
            menu_music.instrumental = std::filesystem::path(arguments[1]);
            require(
                transport.load(menu_music, 60'000.0, 120.0, &error),
                "supplied menu MP3 decodes: " + error
            );
            require(
                transport.duration_ms() > 5'000.0,
                "supplied menu MP3 has a real decoded duration"
            );
            transport.set_looping(true);
            transport.set_playback_rate(4.0);
            transport.seek_ms(transport.duration_ms() - 20.0);
            transport.play();
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            require(
                transport.state() == pulseforge::AudioTransportState::playing,
                "looping menu transport stays in playing state at EOF"
            );
            require(
                transport.position_ms() < transport.duration_ms() - 20.0,
                "looping menu transport wraps its playhead"
            );
            transport.stop();
            transport.set_looping(false);
            transport.set_playback_rate(settings.playback_rate);
        }

        fast_chart.audio.instrumental = root / "missing.ogg";
        require(
            !transport.load(fast_chart, &error),
            "an explicit missing stem is a load error"
        );
        require(
            !transport.loaded(),
            "failed replacement cannot leave the previous song loaded"
        );
        require(
            error.find("missing.ogg") != std::string::npos,
            "missing-stem error names the requested file"
        );

        transport.shutdown();
        std::filesystem::remove_all(root);
        std::cout << "audio transport tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "audio transport test failed: " << exception.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
