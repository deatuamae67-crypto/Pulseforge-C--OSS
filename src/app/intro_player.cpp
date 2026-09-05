#include "intro_player.hpp"

#include "pulseforge/audio_transport.hpp"

#include <SDL3/SDL.h>

#include <nlohmann/json.hpp>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <mfplay.h>
#include <objbase.h>
#endif

namespace pulseforge::detail {

std::uint64_t native_intro_timeout_ns(
    const std::uint64_t media_duration_100ns
) noexcept {
    constexpr std::uint64_t nanoseconds_per_100ns = 100ULL;
    constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000ULL;
    constexpr std::uint64_t completion_margin_ns =
        30ULL * nanoseconds_per_second;
    constexpr std::uint64_t minimum_known_timeout_ns =
        60ULL * nanoseconds_per_second;
    constexpr std::uint64_t unknown_duration_timeout_ns =
        10ULL * 60ULL * nanoseconds_per_second;
    constexpr std::uint64_t maximum_timeout_ns =
        6ULL * 60ULL * 60ULL * nanoseconds_per_second;

    if (media_duration_100ns == 0U) {
        return unknown_duration_timeout_ns;
    }
    const auto duration_ns = media_duration_100ns
            > maximum_timeout_ns / nanoseconds_per_100ns
        ? maximum_timeout_ns
        : media_duration_100ns * nanoseconds_per_100ns;
    if (duration_ns >= maximum_timeout_ns - completion_margin_ns) {
        return maximum_timeout_ns;
    }
    return std::clamp(
        duration_ns + completion_margin_ns,
        minimum_known_timeout_ns,
        maximum_timeout_ns
    );
}

namespace {

constexpr float logical_width = 1'280.0F;
constexpr float logical_height = 720.0F;
constexpr std::uintmax_t maximum_sequence_manifest_bytes = 64U * 1024U;
constexpr std::uintmax_t maximum_sequence_frame_bytes = 4U * 1024U * 1024U;

using Json = nlohmann::json;

struct DecodedSequenceManifest {
    std::filesystem::path directory;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t frames_per_second{};
    std::uint32_t frame_count{};
    std::filesystem::path audio_path;
};

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const noexcept {
        SDL_DestroyTexture(texture);
    }
};

using TexturePointer = std::unique_ptr<SDL_Texture, TextureDeleter>;

void fill_rect(
    SDL_Renderer* renderer,
    const SDL_FRect& rectangle,
    const SDL_Color color
) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    static_cast<void>(SDL_RenderFillRect(renderer, &rectangle));
}

void draw_text(
    SDL_Renderer* renderer,
    const float x,
    const float y,
    const char* text,
    const SDL_Color color,
    const float scale
) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    float old_x = 1.0F;
    float old_y = 1.0F;
    static_cast<void>(SDL_GetRenderScale(renderer, &old_x, &old_y));
    static_cast<void>(SDL_SetRenderScale(renderer, scale, scale));
    static_cast<void>(SDL_RenderDebugText(
        renderer,
        x / scale,
        y / scale,
        text
    ));
    static_cast<void>(SDL_SetRenderScale(renderer, old_x, old_y));
}

[[nodiscard]] bool is_skip_event(const SDL_Event& event) noexcept {
    return event.type == SDL_EVENT_FINGER_UP
        || (event.type == SDL_EVENT_KEY_DOWN
            && (event.key.scancode == SDL_SCANCODE_RETURN
                || event.key.scancode == SDL_SCANCODE_KP_ENTER
                || event.key.scancode == SDL_SCANCODE_SPACE));
}

