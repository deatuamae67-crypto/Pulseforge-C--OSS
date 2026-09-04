#pragma once

#include "pulseforge/chart.hpp"
#include "pulseforge/settings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge {

enum class AudioTransportState : std::uint8_t {
    uninitialized,
    stopped,
    playing,
    paused,
    ended,
};

enum class AudioTransportBackend : std::uint8_t {
    system_default,
    // Deterministic callback-driven device with no hardware output. Useful for
    // automated tests, headless validation and servers.
    null_device,
};

// Low-latency, pull-driven song transport. Control methods are intended to be
// called by the main thread; all query methods are safe to call concurrently
// with the audio callback.
class AudioTransport final {
public:
    static constexpr std::size_t visualizer_bin_count = 32U;

    AudioTransport();
    ~AudioTransport();

    AudioTransport(const AudioTransport&) = delete;
    AudioTransport& operator=(const AudioTransport&) = delete;
    AudioTransport(AudioTransport&&) = delete;
    AudioTransport& operator=(AudioTransport&&) = delete;

    [[nodiscard]] bool initialize(
        const AudioSettings& settings,
        std::string* error = nullptr
    );
    [[nodiscard]] bool initialize(
        const AudioSettings& settings,
        AudioTransportBackend backend,
        std::string* error = nullptr
    );
    void shutdown() noexcept;

    // Empty manifests produce an explicit silent clock. PulseForge never
    // substitutes another song or the old procedural demo backing track.
    [[nodiscard]] bool load(const Chart& chart, std::string* error = nullptr);
    [[nodiscard]] bool load(
        const AudioManifest& audio,
        double fallback_duration_ms,
        // Legacy 0.3 parameter, ignored. BPM never controls audio playback.
        double fallback_bpm = 120.0,
        std::string* error = nullptr
    );
    void unload() noexcept;

    void play() noexcept;
    void pause() noexcept;
    void resume() noexcept;
    void stop() noexcept;
    void seek_ms(double position_ms) noexcept;

    void set_playback_rate(double rate) noexcept;
    void set_master_volume(float volume) noexcept;
    void set_instrumental_volume(float volume) noexcept;
    void set_vocals_volume(float volume) noexcept;
    void set_muted(bool muted) noexcept;
    void set_looping(bool looping) noexcept;
    void set_audio_offset_ms(double offset_ms) noexcept;
    void set_output_latency_compensation_ms(double compensation_ms) noexcept;
    void set_mute_when_unfocused(bool enabled) noexcept;
    void set_focused(bool focused) noexcept;

    // PULSEFORGE_P1_1_18_PSYCH_SOUND_BANK_API_V1
    // Bounded one-shot/auxiliary audio mixed into the same device callback as
    // the authoritative song stems. Paths are decoded to the output sample
    // rate once and cached; tagged voices expose Psych-compatible lifecycle.
    [[nodiscard]] bool precache_sound(
        const std::filesystem::path& path,
        std::string* error = nullptr
    );
    [[nodiscard]] bool play_sound(
        const std::filesystem::path& path,
        float volume = 1.0F,
        std::string_view tag = {},
        bool looping = false,
        std::string* error = nullptr
    );
    [[nodiscard]] bool stop_sound(std::string_view tag) noexcept;
    [[nodiscard]] bool pause_sound(std::string_view tag) noexcept;
    [[nodiscard]] bool resume_sound(std::string_view tag) noexcept;
    [[nodiscard]] bool set_sound_volume(
        std::string_view tag,
        float volume
    ) noexcept;
    [[nodiscard]] bool sound_volume(
        std::string_view tag,
        float& volume
    ) const noexcept;
    [[nodiscard]] bool set_sound_time_ms(
        std::string_view tag,
        double position_ms
    ) noexcept;
    [[nodiscard]] bool sound_time_ms(
        std::string_view tag,
        double& position_ms
    ) const noexcept;
    [[nodiscard]] bool sound_playing(std::string_view tag) const noexcept;
    [[nodiscard]] bool fade_sound(
        std::string_view tag,
        double duration_seconds,
        float from_volume,
        float to_volume
    ) noexcept;
    [[nodiscard]] bool cancel_sound_fade(std::string_view tag) noexcept;
    [[nodiscard]] std::vector<std::string> consume_sound_completions();
    void stop_all_sounds() noexcept;

    [[nodiscard]] std::uint64_t clock_frames() const noexcept;
    [[nodiscard]] double position_ms() const noexcept;
    [[nodiscard]] double compensated_position_ms() const noexcept;
    [[nodiscard]] double duration_ms() const noexcept;
    [[nodiscard]] std::uint32_t sample_rate() const noexcept;
    [[nodiscard]] double playback_rate() const noexcept;
    [[nodiscard]] float master_volume() const noexcept;
    [[nodiscard]] float instrumental_volume() const noexcept;
    [[nodiscard]] float vocals_volume() const noexcept;
    // Snapshot of the latest real output waveform envelope. Each bin is
    // normalized to 0..1 and can be read safely by the render thread.
    [[nodiscard]] std::array<float, visualizer_bin_count>
        visualizer_levels() const noexcept;
    [[nodiscard]] bool muted() const noexcept;
    [[nodiscard]] bool looping() const noexcept;
    [[nodiscard]] AudioTransportState state() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] bool using_silent_audio() const noexcept;
    // Retained for source compatibility with 0.3 embedders. Procedural song
    // substitution was removed; this always returns false.
    [[nodiscard]] bool using_procedural_audio() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pulseforge
