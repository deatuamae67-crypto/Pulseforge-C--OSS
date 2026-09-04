#include "pulseforge/audio_transport.hpp"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#if defined(__APPLE__)
// Zig's cross-compilation SDK intentionally does not ship Apple's proprietary
// CoreAudio headers. PulseForge only needs miniaudio's decoders and mixer on
// macOS; SDL's official framework owns the physical output device instead.
#define MA_NO_DEVICE_IO
#endif
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#ifdef _MSC_VER
#pragma warning(disable : 4701)
#endif
#include <stb_vorbis.c>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#if defined(__APPLE__)
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulseforge {
namespace {

constexpr std::uint32_t kOutputChannels = 2;
constexpr std::uint32_t kMinimumSampleRate = 8'000;
constexpr std::uint32_t kMaximumSampleRate = 192'000;
constexpr std::uint32_t kMinimumBufferFrames = 32;
constexpr std::uint32_t kMaximumBufferFrames = 8'192;
constexpr std::size_t kDecodeChunkFrames = 4'096;
constexpr std::uint64_t kQ32One = std::uint64_t{1} << 32U;
constexpr std::uint64_t kMaximumFileBytes = std::uint64_t{1} << 27U;
constexpr std::uint64_t kMaximumDecodedBytesPerStem = std::uint64_t{1} << 28U;
constexpr std::uint64_t kMaximumDecodedFrames =
    kMaximumDecodedBytesPerStem / (sizeof(float) * kOutputChannels);
constexpr std::size_t kMaximumVocalStems = 8;
// PULSEFORGE_P1_1_18_PSYCH_SOUND_BANK_LIMITS_V1
constexpr std::size_t kMaximumSoundVoices = 32U;
constexpr std::size_t kMaximumCachedSounds = 64U;
constexpr std::uint64_t kMaximumDecodedSoundBytes =
    64U * 1024U * 1024U;
constexpr std::uint64_t kMaximumSoundCacheBytes =
    128U * 1024U * 1024U;
constexpr std::size_t kMaximumSoundTagBytes = 128U;
constexpr double kMaximumSoundFadeSeconds = 600.0;
constexpr std::uint32_t kSoundVoiceStopped = 0U;
constexpr std::uint32_t kSoundVoicePlaying = 1U;
constexpr std::uint32_t kSoundVoicePaused = 2U;
constexpr std::uint64_t kMaximumClockFrames =
    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) - 8U;
constexpr double kDefaultFallbackDurationMs = 30'000.0;
constexpr double kMinimumFallbackDurationMs = 1'000.0;
constexpr double kMaximumFallbackDurationMs = 12.0 * 60.0 * 60.0 * 1'000.0;
constexpr double kMinimumRate = 0.25;
constexpr double kMaximumRate = 4.0;
constexpr float kMaximumVolume = 2.0F;
constexpr double kMaximumTimingAdjustmentMs = 10'000.0;

static_assert(
    std::atomic<std::uint64_t>::is_always_lock_free,
    "The real-time transport clock requires lock-free 64-bit atomics."
);
static_assert(
    std::atomic<std::uint32_t>::is_always_lock_free,
    "The real-time transport controls require lock-free 32-bit atomics."
);

[[nodiscard]] float sanitize_volume(const float value) noexcept {
    if (!std::isfinite(value)) {
        return 0.0F;
    }
    return std::clamp(value, 0.0F, kMaximumVolume);
}

[[nodiscard]] double sanitize_rate(const double value) noexcept {
    if (!std::isfinite(value)) {
        return 1.0;
    }
    return std::clamp(value, kMinimumRate, kMaximumRate);
}

[[nodiscard]] double sanitize_timing_adjustment(const double value) noexcept {
    if (!std::isfinite(value)) {
        return 0.0;
    }
    return std::clamp(
        value,
        -kMaximumTimingAdjustmentMs,
        kMaximumTimingAdjustmentMs
    );
}

[[nodiscard]] std::uint32_t float_bits(const float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

[[nodiscard]] float bits_float(const std::uint32_t value) noexcept {
    return std::bit_cast<float>(value);
}

[[nodiscard]] std::uint64_t double_bits(const double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] double bits_double(const std::uint64_t value) noexcept {
    return std::bit_cast<double>(value);
}

[[nodiscard]] std::uint64_t rate_to_q32(const double rate) noexcept {
    const auto scaled = sanitize_rate(rate) * static_cast<double>(kQ32One);
    return static_cast<std::uint64_t>(std::llround(scaled));
}

[[nodiscard]] std::string path_text(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(utf8.data()),
        static_cast<std::size_t>(utf8.size())
    };
}

void assign_error(std::string* const destination, std::string message) {
    if (destination != nullptr) {
        *destination = std::move(message);
    }
}

void clear_error(std::string* const destination) {
    if (destination != nullptr) {
        destination->clear();
    }
}

[[nodiscard]] bool is_ogg(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes
) {
    if (bytes.size() >= 4U
        && bytes[0] == static_cast<std::uint8_t>('O')
        && bytes[1] == static_cast<std::uint8_t>('g')
        && bytes[2] == static_cast<std::uint8_t>('g')
        && bytes[3] == static_cast<std::uint8_t>('S')) {
        return true;
    }

    auto extension = path.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const unsigned char character) {
            if (character >= static_cast<unsigned char>('A')
                && character <= static_cast<unsigned char>('Z')) {
                return static_cast<char>(
                    character - static_cast<unsigned char>('A')
                    + static_cast<unsigned char>('a')
                );
            }
            return static_cast<char>(character);
        }
    );
    return extension == ".ogg" || extension == ".oga";
}

[[nodiscard]] bool read_file(
    const std::filesystem::path& path,
    std::vector<std::uint8_t>& bytes,
    std::string& error
) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        error = "Unable to open audio file: " + path_text(path);
        return false;
    }

    const auto end = stream.tellg();
    if (end <= std::streampos{0}) {
        error = "Audio file is empty or unreadable: " + path_text(path);
        return false;
    }

    const auto unsigned_size = static_cast<std::uint64_t>(end);
    if (unsigned_size > kMaximumFileBytes
        || unsigned_size > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()
        )) {
        error = "Audio file is too large: " + path_text(path);
        return false;
    }

    bytes.resize(static_cast<std::size_t>(unsigned_size));
    stream.seekg(0, std::ios::beg);
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    if (!stream) {
        error = "Failed while reading audio file: " + path_text(path);
        bytes.clear();
        return false;
    }
    return true;
}

struct DecodedTrack {
    std::vector<float> stereo;

    [[nodiscard]] std::uint64_t frames() const noexcept {
        return static_cast<std::uint64_t>(stereo.size() / kOutputChannels);
    }
};

struct CachedSound {
    std::string key;
    std::vector<float> stereo;

    [[nodiscard]] std::uint64_t frames() const noexcept {
        return static_cast<std::uint64_t>(stereo.size() / kOutputChannels);
    }

};

[[nodiscard]] std::string sound_cache_key(
    const std::filesystem::path& path
) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return path_text(error ? path.lexically_normal() : canonical);
}