[[nodiscard]] std::optional<std::vector<stbi_uc>> read_bounded_binary(
    const std::filesystem::path& path,
    const std::uintmax_t maximum_bytes,
    std::string& error
) {
    std::error_code filesystem_error;
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || size == 0U || size > maximum_bytes
        || size > static_cast<std::uintmax_t>(
            std::numeric_limits<std::streamsize>::max()
        )) {
        error = "decoded intro asset is missing or exceeds its byte limit";
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "decoded intro asset could not be opened";
        return std::nullopt;
    }
    std::vector<stbi_uc> bytes(static_cast<std::size_t>(size));
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    if (!input) {
        error = "decoded intro asset could not be read completely";
        return std::nullopt;
    }
    return bytes;
}

[[nodiscard]] std::optional<DecodedSequenceManifest> load_decoded_manifest(
    const std::filesystem::path& directory,
    std::string& error
) {
    const auto manifest_path = directory / "manifest.json";
    const auto bytes = read_bounded_binary(
        manifest_path,
        maximum_sequence_manifest_bytes,
        error
    );
    if (!bytes.has_value()) {
        error = "decoded intro manifest is unavailable";
        return std::nullopt;
    }
    try {
        const auto manifest = Json::parse(bytes->begin(), bytes->end());
        if (!manifest.is_object()
            || manifest.value("format", std::string{})
                != "pulseforge-intro-sequence"
            || manifest.value("version", 0U) != 1U
            || manifest.value("framePattern", std::string{})
                != "frames/frame-%04u.png"
            || manifest.value("audio", std::string{}) != "audio.ogg") {
            error = "decoded intro manifest has an unsupported schema";
            return std::nullopt;
        }
        DecodedSequenceManifest result;
        result.directory = directory;
        result.width = manifest.value("width", 0U);
        result.height = manifest.value("height", 0U);
        result.frames_per_second = manifest.value("framesPerSecond", 0U);
        result.frame_count = manifest.value("frameCount", 0U);
        result.audio_path = directory / "audio.ogg";
        if (result.width < 16U || result.width > 1'920U
            || result.height < 16U || result.height > 1'080U
            || result.frames_per_second == 0U
            || result.frames_per_second > 60U
            || result.frame_count == 0U || result.frame_count > 9'999U) {
            error = "decoded intro manifest exceeds its runtime limits";
            return std::nullopt;
        }
        std::error_code audio_error;
        if (!std::filesystem::is_regular_file(result.audio_path, audio_error)
            || audio_error) {
            error = "decoded intro audio is unavailable";
            return std::nullopt;
        }
        return result;
    } catch (const std::exception& exception) {
        error = "decoded intro manifest is invalid: "
            + std::string(exception.what());
        return std::nullopt;
    }
}

[[nodiscard]] bool upload_sequence_frame(
    SDL_Texture* texture,
    const DecodedSequenceManifest& sequence,
    const std::uint32_t zero_based_index,
    std::string& error
) {
    std::array<char, 32U> filename{};
    const int written = std::snprintf(
        filename.data(),
        filename.size(),
        "frame-%04u.png",
        zero_based_index + 1U
    );
    if (written <= 0
        || static_cast<std::size_t>(written) >= filename.size()) {
        error = "decoded intro frame name overflowed";
        return false;
    }
    const auto bytes = read_bounded_binary(
        sequence.directory / "frames" / filename.data(),
        maximum_sequence_frame_bytes,
        error
    );
    if (!bytes.has_value()
        || bytes->size() > static_cast<std::size_t>(
            std::numeric_limits<int>::max()
        )) {
        return false;
    }
    int width{};
    int height{};
    int components{};
    stbi_uc* pixels = stbi_load_from_memory(
        bytes->data(),
        static_cast<int>(bytes->size()),
        &width,
        &height,
        &components,
        STBI_rgb_alpha
    );
    if (pixels == nullptr
        || width != static_cast<int>(sequence.width)
        || height != static_cast<int>(sequence.height)) {
        stbi_image_free(pixels);
        error = "decoded intro PNG has invalid dimensions or data";
        return false;
    }
    const bool updated = SDL_UpdateTexture(
        texture,
        nullptr,
        pixels,
        width * 4
    );
    stbi_image_free(pixels);
    if (!updated) {
        error = "decoded intro texture upload failed: "
            + std::string(SDL_GetError());
    }
    return updated;
}

[[nodiscard]] StartupIntroResult play_decoded_sequence(
    SDL_Renderer* renderer,
    const std::filesystem::path& directory,
    const AudioSettings& configured_audio,
    const StartupIntroPlaybackOptions& playback
) {
    std::string error;
    const auto sequence = load_decoded_manifest(directory, error);
    if (!sequence.has_value()) {
        return {StartupIntroStatus::fallback_played, std::move(error)};
    }
    TexturePointer texture(SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING,
        static_cast<int>(sequence->width),
        static_cast<int>(sequence->height)
    ));
    if (!texture) {
        return {
            StartupIntroStatus::fallback_played,
            "decoded intro texture creation failed: "
                + std::string(SDL_GetError()),
        };
    }
    static_cast<void>(SDL_SetTextureScaleMode(
        texture.get(),
        SDL_SCALEMODE_LINEAR
    ));

    AudioTransport audio;
    AudioSettings audio_settings = configured_audio;
    audio_settings.playback_rate = 1.0;
    AudioManifest soundtrack;
    soundtrack.instrumental = sequence->audio_path;
    std::string audio_error;
    const double duration_ms = static_cast<double>(sequence->frame_count)
        * 1'000.0 / static_cast<double>(sequence->frames_per_second);
    const bool audio_ready = audio.initialize(audio_settings, &audio_error)
        && audio.load(soundtrack, duration_ms, 120.0, &audio_error);
    const std::string audio_diagnostic = audio_ready
        ? std::string{}
        : "decoded intro video is playing without audio: "
            + (audio_error.empty()
                ? std::string{"audio initialization/load failed"}
                : audio_error);
    if (audio_ready) {
        audio.set_looping(playback.loop_until_skip);
        audio.play();
    }

    const auto begin = SDL_GetTicksNS();
    std::uint32_t uploaded_frame = std::numeric_limits<std::uint32_t>::max();
    const float source_aspect = static_cast<float>(sequence->width)
        / static_cast<float>(sequence->height);
    const float target_aspect = logical_width / logical_height;
    SDL_FRect destination{};
    if (source_aspect > target_aspect) {
        destination.w = logical_width;
        destination.h = logical_width / source_aspect;
        destination.x = 0.0F;
        destination.y = (logical_height - destination.h) * 0.5F;
    } else {
        destination.h = logical_height;
        destination.w = logical_height * source_aspect;
        destination.x = (logical_width - destination.w) * 0.5F;
        destination.y = 0.0F;
    }
    while (true) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                audio.stop();
                return {
                    StartupIntroStatus::quit_requested,
                    audio_diagnostic,
                };
            }
            if (is_skip_event(event)) {
                audio.stop();
                return {StartupIntroStatus::skipped, audio_diagnostic};
            }
        }
        const auto elapsed_ns = SDL_GetTicksNS() - begin;
        const auto absolute_frame = static_cast<std::uint64_t>(
            (static_cast<long double>(elapsed_ns)
                * static_cast<long double>(sequence->frames_per_second))
            / 1'000'000'000.0L
        );
        if (!playback.loop_until_skip
            && absolute_frame >= sequence->frame_count) {
            audio.stop();
            return {StartupIntroStatus::played, audio_diagnostic};
        }
        const auto frame = playback.loop_until_skip
            ? absolute_frame % sequence->frame_count
            : absolute_frame;
        const auto current_frame = static_cast<std::uint32_t>(frame);
        if (current_frame != uploaded_frame) {
            if (!upload_sequence_frame(
                    texture.get(),
                    *sequence,
                    current_frame,
                    error
                )) {
                audio.stop();
                return {
                    StartupIntroStatus::fallback_played,
                    std::move(error),
                };
            }
            uploaded_frame = current_frame;
        }
        SDL_SetRenderDrawColor(renderer, 0U, 0U, 0U, 255U);
        static_cast<void>(SDL_RenderClear(renderer));
        static_cast<void>(SDL_RenderTexture(
            renderer,
            texture.get(),
            nullptr,
            &destination
        ));
        if (playback.overlay != nullptr) {
            playback.overlay(
                renderer,
                elapsed_ns / 1'000'000ULL,
                playback.overlay_userdata
            );
        }
        if (playback.show_skip_hint) {
            draw_text(
                renderer,
                486.0F,
                650.0F,
                "PRESS ENTER OR SPACE TO SKIP",
                {90U, 224U, 232U, 255U},
                1.2F
            );
        }
        static_cast<void>(SDL_RenderPresent(renderer));
        SDL_Delay(1U);
    }
}