[[nodiscard]] bool decode_ogg(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes,
    const std::uint32_t target_sample_rate,
    DecodedTrack& track,
    std::string& error
) {
    if (bytes.size() > static_cast<std::size_t>(
        std::numeric_limits<int>::max()
    )) {
        error = "Ogg file is too large for the decoder: " + path_text(path);
        return false;
    }

    int open_error = 0;
    stb_vorbis* const decoder = stb_vorbis_open_memory(
        bytes.data(),
        static_cast<int>(bytes.size()),
        &open_error,
        nullptr
    );
    if (decoder == nullptr) {
        error = "stb_vorbis could not inspect Ogg audio: "
            + path_text(path);
        return false;
    }
    struct VorbisGuard {
        stb_vorbis* decoder{};
        ~VorbisGuard() {
            if (decoder != nullptr) {
                stb_vorbis_close(decoder);
            }
        }
    } decoder_guard{decoder};

    const auto info = stb_vorbis_get_info(decoder);
    const auto probed_frames = static_cast<std::uint64_t>(
        stb_vorbis_stream_length_in_samples(decoder)
    );
    if (info.channels <= 0
        || info.channels > 32
        || info.sample_rate == 0U) {
        error = "Ogg stream metadata is invalid: "
            + path_text(path);
        return false;
    }
    if (probed_frames > kMaximumDecodedFrames) {
        error = "Decoded Ogg source exceeds the safe memory limit: "
            + path_text(path);
        return false;
    }

    // Decode incrementally instead of stb_vorbis_decode_memory(), whose
    // internal realloc loop has no caller-provided ceiling. A dishonest Ogg
    // length field therefore cannot bypass this hard per-chunk check.
    const auto source_channels = static_cast<std::size_t>(info.channels);
    std::vector<float> chunk(kDecodeChunkFrames * source_channels);
    std::vector<float> source_stereo;
    if (probed_frames > 0U) {
        source_stereo.reserve(
            static_cast<std::size_t>(probed_frames * kOutputChannels)
        );
    }
    while (true) {
        const int decoded_frames = stb_vorbis_get_samples_float_interleaved(
            decoder,
            info.channels,
            chunk.data(),
            static_cast<int>(chunk.size())
        );
        if (decoded_frames <= 0) {
            break;
        }
        const auto frames = static_cast<std::uint64_t>(decoded_frames);
        const auto current_frames = static_cast<std::uint64_t>(
            source_stereo.size() / kOutputChannels
        );
        if (frames > kMaximumDecodedFrames - current_frames) {
            error = "Decoded Ogg source exceeds the safe memory limit: "
                + path_text(path);
            return false;
        }

        const auto first_output = source_stereo.size();
        source_stereo.resize(
            first_output
            + static_cast<std::size_t>(frames * kOutputChannels)
        );
        for (std::uint64_t frame = 0; frame < frames; ++frame) {
            const auto source_index = static_cast<std::size_t>(
                frame * source_channels
            );
            const auto output_index = first_output + static_cast<std::size_t>(
                frame * kOutputChannels
            );
            source_stereo[output_index] = chunk[source_index];
            source_stereo[output_index + 1U] =
                source_channels > 1U
                    ? chunk[source_index + 1U]
                    : chunk[source_index];
        }
    }
    if (source_stereo.empty()
        || stb_vorbis_get_error(decoder) != VORBIS__no_error) {
        error = "stb_vorbis could not decode Ogg audio: " + path_text(path);
        return false;
    }

    const auto source_frame_count = static_cast<std::uint64_t>(
        source_stereo.size() / kOutputChannels
    );
    if (info.sample_rate == target_sample_rate) {
        track.stereo = std::move(source_stereo);
        return true;
    }
    const auto numerator = source_frame_count
        * static_cast<std::uint64_t>(target_sample_rate);
    const auto output_frames = (
        numerator + static_cast<std::uint64_t>(info.sample_rate) - 1U
    ) / static_cast<std::uint64_t>(info.sample_rate);

    if (output_frames == 0U || output_frames > kMaximumDecodedFrames
        || output_frames > (
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
            / kOutputChannels
        )) {
        error = "Decoded Ogg audio exceeds the safe memory limit: "
            + path_text(path);
        return false;
    }

    track.stereo.resize(
        static_cast<std::size_t>(output_frames * kOutputChannels)
    );

    for (std::uint64_t output_frame = 0; output_frame < output_frames;
         ++output_frame) {
        const auto source_numerator = output_frame
            * static_cast<std::uint64_t>(info.sample_rate);
        const auto source_frame = std::min(
            source_numerator / static_cast<std::uint64_t>(target_sample_rate),
            source_frame_count - 1U
        );
        const auto next_source_frame = std::min(
            source_frame + 1U,
            source_frame_count - 1U
        );
        const auto source_remainder = source_numerator
            % static_cast<std::uint64_t>(target_sample_rate);
        const auto fraction = static_cast<float>(
            static_cast<double>(source_remainder)
            / static_cast<double>(target_sample_rate)
        );

        const auto output_index = static_cast<std::size_t>(
            output_frame * kOutputChannels
        );
        const auto first_source_index = source_frame * kOutputChannels;
        const auto next_source_index = next_source_frame * kOutputChannels;

        for (std::uint32_t channel = 0; channel < kOutputChannels; ++channel) {
            const auto first = source_stereo[static_cast<std::size_t>(
                first_source_index + channel
            )];
            const auto next = source_stereo[static_cast<std::size_t>(
                next_source_index + channel
            )];
            track.stereo[output_index + channel] =
                first + ((next - first) * fraction);
        }
    }

    return true;
}

[[nodiscard]] bool decode_miniaudio(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes,
    const std::uint32_t target_sample_rate,
    DecodedTrack& track,
    std::string& error
) {
    auto decoder_config = ma_decoder_config_init(
        ma_format_f32,
        kOutputChannels,
        target_sample_rate
    );
    ma_decoder decoder{};
    const auto init_result = ma_decoder_init_memory(
        bytes.data(),
        bytes.size(),
        &decoder_config,
        &decoder
    );
    if (init_result != MA_SUCCESS) {
        error = "miniaudio could not decode " + path_text(path) + ": "
            + ma_result_description(init_result);
        return false;
    }

    struct DecoderGuard {
        ma_decoder* decoder{};
        ~DecoderGuard() {
            if (decoder != nullptr) {
                ma_decoder_uninit(decoder);
            }
        }
    } guard{&decoder};

    ma_uint64 expected_frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(
            &decoder,
            &expected_frames
        ) == MA_SUCCESS) {
        if (expected_frames > kMaximumDecodedFrames) {
            error = "Decoded audio exceeds the safe memory limit: "
                + path_text(path);
            return false;
        }
        if (expected_frames > 0U) {
            track.stereo.reserve(
                static_cast<std::size_t>(expected_frames * kOutputChannels)
            );
        }
    }

    std::array<float, kDecodeChunkFrames * kOutputChannels> chunk{};
    while (true) {
        ma_uint64 frames_read = 0;
        const auto read_result = ma_decoder_read_pcm_frames(
            &decoder,
            chunk.data(),
            kDecodeChunkFrames,
            &frames_read
        );
        if (read_result != MA_SUCCESS && read_result != MA_AT_END) {
            error = "Failed while decoding " + path_text(path) + ": "
                + ma_result_description(read_result);
            return false;
        }

        if (frames_read > 0U) {
            const auto current_frames = track.frames();
            if (frames_read > (kMaximumDecodedFrames - current_frames)) {
                error = "Decoded audio exceeds the safe memory limit: "
                    + path_text(path);
                return false;
            }

            const auto sample_count = static_cast<std::size_t>(
                frames_read * kOutputChannels
            );
            track.stereo.insert(
                track.stereo.end(),
                chunk.begin(),
                chunk.begin() + static_cast<std::ptrdiff_t>(sample_count)
            );
        }

        if (frames_read < kDecodeChunkFrames || read_result == MA_AT_END) {
            break;
        }
    }

    if (track.stereo.empty()) {
        error = "Decoded audio contains no PCM frames: " + path_text(path);
        return false;
    }
    return true;
}

[[nodiscard]] bool decode_file(
    const std::filesystem::path& path,
    const std::uint32_t target_sample_rate,
    DecodedTrack& track,
    std::string& error
) {
    std::vector<std::uint8_t> bytes;
    if (!read_file(path, bytes, error)) {
        return false;
    }

    if (is_ogg(path, bytes)) {
        return decode_ogg(path, bytes, target_sample_rate, track, error);
    }
    return decode_miniaudio(path, bytes, target_sample_rate, track, error);
}

[[nodiscard]] std::uint64_t milliseconds_to_frames(
    const double milliseconds,
    const std::uint32_t sample_rate
) noexcept {
    if (!std::isfinite(milliseconds) || milliseconds <= 0.0) {
        return 0U;
    }
    const auto frames = milliseconds
        * static_cast<double>(sample_rate) / 1'000.0;
    if (frames >= static_cast<double>(kMaximumClockFrames)) {
        return kMaximumClockFrames;
    }
    return static_cast<std::uint64_t>(std::llround(frames));
}

[[nodiscard]] std::uint64_t frames_to_q32(
    const std::uint64_t frames
) noexcept {
    return std::min(frames, kMaximumClockFrames) << 32U;
}

[[nodiscard]] float sample_track(
    const std::vector<float>& samples,
    const std::uint64_t frame_q32,
    const std::uint32_t channel
) noexcept {
    const auto frame = frame_q32 >> 32U;
    const auto total_frames =
        static_cast<std::uint64_t>(samples.size() / kOutputChannels);
    if (frame >= total_frames) {
        return 0.0F;
    }

    const auto next_frame = std::min(frame + 1U, total_frames - 1U);
    const auto fraction = static_cast<float>(
        static_cast<double>(
            static_cast<std::uint32_t>(frame_q32 & 0xFFFF'FFFFULL)
        ) / static_cast<double>(kQ32One)
    );
    const auto first = samples[static_cast<std::size_t>(
        (frame * kOutputChannels) + channel
    )];
    const auto next = samples[static_cast<std::size_t>(
        (next_frame * kOutputChannels) + channel
    )];
    return first + ((next - first) * fraction);
}

}  // namespace

struct AudioTransport::Impl {
#if defined(__APPLE__)
    SDL_AudioStream* device_stream{};
    bool audio_subsystem_initialized{};
    bool null_backend{};
    std::atomic<bool> null_thread_stop{};
    std::thread null_thread;
    std::mutex null_render_mutex;
    std::array<float, kMaximumBufferFrames * kOutputChannels>
        callback_scratch{};
#else
    ma_context context{};
    bool context_initialized{};
    ma_device device{};
#endif
    bool device_initialized{};

    std::vector<float> instrumental;
    std::vector<float> vocals;