[[nodiscard]] StartupIntroStatus procedural_fallback(
    SDL_Renderer* renderer
) {
    constexpr std::uint64_t duration_ns = 2'400'000'000ULL;
    const auto begin = SDL_GetTicksNS();
    while (SDL_GetTicksNS() - begin < duration_ns) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                return StartupIntroStatus::quit_requested;
            }
            if (is_skip_event(event)) {
                return StartupIntroStatus::skipped;
            }
        }

        const auto elapsed = SDL_GetTicksNS() - begin;
        const float phase = std::clamp(
            static_cast<float>(elapsed) / static_cast<float>(duration_ns),
            0.0F,
            1.0F
        );
        SDL_SetRenderDrawColor(renderer, 2U, 5U, 7U, 255U);
        static_cast<void>(SDL_RenderClear(renderer));
        fill_rect(renderer, {0.0F, 0.0F, logical_width, logical_height},
                  {2U, 5U, 7U, 255U});

        for (int index = 0; index < 28; ++index) {
            const auto seed = static_cast<std::uint64_t>(index) * 4'099ULL;
            const auto movement = elapsed / 2'300'000ULL;
            const float x = static_cast<float>((seed + movement) % 1'360ULL)
                - 40.0F;
            const float y = 72.0F
                + static_cast<float>((seed * 17ULL) % 570ULL);
            fill_rect(
                renderer,
                {x, y, 24.0F + static_cast<float>(index % 5) * 17.0F, 1.0F},
                {45U, 239U, 247U, static_cast<std::uint8_t>(40 + index * 3)}
            );
        }

        const float progress = std::clamp(phase * 1.18F, 0.0F, 1.0F);
        fill_rect(renderer, {195.0F, 500.0F, 890.0F, 3.0F},
                  {24U, 42U, 44U, 255U});
        fill_rect(renderer, {195.0F, 500.0F, 890.0F * progress, 3.0F},
                  {53U, 237U, 245U, 255U});
        draw_text(
            renderer,
            385.0F + std::sin(phase * 41.0F) * 2.0F,
            298.0F,
            "PULSEFORGE // ctOS BOOT",
            {224U, 254U, 255U, 255U},
            2.2F
        );
        draw_text(
            renderer,
            472.0F,
            532.0F,
            "PRESS ENTER OR SPACE TO SKIP",
            {92U, 224U, 232U, 255U},
            1.25F
        );
        static_cast<void>(SDL_RenderPresent(renderer));
        SDL_Delay(1U);
    }
    return StartupIntroStatus::fallback_played;
}

#if defined(_WIN32)

class MediaPlayerCallback final : public IMFPMediaPlayerCallback {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(
        const IID& interface_id,
        void** object
    ) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (interface_id == IID_IUnknown
            || interface_id == IID_IMFPMediaPlayerCallback) {
            *object = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return references_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = references_.fetch_sub(
            1U,
            std::memory_order_acq_rel
        ) - 1U;
        if (remaining == 0U) {
            delete this;
        }
        return remaining;
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(
        MFP_EVENT_HEADER* event
    ) override {
        if (event == nullptr) {
            failed_.store(true, std::memory_order_release);
            done_.store(true, std::memory_order_release);
            return;
        }
        if (FAILED(event->hrEvent)) {
            record_failure(event->hrEvent, event->eEventType);
            return;
        }
        if (event->eEventType == MFP_EVENT_TYPE_MEDIAITEM_CREATED) {
            const auto* const created = MFP_GET_MEDIAITEM_CREATED_EVENT(event);
            capture_duration(created->pMediaItem);
            const auto result = event->pMediaPlayer != nullptr
                    && created->pMediaItem != nullptr
                ? event->pMediaPlayer->SetMediaItem(created->pMediaItem)
                : E_POINTER;
            if (FAILED(result)) {
                record_failure(result, event->eEventType);
            }
        } else if (event->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET) {
            const auto result = event->pMediaPlayer != nullptr
                ? event->pMediaPlayer->Play()
                : E_POINTER;
            if (FAILED(result)) {
                record_failure(result, event->eEventType);
            }
        } else if (event->eEventType == MFP_EVENT_TYPE_PLAY) {
            started_.store(true, std::memory_order_release);
        } else if (event->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) {
            done_.store(true, std::memory_order_release);
        } else if (event->eEventType == MFP_EVENT_TYPE_ERROR) {
            failed_.store(true, std::memory_order_release);
            done_.store(true, std::memory_order_release);
        }
    }

    [[nodiscard]] bool started() const noexcept {
        return started_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool done() const noexcept {
        return done_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool failed() const noexcept {
        return failed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] HRESULT result() const noexcept {
        return result_.load(std::memory_order_acquire);
    }

    [[nodiscard]] int failed_event_type() const noexcept {
        return failed_event_type_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t duration_100ns() const noexcept {
        return duration_100ns_.load(std::memory_order_acquire);
    }

private:
    void capture_duration(IMFPMediaItem* const media_item) noexcept {
        if (media_item == nullptr) {
            return;
        }
        PROPVARIANT duration{};
        PropVariantInit(&duration);
        const HRESULT result = media_item->GetDuration(
            MFP_POSITIONTYPE_100NS,
            &duration
        );
        std::uint64_t value{};
        if (SUCCEEDED(result)) {
            if (duration.vt == VT_UI8) {
                value = duration.uhVal.QuadPart;
            } else if (duration.vt == VT_I8 && duration.hVal.QuadPart > 0) {
                value = static_cast<std::uint64_t>(duration.hVal.QuadPart);
            }
        }
        static_cast<void>(PropVariantClear(&duration));
        if (value > 0U) {
            duration_100ns_.store(value, std::memory_order_release);
        }
    }

    void record_failure(
        const HRESULT result,
        const MFP_EVENT_TYPE event_type
    ) noexcept {
        result_.store(result, std::memory_order_release);
        failed_event_type_.store(
            static_cast<int>(event_type),
            std::memory_order_release
        );
        failed_.store(true, std::memory_order_release);
        done_.store(true, std::memory_order_release);
    }

    std::atomic<ULONG> references_{1U};
    std::atomic<bool> started_{};
    std::atomic<bool> done_{};
    std::atomic<bool> failed_{};
    std::atomic<HRESULT> result_{S_OK};
    std::atomic<int> failed_event_type_{-1};
    std::atomic<std::uint64_t> duration_100ns_{};
};

[[nodiscard]] std::string hresult_message(
    const char* operation,
    const HRESULT result
) {
    std::ostringstream stream;
    stream << operation << " failed (HRESULT 0x" << std::hex
           << std::uppercase << static_cast<std::uint32_t>(result) << ')';
    return stream.str();
}

[[nodiscard]] StartupIntroResult play_with_media_foundation(
    SDL_Window* window,
    SDL_Renderer* renderer,
    const std::filesystem::path& movie_path,
    const AudioSettings& audio_settings
) {
    const auto window_properties = SDL_GetWindowProperties(window);
    auto* const native_window = static_cast<HWND>(SDL_GetPointerProperty(
        window_properties,
        SDL_PROP_WINDOW_WIN32_HWND_POINTER,
        nullptr
    ));
    if (native_window == nullptr) {
        return {
            StartupIntroStatus::fallback_played,
            "SDL did not expose a Win32 window for native movie playback",
        };
    }

    const HRESULT com_result = CoInitializeEx(
        nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE
    );
    const bool owns_com = SUCCEEDED(com_result);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
        return {
            StartupIntroStatus::fallback_played,
            hresult_message("COM initialization", com_result),
        };
    }

    SDL_SetRenderDrawColor(renderer, 0U, 0U, 0U, 255U);
    static_cast<void>(SDL_RenderClear(renderer));
    draw_text(
        renderer,
        486.0F,
        650.0F,
        "PRESS ENTER OR SPACE TO SKIP",
        {90U, 224U, 232U, 255U},
        1.2F
    );
    static_cast<void>(SDL_RenderPresent(renderer));

    auto* const callback = new MediaPlayerCallback();
    IMFPMediaPlayer* player = nullptr;
    const HRESULT create_result = MFPCreateMediaPlayer(
        nullptr,
        FALSE,
        MFP_OPTION_NONE,
        callback,
        native_window,
        &player
    );
    if (FAILED(create_result) || player == nullptr) {
        callback->Release();
        if (owns_com) {
            CoUninitialize();
        }
        return {
            StartupIntroStatus::fallback_played,
            hresult_message("MP4 player creation", create_result),
        };
    }
    static_cast<void>(player->SetVolume(
        audio_settings.muted ? 0.0F : audio_settings.master_volume
    ));
    const HRESULT media_item_result = player->CreateMediaItemFromURL(
        movie_path.c_str(),
        FALSE,
        0U,
        nullptr
    );
    if (FAILED(media_item_result)) {
        static_cast<void>(player->Shutdown());
        player->Release();
        callback->Release();
        if (owns_com) {
            CoUninitialize();
        }
        return {
            StartupIntroStatus::fallback_played,
            hresult_message("MP4 media item creation", media_item_result),
        };
    }

    constexpr std::uint64_t maximum_startup_ns = 12'000'000'000ULL;
    const auto begin = SDL_GetTicksNS();
    auto status = StartupIntroStatus::played;
    bool native_failed = false;
    bool timed_out = false;
    while (!callback->done()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                status = StartupIntroStatus::quit_requested;
                break;
            }
            if (is_skip_event(event)) {
                status = StartupIntroStatus::skipped;
                break;
            }
            if (event.type == SDL_EVENT_WINDOW_EXPOSED
                || event.type == SDL_EVENT_WINDOW_RESIZED
                || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                // UpdateVideo is invalid while MFPlay is still in EMPTY/
                // opening state and can itself raise MFP_EVENT_TYPE_ERROR.
                if (callback->started()) {
                    static_cast<void>(player->UpdateVideo());
                }
            }
        }
        if (status != StartupIntroStatus::played) {
            break;
        }
        const auto elapsed = SDL_GetTicksNS() - begin;
        const auto maximum_wait_ns = native_intro_timeout_ns(
            callback->duration_100ns()
        );
        if (elapsed >= maximum_wait_ns
            || (!callback->started() && elapsed >= maximum_startup_ns)) {
            native_failed = true;
            timed_out = true;
            break;
        }
        SDL_Delay(1U);
    }
    native_failed = native_failed || callback->failed();
    const HRESULT event_result = callback->result();
    const int failed_event_type = callback->failed_event_type();

    static_cast<void>(player->Stop());
    static_cast<void>(player->Shutdown());
    player->Release();
    callback->Release();
    if (owns_com) {
        CoUninitialize();
    }

    if (native_failed && status == StartupIntroStatus::played) {
        auto diagnostic = timed_out
            ? std::string{"native MP4 playback timed out"}
            : hresult_message(
                "native MP4 playback",
                FAILED(event_result) ? event_result : E_FAIL
            );
        if (!timed_out && failed_event_type >= 0) {
            diagnostic += " on MFPlay event "
                + std::to_string(failed_event_type);
        }
        return {
            StartupIntroStatus::fallback_played,
            std::move(diagnostic),
        };
    }
    return {status, {}};
}

#endif

}  // namespace