    // PULSEFORGE_P1_1_18_PSYCH_SOUND_BANK_STATE_V1
    struct SoundVoice {
        std::atomic<CachedSound*> sound{};
        std::atomic<std::uint64_t> frame{};
        std::atomic<std::uint32_t> state{kSoundVoiceStopped};
        std::atomic<std::uint32_t> current_volume_bits{float_bits(1.0F)};
        std::atomic<bool> looping{};
        std::atomic<std::uint64_t> play_generation{};
        std::atomic<std::uint64_t> completion_generation{};

        std::atomic<std::uint32_t> fade_from_bits{float_bits(1.0F)};
        std::atomic<std::uint32_t> fade_to_bits{float_bits(1.0F)};
        std::atomic<std::uint64_t> fade_total_frames{};
        std::atomic<std::uint64_t> fade_sequence{};

        // Audio-callback-owned interpolation state. Main-thread commands are
        // published only through the atomics above.
        std::uint64_t applied_fade_sequence{};
        std::uint64_t local_fade_elapsed{};
        std::uint64_t local_fade_total{};
        float local_fade_from{1.0F};
        float local_fade_to{1.0F};

        // Main-thread-only tag/completion bookkeeping. The callback never
        // reads or writes these strings.
        std::string tag;
        std::uint64_t reported_completion_generation{};
    };

    std::vector<std::unique_ptr<CachedSound>> sound_cache;
    std::unordered_map<std::string, CachedSound*> sound_cache_by_key;
    std::array<SoundVoice, kMaximumSoundVoices> sound_voices{};
    std::unordered_map<std::string, std::size_t> sound_tag_to_voice;
    std::uint64_t sound_cache_bytes{};
    std::uint64_t next_sound_generation{1U};

    std::atomic<std::uint64_t> playhead_q32{};
    std::atomic<std::uint64_t> requested_seek_q32{};
    std::atomic<std::uint64_t> requested_seek_sequence{};
    std::atomic<std::uint64_t> applied_seek_sequence{};
    std::atomic<std::uint64_t> duration_frames{};
    std::atomic<std::uint64_t> rate_q32{rate_to_q32(1.0)};
    std::atomic<std::uint64_t> audio_offset_bits{double_bits(0.0)};
    std::atomic<std::uint64_t> latency_compensation_bits{double_bits(0.0)};
    std::atomic<std::uint32_t> state_value{
        static_cast<std::uint32_t>(AudioTransportState::uninitialized)
    };
    std::atomic<std::uint32_t> master_volume_bits{float_bits(1.0F)};
    std::atomic<std::uint32_t> instrumental_volume_bits{float_bits(1.0F)};
    std::atomic<std::uint32_t> vocals_volume_bits{float_bits(1.0F)};
    std::array<std::atomic<std::uint32_t>,
               AudioTransport::visualizer_bin_count> visualizer_level_bits{};
    std::atomic<bool> is_initialized{};
    std::atomic<bool> has_audio{};
    std::atomic<bool> silent{};
    std::atomic<bool> muted{};
    std::atomic<bool> looping{};
    std::atomic<bool> mute_when_unfocused{true};
    std::atomic<bool> focused{true};