StartupIntroResult play_startup_intro_ex(
    SDL_Window* window,
    SDL_Renderer* renderer,
    const std::filesystem::path& movie_path,
    const AudioSettings& audio_settings,
    const StartupIntroPlaybackOptions& playback
) {
    if (window == nullptr || renderer == nullptr) {
        return {
            StartupIntroStatus::fallback_played,
            "startup intro requires an initialized SDL window and renderer",
        };
    }

    StartupIntroResult decoded;
    if (playback.allow_decoded_derivative) {
        auto derivative_name = movie_path.stem().string();
        derivative_name += "-decoded";
        decoded = play_decoded_sequence(
            renderer,
            movie_path.parent_path() / derivative_name,
            audio_settings,
            playback
        );
        if (decoded.status != StartupIntroStatus::fallback_played) {
            return decoded;
        }

        const auto legacy_directory =
            movie_path.parent_path() / "watch-dogs-decoded";
        if (legacy_directory.filename() != derivative_name
            && playback.overlay == nullptr
            && !playback.loop_until_skip) {
            auto legacy = play_decoded_sequence(
                renderer,
                legacy_directory,
                audio_settings,
                playback
            );
            if (legacy.status != StartupIntroStatus::fallback_played) {
                return legacy;
            }
            if (!legacy.diagnostic.empty()) {
                if (!decoded.diagnostic.empty()) decoded.diagnostic += "; ";
                decoded.diagnostic += legacy.diagnostic;
            }
        }
    }

    std::error_code filesystem_error;
    const bool regular_file = std::filesystem::is_regular_file(
        movie_path,
        filesystem_error
    );
    std::string diagnostic = std::move(decoded.diagnostic);
    const auto append_diagnostic = [&](std::string message) {
        if (!diagnostic.empty() && !message.empty()) diagnostic += "; ";
        diagnostic += std::move(message);
    };
#if defined(_WIN32)
    if (playback.allow_native_movie
        && regular_file && !filesystem_error) {
        auto result = play_with_media_foundation(
            window,
            renderer,
            movie_path,
            audio_settings
        );
        if (result.status != StartupIntroStatus::fallback_played) {
            return result;
        }
        append_diagnostic(std::move(result.diagnostic));
    } else if (playback.allow_native_movie) {
        append_diagnostic(filesystem_error
            ? "cannot inspect startup movie: " + filesystem_error.message()
            : "startup movie is missing");
    }
#else
    static_cast<void>(movie_path);
    if (playback.allow_native_movie) {
        append_diagnostic(regular_file
            ? "native MP4 playback is currently implemented on Windows"
            : "startup movie is missing");
    }
#endif

    if (!playback.allow_procedural_fallback) {
        return {StartupIntroStatus::fallback_played, std::move(diagnostic)};
    }
    const auto fallback_status = procedural_fallback(renderer);
    return {fallback_status, std::move(diagnostic)};
}

StartupIntroResult play_startup_intro(
    SDL_Window* window,
    SDL_Renderer* renderer,
    const std::filesystem::path& movie_path,
    const AudioSettings& audio_settings,
    const bool allow_decoded_derivative
) {
    StartupIntroPlaybackOptions playback;
    playback.allow_decoded_derivative = allow_decoded_derivative;
    return play_startup_intro_ex(
        window,
        renderer,
        movie_path,
        audio_settings,
        playback
    );
}

}  // namespace pulseforge::detail