    std::uint32_t output_sample_rate{48'000};
    std::uint32_t output_buffer_frames{256};
    float smoothed_master{};
    float smoothed_instrumental{1.0F};
    float smoothed_vocals{1.0F};

    [[nodiscard]] bool any_sound_voice_playing() const noexcept {
        return std::any_of(
            sound_voices.begin(),
            sound_voices.end(),
            [](const SoundVoice& voice) {
                return voice.state.load(std::memory_order_acquire)
                    == kSoundVoicePlaying;
            }
        );
    }

    [[nodiscard]] CachedSound* ensure_cached_sound(
        const std::filesystem::path& path,
        std::string& error
    ) {
        const auto key = sound_cache_key(path);
        if (const auto found = sound_cache_by_key.find(key);
            found != sound_cache_by_key.end()) {
            return found->second;
        }
        if (sound_cache.size() >= kMaximumCachedSounds) {
            error = "Psych sound cache reached the 64-source safety limit.";
            return nullptr;
        }

        DecodedTrack decoded;
        if (!decode_file(path, output_sample_rate, decoded, error)) {
            return nullptr;
        }
        const auto decoded_bytes = static_cast<std::uint64_t>(
            decoded.stereo.size() * sizeof(float)
        );
        if (decoded_bytes == 0U || decoded_bytes > kMaximumDecodedSoundBytes) {
            error = "Decoded Psych sound exceeds the 64 MiB per-source limit: "
                + path_text(path);
            return nullptr;
        }
        if (sound_cache_bytes > kMaximumSoundCacheBytes - decoded_bytes) {
            error = "Psych sound cache would exceed its 128 MiB safety limit.";
            return nullptr;
        }

        auto cached = std::make_unique<CachedSound>();
        cached->key = key;
        cached->stereo = std::move(decoded.stereo);
        auto* const pointer = cached.get();
        sound_cache.push_back(std::move(cached));
        sound_cache_by_key.emplace(pointer->key, pointer);
        sound_cache_bytes += decoded_bytes;
        error.clear();
        return pointer;
    }

    void detach_voice_tag(const std::size_t index) {
        if (index >= sound_voices.size()) {
            return;
        }
        auto& voice = sound_voices[index];
        if (!voice.tag.empty()) {
            const auto found = sound_tag_to_voice.find(voice.tag);
            if (found != sound_tag_to_voice.end()
                && found->second == index) {
                sound_tag_to_voice.erase(found);
            }
            voice.tag.clear();
        }
    }

    [[nodiscard]] std::size_t select_sound_voice(
        const std::string_view tag
    ) {
        if (!tag.empty()) {
            const auto existing = sound_tag_to_voice.find(std::string(tag));
            if (existing != sound_tag_to_voice.end()) {
                return existing->second;
            }
        }

        for (std::size_t index = 0U; index < sound_voices.size(); ++index) {
            if (sound_voices[index].state.load(std::memory_order_acquire)
                == kSoundVoiceStopped) {
                detach_voice_tag(index);
                return index;
            }
        }

        // Bounded voice stealing: replace the oldest generation rather than
        // allocating extra voices under modchart bursts.
        std::size_t oldest_index = 0U;
        std::uint64_t oldest_generation =
            sound_voices[0].play_generation.load(std::memory_order_acquire);
        for (std::size_t index = 1U; index < sound_voices.size(); ++index) {
            const auto generation = sound_voices[index].play_generation.load(
                std::memory_order_acquire
            );
            if (generation < oldest_generation) {
                oldest_generation = generation;
                oldest_index = index;
            }
        }
        sound_voices[oldest_index].state.store(
            kSoundVoiceStopped,
            std::memory_order_release
        );
        detach_voice_tag(oldest_index);
        return oldest_index;
    }

    [[nodiscard]] SoundVoice* tagged_sound_voice(
        const std::string_view tag
    ) noexcept {
        if (tag.empty()) {
            return nullptr;
        }
        for (auto& voice : sound_voices) {
            if (voice.tag == tag) {
                return &voice;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const SoundVoice* tagged_sound_voice(
        const std::string_view tag
    ) const noexcept {
        if (tag.empty()) {
            return nullptr;
        }
        for (const auto& voice : sound_voices) {
            if (voice.tag == tag) {
                return &voice;
            }
        }
        return nullptr;
    }

    void reset_sound_runtime() noexcept {
        for (auto& voice : sound_voices) {
            voice.state.store(kSoundVoiceStopped, std::memory_order_release);
            voice.sound.store(nullptr, std::memory_order_release);
            voice.frame.store(0U, std::memory_order_release);
            voice.looping.store(false, std::memory_order_release);
            voice.play_generation.store(0U, std::memory_order_release);
            voice.completion_generation.store(0U, std::memory_order_release);
            voice.current_volume_bits.store(
                float_bits(1.0F),
                std::memory_order_release
            );
            voice.fade_from_bits.store(
                float_bits(1.0F),
                std::memory_order_release
            );
            voice.fade_to_bits.store(
                float_bits(1.0F),
                std::memory_order_release
            );
            voice.fade_total_frames.store(0U, std::memory_order_release);
            static_cast<void>(voice.fade_sequence.fetch_add(
                1U,
                std::memory_order_acq_rel
            ));
            voice.tag.clear();
            voice.reported_completion_generation = 0U;
        }
        sound_tag_to_voice.clear();
    }

    void clear_visualizer() noexcept {
        for (auto& level : visualizer_level_bits) {
            level.store(float_bits(0.0F), std::memory_order_release);
        }
    }

    void decay_visualizer() noexcept {
        for (auto& level : visualizer_level_bits) {
            const float previous = bits_float(
                level.load(std::memory_order_relaxed)
            );
            const float decayed = previous * 0.78F;
            level.store(
                float_bits(decayed < 0.002F ? 0.0F : decayed),
                std::memory_order_release
            );
        }
    }

    [[nodiscard]] AudioTransportState state() const noexcept {
        return static_cast<AudioTransportState>(
            state_value.load(std::memory_order_acquire)
        );
    }

    void set_state(const AudioTransportState new_state) noexcept {
        state_value.store(
            static_cast<std::uint32_t>(new_state),
            std::memory_order_release
        );
    }

    [[nodiscard]] std::uint64_t visible_playhead_q32() const noexcept {
        if (requested_seek_sequence.load(std::memory_order_acquire)
            != applied_seek_sequence.load(std::memory_order_acquire)) {
            return requested_seek_q32.load(std::memory_order_acquire);
        }
        return playhead_q32.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool restart_device(std::string* const error) {
        if (!device_initialized) {
            if (error != nullptr) {
                assign_error(
                    error,
                    "The audio device has not been initialized."
                );
            }
            return false;
        }
#if defined(__APPLE__)
        if (null_backend) {
            return true;
        }
        if (device_stream == nullptr) {
            assign_error(error, "The SDL audio stream is unavailable.");
            return false;
        }
        if (!SDL_AudioStreamDevicePaused(device_stream)) {
            return true;
        }
        if (!SDL_ResumeAudioStreamDevice(device_stream)) {
            assign_error(
                error,
                std::string{"Unable to start the SDL audio device: "}
                    + SDL_GetError()
            );
            return false;
        }
        return true;
#else
        if (ma_device_is_started(&device) == MA_TRUE) {
            return true;
        }

        const auto result = ma_device_start(&device);
        if (result != MA_SUCCESS) {
            if (error != nullptr) {
                assign_error(
                    error,
                    std::string{"Unable to start the audio device: "}
                        + ma_result_description(result)
                );
            }
            return false;
        }
        return true;
#endif
    }

    [[nodiscard]] bool stop_device(std::string* const error) {
#if defined(__APPLE__)
        if (!device_initialized) {
            return true;
        }
        if (null_backend) {
            // Every caller publishes a non-playing state before stopping the
            // device. Taking this lock waits for any in-flight null callback,
            // after which future callbacks return before touching track data.
            const std::scoped_lock lock{null_render_mutex};
            return true;
        }
        if (device_stream == nullptr
            || SDL_AudioStreamDevicePaused(device_stream)) {
            return true;
        }
        if (!SDL_PauseAudioStreamDevice(device_stream)) {
            assign_error(
                error,
                std::string{"Unable to pause the SDL audio device: "}
                    + SDL_GetError()
            );
            return false;
        }
        return true;
#else
        if (!device_initialized || ma_device_is_started(&device) != MA_TRUE) {
            return true;
        }

        const auto result = ma_device_stop(&device);
        if (result != MA_SUCCESS) {
            if (error != nullptr) {
                assign_error(
                    error,
                    std::string{"Unable to stop the audio device: "}
                        + ma_result_description(result)
                );
            }
            return false;
        }
        return true;
#endif
    }

#if defined(__APPLE__)
    static void SDLCALL data_callback(
        void* const userdata,
        SDL_AudioStream* const stream,
        int additional_amount,
        int
    ) noexcept {
        auto* const implementation = static_cast<Impl*>(userdata);
        constexpr int bytes_per_frame = static_cast<int>(
            sizeof(float) * kOutputChannels
        );
        while (additional_amount > 0) {
            const auto requested_frames = static_cast<std::uint32_t>(
                (additional_amount + bytes_per_frame - 1) / bytes_per_frame
            );
            const auto frames = std::min(
                requested_frames,
                kMaximumBufferFrames
            );
            implementation->render(
                implementation->callback_scratch.data(),
                frames
            );
            const auto produced_bytes = static_cast<int>(
                frames * kOutputChannels * sizeof(float)
            );
            if (!SDL_PutAudioStreamData(
                    stream,
                    implementation->callback_scratch.data(),
                    produced_bytes
                )) {
                return;
            }
            additional_amount -= produced_bytes;
        }
    }
#else
    static void data_callback(
        ma_device* const device_pointer,
        void* const output,
        const void*,
        const ma_uint32 frame_count
    ) noexcept {
        auto* const implementation =
            static_cast<Impl*>(device_pointer->pUserData);
        implementation->render(
            static_cast<float*>(output),
            static_cast<std::uint32_t>(frame_count)
        );
    }
#endif

    void render(
        float* const output,
        const std::uint32_t frame_count
    ) noexcept {
        std::fill_n(
            output,
            static_cast<std::size_t>(frame_count) * kOutputChannels,
            0.0F
        );

        std::array<float, AudioTransport::visualizer_bin_count>
            visualizer_peaks{};

        auto position = playhead_q32.load(std::memory_order_relaxed);
        const auto seek_sequence =
            requested_seek_sequence.load(std::memory_order_acquire);
        if (seek_sequence
            != applied_seek_sequence.load(std::memory_order_relaxed)) {
            position = requested_seek_q32.load(std::memory_order_acquire);
            playhead_q32.store(position, std::memory_order_release);
            applied_seek_sequence.store(
                seek_sequence,
                std::memory_order_release
            );
        }

        bool song_rendering =
            state() == AudioTransportState::playing
            && has_audio.load(std::memory_order_acquire);
        const bool sounds_rendering = any_sound_voice_playing();
        if (!song_rendering && !sounds_rendering) {
            decay_visualizer();
            return;
        }

        const auto total_frames =
            duration_frames.load(std::memory_order_acquire);
        const auto total_q32 = frames_to_q32(total_frames);
        const auto increment = rate_q32.load(std::memory_order_relaxed);
        const bool output_muted = muted.load(std::memory_order_relaxed)
            || (mute_when_unfocused.load(std::memory_order_relaxed)
                && !focused.load(std::memory_order_relaxed));
        const float target_master = output_muted
            ? 0.0F
            : bits_float(master_volume_bits.load(std::memory_order_relaxed));
        const float target_instrumental = bits_float(
            instrumental_volume_bits.load(std::memory_order_relaxed)
        );
        const float target_vocals = bits_float(
            vocals_volume_bits.load(std::memory_order_relaxed)
        );

        constexpr float smoothing = 0.0025F;
        for (std::uint32_t frame_index = 0U;
             frame_index < frame_count;
             ++frame_index) {
            smoothed_master +=
                (target_master - smoothed_master) * smoothing;
            smoothed_instrumental +=
                (target_instrumental - smoothed_instrumental) * smoothing;
            smoothed_vocals +=
                (target_vocals - smoothed_vocals) * smoothing;

            float left = 0.0F;
            float right = 0.0F;

            if (song_rendering) {
                if (position >= total_q32) {
                    if (looping.load(std::memory_order_relaxed)
                        && total_q32 != 0U) {
                        position = 0U;
                    } else {
                        position = total_q32;
                        if (requested_seek_sequence.load(
                                std::memory_order_acquire
                            ) == seek_sequence) {
                            auto expected = static_cast<std::uint32_t>(
                                AudioTransportState::playing
                            );
                            const bool marked_ended =
                                state_value.compare_exchange_strong(
                                    expected,
                                    static_cast<std::uint32_t>(
                                        AudioTransportState::ended
                                    ),
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire
                                );
                            if (marked_ended
                                && requested_seek_sequence.load(
                                    std::memory_order_acquire
                                ) != seek_sequence) {
                                expected = static_cast<std::uint32_t>(
                                    AudioTransportState::ended
                                );
                                static_cast<void>(
                                    state_value.compare_exchange_strong(
                                        expected,
                                        static_cast<std::uint32_t>(
                                            AudioTransportState::playing
                                        ),
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire
                                    )
                                );
                            }
                        }
                        song_rendering = false;
                    }
                }

                if (song_rendering && total_q32 != 0U) {
                    left += (
                        sample_track(instrumental, position, 0U)
                            * smoothed_instrumental
                    ) + (
                        sample_track(vocals, position, 0U)
                            * smoothed_vocals
                    );
                    right += (
                        sample_track(instrumental, position, 1U)
                            * smoothed_instrumental
                    ) + (
                        sample_track(vocals, position, 1U)
                            * smoothed_vocals
                    );

                    if (increment > (total_q32 - position)) {
                        position = total_q32;
                    } else {
                        position += increment;
                    }
                }
            }

            // PULSEFORGE_P1_1_18_PSYCH_SOUND_MIXER_V1
            for (auto& voice : sound_voices) {
                if (voice.state.load(std::memory_order_acquire)
                    != kSoundVoicePlaying) {
                    continue;
                }

                // Snapshot the generation before dereferencing the voice.
                // A main-thread tag reuse that races this callback can then be
                // distinguished from the completion of the previous play.
                const auto voice_generation = voice.play_generation.load(
                    std::memory_order_acquire
                );
                auto* const sound = voice.sound.load(std::memory_order_acquire);
                if (sound == nullptr || sound->stereo.empty()) {
                    voice.state.store(
                        kSoundVoiceStopped,
                        std::memory_order_release
                    );
                    continue;
                }

                auto sound_frame = voice.frame.load(std::memory_order_relaxed);
                const auto sound_frames = sound->frames();
                if (sound_frame >= sound_frames) {
                    if (voice.looping.load(std::memory_order_relaxed)
                        && sound_frames != 0U) {
                        sound_frame = 0U;
                    } else {
                        voice.state.store(
                            kSoundVoiceStopped,
                            std::memory_order_release
                        );
                        voice.completion_generation.store(
                            voice_generation,
                            std::memory_order_release
                        );
                        continue;
                    }
                }

                const auto fade_sequence_value =
                    voice.fade_sequence.load(std::memory_order_acquire);
                if (fade_sequence_value != voice.applied_fade_sequence) {
                    voice.applied_fade_sequence = fade_sequence_value;
                    voice.local_fade_elapsed = 0U;
                    voice.local_fade_total =
                        voice.fade_total_frames.load(
                            std::memory_order_acquire
                        );
                    voice.local_fade_from = bits_float(
                        voice.fade_from_bits.load(std::memory_order_acquire)
                    );
                    voice.local_fade_to = bits_float(
                        voice.fade_to_bits.load(std::memory_order_acquire)
                    );
                }

                float voice_volume = bits_float(
                    voice.current_volume_bits.load(std::memory_order_relaxed)
                );
                if (voice.local_fade_total != 0U) {
                    const double progress = std::min(
                        1.0,
                        static_cast<double>(voice.local_fade_elapsed)
                            / static_cast<double>(voice.local_fade_total)
                    );
                    voice_volume = static_cast<float>(
                        static_cast<double>(voice.local_fade_from)
                        + (
                            static_cast<double>(
                                voice.local_fade_to - voice.local_fade_from
                            ) * progress
                        )
                    );
                    ++voice.local_fade_elapsed;
                    if (voice.local_fade_elapsed
                        >= voice.local_fade_total) {
                        voice_volume = voice.local_fade_to;
                        voice.local_fade_total = 0U;
                    }
                    voice.current_volume_bits.store(
                        float_bits(sanitize_volume(voice_volume)),
                        std::memory_order_release
                    );
                }

                const auto sample_index = static_cast<std::size_t>(
                    sound_frame * kOutputChannels
                );
                left += sound->stereo[sample_index] * voice_volume;
                right += sound->stereo[sample_index + 1U] * voice_volume;

                ++sound_frame;
                voice.frame.store(sound_frame, std::memory_order_release);
                if (sound_frame >= sound_frames
                    && !voice.looping.load(std::memory_order_relaxed)) {
                    voice.state.store(
                        kSoundVoiceStopped,
                        std::memory_order_release
                    );
                    voice.completion_generation.store(
                        voice_generation,
                        std::memory_order_release
                    );
                }
            }

            const auto output_index =
                static_cast<std::size_t>(frame_index) * kOutputChannels;
            output[output_index] = std::clamp(
                left * smoothed_master,
                -1.0F,
                1.0F
            );
            output[output_index + 1U] = std::clamp(
                right * smoothed_master,
                -1.0F,
                1.0F
            );

            if (frame_count != 0U) {
                const auto bin = std::min<std::size_t>(
                    static_cast<std::size_t>(
                        (static_cast<std::uint64_t>(frame_index)
                            * AudioTransport::visualizer_bin_count)
                        / frame_count
                    ),
                    AudioTransport::visualizer_bin_count - 1U
                );
                const float level = std::clamp(
                    (std::abs(output[output_index])
                        + std::abs(output[output_index + 1U])) * 0.5F,
                    0.0F,
                    1.0F
                );
                visualizer_peaks[bin] =
                    std::max(visualizer_peaks[bin], level);
            }
        }

        for (std::size_t index = 0U;
             index < visualizer_level_bits.size();
             ++index) {
            const float previous = bits_float(
                visualizer_level_bits[index].load(std::memory_order_relaxed)
            );
            const float target = std::clamp(
                visualizer_peaks[index] * 1.55F,
                0.0F,
                1.0F
            );
            const float response = target >= previous ? 0.70F : 0.18F;
            const float smoothed =
                previous + ((target - previous) * response);
            visualizer_level_bits[index].store(
                float_bits(smoothed),
                std::memory_order_release
            );
        }

        playhead_q32.store(position, std::memory_order_release);
    }

};

AudioTransport::AudioTransport()
    : impl_(std::make_unique<Impl>()) {
}

AudioTransport::~AudioTransport() {
    shutdown();
}

bool AudioTransport::initialize(
    const AudioSettings& settings,
    std::string* const error
) {
    return initialize(
        settings,
        AudioTransportBackend::system_default,
        error
    );
}

bool AudioTransport::initialize(
    const AudioSettings& settings,
    const AudioTransportBackend backend,
    std::string* const error
) {
    clear_error(error);
    shutdown();

    impl_->output_sample_rate = std::clamp(
        settings.sample_rate,
        kMinimumSampleRate,
        kMaximumSampleRate
    );
    impl_->output_buffer_frames = std::clamp(
        settings.buffer_frames,
        kMinimumBufferFrames,
        kMaximumBufferFrames
    );
    set_master_volume(settings.master_volume);
    set_instrumental_volume(settings.instrumental_volume);
    set_vocals_volume(settings.vocals_volume);
    set_muted(settings.muted);
    set_looping(false);
    set_playback_rate(settings.playback_rate);
    set_audio_offset_ms(settings.audio_offset_ms);
    set_output_latency_compensation_ms(
        settings.output_latency_compensation_ms
    );
    set_mute_when_unfocused(settings.mute_when_unfocused);
    impl_->focused.store(true, std::memory_order_release);

#if defined(__APPLE__)
    if (backend == AudioTransportBackend::null_device) {
        impl_->null_backend = true;
        impl_->device_initialized = true;
        impl_->is_initialized.store(true, std::memory_order_release);
        impl_->set_state(AudioTransportState::stopped);
        impl_->null_thread_stop.store(false, std::memory_order_release);
        try {
            impl_->null_thread = std::thread([implementation = impl_.get()] {
                using Clock = std::chrono::steady_clock;
                const auto period = std::chrono::duration_cast<
                    Clock::duration
                >(std::chrono::duration<double>{
                    static_cast<double>(implementation->output_buffer_frames)
                        / static_cast<double>(
                            implementation->output_sample_rate
                        )
                });
                auto deadline = Clock::now();
                while (!implementation->null_thread_stop.load(
                    std::memory_order_acquire
                )) {
                    {
                        const std::scoped_lock lock{
                            implementation->null_render_mutex
                        };
                        implementation->render(
                            implementation->callback_scratch.data(),
                            implementation->output_buffer_frames
                        );
                    }
                    deadline += period;
                    const auto now = Clock::now();
                    if (deadline < now - period) {
                        deadline = now;
                    }
                    std::this_thread::sleep_until(deadline);
                }
            });
        } catch (const std::exception& exception) {
            assign_error(
                error,
                std::string{"Unable to start the null audio clock: "}
                    + exception.what()
            );
            impl_->device_initialized = false;
            impl_->null_backend = false;
            impl_->is_initialized.store(false, std::memory_order_release);
            impl_->set_state(AudioTransportState::uninitialized);
            return false;
        }
        return true;
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        assign_error(
            error,
            std::string{"Unable to initialize SDL audio: "} + SDL_GetError()
        );
        impl_->set_state(AudioTransportState::uninitialized);
        return false;
    }
    impl_->audio_subsystem_initialized = true;

    const auto requested_frames = std::to_string(
        impl_->output_buffer_frames
    );
    static_cast<void>(SDL_SetHint(
        SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES,
        requested_frames.c_str()
    ));
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = static_cast<int>(kOutputChannels);
    spec.freq = static_cast<int>(impl_->output_sample_rate);
    impl_->device_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec,
        &Impl::data_callback,
        impl_.get()
    );
    if (impl_->device_stream == nullptr) {
        assign_error(
            error,
            std::string{"Unable to initialize the SDL audio device: "}
                + SDL_GetError()
        );
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        impl_->audio_subsystem_initialized = false;
        impl_->set_state(AudioTransportState::uninitialized);
        return false;
    }

    impl_->device_initialized = true;
    impl_->is_initialized.store(true, std::memory_order_release);
    impl_->set_state(AudioTransportState::stopped);
    if (!impl_->restart_device(error)) {
        shutdown();
        return false;
    }
    return true;
#else
    auto config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = kOutputChannels;
    config.sampleRate = impl_->output_sample_rate;
    config.periodSizeInFrames = impl_->output_buffer_frames;
    config.periods = 2U;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.noPreSilencedOutputBuffer = MA_TRUE;
    config.noClip = MA_TRUE;
    config.dataCallback = &Impl::data_callback;
    config.pUserData = impl_.get();

    ma_context* context = nullptr;
    if (backend == AudioTransportBackend::null_device) {
        constexpr ma_backend backends[]{ma_backend_null};
        const auto context_config = ma_context_config_init();
        const auto context_result = ma_context_init(
            backends,
            static_cast<ma_uint32>(std::size(backends)),
            &context_config,
            &impl_->context
        );
        if (context_result != MA_SUCCESS) {
            assign_error(
                error,
                std::string{"Unable to initialize the null audio context: "}
                    + ma_result_description(context_result)
            );
            impl_->set_state(AudioTransportState::uninitialized);
            return false;
        }
        impl_->context_initialized = true;
        context = &impl_->context;
    }

    const auto init_result = ma_device_init(context, &config, &impl_->device);
    if (init_result != MA_SUCCESS) {
        assign_error(
            error,
            std::string{"Unable to initialize the audio device: "}
                + ma_result_description(init_result)
        );
        impl_->set_state(AudioTransportState::uninitialized);
        if (impl_->context_initialized) {
            ma_context_uninit(&impl_->context);
            impl_->context_initialized = false;
        }
        return false;
    }

    impl_->device_initialized = true;
    impl_->output_sample_rate = impl_->device.sampleRate;
    impl_->is_initialized.store(true, std::memory_order_release);
    impl_->set_state(AudioTransportState::stopped);
    if (!impl_->restart_device(error)) {
        shutdown();
        return false;
    }
    return true;
#endif
}

void AudioTransport::shutdown() noexcept {
    if (!impl_) {
        return;
    }

    impl_->set_state(AudioTransportState::uninitialized);
    impl_->is_initialized.store(false, std::memory_order_release);
    impl_->has_audio.store(false, std::memory_order_release);
#if defined(__APPLE__)
    impl_->null_thread_stop.store(true, std::memory_order_release);
    if (impl_->null_thread.joinable()) {
        impl_->null_thread.join();
    }
    if (impl_->device_stream != nullptr) {
        SDL_DestroyAudioStream(impl_->device_stream);
        impl_->device_stream = nullptr;
    }
    impl_->device_initialized = false;
    impl_->null_backend = false;
    if (impl_->audio_subsystem_initialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        impl_->audio_subsystem_initialized = false;
    }
#else
    if (impl_->device_initialized) {
        ma_device_uninit(&impl_->device);
        impl_->device_initialized = false;
    }
    if (impl_->context_initialized) {
        ma_context_uninit(&impl_->context);
        impl_->context_initialized = false;
    }
#endif
    impl_->reset_sound_runtime();
    impl_->sound_cache_by_key.clear();
    impl_->sound_cache.clear();
    impl_->sound_cache_bytes = 0U;
    impl_->instrumental.clear();
    impl_->vocals.clear();
    impl_->duration_frames.store(0U, std::memory_order_release);
    impl_->playhead_q32.store(0U, std::memory_order_release);
    impl_->requested_seek_q32.store(0U, std::memory_order_release);
    impl_->requested_seek_sequence.store(0U, std::memory_order_release);
    impl_->applied_seek_sequence.store(0U, std::memory_order_release);
    impl_->silent.store(false, std::memory_order_release);
    impl_->clear_visualizer();
}

bool AudioTransport::load(
    const Chart& chart,
    std::string* const error
) {
    const double fallback_duration = std::max(
        kMinimumFallbackDurationMs,
        chart.duration_ms()
    );
    // Chart tempo drives musical callbacks and note placement only. Audio is
    // always decoded at its native duration and advances at the independently
    // configured playbackRate (1.0 by default).
    return load(chart.audio, fallback_duration, 120.0, error);
}

bool AudioTransport::load(
    const AudioManifest& audio,
    const double fallback_duration_ms,
    const double fallback_bpm,
    std::string* const error
) {
    clear_error(error);
    if (!initialized()) {
        assign_error(error, "Initialize AudioTransport before loading audio.");
        return false;
    }
    // Compatibility parameter retained for embedders using the 0.3 API. It no
    // longer affects audio generation or transport speed.
    static_cast<void>(fallback_bpm);

    // A failed replacement must never leave the previous song armed. This is
    // especially important for menus which reuse one transport while moving
    // between catalog entries.
    unload();

    try {
        DecodedTrack new_instrumental;
        DecodedTrack new_vocals;
        std::string decode_error;
        bool has_explicit_audio = false;

        if (!audio.instrumental.empty()) {
            has_explicit_audio = true;
            if (!decode_file(
                    audio.instrumental,
                    impl_->output_sample_rate,
                    new_instrumental,
                    decode_error
                )) {
                assign_error(error, std::move(decode_error));
                return false;
            }
        }

        std::size_t decoded_vocal_stems = 0U;
        for (const auto& vocal_path : audio.vocals) {
            if (vocal_path.empty()) {
                continue;
            }
            if (decoded_vocal_stems >= kMaximumVocalStems) {
                assign_error(
                    error,
                    "Audio manifest exceeds the eight-vocal-stem safety limit."
                );
                return false;
            }
            has_explicit_audio = true;

            DecodedTrack stem;
            if (!decode_file(
                    vocal_path,
                    impl_->output_sample_rate,
                    stem,
                    decode_error
                )) {
                assign_error(error, std::move(decode_error));
                return false;
            }

            if (stem.stereo.size() > new_vocals.stereo.size()) {
                new_vocals.stereo.resize(stem.stereo.size(), 0.0F);
            }
            for (std::size_t sample = 0; sample < stem.stereo.size();
                 ++sample) {
                new_vocals.stereo[sample] += stem.stereo[sample];
            }
            ++decoded_vocal_stems;
        }

        if (decoded_vocal_stems > 1U) {
            const float normalization = static_cast<float>(
                1.0 / std::sqrt(static_cast<double>(decoded_vocal_stems))
            );
            for (auto& sample : new_vocals.stereo) {
                sample *= normalization;
            }
        }

        const bool silent = !has_explicit_audio;
        std::uint64_t new_duration_frames = 0U;
        if (silent) {
            double safe_duration = fallback_duration_ms;
            if (!std::isfinite(safe_duration) || safe_duration <= 0.0) {
                safe_duration = kDefaultFallbackDurationMs;
            }
            safe_duration = std::clamp(
                safe_duration,
                kMinimumFallbackDurationMs,
                kMaximumFallbackDurationMs
            );
            const double maximum_clock_duration_ms =
                static_cast<double>(kMaximumClockFrames)
                / static_cast<double>(impl_->output_sample_rate)
                * 1'000.0;
            if (safe_duration > maximum_clock_duration_ms) {
                assign_error(
                    error,
                    "The silent song clock is too long for the configured "
                    "sample rate."
                );
                return false;
            }
            new_duration_frames = milliseconds_to_frames(
                safe_duration,
                impl_->output_sample_rate
            );
        } else {
            new_duration_frames = std::max(
                new_instrumental.frames(),
                new_vocals.frames()
            );
        }

        if (new_duration_frames == 0U) {
            assign_error(error, "The decoded song contains no audio frames.");
            return false;
        }
        if (new_duration_frames > kMaximumClockFrames) {
            assign_error(error, "The decoded song is too long for the clock.");
            return false;
        }

        impl_->set_state(AudioTransportState::stopped);
        if (!impl_->stop_device(error)) {
            return false;
        }

        impl_->instrumental = std::move(new_instrumental.stereo);
        impl_->vocals = std::move(new_vocals.stereo);
        impl_->duration_frames.store(
            new_duration_frames,
            std::memory_order_release
        );
        impl_->playhead_q32.store(0U, std::memory_order_release);
        impl_->requested_seek_q32.store(0U, std::memory_order_release);
        impl_->requested_seek_sequence.store(0U, std::memory_order_release);
        impl_->applied_seek_sequence.store(0U, std::memory_order_release);
        impl_->silent.store(silent, std::memory_order_release);
        impl_->smoothed_master = 0.0F;
        impl_->smoothed_instrumental = instrumental_volume();
        impl_->smoothed_vocals = vocals_volume();
        impl_->has_audio.store(true, std::memory_order_release);

        if (!impl_->restart_device(error)) {
            impl_->has_audio.store(false, std::memory_order_release);
            return false;
        }
        return true;
    } catch (const std::bad_alloc&) {
        assign_error(error, "Not enough memory to decode the song audio.");
        return false;
    } catch (const std::exception& exception) {
        assign_error(
            error,
            std::string{"Unable to load song audio: "} + exception.what()
        );
        return false;
    }
}

void AudioTransport::unload() noexcept {
    if (!impl_ || !initialized()) {
        return;
    }

    stop_all_sounds();
    impl_->set_state(AudioTransportState::stopped);
    if (!impl_->stop_device(nullptr)) {
        return;
    }

    impl_->has_audio.store(false, std::memory_order_release);
    std::vector<float>{}.swap(impl_->instrumental);
    std::vector<float>{}.swap(impl_->vocals);
    impl_->duration_frames.store(0U, std::memory_order_release);
    impl_->playhead_q32.store(0U, std::memory_order_release);
    impl_->requested_seek_q32.store(0U, std::memory_order_release);
    impl_->requested_seek_sequence.store(0U, std::memory_order_release);
    impl_->applied_seek_sequence.store(0U, std::memory_order_release);
    impl_->silent.store(false, std::memory_order_release);
    impl_->clear_visualizer();
    static_cast<void>(impl_->restart_device(nullptr));
}

void AudioTransport::play() noexcept {
    if (!loaded()) {
        return;
    }
    if (state() == AudioTransportState::ended) {
        impl_->requested_seek_q32.store(0U, std::memory_order_release);
        static_cast<void>(impl_->requested_seek_sequence.fetch_add(
            1U,
            std::memory_order_acq_rel
        ));
    }
    static_cast<void>(impl_->restart_device(nullptr));
    impl_->set_state(AudioTransportState::playing);
}

void AudioTransport::pause() noexcept {
    auto expected = static_cast<std::uint32_t>(
        AudioTransportState::playing
    );
    static_cast<void>(impl_->state_value.compare_exchange_strong(
        expected,
        static_cast<std::uint32_t>(AudioTransportState::paused),
        std::memory_order_acq_rel,
        std::memory_order_acquire
    ));
}

void AudioTransport::resume() noexcept {
    if (!loaded()) {
        return;
    }
    auto expected = static_cast<std::uint32_t>(
        AudioTransportState::paused
    );
    if (impl_->state_value.compare_exchange_strong(
            expected,
            static_cast<std::uint32_t>(AudioTransportState::playing),
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        static_cast<void>(impl_->restart_device(nullptr));
    }
}

void AudioTransport::stop() noexcept {
    if (!initialized()) {
        return;
    }
    impl_->set_state(AudioTransportState::stopped);
    impl_->requested_seek_q32.store(0U, std::memory_order_release);
    static_cast<void>(impl_->requested_seek_sequence.fetch_add(
        1U,
        std::memory_order_acq_rel
    ));
}

void AudioTransport::seek_ms(const double position_ms) noexcept {
    if (!loaded()) {
        return;
    }

    double safe_position = position_ms;
    if (!std::isfinite(safe_position)) {
        safe_position = 0.0;
    }
    safe_position = std::clamp(safe_position, 0.0, duration_ms());
    const auto target_frames = milliseconds_to_frames(
        safe_position,
        impl_->output_sample_rate
    );
    impl_->requested_seek_q32.store(
        frames_to_q32(target_frames),
        std::memory_order_release
    );
    static_cast<void>(impl_->requested_seek_sequence.fetch_add(
        1U,
        std::memory_order_acq_rel
    ));
    if (state() == AudioTransportState::ended
        && target_frames < impl_->duration_frames.load(
            std::memory_order_acquire
        )) {
        impl_->set_state(AudioTransportState::paused);
    }
}

void AudioTransport::set_playback_rate(const double rate) noexcept {
    impl_->rate_q32.store(rate_to_q32(rate), std::memory_order_release);
}

void AudioTransport::set_master_volume(const float volume) noexcept {
    impl_->master_volume_bits.store(
        float_bits(sanitize_volume(volume)),
        std::memory_order_release
    );
}

void AudioTransport::set_instrumental_volume(const float volume) noexcept {
    impl_->instrumental_volume_bits.store(
        float_bits(sanitize_volume(volume)),
        std::memory_order_release
    );
}

void AudioTransport::set_vocals_volume(const float volume) noexcept {
    impl_->vocals_volume_bits.store(
        float_bits(sanitize_volume(volume)),
        std::memory_order_release
    );
}

void AudioTransport::set_muted(const bool should_mute) noexcept {
    impl_->muted.store(should_mute, std::memory_order_release);
}

void AudioTransport::set_looping(const bool should_loop) noexcept {
    impl_->looping.store(should_loop, std::memory_order_release);
}

void AudioTransport::set_audio_offset_ms(const double offset_ms) noexcept {
    impl_->audio_offset_bits.store(
        double_bits(sanitize_timing_adjustment(offset_ms)),
        std::memory_order_release
    );
}

void AudioTransport::set_output_latency_compensation_ms(
    const double compensation_ms
) noexcept {
    impl_->latency_compensation_bits.store(
        double_bits(sanitize_timing_adjustment(compensation_ms)),
        std::memory_order_release
    );
}

void AudioTransport::set_mute_when_unfocused(const bool enabled) noexcept {
    impl_->mute_when_unfocused.store(enabled, std::memory_order_release);
}

void AudioTransport::set_focused(const bool is_focused) noexcept {
    impl_->focused.store(is_focused, std::memory_order_release);
}

// PULSEFORGE_P1_1_18_PSYCH_SOUND_BANK_IMPL_V1
bool AudioTransport::precache_sound(
    const std::filesystem::path& path,
    std::string* const error
) {
    clear_error(error);
    if (!initialized()) {
        assign_error(error, "Initialize AudioTransport before precaching sounds.");
        return false;
    }
    if (path.empty()) {
        assign_error(error, "Psych sound path cannot be empty.");
        return false;
    }
    try {
        std::string local_error;
        const bool cached =
            impl_->ensure_cached_sound(path, local_error) != nullptr;
        if (!cached) {
            assign_error(error, std::move(local_error));
        }
        return cached;
    } catch (const std::bad_alloc&) {
        assign_error(error, "Not enough memory to cache the Psych sound.");
        return false;
    } catch (const std::exception& exception) {
        assign_error(
            error,
            std::string{"Unable to cache Psych sound: "} + exception.what()
        );
        return false;
    }
}

bool AudioTransport::play_sound(
    const std::filesystem::path& path,
    const float volume,
    const std::string_view tag,
    const bool should_loop,
    std::string* const error
) {
    clear_error(error);
    if (!initialized()) {
        assign_error(error, "Initialize AudioTransport before playing sounds.");
        return false;
    }
    if (tag.size() > kMaximumSoundTagBytes) {
        assign_error(error, "Psych sound tag exceeds 128 bytes.");
        return false;
    }

    try {
        std::string local_error;
        auto* const sound = impl_->ensure_cached_sound(path, local_error);
        if (sound == nullptr) {
            assign_error(error, std::move(local_error));
            return false;
        }

        const auto index = impl_->select_sound_voice(tag);
        auto& voice = impl_->sound_voices[index];
        voice.state.store(kSoundVoiceStopped, std::memory_order_release);
        voice.sound.store(sound, std::memory_order_release);
        voice.frame.store(0U, std::memory_order_release);
        voice.looping.store(should_loop, std::memory_order_release);

        const float safe_volume = sanitize_volume(volume);
        voice.current_volume_bits.store(
            float_bits(safe_volume),
            std::memory_order_release
        );
        voice.fade_from_bits.store(
            float_bits(safe_volume),
            std::memory_order_release
        );
        voice.fade_to_bits.store(
            float_bits(safe_volume),
            std::memory_order_release
        );
        voice.fade_total_frames.store(0U, std::memory_order_release);
        static_cast<void>(voice.fade_sequence.fetch_add(
            1U,
            std::memory_order_acq_rel
        ));

        auto generation = impl_->next_sound_generation++;
        if (generation == 0U) {
            generation = impl_->next_sound_generation++;
        }
        voice.play_generation.store(generation, std::memory_order_release);
        voice.completion_generation.store(0U, std::memory_order_release);
        voice.reported_completion_generation = 0U;

        if (!tag.empty()) {
            voice.tag.assign(tag);
            impl_->sound_tag_to_voice[voice.tag] = index;
        } else {
            voice.tag.clear();
        }

        if (!impl_->restart_device(error)) {
            voice.state.store(kSoundVoiceStopped, std::memory_order_release);
            return false;
        }
        voice.state.store(kSoundVoicePlaying, std::memory_order_release);
        return true;
    } catch (const std::bad_alloc&) {
        assign_error(error, "Not enough memory to start the Psych sound.");
        return false;
    } catch (const std::exception& exception) {
        assign_error(
            error,
            std::string{"Unable to start Psych sound: "} + exception.what()
        );
        return false;
    }
}

bool AudioTransport::stop_sound(const std::string_view tag) noexcept {
    auto* const voice = impl_->tagged_sound_voice(tag);
    if (voice == nullptr) {
        return false;
    }
    voice->state.store(kSoundVoiceStopped, std::memory_order_release);
    return true;
}

bool AudioTransport::pause_sound(const std::string_view tag) noexcept {
    auto* const voice = impl_->tagged_sound_voice(tag);
    if (voice == nullptr) {
        return false;
    }
    auto expected = kSoundVoicePlaying;
    return voice->state.compare_exchange_strong(
        expected,
        kSoundVoicePaused,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
}

bool AudioTransport::resume_sound(const std::string_view tag) noexcept {
    auto* const voice = impl_->tagged_sound_voice(tag);
    if (voice == nullptr) {
        return false;
    }
    auto expected = kSoundVoicePaused;
    const bool resumed = voice->state.compare_exchange_strong(
        expected,
        kSoundVoicePlaying,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
    if (resumed) {
        static_cast<void>(impl_->restart_device(nullptr));
    }
    return resumed;
}

bool AudioTransport::set_sound_volume(
    const std::string_view tag,
    const float volume
) noexcept {
    auto* const voice = impl_->tagged_sound_voice(tag);
    if (voice == nullptr) {
        return false;
    }
    const float safe_volume = sanitize_volume(volume);
    voice->current_volume_bits.store(
        float_bits(safe_volume),
        std::memory_order_release
    );
    voice->fade_from_bits.store(
        float_bits(safe_volume),
        std::memory_order_release
    );
    voice->fade_to_bits.store(
        float_bits(safe_volume),
        std::memory_order_release
    );
    voice->fade_total_frames.store(0U, std::memory_order_release);
    static_cast<void>(voice->fade_sequence.fetch_add(
        1U,
        std::memory_order_acq_rel
    ));
    return true;
}

bool AudioTransport::sound_volume(
    const std::string_view tag,
    float& volume
) const noexcept {
    const auto* const voice = impl_->tagged_sound_voice(tag);
    if (voice == nullptr) {
        return false;
    }
    volume = bits_float(
        voice->current_volume_bits.load(std::memory_order_acquire)
    );
    return true;
}

bool AudioTransport::set_sound_time_ms(
    const std::string_view tag,
    const double position_ms
) noexcept {
    auto* const voice = impl_->tagged_sound_voice(tag);
    if (voice == nullptr) {
        return false;
    }
    auto* const sound = voice->sound.load(std::memory_order_acquire);
    if (sound == nullptr || sound->frames() == 0U) {
        return false;
    }
    double safe_ms = std::isfinite(position_ms) ? position_ms : 0.0;
    const double duration = static_cast<double>(sound->frames()) * 1'000.0
        / static_cast<double>(impl_->output_sample_rate);
    safe_ms = std::clamp(safe_ms, 0.0, duration);
    const auto frame = std::min(
        milliseconds_to_frames(safe_ms, impl_->output_sample_rate),
        sound->frames()
    );
    voice->frame.store(frame, std::memory_order_release);
    return true;
}

bool AudioTransport::sound_time_ms(
    const std::string_view tag,
    double& position_ms
) const noexcept {
    const auto* const voice = impl_->tagged_sound_voice(tag);
    if (voice == nullptr || impl_->output_sample_rate == 0U) {
        return false;
    }
    position_ms = static_cast<double>(
        voice->frame.load(std::memory_order_acquire)
    ) * 1'000.0 / static_cast<double>(impl_->output_sample_rate);
    return true;
}

bool AudioTransport::sound_playing(const std::string_view tag) const noexcept {
    const auto* const voice = impl_->tagged_sound_voice(tag);
    return voice != nullptr
        && voice->state.load(std::memory_order_acquire)
            == kSoundVoicePlaying;
}

bool AudioTransport::fade_sound(
    const std::string_view tag,
    const double duration_seconds,
    const float from_volume,
    const float to_volume
) noexcept {
    auto* const voice = impl_->tagged_sound_voice(tag);
    if (voice == nullptr) {
        return false;
    }

    const double safe_seconds = std::clamp(
        std::isfinite(duration_seconds) ? duration_seconds : 0.0,
        0.0,
        kMaximumSoundFadeSeconds
    );
    const float safe_from = sanitize_volume(from_volume);
    const float safe_to = sanitize_volume(to_volume);
    const auto frames = static_cast<std::uint64_t>(std::llround(
        safe_seconds * static_cast<double>(impl_->output_sample_rate)
    ));

    voice->current_volume_bits.store(
        float_bits(safe_from),
        std::memory_order_release
    );
    voice->fade_from_bits.store(
        float_bits(safe_from),
        std::memory_order_release
    );
    voice->fade_to_bits.store(
        float_bits(safe_to),
        std::memory_order_release
    );
    voice->fade_total_frames.store(frames, std::memory_order_release);
    static_cast<void>(voice->fade_sequence.fetch_add(
        1U,
        std::memory_order_acq_rel
    ));
    if (frames == 0U) {
        voice->current_volume_bits.store(
            float_bits(safe_to),
            std::memory_order_release
        );
    }
    return true;
}

bool AudioTransport::cancel_sound_fade(
    const std::string_view tag
) noexcept {
    auto* const voice = impl_->tagged_sound_voice(tag);
    if (voice == nullptr) {
        return false;
    }
    const float current = bits_float(
        voice->current_volume_bits.load(std::memory_order_acquire)
    );
    voice->fade_from_bits.store(
        float_bits(current),
        std::memory_order_release
    );
    voice->fade_to_bits.store(
        float_bits(current),
        std::memory_order_release
    );
    voice->fade_total_frames.store(0U, std::memory_order_release);
    static_cast<void>(voice->fade_sequence.fetch_add(
        1U,
        std::memory_order_acq_rel
    ));
    return true;
}

std::vector<std::string> AudioTransport::consume_sound_completions() {
    std::vector<std::string> result;
    result.reserve(4U);
    for (std::size_t index = 0U;
         index < impl_->sound_voices.size();
         ++index) {
        auto& voice = impl_->sound_voices[index];
        const auto completed = voice.completion_generation.load(
            std::memory_order_acquire
        );
        if (completed == 0U
            || completed == voice.reported_completion_generation) {
            continue;
        }
        voice.reported_completion_generation = completed;
        const auto current_generation = voice.play_generation.load(
            std::memory_order_acquire
        );
        if (completed != current_generation || voice.tag.empty()) {
            continue;
        }
        result.push_back(voice.tag);
        if (voice.state.load(std::memory_order_acquire)
            == kSoundVoiceStopped) {
            const auto found = impl_->sound_tag_to_voice.find(voice.tag);
            if (found != impl_->sound_tag_to_voice.end()
                && found->second == index) {
                impl_->sound_tag_to_voice.erase(found);
            }
            voice.tag.clear();
        }
    }
    return result;
}

void AudioTransport::stop_all_sounds() noexcept {
    if (!impl_) {
        return;
    }
    for (auto& voice : impl_->sound_voices) {
        voice.state.store(kSoundVoiceStopped, std::memory_order_release);
        voice.completion_generation.store(0U, std::memory_order_release);
        voice.tag.clear();
    }
    impl_->sound_tag_to_voice.clear();
}

std::uint64_t AudioTransport::clock_frames() const noexcept {
    return impl_->visible_playhead_q32() >> 32U;
}

double AudioTransport::position_ms() const noexcept {
    if (impl_->output_sample_rate == 0U) {
        return 0.0;
    }
    const auto q32 = impl_->visible_playhead_q32();
    const double frames =
        static_cast<double>(q32) / static_cast<double>(kQ32One);
    return frames * 1'000.0
        / static_cast<double>(impl_->output_sample_rate);
}

double AudioTransport::compensated_position_ms() const noexcept {
    return position_ms()
        + bits_double(impl_->audio_offset_bits.load(std::memory_order_acquire))
        - bits_double(
            impl_->latency_compensation_bits.load(std::memory_order_acquire)
        );
}

double AudioTransport::duration_ms() const noexcept {
    if (impl_->output_sample_rate == 0U) {
        return 0.0;
    }
    return static_cast<double>(
        impl_->duration_frames.load(std::memory_order_acquire)
    ) * 1'000.0 / static_cast<double>(impl_->output_sample_rate);
}

std::uint32_t AudioTransport::sample_rate() const noexcept {
    return impl_->output_sample_rate;
}

double AudioTransport::playback_rate() const noexcept {
    return static_cast<double>(
        impl_->rate_q32.load(std::memory_order_acquire)
    ) / static_cast<double>(kQ32One);
}

float AudioTransport::master_volume() const noexcept {
    return bits_float(
        impl_->master_volume_bits.load(std::memory_order_acquire)
    );
}

float AudioTransport::instrumental_volume() const noexcept {
    return bits_float(
        impl_->instrumental_volume_bits.load(std::memory_order_acquire)
    );
}

float AudioTransport::vocals_volume() const noexcept {
    return bits_float(
        impl_->vocals_volume_bits.load(std::memory_order_acquire)
    );
}

std::array<float, AudioTransport::visualizer_bin_count>
AudioTransport::visualizer_levels() const noexcept {
    std::array<float, visualizer_bin_count> result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        result[index] = std::clamp(
            bits_float(
                impl_->visualizer_level_bits[index].load(
                    std::memory_order_acquire
                )
            ),
            0.0F,
            1.0F
        );
    }
    return result;
}

bool AudioTransport::muted() const noexcept {
    return impl_->muted.load(std::memory_order_acquire);
}

bool AudioTransport::looping() const noexcept {
    return impl_->looping.load(std::memory_order_acquire);
}

AudioTransportState AudioTransport::state() const noexcept {
    return impl_->state();
}

bool AudioTransport::initialized() const noexcept {
    return impl_->is_initialized.load(std::memory_order_acquire);
}

bool AudioTransport::loaded() const noexcept {
    return impl_->has_audio.load(std::memory_order_acquire);
}

bool AudioTransport::using_silent_audio() const noexcept {
    return impl_->silent.load(std::memory_order_acquire);
}

bool AudioTransport::using_procedural_audio() const noexcept {
    return false;
}

}  // namespace pulseforge
