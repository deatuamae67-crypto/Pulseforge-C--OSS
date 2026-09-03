#include "application_runner.hpp"
#include "discord_presence.hpp"
#include "controls_ui.hpp"
#include "editor_ui.hpp"
#include "intro_player.hpp"
#include "media_routes.hpp"
#include "ps2_theme.hpp"
#include "menu_layout.hpp"
#include "sdl_input_actions.hpp"
#include "sniff_bridge.hpp"

#include "pulseforge/audio_controls.hpp"
#include "pulseforge/autochart.hpp"
#include "pulseforge/audio_transport.hpp"
#include "pulseforge/chart_loader.hpp"
#include "pulseforge/content_catalog.hpp"
#include "pulseforge/mod_installer.hpp"
#include "pulseforge/musical_chart.hpp"
#include "pulseforge/note_skin_catalog.hpp"
#include "pulseforge/packed_chart.hpp"
#include "pulseforge/streaming_chart_importer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <stb_image.h>

#if !defined(PULSEFORGE_VERSION)
#define PULSEFORGE_VERSION "development"
#endif

namespace pulseforge::detail {
namespace {

constexpr float launcher_width = 1280.0F;
constexpr float launcher_height = 720.0F;
constexpr std::size_t visible_rows = 18;

class MenuSession final {
public:
    MenuSession(
        EngineSettings& settings,
        std::filesystem::path settings_path,
        std::vector<std::filesystem::path> music_paths,
        std::shared_ptr<DiscordPresenceSession> discord_session
    )
        : settings_(&settings),
          settings_path_(std::move(settings_path)),
          music_paths_(std::move(music_paths)),
          discord_presence_(std::move(discord_session)),
          theme_(settings.visual.theme) {
        SDL_SetAppMetadata(
            "PulseForge",
            PULSEFORGE_VERSION,
            "org.pulseforge.engine.menu"
        );
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
            error_ = std::string{"Menu SDL initialization failed: "}
                + SDL_GetError();
            return;
        }
        initialized_ = true;
        window_ = SDL_CreateWindow(
            "PulseForge",
            settings.visual.width,
            settings.visual.height,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
        );
        renderer_ = window_ != nullptr
            ? SDL_CreateRenderer(window_, nullptr)
            : nullptr;
        if (window_ == nullptr || renderer_ == nullptr) {
            error_ = std::string{"Menu creation failed: "} + SDL_GetError();
            return;
        }
        if (!SDL_SetRenderLogicalPresentation(
                renderer_,
                static_cast<int>(launcher_width),
                static_cast<int>(launcher_height),
                SDL_LOGICAL_PRESENTATION_LETTERBOX
            )) {
            error_ = std::string{"Menu presentation failed: "}
                + SDL_GetError();
            return;
        }
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        apply_visual_settings(settings.visual);
        const auto seed = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
        music_random_state_ = seed == 0U
            ? 0x9E3779B97F4A7C15ULL
            : seed;
        publish_presence(RuntimeActivityKind::launcher, "In the main menu");
    }

    ~MenuSession() {
        discord_presence_.clear();
        music_.shutdown();
        if (renderer_ != nullptr) {
            SDL_DestroyRenderer(renderer_);
        }
        if (window_ != nullptr) {
            SDL_DestroyWindow(window_);
        }
        if (initialized_) {
            SDL_Quit();
        }
    }

    MenuSession(const MenuSession&) = delete;
    MenuSession& operator=(const MenuSession&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return window_ != nullptr && renderer_ != nullptr && error_.empty();
    }

    [[nodiscard]] SDL_Window* window() const noexcept { return window_; }
    [[nodiscard]] SDL_Renderer* renderer() const noexcept { return renderer_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] PresentationTheme theme() const noexcept { return theme_; }
    [[nodiscard]] bool close_requested() const noexcept {
        return close_requested_;
    }

    void request_close() noexcept {
        close_requested_ = true;
    }

    [[nodiscard]] TransferredPlatform release_platform() noexcept {
        music_.shutdown();
        music_ready_ = false;
        music_suspended_ = true;
        TransferredPlatform platform{window_, renderer_, initialized_};
        window_ = nullptr;
        renderer_ = nullptr;
        initialized_ = false;
        return platform;
    }

    [[nodiscard]] bool adopt_platform(TransferredPlatform platform) {
        music_.shutdown();
        music_ready_ = false;
        music_suspended_ = true;
        if (window_ != nullptr || renderer_ != nullptr || initialized_) {
            error_ = "Menu already owns an SDL platform";
            return false;
        }
        if (!platform.complete()) {
            error_ = "Gameplay did not return a complete SDL platform";
            return false;
        }
        platform.detach(window_, renderer_, initialized_);
        SDL_SetWindowTitle(window_, "PulseForge");
        if (!SDL_SetRenderLogicalPresentation(
                renderer_,
                static_cast<int>(launcher_width),
                static_cast<int>(launcher_height),
                SDL_LOGICAL_PRESENTATION_LETTERBOX
            )) {
            error_ = std::string{"Menu presentation restore failed: "}
                + SDL_GetError();
            return false;
        }
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        error_.clear();
        if (settings_ != nullptr) {
            apply_visual_settings(settings_->visual);
        }
        SDL_ShowWindow(window_);
        SDL_RaiseWindow(window_);
        publish_presence(RuntimeActivityKind::launcher, "In the main menu");
        return true;
    }

    [[nodiscard]] std::string volume_label() const {
        if (settings_ == nullptr) {
            return "VOL --";
        }
        return std::string(settings_->audio.muted ? "MUTED  //  " : "")
            + "VOL " + std::to_string(master_volume_percent(settings_->audio))
            + '%';
    }

    void resume_music() {
        music_suspended_ = false;
        if (music_ready_
            && music_.state() == AudioTransportState::ended) {
            music_ready_ = false;
        }
        if (!ensure_music()) {
            return;
        }
        apply_audio_settings();
        if (music_.state() == AudioTransportState::paused) {
            music_.resume();
        } else if (music_.state() != AudioTransportState::playing) {
            music_.play();
        }
    }

    void update_music() {
        // Keep Discord callbacks/reconnects alive in every launcher loop that
        // services menu audio, including idle screens and native-dialog waits.
        discord_presence_.pump();
        if (music_suspended_) {
            return;
        }
        if (!ensure_music()) {
            return;
        }
        if (music_.state() != AudioTransportState::ended) {
            return;
        }
        music_ready_ = false;
        if (load_next_music()) {
            apply_audio_settings();
            music_.play();
        }
    }

    void suspend_music() noexcept {
        music_suspended_ = true;
        music_.pause();
    }

    [[nodiscard]] const std::vector<std::filesystem::path>& music_paths()
        const noexcept {
        return music_paths_;
    }

    void reconfigure_music() {
        const bool was_active = !music_suspended_;
        music_.shutdown();
        music_ready_ = false;
        music_suspended_ = !was_active;
        if (was_active) {
            resume_music();
        }
    }

    [[nodiscard]] bool handle_audio_action(const SDL_KeyboardEvent& event) {
        if (settings_ == nullptr) {
            return false;
        }
        if (keyboard_action_matches(settings_->controls, "volume_up", event)) {
            static_cast<void>(adjust_master_volume(settings_->audio, 1));
        } else if (keyboard_action_matches(
                       settings_->controls,
                       "volume_down",
                       event
                   )) {
            static_cast<void>(adjust_master_volume(settings_->audio, -1));
        } else if (keyboard_action_matches(
                       settings_->controls,
                       "volume_mute",
                       event
                   )) {
            toggle_master_mute(settings_->audio);
        } else {
            return false;
        }
        apply_audio_settings();
        persist_audio_settings();
        return true;
    }

    [[nodiscard]] bool handle_audio_action(
        const SDL_GamepadButtonEvent& event
    ) {
        if (settings_ == nullptr) {
            return false;
        }
        if (gamepad_action_matches(settings_->controls, "volume_up", event)) {
            static_cast<void>(adjust_master_volume(settings_->audio, 1));
        } else if (gamepad_action_matches(
                       settings_->controls,
                       "volume_down",
                       event
                   )) {
            static_cast<void>(adjust_master_volume(settings_->audio, -1));
        } else if (gamepad_action_matches(
                       settings_->controls,
                       "volume_mute",
                       event
                   )) {
            toggle_master_mute(settings_->audio);
        } else {
            return false;
        }
        apply_audio_settings();
        persist_audio_settings();
        return true;
    }

    void set_title(const std::string_view title) const {
        if (window_ != nullptr) {
            SDL_SetWindowTitle(window_, std::string(title).c_str());
        }
    }

    void publish_presence(
        const RuntimeActivityKind activity,
        std::string status = {},
        std::string chart_title = {},
        std::string difficulty = {},
        const std::uint16_t key_count = 0U,
        const double task_progress = 0.0,
        const std::uint64_t logical_note_total = 0U
    ) noexcept {
        if (settings_ == nullptr) return;
        RuntimeTelemetrySnapshot snapshot;
        snapshot.activity = activity;
        snapshot.status = std::move(status);
        snapshot.chart_title = std::move(chart_title);
        snapshot.difficulty = std::move(difficulty);
        snapshot.key_count = key_count;
        snapshot.task_progress = task_progress;
        snapshot.logical_note_total = logical_note_total;
        snapshot.media_title = current_media_title_;
        discord_presence_.publish(snapshot, settings_->discord);
    }

    void pump_presence() noexcept {
        discord_presence_.pump();
    }

    void request_discord_account_link() noexcept {
        if (settings_ != nullptr) {
            discord_presence_.request_account_link(settings_->discord);
        }
    }

    void unlink_discord_account() noexcept {
        if (settings_ != nullptr) discord_presence_.unlink_account(settings_->discord);
    }

    [[nodiscard]] DiscordAccountLinkState discord_account_link_state() const noexcept {
        return discord_presence_.account_link_state();
    }

    [[nodiscard]] std::string discord_account_link_message() const {
        return discord_presence_.account_link_message();
    }

    [[nodiscard]] bool open_discord_connected_games() noexcept {
        return settings_ != nullptr
            && discord_presence_.open_connected_games_settings(settings_->discord);
    }

    [[nodiscard]] std::shared_ptr<DiscordPresenceSession> discord_session() const noexcept {
        return discord_presence_.session();
    }

    void apply_visual_settings(const VisualSettings& settings) const {
        theme_ = settings.theme;
        if (renderer_ != nullptr) {
            SDL_SetRenderVSync(renderer_, settings.vsync ? 1 : 0);
        }
        if (window_ != nullptr) {
            SDL_SetWindowFullscreen(window_, settings.fullscreen);
        }
    }

private:
    [[nodiscard]] static std::string media_title_for_path(
        const std::filesystem::path& path
    ) noexcept {
        try {
            const auto value = path.stem().generic_u8string();
            return {
                reinterpret_cast<const char*>(value.data()),
                value.size(),
            };
        } catch (...) {
            return {};
        }
    }

    [[nodiscard]] bool ensure_music() {
        if (music_ready_) {
            return true;
        }
        if (settings_ == nullptr || settings_->audio.menu_music_muted) {
            return false;
        }
        const bool custom_requested =
            settings_->audio.menu_music_selection == "custom"
            && !settings_->audio.custom_menu_music_path.empty();
        if (music_paths_.empty() && !custom_requested) {
            return false;
        }
        if (!music_.initialized()) {
            std::string error;
            AudioSettings music_settings = settings_->audio;
            music_settings.playback_rate = 1.0;
            if (!music_.initialize(music_settings, &error)) {
                music_diagnostic_ = std::move(error);
                return false;
            }
        }
        return load_next_music();
    }

    [[nodiscard]] bool load_next_music() {
        if (!music_.initialized() || settings_ == nullptr) {
            return false;
        }
        current_media_title_.clear();

        // PULSEFORGE_P1_5_0E_CUSTOM_MENU_MUSIC_V1
        if (settings_->audio.menu_music_selection == "custom"
            && !settings_->audio.custom_menu_music_path.empty()) {
            const std::filesystem::path custom(
                settings_->audio.custom_menu_music_path
            );
            std::error_code custom_error;
            auto extension = custom.extension().string();
            std::transform(
                extension.begin(),
                extension.end(),
                extension.begin(),
                [](const unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                }
            );
            const bool supported = extension == ".mp3" || extension == ".ogg"
                || extension == ".wav" || extension == ".flac";
            if (supported
                && std::filesystem::is_regular_file(custom, custom_error)
                && !custom_error) {
                AudioManifest manifest;
                manifest.instrumental = custom;
                std::string error;
                if (music_.load(manifest, 60'000.0, 120.0, &error)) {
                    music_.set_looping(settings_->audio.menu_music_loop_selected);
                    music_ready_ = true;
                    current_media_title_ = media_title_for_path(custom);
                    last_music_index_.reset();
                    return true;
                }
                music_diagnostic_ = std::move(error);
            }
            // A missing removable/external file never wedges the launcher.
            // Fall through to the discovered playlist for this run.
        }

        if (music_paths_.empty()) {
            return false;
        }

        std::optional<std::size_t> preferred;
        if (settings_ != nullptr
            && settings_->audio.menu_music_loop_selected
            && settings_->audio.menu_music_selection != "randomized") {
            const auto selected = std::ranges::find_if(
                music_paths_,
                [this](const std::filesystem::path& path) {
                    return settings_ != nullptr
                        && path.filename().string()
                            == settings_->audio.menu_music_selection;
                }
            );
            if (selected != music_paths_.end()) {
                preferred = static_cast<std::size_t>(
                    std::distance(music_paths_.begin(), selected)
                );
            }
        }

        std::vector<std::size_t> candidates;
        candidates.reserve(music_paths_.size());
        if (preferred.has_value()) {
            candidates.push_back(*preferred);
        }
        const auto random_start = next_random_music_index();
        for (std::size_t offset = 0U; offset < music_paths_.size(); ++offset) {
            const auto index = (random_start + offset) % music_paths_.size();
            if (!preferred.has_value() || index != *preferred) {
                candidates.push_back(index);
            }
        }

        for (const auto index : candidates) {
            AudioManifest manifest;
            manifest.instrumental = music_paths_[index];
            std::string error;
            if (!music_.load(manifest, 60'000.0, 120.0, &error)) {
                music_diagnostic_ = std::move(error);
                continue;
            }
            music_.set_looping(preferred.has_value() && index == *preferred);
            music_ready_ = true;
            current_media_title_ = media_title_for_path(music_paths_[index]);
            last_music_index_ = index;
            return true;
        }
        music_.shutdown();
        return false;
    }

    [[nodiscard]] std::size_t next_random_music_index() noexcept {
        music_random_state_ ^= music_random_state_ << 13U;
        music_random_state_ ^= music_random_state_ >> 7U;
        music_random_state_ ^= music_random_state_ << 17U;
        if (music_paths_.size() <= 1U) {
            return 0U;
        }
        if (!last_music_index_.has_value()) {
            return static_cast<std::size_t>(
                music_random_state_
                % static_cast<std::uint64_t>(music_paths_.size())
            );
        }
        const auto available = music_paths_.size() - 1U;
        auto candidate = static_cast<std::size_t>(
            music_random_state_ % static_cast<std::uint64_t>(available)
        );
        if (last_music_index_.has_value() && candidate >= *last_music_index_) {
            ++candidate;
        }
        return candidate;
    }

    void apply_audio_settings() noexcept {
        if (settings_ == nullptr || !music_.initialized()) {
            return;
        }
        music_.set_master_volume(settings_->audio.master_volume);
        music_.set_muted(
            settings_->audio.muted || settings_->audio.menu_music_muted
        );
        if (music_suspended_) {
            music_.pause();
        }
    }

    void persist_audio_settings() {
        if (settings_ == nullptr || settings_path_.empty()) {
            return;
        }
        std::string error;
        if (!save_settings(settings_path_, *settings_, &error)) {
            music_diagnostic_ = std::move(error);
        }
    }

    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    std::string error_;
    EngineSettings* settings_{};
    std::filesystem::path settings_path_;
    std::vector<std::filesystem::path> music_paths_;
    AudioTransport music_;
    DiscordPresencePublisher discord_presence_;
    std::string music_diagnostic_;
    std::string current_media_title_;
    bool music_ready_{};
    bool music_suspended_{true};
    bool initialized_{};
    bool close_requested_{};
    std::uint64_t music_random_state_{0x9E3779B97F4A7C15ULL};
    std::optional<std::size_t> last_music_index_;
    mutable PresentationTheme theme_{PresentationTheme::pulseforge};
};

[[nodiscard]] std::string printable_text(
    const std::string_view text,
    const std::size_t limit
) {
    std::string result;
    result.reserve(std::min(text.size(), limit));
    for (const unsigned char value : text) {
        if (result.size() >= limit) {
            break;
        }
        result.push_back(value >= 32U && value <= 126U
            ? static_cast<char>(value)
            : '?');
    }
    if (text.size() > limit && limit >= 3) {
        result.resize(limit - 3);
        result += "...";
    }
    return result;
}

[[nodiscard]] std::string format_bytes(const std::uintmax_t bytes) {
    constexpr double kib = 1024.0;
    constexpr double mib = kib * 1024.0;
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1);
    if (static_cast<double>(bytes) >= mib) {
        stream << static_cast<double>(bytes) / mib << " MiB";
    } else if (static_cast<double>(bytes) >= kib) {
        stream << static_cast<double>(bytes) / kib << " KiB";
    } else {
        stream.unsetf(std::ios::floatfield);
        stream << bytes << " B";
    }
    return stream.str();
}

[[nodiscard]] std::vector<std::filesystem::path> startup_movie_paths(
    const AppLaunchOptions& options
) {
    return discover_startup_movies(options.content_roots);
}

[[nodiscard]] std::filesystem::path ps2_startup_movie_path(
    const AppLaunchOptions& options
) {
    std::error_code error;
    for (const auto& root : options.content_roots) {
        const auto candidate = root / "intro/ps2/startup_ps2.mp4";
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate;
        }
        error.clear();
    }
    return options.content_roots.empty()
        ? std::filesystem::path{"intro/ps2/startup_ps2.mp4"}
        : options.content_roots.front() / "intro/ps2/startup_ps2.mp4";
}

[[nodiscard]] std::vector<std::filesystem::path> return_movie_paths(
    const AppLaunchOptions& options
) {
    // Return transitions are deliberately isolated from startup and exit-only
    // media. A mod/user may add one without ever making DedSec an intro.
    return discover_return_movies(options.content_roots);
}

[[nodiscard]] std::filesystem::path choose_startup_movie(
    const std::vector<std::filesystem::path>& paths
) {
    if (paths.empty()) {
        return {};
    }
    const auto seed = static_cast<std::uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count()
    );
    return paths[static_cast<std::size_t>(seed % paths.size())];
}

[[nodiscard]] std::filesystem::path exit_movie_path(
    const AppLaunchOptions& options
) {
    return discover_explicit_exit_movie(options.content_roots);
}

[[nodiscard]] std::vector<std::filesystem::path> menu_music_paths(
    const AppLaunchOptions& options
) {
    std::vector<std::filesystem::path> result;
    std::set<std::filesystem::path> unique;
    std::error_code error;
    for (const auto& root : options.content_roots) {
        const auto directory = root / "menu";
        if (!std::filesystem::is_directory(directory, error) || error) {
            error.clear();
            continue;
        }
        for (std::filesystem::directory_iterator iterator(directory, error), end;
             iterator != end && !error;
             iterator.increment(error)) {
            if (!iterator->is_regular_file(error) || error) {
                error.clear();
                continue;
            }
            auto extension = iterator->path().extension().string();
            std::transform(
                extension.begin(),
                extension.end(),
                extension.begin(),
                [](const unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                }
            );
            if (extension != ".mp3" && extension != ".ogg"
                && extension != ".wav" && extension != ".flac") {
                continue;
            }
            const auto normalized = std::filesystem::weakly_canonical(
                iterator->path(),
                error
            );
            const auto selected = error ? iterator->path() : normalized;
            error.clear();
            if (unique.insert(selected).second) {
                result.push_back(selected);
            }
        }
        error.clear();
    }
    std::sort(result.begin(), result.end());
    return result;
}

void play_explicit_exit_video(
    MenuSession& menu,
    const AppLaunchOptions& options
) {
    menu.suspend_music();
    const auto exit_video = play_startup_intro(
        menu.window(),
        menu.renderer(),
        exit_movie_path(options),
        options.settings.audio,
        false
    );
    if (!exit_video.diagnostic.empty()) {
        std::cerr << "Exit video warning: "
                  << exit_video.diagnostic << '\n';
    }
}

[[nodiscard]] bool play_procedural_return_stinger(MenuSession& menu);

[[nodiscard]] bool play_return_transition(
    MenuSession& menu,
    const AppLaunchOptions& options
) {
    const auto movies = return_movie_paths(options);
    if (movies.empty()) {
        return play_procedural_return_stinger(menu);
    }
    menu.suspend_music();
    const auto transition = play_startup_intro(
        menu.window(),
        menu.renderer(),
        choose_startup_movie(movies),
        options.settings.audio,
        false
    );
    if (!transition.diagnostic.empty()) {
        std::cerr << "Return transition warning: "
                  << transition.diagnostic << '\n';
    }
    if (transition.status == StartupIntroStatus::quit_requested) {
        menu.request_close();
        return false;
    }
    return true;
}

[[nodiscard]] char ascii_lower(const char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

[[nodiscard]] std::string lower_ascii(const std::string_view value) {
    std::string result(value);
    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        ascii_lower
    );
    return result;
}

[[nodiscard]] bool entry_matches(
    const SongCatalogEntry& entry,
    const std::string_view query
) {
    if (query.empty()) {
        return true;
    }
    const auto needle = lower_ascii(query);
    const std::array fields{
        std::string_view(entry.id),
        std::string_view(entry.song_id),
        std::string_view(entry.title),
        std::string_view(entry.difficulty),
        std::string_view(entry.mod_name),
        std::string_view(entry.week),
    };
    return std::ranges::any_of(fields, [&](const std::string_view field) {
        return lower_ascii(field).find(needle) != std::string::npos;
    });
}

void erase_last_utf8_codepoint(std::string& value) {
    if (value.empty()) {
        return;
    }
    std::size_t index = value.size() - 1;
    while (index > 0
        && (static_cast<unsigned char>(value[index]) & 0xC0U) == 0x80U) {
        --index;
    }
    value.erase(index);
}

void draw_text(
    SDL_Renderer* renderer,
    const float x,
    const float y,
    const std::string_view text,
    const SDL_Color color,
    const float scale = 1.45F
) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const auto safe = printable_text(text, 150);
    float previous_x = 1.0F;
    float previous_y = 1.0F;
    SDL_GetRenderScale(renderer, &previous_x, &previous_y);
    SDL_SetRenderScale(renderer, scale, scale);
    SDL_RenderDebugText(renderer, x / scale, y / scale, safe.c_str());
    SDL_SetRenderScale(renderer, previous_x, previous_y);
}

// PULSEFORGE_AUTOCHART_LAUNCHER_COMPILE_COMPAT_V1
// The integrated AutoChart UI uses a bounded-text overload. Keep the original
// six-argument renderer as the canonical implementation.
void draw_text(
    SDL_Renderer* renderer,
    const float x,
    const float y,
    const std::string_view text,
    const SDL_Color color,
    const float scale,
    const std::size_t maximum_characters
) {
    const auto bounded = printable_text(
        text,
        (std::max)(std::size_t{1U}, maximum_characters)
    );
    draw_text(renderer, x, y, bounded, color, scale);
}

// Compatibility for the compact numeric progress label used by one launcher
// revision. It deliberately stays presentation-only; generation progress is
// still carried by AutoChartProgress.
void draw_text(
    SDL_Renderer* renderer,
    const float x,
    const float y,
    const float value,
    const unsigned int precision
) {
    std::ostringstream stream;
    stream << std::fixed
           << std::setprecision(static_cast<int>((std::min)(precision, 6U)))
           << value;
    draw_text(
        renderer,
        x,
        y,
        stream.str(),
        SDL_Color{220U, 225U, 235U, 255U},
        1.0F
    );
}
void fill_rect(
    SDL_Renderer* renderer,
    const SDL_FRect& rectangle,
    const SDL_Color color
) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &rectangle);
}

void outline_rect(
    SDL_Renderer* renderer,
    const SDL_FRect& rectangle,
    const SDL_Color color
) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    static_cast<void>(SDL_RenderRect(renderer, &rectangle));
}

struct MenuPalette {
    SDL_Color top;
    SDL_Color bottom;
    SDL_Color header;
    SDL_Color panel;
    SDL_Color accent;
    SDL_Color selected;
    SDL_Color title;
    SDL_Color text;
    SDL_Color muted;
};

enum class MenuSurface : std::uint8_t {
    generic,
    options,
    freeplay,
    story,
    mods,
    controls,
};

[[nodiscard]] MenuSurface menu_surface_for(const std::string_view title) noexcept {
    if (title.find("OPTIONS") != std::string_view::npos) {
        return MenuSurface::options;
    }
    if (title.find("FREEPLAY") != std::string_view::npos) {
        return MenuSurface::freeplay;
    }
    if (title.find("STORY") != std::string_view::npos) {
        return MenuSurface::story;
    }
    if (title.find("MOD") != std::string_view::npos) {
        return MenuSurface::mods;
    }
    if (title.find("CONTROL") != std::string_view::npos) {
        return MenuSurface::controls;
    }
    return MenuSurface::generic;
}

[[nodiscard]] std::string_view surface_label(const MenuSurface surface) noexcept {
    switch (surface) {
    case MenuSurface::options: return "CONFIGURATION";
    case MenuSurface::freeplay: return "SONG NETWORK";
    case MenuSurface::story: return "CAMPAIGN GRID";
    case MenuSurface::mods: return "CONTENT VAULT";
    case MenuSurface::controls: return "INPUT MATRIX";
    case MenuSurface::generic: return "SYSTEM MENU";
    }
    return "SYSTEM MENU";
}

[[nodiscard]] std::string_view presentation_theme_name(
    const PresentationTheme theme
) noexcept {
    switch (theme) {
    case PresentationTheme::pulseforge: return "PulseForge";
    case PresentationTheme::watch_dogs: return "Watch Dogs";
    case PresentationTheme::ps2: return "PS2";
    case PresentationTheme::beverly_hills_90210: return "Beverly Hills 90210";
    case PresentationTheme::just_cause_3: return "Just Cause 3";
    case PresentationTheme::just_cause_4: return "Just Cause 4";
    case PresentationTheme::xbox_original: return "Xbox Original";
    case PresentationTheme::xbox_360: return "Xbox 360";
    }
    return "PulseForge";
}

[[nodiscard]] std::string themed_menu_title(
    const std::string_view title,
    const PresentationTheme theme
) {
    if (theme == PresentationTheme::watch_dogs) {
        constexpr std::string_view pulseforge_prefix{"PULSEFORGE  //  "};
        if (title.starts_with(pulseforge_prefix)) {
            return std::string{"ctOS  //  "}
                + std::string(title.substr(pulseforge_prefix.size()));
        }
    }
    if (theme == PresentationTheme::ps2) {
        constexpr std::string_view pulseforge_prefix{"PULSEFORGE  //  "};
        if (title.starts_with(pulseforge_prefix)) {
            return std::string{"PULSEFORGE SYSTEM  //  "}
                + std::string(title.substr(pulseforge_prefix.size()));
        }
    }
    constexpr std::string_view pulseforge_prefix{"PULSEFORGE  //  "};
    if (title.starts_with(pulseforge_prefix)) {
        const auto suffix = title.substr(pulseforge_prefix.size());
        switch (theme) {
        case PresentationTheme::beverly_hills_90210:
            return std::string{"90210 FEVER  //  "} + std::string(suffix);
        case PresentationTheme::just_cause_3:
            return std::string{"REBEL OPS 3  //  "} + std::string(suffix);
        case PresentationTheme::just_cause_4:
            return std::string{"STORM OPS 4  //  "} + std::string(suffix);
        case PresentationTheme::xbox_original:
            return std::string{"GREEN SYSTEM  //  "} + std::string(suffix);
        case PresentationTheme::xbox_360:
            return std::string{"ORBIT SYSTEM  //  "} + std::string(suffix);
        default:
            break;
        }
    }
    return std::string(title);
}

[[nodiscard]] MenuPalette palette_for(
    const PresentationTheme theme,
    const std::optional<SDL_Color> accent_override = std::nullopt
) {
    if (theme == PresentationTheme::watch_dogs) {
        const auto accent = accent_override.value_or(SDL_Color{47, 233, 241, 255});
        return {
            {2, 5, 7, 255},
            {12, 15, 17, 255},
            {4, 8, 10, 255},
            {8, 13, 15, 255},
            accent,
            {17, 39, 42, 255},
            {225, 254, 255, 255},
            {214, 224, 224, 255},
            {120, 159, 162, 255},
        };
    }
    if (theme == PresentationTheme::ps2) {
        const auto accent = accent_override.value_or(SDL_Color{53, 100, 255, 255});
        return {
            {0, 0, 9, 255},
            {2, 4, 26, 255},
            {0, 1, 14, 255},
            {2, 4, 20, 245},
            accent,
            {8, 17, 57, 255},
            {219, 228, 255, 255},
            {200, 211, 245, 255},
            {110, 133, 194, 255},
        };
    }
    if (theme == PresentationTheme::beverly_hills_90210) {
        const auto accent = accent_override.value_or(SDL_Color{255, 54, 199, 255});
        return {
            {18, 18, 42, 255}, {24, 183, 207, 255}, {72, 23, 91, 255},
            {31, 20, 56, 238}, accent, {92, 31, 103, 255},
            {255, 245, 225, 255}, {255, 232, 242, 255}, {183, 216, 224, 255},
        };
    }
    if (theme == PresentationTheme::just_cause_3) {
        const auto accent = accent_override.value_or(SDL_Color{238, 62, 48, 255});
        return {
            {5, 28, 44, 255}, {20, 86, 111, 255}, {7, 37, 57, 255},
            {8, 34, 50, 242}, accent, {70, 45, 47, 255},
            {245, 243, 229, 255}, {225, 236, 235, 255}, {140, 176, 181, 255},
        };
    }
    if (theme == PresentationTheme::just_cause_4) {
        const auto accent = accent_override.value_or(SDL_Color{255, 144, 39, 255});
        return {
            {5, 17, 24, 255}, {17, 68, 76, 255}, {5, 28, 34, 255},
            {7, 31, 36, 242}, accent, {71, 55, 35, 255},
            {232, 252, 247, 255}, {217, 236, 230, 255}, {126, 164, 158, 255},
        };
    }
    if (theme == PresentationTheme::xbox_original) {
        const auto accent = accent_override.value_or(SDL_Color{91, 214, 39, 255});
        return {
            {0, 5, 1, 255}, {5, 22, 7, 255}, {1, 12, 3, 255},
            {3, 16, 5, 244}, accent, {20, 55, 22, 255},
            {232, 255, 229, 255}, {218, 239, 216, 255}, {102, 150, 99, 255},
        };
    }
    if (theme == PresentationTheme::xbox_360) {
        const auto accent = accent_override.value_or(SDL_Color{94, 190, 31, 255});
        return {
            {226, 231, 226, 255}, {246, 248, 246, 255}, {48, 55, 49, 255},
            {239, 243, 239, 246}, accent, {211, 225, 209, 255},
            {24, 31, 25, 255}, {42, 49, 43, 255}, {102, 112, 103, 255},
        };
    }
    const auto accent = accent_override.value_or(SDL_Color{100, 71, 220, 255});
    return {
        {8, 7, 24, 255},
        {21, 13, 49, 255},
        {31, 16, 67, 255},
        {17, 12, 38, 255},
        accent,
        {55, 34, 116, 255},
        {118, 241, 255, 255},
        {224, 216, 239, 255},
        {190, 182, 224, 255},
    };
}

void draw_menu_background(
    SDL_Renderer* renderer,
    const std::uint64_t ticks,
    const PresentationTheme theme,
    const std::optional<SDL_Color> accent_override = std::nullopt
) {
    const auto palette = palette_for(theme, accent_override);
    SDL_SetRenderDrawColor(
        renderer,
        palette.top.r,
        palette.top.g,
        palette.top.b,
        255
    );
    static_cast<void>(SDL_RenderClear(renderer));

    constexpr int bands = 36;
    for (int index = 0; index < bands; ++index) {
        const float ratio = static_cast<float>(index)
            / static_cast<float>(bands - 1);
        const auto mix = [ratio](const std::uint8_t first, const std::uint8_t last) {
            return static_cast<std::uint8_t>(
                static_cast<float>(first)
                + (static_cast<float>(last) - static_cast<float>(first)) * ratio
            );
        };
        const float y0 = std::floor(
            static_cast<float>(index) * launcher_height
            / static_cast<float>(bands)
        );
        const float y1 = std::floor(
            static_cast<float>(index + 1) * launcher_height
            / static_cast<float>(bands)
        );
        fill_rect(
            renderer,
            {0.0F, y0, launcher_width, y1 - y0},
            {
                mix(palette.top.r, palette.bottom.r),
                mix(palette.top.g, palette.bottom.g),
                mix(palette.top.b, palette.bottom.b),
                255,
            }
        );
    }
    fill_rect(
        renderer,
        {0.0F, 0.0F, launcher_width, 108.0F},
        palette.header
    );
    fill_rect(
        renderer,
        {0.0F, 106.0F, launcher_width, 2.0F},
        palette.accent
    );

    if (theme == PresentationTheme::watch_dogs) {
        for (int index = 0; index < 56; ++index) {
            const float y = 114.0F + static_cast<float>(index) * 10.5F;
            fill_rect(renderer, {0.0F, y, launcher_width, 1.0F},
                      {38, 56, 58, 35});
        }
        for (int index = 0; index < 11; ++index) {
            const auto seed = static_cast<std::uint64_t>(index) * 9'973ULL;
            const float x = static_cast<float>((seed + ticks / 7ULL) % 1'390ULL)
                - 110.0F;
            const float y = 125.0F + static_cast<float>((seed * 19ULL) % 545ULL);
            const float width = 28.0F + static_cast<float>(index % 4) * 31.0F;
            fill_rect(renderer, {x, y, width, 2.0F},
                      {palette.accent.r, palette.accent.g, palette.accent.b, 100});
        }
        draw_text(renderer, 1'052.0F, 39.0F, "ctOS // ONLINE",
                  palette.accent, 1.15F);
    } else if (theme == PresentationTheme::ps2) {
        // PS2-inspired system-browser space: slow blue memory blocks and
        // perspective rails. Geometry is procedural and therefore scales with
        // any renderer without shipping a copied console UI asset.
        const float drift = static_cast<float>(ticks % 18'000ULL) / 18'000.0F;
        for (int rail = 0; rail < 13; ++rail) {
            const float depth = static_cast<float>(rail + 1) / 13.0F;
            const float width = 82.0F + depth * 920.0F;
            const float height = 24.0F + depth * 420.0F;
            const float wobble = 10.0F * static_cast<float>(std::sin(
                static_cast<double>(ticks) * 0.00045 + rail * 0.8
            ));
            outline_rect(
                renderer,
                {640.0F - width * 0.5F + wobble,
                 385.0F - height * 0.5F,
                 width,
                 height},
                {palette.accent.r, palette.accent.g, palette.accent.b,
                 static_cast<std::uint8_t>(28 + rail * 8)}
            );
        }
        for (int index = 0; index < 22; ++index) {
            const auto seed = static_cast<std::uint64_t>(index) * 8'173ULL;
            const float phase = std::fmod(
                drift + static_cast<float>((seed % 10'000ULL)) / 10'000.0F,
                1.0F
            );
            const float z = 0.10F + phase * 0.90F;
            const float x = 640.0F
                + static_cast<float>(std::sin(index * 1.73 + ticks * 0.00031))
                    * (90.0F + z * 520.0F);
            const float y = 355.0F
                + static_cast<float>(std::cos(index * 1.17 + ticks * 0.00024))
                    * (45.0F + z * 210.0F);
            const float size = 5.0F + z * 27.0F;
            outline_rect(renderer, {x, y, size, size},
                {78, 121, 255, static_cast<std::uint8_t>(45 + z * 150.0F)});
        }
        draw_text(renderer, 1'004.0F, 39.0F, "SYSTEM BROWSER // READY",
                  palette.accent, 1.05F);
    } else if (theme != PresentationTheme::pulseforge) {
        // PULSEFORGE_P1_5_0E_PROCEDURAL_THEME_PACK_V1
        // These themes are original procedural compositions inspired by broad
        // era/game/console visual motifs; no third-party logos or artwork ship.
        switch (theme) {
        case PresentationTheme::beverly_hills_90210: {
            const float horizon = 430.0F;
            for (int index = 0; index < 14; ++index) {
                const float y = horizon + static_cast<float>(index * index) * 1.25F;
                fill_rect(renderer, {0.0F, y, launcher_width, 1.0F},
                    {255, 65, 199, static_cast<std::uint8_t>(70 + index * 5)});
            }
            for (int index = -9; index <= 9; ++index) {
                const float top_x = 640.0F + static_cast<float>(index) * 34.0F;
                const float bottom_x = 640.0F + static_cast<float>(index) * 118.0F;
                SDL_SetRenderDrawColor(renderer, 41, 231, 236, 70);
                SDL_RenderLine(renderer, top_x, horizon, bottom_x, 720.0F);
            }
            for (int palm = 0; palm < 4; ++palm) {
                const float x = 110.0F + static_cast<float>(palm) * 350.0F;
                const float sway = 9.0F * static_cast<float>(std::sin(
                    ticks * 0.0006 + palm
                ));
                fill_rect(renderer, {x + sway, 275.0F, 5.0F, 150.0F},
                    {20, 39, 45, 170});
                for (int leaf = -2; leaf <= 2; ++leaf) {
                    SDL_SetRenderDrawColor(renderer, 20, 55, 56, 160);
                    SDL_RenderLine(renderer, x + sway + 2.0F, 282.0F,
                        x + sway + static_cast<float>(leaf) * 27.0F,
                        252.0F + std::abs(leaf) * 10.0F);
                }
            }
            draw_text(renderer, 1'018.0F, 39.0F, "90210 // SUMMER",
                palette.accent, 1.10F);
            break;
        }
        case PresentationTheme::just_cause_3: {
            for (int index = -4; index < 12; ++index) {
                const float x = static_cast<float>(index) * 145.0F
                    + static_cast<float>((ticks / 18ULL) % 145ULL);
                SDL_SetRenderDrawColor(renderer, 238, 62, 48, 72);
                for (int offset = 0; offset < 5; ++offset) {
                    SDL_RenderLine(renderer, x + offset * 2.0F, 145.0F,
                        x - 330.0F + offset * 2.0F, 720.0F);
                }
            }
            outline_rect(renderer, {870.0F, 170.0F, 300.0F, 300.0F},
                {238, 62, 48, 80});
            draw_text(renderer, 1'016.0F, 39.0F, "REBEL NETWORK // 3",
                palette.accent, 1.05F);
            break;
        }
        case PresentationTheme::just_cause_4: {
            const float pulse = 55.0F + 20.0F * static_cast<float>(std::sin(
                ticks * 0.0017
            ));
            for (int ring = 1; ring <= 8; ++ring) {
                const float size = pulse + static_cast<float>(ring) * 62.0F;
                outline_rect(renderer,
                    {640.0F - size * 0.5F, 390.0F - size * 0.35F, size, size * 0.7F},
                    {61, 210, 201, static_cast<std::uint8_t>(28 + ring * 9)});
            }
            for (int bolt = 0; bolt < 7; ++bolt) {
                const float x = 80.0F + bolt * 190.0F;
                SDL_SetRenderDrawColor(renderer, 255, 144, 39, 100);
                SDL_RenderLine(renderer, x, 160.0F, x + 70.0F, 270.0F);
                SDL_RenderLine(renderer, x + 70.0F, 270.0F, x + 25.0F, 390.0F);
            }
            draw_text(renderer, 1'004.0F, 39.0F, "STORM FRONT // 4",
                palette.accent, 1.05F);
            break;
        }
        case PresentationTheme::xbox_original: {
            const float breathe = 0.75F + 0.25F * static_cast<float>(std::sin(
                ticks * 0.0015
            ));
            for (int width = 0; width < 9; ++width) {
                const auto alpha = static_cast<std::uint8_t>(
                    32 + breathe * static_cast<float>(110 - width * 7)
                );
                SDL_SetRenderDrawColor(renderer, 91, 214, 39, alpha);
                SDL_RenderLine(renderer, 180.0F + width, 150.0F,
                    1'100.0F - width, 690.0F);
                SDL_RenderLine(renderer, 1'100.0F - width, 150.0F,
                    180.0F + width, 690.0F);
            }
            for (int ring = 0; ring < 8; ++ring) {
                const float inset = static_cast<float>(ring) * 24.0F;
                outline_rect(renderer, {370.0F + inset, 230.0F + inset * 0.55F,
                    540.0F - inset * 2.0F, 330.0F - inset * 1.1F},
                    {91, 214, 39, static_cast<std::uint8_t>(82 - ring * 7)});
            }
            draw_text(renderer, 1'010.0F, 39.0F, "GREEN CORE // READY",
                palette.accent, 1.05F);
            break;
        }
        case PresentationTheme::xbox_360: {
            for (int ring = 0; ring < 14; ++ring) {
                const float phase = static_cast<float>(ticks % 12'000ULL) / 12'000.0F;
                const float size = 90.0F + ring * 38.0F + phase * 38.0F;
                outline_rect(renderer, {640.0F - size, 400.0F - size * 0.45F,
                    size * 2.0F, size * 0.9F},
                    {94, 190, 31, static_cast<std::uint8_t>(68 - ring * 3)});
            }
            for (int blade = 0; blade < 5; ++blade) {
                const float y = 185.0F + blade * 88.0F;
                fill_rect(renderer, {890.0F + blade * 15.0F, y, 260.0F, 5.0F},
                    {94, 190, 31, static_cast<std::uint8_t>(100 - blade * 12)});
            }
            draw_text(renderer, 1'016.0F, 39.0F, "ORBIT // CONNECTED",
                palette.accent, 1.05F);
            break;
        }
        default:
            break;
        }
    } else {
        for (int index = 0; index < 18; ++index) {
            const auto seed = static_cast<std::uint64_t>(index) * 7'919ULL;
            const float x = static_cast<float>((seed + ticks / 19ULL) % 1'340ULL)
                - 30.0F;
            const float y = 118.0F + static_cast<float>((seed * 13ULL) % 560ULL);
            const float size = 2.0F + static_cast<float>(index % 3);
            fill_rect(renderer, {x, y, size, size}, {130, 108, 230, 70});
        }
    }
}

void draw_watch_dogs_options_backdrop(
    SDL_Renderer* renderer,
    const std::uint64_t ticks,
    const MenuPalette& palette
) {
    constexpr std::array<std::string_view, 5> labels{
        "AUDIO", "VIDEO", "INPUT", "GAMEPLAY", "SYSTEM",
    };
    for (std::size_t index = 0U; index < labels.size(); ++index) {
        const float y = 154.0F + static_cast<float>(index) * 104.0F;
        fill_rect(
            renderer,
            {22.0F, y, 104.0F, 2.0F},
            {palette.accent.r, palette.accent.g, palette.accent.b, 125}
        );
        draw_text(renderer, 24.0F, y + 20.0F, labels[index], palette.muted, 0.9F);
        const auto activity = static_cast<float>(
            (ticks / 19ULL + static_cast<std::uint64_t>(index) * 31ULL) % 90ULL
        );
        fill_rect(
            renderer,
            {24.0F, y + 48.0F, 12.0F + activity, 3.0F},
            palette.accent
        );
    }
    fill_rect(renderer, {1'154.0F, 142.0F, 3.0F, 520.0F}, palette.accent);
    draw_text(renderer, 1'174.0F, 154.0F, "CFG", palette.title, 1.15F);
    draw_text(renderer, 1'174.0F, 184.0F, "LIVE", palette.accent, 0.95F);
    for (int index = 0; index < 15; ++index) {
        const float y = 236.0F + static_cast<float>(index) * 27.0F;
        const auto width = 16.0F + static_cast<float>((index * 17) % 62);
        fill_rect(
            renderer,
            {1'174.0F, y, width, 2.0F},
            {palette.accent.r, palette.accent.g, palette.accent.b, 105}
        );
    }
}

void draw_menu_surface_backdrop(
    SDL_Renderer* renderer,
    const std::uint64_t ticks,
    const PresentationTheme theme,
    const MenuSurface surface,
    const MenuPalette& palette
) {
    if (surface == MenuSurface::generic) {
        return;
    }

    if (theme == PresentationTheme::ps2) {
        const auto label = surface_label(surface);
        fill_rect(renderer, {22.0F, 132.0F, 106.0F, 532.0F}, {0, 1, 16, 190});
        outline_rect(renderer, {22.0F, 132.0F, 106.0F, 532.0F}, palette.accent);
        draw_text(renderer, 34.0F, 154.0F, "SYSTEM", palette.title, 0.95F);
        draw_text(renderer, 34.0F, 184.0F, label, palette.accent, 0.74F);
        for (int index = 0; index < 11; ++index) {
            const float y = 244.0F + static_cast<float>(index) * 33.0F;
            const float phase = 0.5F + 0.5F * static_cast<float>(std::sin(
                ticks * 0.002 + index * 0.7
            ));
            outline_rect(renderer, {34.0F, y, 22.0F + phase * 44.0F, 8.0F},
                {palette.accent.r, palette.accent.g, palette.accent.b, 145});
        }
        draw_text(renderer, 34.0F, 626.0F, "MEMORY", palette.muted, 0.80F);
        fill_rect(renderer, {1'154.0F, 132.0F, 106.0F, 532.0F}, {0, 1, 16, 175});
        outline_rect(renderer, {1'154.0F, 132.0F, 106.0F, 532.0F}, palette.accent);
        draw_text(renderer, 1'169.0F, 154.0F, "PULSE", palette.title, 0.9F);
        draw_text(renderer, 1'169.0F, 181.0F, "FORGE", palette.accent, 0.9F);
        for (int index = 0; index < 7; ++index) {
            const float inset = static_cast<float>(index) * 5.0F;
            outline_rect(renderer,
                {1'174.0F + inset, 265.0F + inset, 66.0F - inset * 2.0F,
                 66.0F - inset * 2.0F},
                {palette.accent.r, palette.accent.g, palette.accent.b,
                 static_cast<std::uint8_t>(155 - index * 16)});
        }
        return;
    }

    if (theme == PresentationTheme::watch_dogs) {
        if (surface == MenuSurface::options) {
            draw_watch_dogs_options_backdrop(renderer, ticks, palette);
            return;
        }
        const auto label = surface_label(surface);
        fill_rect(renderer, {18.0F, 132.0F, 112.0F, 532.0F}, {2, 8, 9, 205});
        outline_rect(renderer, {18.0F, 132.0F, 112.0F, 532.0F}, palette.accent);
        draw_text(renderer, 31.0F, 151.0F, "ctOS", palette.title, 1.35F);
        draw_text(renderer, 31.0F, 181.0F, label, palette.accent, 0.82F);
        for (int index = 0; index < 19; ++index) {
            const float y = 226.0F + static_cast<float>(index) * 21.0F;
            const auto phase = static_cast<std::uint64_t>(index) * 37ULL
                + ticks / 23ULL;
            const float width = 12.0F + static_cast<float>(phase % 72ULL);
            fill_rect(
                renderer,
                {31.0F, y, width, index % 4 == 0 ? 3.0F : 1.0F},
                {palette.accent.r, palette.accent.g, palette.accent.b,
                 static_cast<std::uint8_t>(index % 4 == 0 ? 190 : 90)}
            );
        }
        draw_text(renderer, 31.0F, 626.0F, "LINKED", palette.muted, 0.85F);
        return;
    }

    if (theme != PresentationTheme::pulseforge) {
        const auto label = surface_label(surface);
        fill_rect(renderer, {20.0F, 132.0F, 108.0F, 532.0F},
            {palette.panel.r, palette.panel.g, palette.panel.b, 210});
        outline_rect(renderer, {20.0F, 132.0F, 108.0F, 532.0F}, palette.accent);
        draw_text(renderer, 31.0F, 153.0F, label, palette.accent, 0.72F);
        for (int index = 0; index < 13; ++index) {
            const float wave = 0.5F + 0.5F * static_cast<float>(std::sin(
                ticks * 0.0015 + index * 0.81
            ));
            fill_rect(renderer, {31.0F, 220.0F + index * 29.0F,
                16.0F + wave * 65.0F, 2.0F},
                {palette.accent.r, palette.accent.g, palette.accent.b, 115});
        }
        fill_rect(renderer, {1'154.0F, 132.0F, 106.0F, 532.0F},
            {palette.panel.r, palette.panel.g, palette.panel.b, 180});
        outline_rect(renderer, {1'154.0F, 132.0F, 106.0F, 532.0F}, palette.accent);
        return;
    }

    // PulseForge receives its own surface identity instead of falling back to
    // an unadorned generic list.  The rails deliberately stay outside the
    // central list panel so every row, including row zero, remains unobscured.
    const auto label = surface_label(surface);
    fill_rect(renderer, {20.0F, 132.0F, 108.0F, 532.0F}, {24, 13, 53, 220});
    outline_rect(renderer, {20.0F, 132.0F, 108.0F, 532.0F}, palette.accent);
    fill_rect(renderer, {31.0F, 151.0F, 86.0F, 5.0F}, palette.accent);
    draw_text(renderer, 31.0F, 177.0F, "PF//CORE", palette.title, 1.0F);
    draw_text(renderer, 31.0F, 207.0F, label, palette.muted, 0.78F);
    for (int index = 0; index < 12; ++index) {
        const float y = 265.0F + static_cast<float>(index) * 29.0F;
        const float wave = 0.5F + 0.5F * static_cast<float>(std::sin(
            static_cast<double>(ticks) * 0.003
            + static_cast<double>(index) * 0.72
        ));
        fill_rect(renderer, {31.0F, y, 22.0F + 52.0F * wave, 3.0F},
                  {palette.accent.r, palette.accent.g, palette.accent.b, 165});
    }
    fill_rect(renderer, {1'154.0F, 132.0F, 106.0F, 532.0F}, {18, 10, 43, 185});
    outline_rect(renderer, {1'154.0F, 132.0F, 106.0F, 532.0F},
                 {palette.accent.r, palette.accent.g, palette.accent.b, 130});
    draw_text(renderer, 1'171.0F, 157.0F, "FORGE", palette.title, 0.95F);
    for (int index = 0; index < 9; ++index) {
        const float inset = static_cast<float>(index) * 5.0F;
        outline_rect(
            renderer,
            {1'174.0F + inset, 236.0F + inset, 66.0F - inset * 2.0F,
             66.0F - inset * 2.0F},
            {palette.accent.r, palette.accent.g, palette.accent.b,
             static_cast<std::uint8_t>(150 - index * 12)}
        );
    }
    draw_text(renderer, 1'171.0F, 626.0F, "READY", palette.accent, 0.9F);
}

[[nodiscard]] bool play_procedural_return_stinger(MenuSession& menu) {
    menu.suspend_music();
    constexpr std::uint64_t duration_ns = 800'000'000ULL;
    const auto started = SDL_GetTicksNS();
    while (true) {
        menu.pump_presence();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            SDL_ConvertEventToRenderCoordinates(menu.renderer(), &event);
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                menu.request_close();
                return false;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                static_cast<void>(menu.handle_audio_action(event.key));
            } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                static_cast<void>(menu.handle_audio_action(event.gbutton));
            }
        }
        const auto now = SDL_GetTicksNS();
        if (now - started >= duration_ns) {
            return true;
        }
        const float progress = static_cast<float>(now - started)
            / static_cast<float>(duration_ns);
        const auto ticks = SDL_GetTicks();
        const auto palette = palette_for(menu.theme());
        draw_menu_background(menu.renderer(), ticks, menu.theme());
        fill_rect(
            menu.renderer(),
            {190.0F, 236.0F, 900.0F, 248.0F},
            palette.panel
        );
        outline_rect(
            menu.renderer(),
            {190.0F, 236.0F, 900.0F, 248.0F},
            palette.accent
        );
        draw_text(
            menu.renderer(),
            246.0F,
            285.0F,
            menu.theme() == PresentationTheme::watch_dogs
                ? "ctOS // RETURN CHANNEL"
                : menu.theme() == PresentationTheme::ps2
                    ? "PULSEFORGE // SYSTEM BROWSER RETURN"
                    : "PULSEFORGE // SESSION RETURN",
            palette.title,
            1.8F
        );
        draw_text(
            menu.renderer(),
            246.0F,
            344.0F,
            "Reconnecting to the previous interface in the same window...",
            palette.muted,
            1.1F
        );
        fill_rect(
            menu.renderer(),
            {246.0F, 410.0F, 788.0F, 8.0F},
            palette.header
        );
        fill_rect(
            menu.renderer(),
            {246.0F, 410.0F, 788.0F * progress, 8.0F},
            palette.accent
        );
        SDL_RenderPresent(menu.renderer());
        SDL_Delay(1);
    }
}

void show_loading_screen(
    MenuSession& menu,
    const std::string_view title,
    const std::string_view detail
) {
    auto* const renderer = menu.renderer();
    const auto palette = palette_for(
        menu.theme(),
        SDL_Color{78, 220, 230, 255}
    );
    draw_menu_background(renderer, SDL_GetTicks(), menu.theme());
    fill_rect(renderer, {150.0F, 190.0F, 980.0F, 320.0F}, palette.panel);
    outline_rect(
        renderer,
        {150.0F, 190.0F, 980.0F, 320.0F},
        palette.accent
    );
    draw_text(renderer, 205.0F, 245.0F, title, palette.title, 2.0F);
    draw_text(renderer, 205.0F, 305.0F, detail, palette.text, 1.25F);
    draw_text(
        renderer,
        205.0F,
        365.0F,
        "Loading and validating content inside PulseForge...",
        palette.muted,
        1.2F
    );
    draw_text(
        renderer,
        205.0F,
        405.0F,
        "Resolving content - progress becomes determinate after the chart header opens",
        palette.muted,
        1.0F
    );
    const auto active = static_cast<std::size_t>((SDL_GetTicks() / 90ULL) % 12ULL);
    for (std::size_t index = 0U; index < 12U; ++index) {
        fill_rect(
            renderer,
            {205.0F + static_cast<float>(index) * 72.0F, 442.0F, 54.0F, 8.0F},
            index == active ? palette.accent : palette.header
        );
    }
    SDL_RenderPresent(renderer);
}

[[nodiscard]] std::optional<std::size_t> browse_choices(
    MenuSession& menu,
    const std::string_view title,
    const std::span<const std::string> choices,
    const std::string_view footer = "UP/DOWN select   ENTER accept   ESC back",
    const std::size_t initial_selected = 0U,
    const bool pointer_enabled = true
) {
    if (menu.close_requested() || choices.empty()) {
        return std::nullopt;
    }
    auto* const renderer = menu.renderer();
    menu.set_title(std::string{"PulseForge - "} + std::string(title));

    std::size_t selected = std::min(initial_selected, choices.size() - 1U);
    SDL_Scancode held_navigation = SDL_SCANCODE_UNKNOWN;
    std::uint64_t next_navigation_repeat_ns{};
    bool running = true;
    bool accepted = false;
    while (running) {
        menu.update_music();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            SDL_ConvertEventToRenderCoordinates(renderer, &event);
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                menu.request_close();
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (menu.handle_audio_action(event.key)) {
                    continue;
                }
                switch (event.key.scancode) {
                case SDL_SCANCODE_ESCAPE:
                    running = false;
                    break;
                case SDL_SCANCODE_UP:
                    selected = selected == 0
                        ? choices.size() - 1
                        : selected - 1;
                    held_navigation = SDL_SCANCODE_UP;
                    next_navigation_repeat_ns = SDL_GetTicksNS()
                        + 1'000'000'000ULL;
                    break;
                case SDL_SCANCODE_DOWN:
                    selected = (selected + 1) % choices.size();
                    held_navigation = SDL_SCANCODE_DOWN;
                    next_navigation_repeat_ns = SDL_GetTicksNS()
                        + 1'000'000'000ULL;
                    break;
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER:
                    accepted = true;
                    running = false;
                    break;
                default:
                    break;
                }
            } else if (event.type == SDL_EVENT_KEY_UP
                && event.key.scancode == held_navigation) {
                held_navigation = SDL_SCANCODE_UNKNOWN;
            } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                static_cast<void>(menu.handle_audio_action(event.gbutton));
            } else if (pointer_enabled
                && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                && event.button.button == SDL_BUTTON_LEFT) {
                const float mouse_x = event.button.x;
                const float mouse_y = event.button.y;
                const auto range = menu_visible_range(
                    choices.size(),
                    selected,
                    14U
                );
                const auto visible = range.last - range.first;
                const auto first = range.first;
                if (mouse_x >= 170.0F && mouse_x <= 1'110.0F
                    && mouse_y >= 124.0F) {
                    const auto row = static_cast<std::size_t>(
                        (mouse_y - 124.0F) / 40.0F
                    );
                    const auto index = first + row;
                    if (row < visible && index < choices.size()) {
                        selected = index;
                        accepted = true;
                        running = false;
                    }
                }
            }
            if (!running) {
                break;
            }
        }

        if (held_navigation != SDL_SCANCODE_UNKNOWN) {
            const bool* const keyboard = SDL_GetKeyboardState(nullptr);
            if (keyboard == nullptr || !keyboard[held_navigation]) {
                held_navigation = SDL_SCANCODE_UNKNOWN;
            } else {
                const auto now = SDL_GetTicksNS();
                if (now >= next_navigation_repeat_ns) {
                    if (held_navigation == SDL_SCANCODE_UP) {
                        selected = selected == 0
                            ? choices.size() - 1
                            : selected - 1;
                    } else {
                        selected = (selected + 1) % choices.size();
                    }
                    next_navigation_repeat_ns = now + 90'000'000ULL;
                }
            }
        }

        const auto ticks = SDL_GetTicks();
        const float pulse = 0.5F + 0.5F * static_cast<float>(
            std::sin(static_cast<double>(ticks) * 0.004)
        );
        const auto palette = palette_for(menu.theme());
        draw_menu_background(renderer, ticks, menu.theme());
        draw_menu_surface_backdrop(
            renderer,
            ticks,
            menu.theme(),
            menu_surface_for(title),
            palette
        );
        draw_text(
            renderer,
            44.0F,
            30.0F,
            themed_menu_title(title, menu.theme()),
            palette.title,
            2.1F
        );
        draw_text(renderer, 44.0F, 73.0F, footer, palette.muted, 1.15F);
        draw_text(
            renderer,
            1'040.0F,
            73.0F,
            menu.volume_label(),
            palette.accent,
            1.15F
        );

        const auto range = menu_visible_range(choices.size(), selected, 14U);
        const auto first = range.first;
        const auto last = range.last;
        // Every row is already bounded by visible/last. Avoid a renderer clip
        // here: SDL applies clip rectangles together with the temporary text
        // scale, which could hide row zero while leaving its highlight visible.
        fill_rect(renderer, {150.0F, 118.0F, 980.0F, 588.0F}, palette.panel);
        outline_rect(
            renderer,
            {150.0F, 118.0F, 980.0F, 588.0F},
            {palette.accent.r, palette.accent.g, palette.accent.b, 90}
        );
        float y = 139.0F;
        for (std::size_t index = first; index < last; ++index) {
            const bool active = index == selected;
            if (active) {
                fill_rect(
                    renderer,
                    {170.0F, y - 15.0F, 940.0F, 36.0F},
                    palette.selected
                );
                outline_rect(
                    renderer,
                    {170.0F, y - 15.0F, 940.0F, 36.0F},
                    {
                        palette.accent.r,
                        palette.accent.g,
                        palette.accent.b,
                        static_cast<std::uint8_t>(190.0F + pulse * 65.0F),
                    }
                );
                fill_rect(
                    renderer,
                    {170.0F, y - 15.0F, 4.0F, 36.0F},
                    palette.accent
                );
            }
            draw_text(
                renderer,
                210.0F,
                y,
                std::string(active ? "> " : "  ") + choices[index],
                active ? palette.title : palette.text
            );
            y += 40.0F;
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(1);
    }

    return accepted && !menu.close_requested()
        ? std::optional<std::size_t>(selected)
        : std::nullopt;
}

// AutoChart preview/cancel prompts only need the default menu chrome. This
// overload makes the two-argument launcher call source-compatible with the
// canonical browse_choices(title, span, ...) implementation.
[[nodiscard]] std::optional<std::size_t> browse_choices(
    MenuSession& menu,
    const std::span<const std::string> choices
) {
    return browse_choices(menu, "AutoChart", choices);
}

template <std::size_t Size>
[[nodiscard]] std::optional<std::size_t> browse_choices(
    MenuSession& menu,
    const std::array<std::string, Size>& choices
) {
    return browse_choices(
        menu,
        "AutoChart",
        std::span<const std::string>{choices.data(), choices.size()}
    );
}
[[nodiscard]] std::optional<std::size_t> browse_catalog(
    MenuSession& menu,
    const ContentCatalog& catalog
) {
    if (menu.close_requested() || catalog.entries().empty()) {
        return std::nullopt;
    }
    auto* const renderer = menu.renderer();
    auto* const window = menu.window();
    menu.set_title("PulseForge - Freeplay");
    menu.publish_presence(
        RuntimeActivityKind::freeplay,
        std::to_string(catalog.entries().size()) + " charts available"
    );
    SDL_StartTextInput(window);

    std::size_t selected = 0;
    std::string query;
    std::vector<std::size_t> filtered;
    const auto rebuild_filter = [&]() {
        filtered.clear();
        for (std::size_t index = 0; index < catalog.entries().size(); ++index) {
            if (entry_matches(catalog.entries()[index], query)) {
                filtered.push_back(index);
            }
        }
        if (filtered.empty()) {
            selected = 0;
        } else {
            selected = std::min(selected, filtered.size() - 1);
        }
    };
    rebuild_filter();
    SDL_Scancode held_navigation = SDL_SCANCODE_UNKNOWN;
    std::uint64_t next_navigation_repeat_ns{};
    bool running = true;
    bool accepted = false;
    while (running) {
        menu.update_music();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            SDL_ConvertEventToRenderCoordinates(renderer, &event);
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                menu.request_close();
                running = false;
            } else if (event.type == SDL_EVENT_TEXT_INPUT) {
                if (query.size() + std::char_traits<char>::length(event.text.text)
                    <= 96U) {
                    query += event.text.text;
                    rebuild_filter();
                }
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (menu.handle_audio_action(event.key)) {
                    continue;
                }
                switch (event.key.scancode) {
                case SDL_SCANCODE_ESCAPE:
                    if (!query.empty()) {
                        query.clear();
                        rebuild_filter();
                    } else {
                        running = false;
                    }
                    break;
                case SDL_SCANCODE_BACKSPACE:
                    erase_last_utf8_codepoint(query);
                    rebuild_filter();
                    break;
                case SDL_SCANCODE_UP:
                    if (!filtered.empty()) {
                        selected = selected == 0
                            ? filtered.size() - 1
                            : selected - 1;
                        held_navigation = SDL_SCANCODE_UP;
                        next_navigation_repeat_ns = SDL_GetTicksNS()
                            + 1'000'000'000ULL;
                    }
                    break;
                case SDL_SCANCODE_DOWN:
                    if (!filtered.empty()) {
                        selected = (selected + 1) % filtered.size();
                        held_navigation = SDL_SCANCODE_DOWN;
                        next_navigation_repeat_ns = SDL_GetTicksNS()
                            + 1'000'000'000ULL;
                    }
                    break;
                case SDL_SCANCODE_PAGEUP:
                    if (!filtered.empty()) {
                        selected = selected > visible_rows
                            ? selected - visible_rows
                            : 0;
                    }
                    break;
                case SDL_SCANCODE_PAGEDOWN:
                    if (!filtered.empty()) {
                        selected = std::min(
                            selected + visible_rows,
                            filtered.size() - 1
                        );
                    }
                    break;
                case SDL_SCANCODE_HOME:
                    selected = 0;
                    break;
                case SDL_SCANCODE_END:
                    if (!filtered.empty()) {
                        selected = filtered.size() - 1;
                    }
                    break;
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER:
                    if (!filtered.empty()) {
                        accepted = true;
                        running = false;
                    }
                    break;
                default:
                    break;
                }
            } else if (event.type == SDL_EVENT_KEY_UP
                && event.key.scancode == held_navigation) {
                held_navigation = SDL_SCANCODE_UNKNOWN;
            } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                static_cast<void>(menu.handle_audio_action(event.gbutton));
            }
            if (!running) {
                break;
            }
        }

        if (held_navigation != SDL_SCANCODE_UNKNOWN && !filtered.empty()) {
            const bool* const keyboard = SDL_GetKeyboardState(nullptr);
            if (keyboard == nullptr || !keyboard[held_navigation]) {
                held_navigation = SDL_SCANCODE_UNKNOWN;
            } else {
                const auto now = SDL_GetTicksNS();
                if (now >= next_navigation_repeat_ns) {
                    if (held_navigation == SDL_SCANCODE_UP) {
                        selected = selected == 0
                            ? filtered.size() - 1
                            : selected - 1;
                    } else {
                        selected = (selected + 1) % filtered.size();
                    }
                    next_navigation_repeat_ns = now + 90'000'000ULL;
                }
            }
        }

        const auto ticks = SDL_GetTicks();
        draw_menu_background(
            renderer,
            ticks,
            menu.theme(),
            SDL_Color{78, 190, 230, 255}
        );
        const auto palette = palette_for(
            menu.theme(),
            SDL_Color{78, 190, 230, 255}
        );
        draw_menu_surface_backdrop(
            renderer,
            ticks,
            menu.theme(),
            MenuSurface::freeplay,
            palette
        );
        draw_text(
            renderer,
            42.0F,
            26.0F,
            themed_menu_title("PULSEFORGE  //  FREEPLAY", menu.theme()),
            palette.title,
            2.05F
        );
        draw_text(
            renderer,
            42.0F,
            66.0F,
            "TYPE search   BACKSPACE edit   ENTER play   ESC clear/back",
            palette.muted,
            1.18F
        );
        draw_text(
            renderer,
            1'040.0F,
            66.0F,
            menu.volume_label(),
            palette.accent,
            1.15F
        );

        const auto range = menu_visible_range(
            filtered.size(),
            selected,
            visible_rows
        );
        const std::size_t first = range.first;
        const std::size_t last = range.last;
        fill_rect(renderer, {150.0F, 118.0F, 980.0F, 546.0F}, palette.panel);
        outline_rect(
            renderer,
            {150.0F, 118.0F, 980.0F, 546.0F},
            {palette.accent.r, palette.accent.g, palette.accent.b, 90}
        );
        float y = 136.0F;
        for (std::size_t row = first; row < last; ++row) {
            const auto& entry = catalog.entries()[filtered[row]];
            const bool active = row == selected;
            if (active) {
                fill_rect(renderer, {164.0F, y - 9.0F, 952.0F, 28.0F}, palette.selected);
                fill_rect(renderer, {164.0F, y - 9.0F, 4.0F, 28.0F}, palette.accent);
            }
            std::string label = active ? "> " : "  ";
            label += entry.title + "  [" + entry.difficulty + "]  // ";
            label += entry.mod_name + "  // " + std::string(to_string(entry.layout));
            draw_text(
                renderer,
                178.0F,
                y,
                label,
                active ? palette.title : palette.text,
                1.15F
            );
            y += 30.0F;
        }
        fill_rect(renderer, {0.0F, 672.0F, launcher_width, 48.0F}, palette.header);
        if (filtered.empty()) {
            draw_text(
                renderer,
                42.0F,
                689.0F,
                "Search: " + query + "  //  no matching charts",
                {255, 154, 154, 255}
            );
        } else {
            const auto& entry = catalog.entries()[filtered[selected]];
            draw_text(
                renderer,
                42.0F,
                689.0F,
                "Search: " + (query.empty() ? std::string("<all>") : query)
                    + "  //  " + std::to_string(selected + 1) + "/"
                    + std::to_string(filtered.size()) + "  "
                    + entry.id + "  " + format_bytes(entry.chart_size_bytes),
                {153, 233, 178, 255}
            );
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(1);
    }

    SDL_StopTextInput(window);
    return accepted && !menu.close_requested() && !filtered.empty()
        ? std::optional<std::size_t>(filtered[selected])
        : std::nullopt;
}

[[nodiscard]] AppLaunchOptions options_for_entry(
    const AppLaunchOptions& base,
    const SongCatalogEntry& entry,
    const bool return_to_launcher
) {
    AppLaunchOptions result = base;
    result.chart_path = entry.chart_path;
    result.catalog_song.reset();
    result.show_launcher = false;
    result.return_to_launcher = return_to_launcher;
    result.selected_content_root = entry.content_root;
    result.selected_mod_root = entry.mod_root;
    if (!result.chart_options.difficulty_explicit
        && entry.layout != ContentLayout::vslice) {
        result.chart_options.difficulty = entry.difficulty;
    }
    if (!result.chart_options.metadata_path.has_value()
        && entry.metadata_path.has_value()) {
        result.chart_options.metadata_path = entry.metadata_path;
    }
    for (const auto& script : entry.script_paths) {
        result.script_paths.push_back(script);
    }
    if (!result.script_path.has_value()) {
        if (!entry.script_paths.empty()) {
            result.script_path = entry.script_paths.front();
        } else if (entry.script_path.has_value()) {
            result.script_path = entry.script_path;
        }
    }
    return result;
}

[[nodiscard]] std::optional<OfflineRenderConfig> configure_render(
    MenuSession& menu
) {
    // PULSEFORGE_P1_5_0E_EXTENDED_RENDER_SETTINGS_UI_V1
    struct Resolution {
        std::uint32_t width;
        std::uint32_t height;
        std::string_view label;
    };
    constexpr std::array resolutions{
        Resolution{640U, 360U, "640x360"},
        Resolution{1280U, 720U, "1280x720"},
        Resolution{1920U, 1080U, "1920x1080"},
        Resolution{2560U, 1440U, "2560x1440"},
        Resolution{3840U, 2160U, "3840x2160"},
        Resolution{5120U, 2880U, "5120x2880"},
        Resolution{7680U, 4320U, "7680x4320"},
    };
    constexpr std::array<std::uint32_t, 9> frame_rates{
        30U, 60U, 120U, 144U, 240U, 360U, 480U, 720U, 1'000U,
    };
    struct EncodingPreset {
        OfflineRenderPreset value;
        std::string_view label;
    };
    constexpr std::array encoding_presets{
        EncodingPreset{OfflineRenderPreset::realtime, "Realtime / ultrafast"},
        EncodingPreset{OfflineRenderPreset::fastest, "Fast / superfast"},
        EncodingPreset{OfflineRenderPreset::balanced, "Balanced / veryfast"},
        EncodingPreset{OfflineRenderPreset::quality, "Quality / slow"},
        EncodingPreset{OfflineRenderPreset::compact, "Compact / slower"},
    };
    struct CodecChoice {
        OfflineRenderVideoCodec value;
        std::string_view label;
    };
    constexpr std::array codecs{
        CodecChoice{OfflineRenderVideoCodec::h264, "H.264 / libx264"},
        CodecChoice{OfflineRenderVideoCodec::h265, "H.265 / libx265"},
        CodecChoice{OfflineRenderVideoCodec::av1, "AV1 / libsvtav1"},
    };
    struct PixelChoice {
        OfflineRenderPixelFormat value;
        std::string_view label;
    };
    constexpr std::array pixel_formats{
        PixelChoice{OfflineRenderPixelFormat::yuv420p, "YUV 4:2:0 (compatible)"},
        PixelChoice{OfflineRenderPixelFormat::yuv422p, "YUV 4:2:2"},
        PixelChoice{OfflineRenderPixelFormat::yuv444p, "YUV 4:4:4"},
    };
    constexpr std::array<std::uint32_t, 6> qualities{30U, 23U, 18U, 14U, 10U, 0U};
    constexpr std::array<std::uint32_t, 6> audio_bitrates{128U, 192U, 256U, 320U, 384U, 512U};
    constexpr std::array<std::uint32_t, 6> thread_counts{0U, 2U, 4U, 8U, 16U, 32U};
    constexpr std::array<std::uint32_t, 5> keyframe_seconds{0U, 1U, 2U, 5U, 10U};

    std::size_t resolution = 2U;
    std::size_t frame_rate = 1U;
    std::size_t encoding_preset = 2U;
    std::size_t codec = 0U;
    std::size_t pixel_format = 0U;
    std::size_t quality = 2U;
    std::size_t audio_bitrate = 2U;
    std::size_t thread_count = 0U;
    std::size_t keyframe_interval = 2U;
    bool faststart = true;
    bool maximum_performance = false;
    bool overwrite = false;
    std::size_t selected_row = 0U;
    while (true) {
        const auto thread_label = thread_counts[thread_count] == 0U
            ? std::string{"Auto"}
            : std::to_string(thread_counts[thread_count]);
        const auto gop_label = keyframe_seconds[keyframe_interval] == 0U
            ? std::string{"Encoder default"}
            : std::to_string(keyframe_seconds[keyframe_interval]) + " s";
        const std::vector<std::string> choices{
            "Resolution: " + std::string(resolutions[resolution].label),
            "Frame rate: " + std::to_string(frame_rates[frame_rate]) + " FPS",
            "Codec: " + std::string(codecs[codec].label),
            "Encoding effort: " + std::string(encoding_presets[encoding_preset].label),
            "Quality: CRF " + std::to_string(qualities[quality]),
            "Pixel format: " + std::string(pixel_formats[pixel_format].label),
            "AAC bitrate: " + std::to_string(audio_bitrates[audio_bitrate]) + " kbps",
            "Encoder threads: " + thread_label,
            "Keyframe interval: " + gop_label,
            std::string{"Fast-start MP4: "} + (faststart ? "ON" : "OFF"),
            std::string{"MAXIMUM PERFORMANCE: "} + (maximum_performance ? "ON" : "OFF"),
            std::string{"Overwrite existing: "} + (overwrite ? "ON" : "OFF"),
            "Start deterministic render",
            "Back",
        };
        const auto action = browse_choices(
            menu,
            "PULSEFORGE  //  RENDERING MODE",
            choices,
            maximum_performance
                ? "MAX PERFORMANCE forces portable H.264 ultrafast/zero-latency and skips faststart"
                : "ENTER changes a setting   MP4 is exported atomically to renders/",
            selected_row,
            false
        );
        if (!action.has_value() || *action == 13U) {
            return std::nullopt;
        }
        selected_row = *action;
        switch (*action) {
        case 0U: resolution = (resolution + 1U) % resolutions.size(); break;
        case 1U: frame_rate = (frame_rate + 1U) % frame_rates.size(); break;
        case 2U: codec = (codec + 1U) % codecs.size(); break;
        case 3U:
            encoding_preset = (encoding_preset + 1U) % encoding_presets.size();
            break;
        case 4U: quality = (quality + 1U) % qualities.size(); break;
        case 5U: pixel_format = (pixel_format + 1U) % pixel_formats.size(); break;
        case 6U: audio_bitrate = (audio_bitrate + 1U) % audio_bitrates.size(); break;
        case 7U: thread_count = (thread_count + 1U) % thread_counts.size(); break;
        case 8U:
            keyframe_interval = (keyframe_interval + 1U) % keyframe_seconds.size();
            break;
        case 9U: faststart = !faststart; break;
        case 10U: maximum_performance = !maximum_performance; break;
        case 11U: overwrite = !overwrite; break;
        default: {
            OfflineRenderConfig config;
            config.enabled = true;
            config.output_directory = "renders";
            config.width = resolutions[resolution].width;
            config.height = resolutions[resolution].height;
            config.fps = frame_rates[frame_rate];
            config.crf = qualities[quality];
            config.preset = encoding_presets[encoding_preset].value;
            config.video_codec = codecs[codec].value;
            config.pixel_format = pixel_formats[pixel_format].value;
            config.audio_bitrate_kbps = audio_bitrates[audio_bitrate];
            config.thread_count = thread_counts[thread_count];
            config.keyframe_interval_seconds = keyframe_seconds[keyframe_interval];
            config.faststart = faststart;
            config.maximum_performance = maximum_performance;
            config.overwrite = overwrite;
            return config;
        }
        }
    }
}

struct StoryCollection {
    std::string key;
    std::string label;
    std::size_t mod_order{};
    std::vector<const SongCatalogEntry*> charts;
};

[[nodiscard]] std::vector<StoryCollection> build_story_collections(
    const ContentCatalog& catalog
) {
    std::map<std::string, StoryCollection> grouped;
    for (const auto& entry : catalog.entries()) {
        const std::string week = entry.week.empty()
            ? "Extra Songs"
            : entry.week;
        const std::string key = entry.mod_id + '\n' + lower_ascii(week);
        auto [iterator, inserted] = grouped.try_emplace(key);
        auto& collection = iterator->second;
        if (inserted) {
            collection.key = key;
            collection.label = entry.mod_name + "  //  " + week;
            collection.mod_order = entry.mod_order;
        }
        collection.charts.push_back(&entry);
    }

    std::vector<StoryCollection> result;
    result.reserve(grouped.size());
    for (auto& [key, collection] : grouped) {
        static_cast<void>(key);
        std::ranges::sort(
            collection.charts,
            [](const SongCatalogEntry* left, const SongCatalogEntry* right) {
                return std::tuple{
                    left->week_song_order,
                    lower_ascii(left->song_id),
                    lower_ascii(left->difficulty),
                    left->id
                } < std::tuple{
                    right->week_song_order,
                    lower_ascii(right->song_id),
                    lower_ascii(right->difficulty),
                    right->id
                };
            }
        );
        result.push_back(std::move(collection));
    }
    std::ranges::sort(
        result,
        [](const StoryCollection& left, const StoryCollection& right) {
            return std::tuple{left.mod_order, lower_ascii(left.label), left.key}
                < std::tuple{right.mod_order, lower_ascii(right.label), right.key};
        }
    );
    return result;
}

[[nodiscard]] std::vector<std::string> collection_difficulties(
    const StoryCollection& collection
) {
    std::map<std::string, std::string> unique;
    for (const auto* entry : collection.charts) {
        unique.try_emplace(lower_ascii(entry->difficulty), entry->difficulty);
    }
    std::vector<std::string> result;
    result.reserve(unique.size());
    for (const auto& [key, value] : unique) {
        static_cast<void>(key);
        result.push_back(value);
    }
    const auto normal = std::ranges::find_if(result, [](const std::string& value) {
        return lower_ascii(value) == "normal";
    });
    if (normal != result.end()) {
        std::rotate(result.begin(), normal, normal + 1);
    }
    return result;
}

[[nodiscard]] std::vector<const SongCatalogEntry*> build_story_playlist(
    const StoryCollection& collection,
    const std::string_view difficulty
) {
    std::vector<const SongCatalogEntry*> result;
    std::map<std::string, std::size_t> song_positions;
    const auto desired = lower_ascii(difficulty);
    for (const auto* entry : collection.charts) {
        const auto song_key = lower_ascii(entry->song_id);
        const auto found = song_positions.find(song_key);
        if (found == song_positions.end()) {
            song_positions.emplace(song_key, result.size());
            result.push_back(entry);
            continue;
        }
        auto*& current = result[found->second];
        const auto candidate_difficulty = lower_ascii(entry->difficulty);
        const auto current_difficulty = lower_ascii(current->difficulty);
        const bool candidate_exact = candidate_difficulty == desired;
        const bool current_exact = current_difficulty == desired;
        const bool candidate_normal = candidate_difficulty == "normal";
        const bool current_normal = current_difficulty == "normal";
        if ((candidate_exact && !current_exact)
            || (!current_exact && candidate_normal && !current_normal)) {
            current = entry;
        }
    }
    return result;
}

[[nodiscard]] std::string on_off(const bool value) {
    return value ? "ON" : "OFF";
}

struct FileDialogState {
    std::mutex mutex;
    std::filesystem::path selected;
    std::string error;
    std::atomic<bool> complete{};
};

void SDLCALL file_dialog_callback(
    void* const userdata,
    const char* const* file_list,
    const int filter
) {
    static_cast<void>(filter);
    auto& state = *static_cast<FileDialogState*>(userdata);
    {
        const std::scoped_lock lock(state.mutex);
        if (file_list == nullptr) {
            state.error = "The native file dialog failed";
        } else if (file_list[0] != nullptr) {
            const std::string_view selected(file_list[0]);
            std::u8string encoded;
            encoded.reserve(selected.size());
            for (const unsigned char character : selected) {
                encoded.push_back(static_cast<char8_t>(character));
            }
            state.selected = std::filesystem::path(encoded);
        }
    }
    state.complete.store(true, std::memory_order_release);
}

[[nodiscard]] std::optional<std::filesystem::path> choose_source_path(
    MenuSession& menu,
    const bool directory,
    const std::span<const SDL_DialogFileFilter> filters,
    const std::string_view title,
    const std::string_view detail,
    std::string& error
) {
    FileDialogState state;
    if (directory) {
        SDL_ShowOpenFolderDialog(
            file_dialog_callback,
            &state,
            menu.window(),
            nullptr,
            false
        );
    } else {
        SDL_ShowOpenFileDialog(
            file_dialog_callback,
            &state,
            menu.window(),
            filters.empty() ? nullptr : filters.data(),
            static_cast<int>(filters.size()),
            nullptr,
            false
        );
    }

    while (!state.complete.load(std::memory_order_acquire)) {
        menu.update_music();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // Native dialogs own cancellation while open. Keeping this state
            // alive until the callback prevents a use-after-free on platforms
            // that invoke the callback from another thread.
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                menu.request_close();
            }
        }
        draw_menu_background(
            menu.renderer(),
            SDL_GetTicks(),
            menu.theme(),
            SDL_Color{89, 220, 183, 255}
        );
        draw_text(
            menu.renderer(),
            94.0F,
            270.0F,
            title,
            {126, 246, 220, 255},
            2.0F
        );
        draw_text(
            menu.renderer(),
            94.0F,
            325.0F,
            detail,
            {218, 214, 236, 255},
            1.35F
        );
        SDL_RenderPresent(menu.renderer());
        SDL_Delay(8);
    }
    const std::scoped_lock lock(state.mutex);
    if (menu.close_requested()) {
        return std::nullopt;
    }
    error = state.error;
    return state.selected.empty()
        ? std::nullopt
        : std::optional<std::filesystem::path>(state.selected);
}

[[nodiscard]] std::optional<std::filesystem::path> choose_mod_source(
    MenuSession& menu,
    const bool directory,
    std::string& error
) {
    constexpr std::array filters{
        SDL_DialogFileFilter{
            "Mod archives (ZIP, 7z, RAR, TAR)",
            "zip;7z;rar;tar"
        },
    };
    return choose_source_path(
        menu,
        directory,
        directory
            ? std::span<const SDL_DialogFileFilter>{}
            : std::span<const SDL_DialogFileFilter>{filters},
        directory ? "SELECT AN UNPACKED MOD FOLDER"
                  : "SELECT A MOD ARCHIVE",
        "The package will be validated and installed atomically.",
        error
    );
}

[[nodiscard]] std::filesystem::path choose_mods_root(
    const AppLaunchOptions& options
) {
    for (const auto& root : options.content_roots) {
        const auto encoded = root.filename().generic_u8string();
        if (lower_ascii(std::string(encoded.begin(), encoded.end())) == "mods") {
            return root;
        }
    }
    return std::filesystem::current_path() / "mods";
}

[[nodiscard]] std::string editor_slug(const std::string_view value) {
    std::string result;
    result.reserve(std::min<std::size_t>(value.size(), 80U));
    bool separator = false;
    for (const unsigned char character : value) {
        const bool alpha = character >= 'A' && character <= 'Z';
        const bool lower = character >= 'a' && character <= 'z';
        const bool digit = character >= '0' && character <= '9';
        if (alpha || lower || digit) {
            if (separator && !result.empty()) {
                result.push_back('-');
            }
            separator = false;
            result.push_back(alpha
                ? static_cast<char>(character - 'A' + 'a')
                : static_cast<char>(character));
            if (result.size() >= 80U) {
                break;
            }
        } else {
            separator = true;
        }
    }
    return result.empty() ? "untitled" : result;
}

[[nodiscard]] std::string path_as_utf8(
    const std::filesystem::path& path
) {
    const auto encoded = path.generic_u8string();
    return std::string(encoded.begin(), encoded.end());
}

[[nodiscard]] bool regular_non_link_file(
    const std::filesystem::path& path
) noexcept {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error
        && !std::filesystem::is_symlink(path, error) && !error;
}

[[nodiscard]] std::optional<std::filesystem::path> find_packaged_runtime_file(
    const std::filesystem::path& relative
) {
    const char* const base_text = SDL_GetBasePath();
    if (base_text == nullptr || *base_text == '\0') {
        return std::nullopt;
    }
    const auto base = std::filesystem::path(
        reinterpret_cast<const char8_t*>(base_text)
    );
    const auto candidate = base / relative;
    if (!regular_non_link_file(candidate)) {
        return std::nullopt;
    }
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(candidate, error);
    return error ? candidate.lexically_normal() : canonical;
}

[[nodiscard]] std::optional<std::filesystem::path> find_runtime_file(
    const std::filesystem::path& relative
) {
    std::vector<std::filesystem::path> candidates;
    std::error_code error;
    candidates.push_back(std::filesystem::current_path(error) / relative);
    const char* const base_text = SDL_GetBasePath();
    if (base_text != nullptr && *base_text != '\0') {
        const auto base = std::filesystem::path(
            reinterpret_cast<const char8_t*>(base_text)
        );
        candidates.push_back(base / relative);
        candidates.push_back(base.parent_path() / relative);
    }
    for (const auto& candidate : candidates) {
        if (regular_non_link_file(candidate)) {
            const auto canonical = std::filesystem::weakly_canonical(
                candidate,
                error
            );
            return error ? candidate.lexically_normal() : canonical;
        }
        error.clear();
    }
    return std::nullopt;
}

[[nodiscard]] std::string available_flp_slug(
    const std::filesystem::path& mod_root,
    const std::string_view preferred
) {
    const auto base = editor_slug(preferred);
    std::error_code error;
    for (std::uint64_t suffix = 0U; suffix < 1'000'000U; ++suffix) {
        const auto candidate = suffix == 0U
            ? base
            : base + '-' + std::to_string(suffix);
        if (!std::filesystem::exists(mod_root / "data" / candidate, error)
            && !error) {
            return candidate;
        }
        error.clear();
    }
    return base + "-" + std::to_string(static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    ));
}

[[nodiscard]] bool ensure_flp_mod_manifest(
    const std::filesystem::path& mod_root,
    std::string& error
) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(mod_root, filesystem_error);
    if (filesystem_error) {
        error = "Cannot create the FLP chart mod folder";
        return false;
    }
    const auto manifest = mod_root / "mod.json";
    if (std::filesystem::is_regular_file(manifest, filesystem_error)
        && !filesystem_error) {
        return true;
    }
    filesystem_error.clear();
    const auto temporary = mod_root / ".mod.json.pulseforge-new";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output
        << "{\n"
        << "  \"id\": \"user.sniff.flp-charts\",\n"
        << "  \"name\": \"Charts feitos no FLP\",\n"
        << "  \"version\": \"1\",\n"
        << "  \"engine\": \"psych\",\n"
        << "  \"description\": \"Charts converted locally with the guarded SNIFF-RUSTED bridge.\"\n"
        << "}\n";
    output.flush();
    if (!output) {
        output.close();
        std::filesystem::remove(temporary, filesystem_error);
        error = "Cannot write the FLP chart mod manifest";
        return false;
    }
    output.close();
    std::filesystem::rename(temporary, manifest, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary, filesystem_error);
        error = "Cannot commit the FLP chart mod manifest";
        return false;
    }
    return true;
}

struct FlpInstallResult {
    bool installed{};
    std::string message;
};

[[nodiscard]] FlpInstallResult install_flp_chart(
    MenuSession& menu,
    const AppLaunchOptions& options
) {
    FlpInstallResult result;
    constexpr std::array flp_filters{
        SDL_DialogFileFilter{"FL Studio projects", "flp"},
    };
    std::string dialog_error;
    const auto input = choose_source_path(
        menu,
        false,
        flp_filters,
        "SELECT A FL STUDIO PROJECT",
        "SNIFF-RUSTED will convert its note data to a Psych JSON chart.",
        dialog_error
    );
    if (!input.has_value()) {
        result.message = dialog_error;
        return result;
    }
    const auto wrapper = find_packaged_runtime_file(
        "tools/sniff/import-sniff.ps1"
    );
    if (!wrapper.has_value()) {
        result.message = "The trusted PulseForge SNIFF wrapper is missing.";
        return result;
    }
    auto executable = find_runtime_file(
        "local-tools/sniff/sniff-rusted.exe"
    );
    if (!executable.has_value()) {
        constexpr std::array executable_filters{
            SDL_DialogFileFilter{"SNIFF-RUSTED executable", "exe"},
        };
        executable = choose_source_path(
            menu,
            false,
            executable_filters,
            "SELECT SNIFF-RUSTED.EXE",
            "It will run only if SHA-256 matches the audited user-supplied build.",
            dialog_error
        );
        if (!executable.has_value()) {
            result.message = dialog_error;
            return result;
        }
    }

    const std::array confirmation{
        std::string{"Convert using the hash-verified SNIFF-RUSTED"},
        std::string{"Cancel"},
    };
    std::size_t selected = 1U;
    const auto confirmed = browse_choices(
        menu,
        "FLP IMPORT  //  EXTERNAL CONVERTER",
        confirmation,
        std::string{"No code runs until you choose Convert. Required SHA-256: "}
            + audited_sniff_sha256,
        selected
    );
    if (!confirmed.has_value() || *confirmed != 0U) {
        return result;
    }

    const auto mods_root = choose_mods_root(options);
    const auto flp_mod = mods_root / "charts feitos no flp";
    const auto slug = available_flp_slug(flp_mod, path_as_utf8(input->stem()));
    std::error_code filesystem_error;
    std::filesystem::create_directories(mods_root, filesystem_error);
    if (filesystem_error) {
        result.message = "Cannot create the mods directory.";
        return result;
    }
    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    const auto staging = mods_root
        / (".sniff-import-" + slug + '-' + std::to_string(stamp));
    if (!std::filesystem::create_directory(staging, filesystem_error)
        || filesystem_error) {
        result.message = "Cannot create an isolated FLP import staging folder.";
        return result;
    }
    const auto staged_chart = staging / (slug + ".json");
    const auto cleanup_staging = [&]() noexcept {
        // `staging` is an engine-created, unique, direct child of mods_root.
        // SNIFF may be terminated before its PowerShell finally block runs, so
        // remove every private temporary it produced instead of assuming the
        // directory contains only the requested JSON path.
        std::error_code root_error;
        const auto expected_parent = std::filesystem::weakly_canonical(
            mods_root,
            root_error
        );
        std::error_code cleanup_error;
        const auto actual_parent = std::filesystem::weakly_canonical(
            staging.parent_path(),
            cleanup_error
        );
        if (!root_error && !cleanup_error && actual_parent == expected_parent
            && path_as_utf8(staging.filename()).starts_with(".sniff-import-")) {
            std::filesystem::remove_all(staging, cleanup_error);
        }
    };
    const auto conversion = run_sniff_bridge(
        SniffBridgeRequest{
            *wrapper,
            *executable,
            *input,
            staged_chart,
            slug,
        },
        [&]() {
            menu.update_music();
            bool keep_running = !menu.close_requested();
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT
                    || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    menu.request_close();
                    keep_running = false;
                } else if (event.type == SDL_EVENT_KEY_DOWN
                    && event.key.key == SDLK_ESCAPE) {
                    keep_running = false;
                }
            }
            draw_menu_background(
                menu.renderer(),
                SDL_GetTicks(),
                menu.theme(),
                SDL_Color{89, 220, 183, 255}
            );
            draw_text(
                menu.renderer(),
                94.0F,
                270.0F,
                "CONVERTING FLP CHART...",
                {126, 246, 220, 255},
                2.0F
            );
            draw_text(
                menu.renderer(),
                94.0F,
                325.0F,
                "The UI remains responsive. ESC cancels the isolated process.",
                {218, 214, 236, 255},
                1.35F
            );
            SDL_RenderPresent(menu.renderer());
            return keep_running;
        }
    );
    if (!conversion.success) {
        cleanup_staging();
        result.message = conversion.error;
        return result;
    }
    if (!ensure_flp_mod_manifest(flp_mod, result.message)) {
        cleanup_staging();
        return result;
    }
    const auto chart_directory = flp_mod / "data" / slug;
    std::filesystem::create_directories(chart_directory, filesystem_error);
    if (filesystem_error) {
        cleanup_staging();
        result.message = "Cannot create the converted chart directory.";
        return result;
    }
    const auto installed_chart = chart_directory / (slug + ".json");
    std::filesystem::rename(staged_chart, installed_chart, filesystem_error);
    if (filesystem_error) {
        std::error_code cleanup_error;
        cleanup_staging();
        std::filesystem::remove(chart_directory, cleanup_error);
        result.message = "Cannot atomically commit the converted chart.";
        return result;
    }
    cleanup_staging();

    const std::array audio_choice{
        std::string{"Add an instrumental audio file now..."},
        std::string{"Skip audio for now"},
    };
    selected = 0U;
    const auto add_audio = browse_choices(
        menu,
        "FLP CHART CONVERTED",
        audio_choice,
        "SNIFF converts notes only; FL Studio does not embed a finished Inst track.",
        selected
    );
    bool audio_installed = false;
    if (add_audio.has_value() && *add_audio == 0U) {
        constexpr std::array audio_filters{
            SDL_DialogFileFilter{
                "Instrumental audio",
                "ogg;mp3;wav;flac"
            },
        };
        const auto audio = choose_source_path(
            menu,
            false,
            audio_filters,
            "SELECT THE INSTRUMENTAL",
            "It will be copied into the converted Psych-style mod.",
            dialog_error
        );
        if (audio.has_value()) {
            const auto audio_directory = flp_mod / "songs" / slug;
            std::filesystem::create_directories(
                audio_directory,
                filesystem_error
            );
            auto extension = lower_ascii(path_as_utf8(audio->extension()));
            const std::array<std::string_view, 4> allowed{
                ".ogg", ".mp3", ".wav", ".flac"
            };
            if (!filesystem_error
                && std::ranges::find(allowed, extension) != allowed.end()) {
                audio_installed = std::filesystem::copy_file(
                    *audio,
                    audio_directory / ("Inst" + extension),
                    std::filesystem::copy_options::none,
                    filesystem_error
                ) && !filesystem_error;
            }
        }
    }
    result.installed = true;
    result.message = "charts feitos no flp  //  " + slug
        + (audio_installed ? "  //  chart + Inst installed"
                           : "  //  chart installed; add Inst audio later");
    return result;
}

[[nodiscard]] bool safe_editor_logical_path(
    const std::filesystem::path& path
) {
    if (path.empty() || path.is_absolute() || path.has_root_name()
        || path.has_root_directory()) {
        return false;
    }
    for (const auto& component : path) {
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::filesystem::path editor_logical_audio_path(
    const std::filesystem::path& source,
    const SongCatalogEntry& entry
) {
    if (source.empty()) {
        return {};
    }
    if (safe_editor_logical_path(source)) {
        return source;
    }
    const std::array<std::filesystem::path, 3> roots{
        entry.content_root,
        entry.mod_root,
        entry.chart_path.parent_path(),
    };
    for (const auto& root : roots) {
        if (root.empty()) {
            continue;
        }
        const auto relative = source.lexically_relative(root);
        if (safe_editor_logical_path(relative)) {
            return relative;
        }
    }
    const auto filename = source.filename();
    return safe_editor_logical_path(filename) ? filename
                                               : std::filesystem::path{};
}

[[nodiscard]] AudioManifest editor_logical_audio_manifest(
    const AudioManifest& source,
    const SongCatalogEntry& entry
) {
    AudioManifest logical;
    logical.instrumental = editor_logical_audio_path(
        source.instrumental,
        entry
    );
    logical.vocals.reserve(source.vocals.size());
    for (const auto& vocal : source.vocals) {
        const auto path = editor_logical_audio_path(vocal, entry);
        if (!path.empty()) {
            logical.vocals.push_back(path);
        }
    }
    return logical;
}

[[nodiscard]] std::string hexadecimal_u64(const std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

[[nodiscard]] std::uint64_t path_fingerprint(
    const std::filesystem::path& path
) {
    constexpr std::uint64_t offset = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t prime = 1'099'511'628'211ULL;
    auto value = offset;
    const auto encoded = path_as_utf8(path.lexically_normal());
    for (const unsigned char byte : encoded) {
        value ^= byte;
        value *= prime;
    }
    return value;
}

[[nodiscard]] std::filesystem::path available_editor_path(
    const EditorStorage& storage,
    const std::filesystem::path& directory,
    const std::string_view preferred_stem,
    const std::string_view extension
) {
    const auto stem = editor_slug(preferred_stem);
    for (std::uint32_t index = 1U; index < 100'000U; ++index) {
        const auto suffix = index == 1U
            ? std::string{}
            : "-" + std::to_string(index);
        const auto relative = directory
            / (stem + suffix + std::string(extension));
        std::error_code error;
        if (!std::filesystem::exists(storage.root() / relative, error)
            && !error) {
            return relative;
        }
    }
    return directory / (stem + "-overflow" + std::string(extension));
}

struct EditorMenuResult {
    bool quit_requested{};
    bool content_changed{};
};

void show_editor_outcome(
    MenuSession& menu,
    const std::string_view title,
    const EditorUiOutcome& outcome
) {
    if (outcome.message.empty() && !outcome.project_saved
        && !outcome.compatible_json_saved) {
        return;
    }
    std::string details = outcome.message;
    if (details.empty()) {
        details = outcome.compatible_json_saved
            ? "Compatible JSON saved atomically"
            : "Editor project saved atomically";
    }
    const std::array<std::string, 1> back{"Continue"};
    static_cast<void>(browse_choices(menu, title, back, details));
}


struct AutoChartWorkflowResult final {
    bool quit_requested{};
    bool content_changed{};
};

struct AutoChartGuiConfig final {
    AutoChartMode mode{AutoChartMode::accurate};
    AutoChartMlMode ml_mode{AutoChartMlMode::automatic};
    AutoChartVideoMode video_mode{AutoChartVideoMode::automatic};
    std::uint16_t key_count{4U};
    std::vector<std::string> difficulties{"expert"};
    bool variable_tempo{};
};

[[nodiscard]] std::optional<std::filesystem::path> choose_autochart_media(
    MenuSession& menu,
    std::string& error
) {
    constexpr std::array filters{
        SDL_DialogFileFilter{
            "Audio / video",
            "mp3;ogg;wav;flac;aac;m4a;opus;wma;mp4;mkv;mov;avi;wmv;webm;m4v"
        },
        SDL_DialogFileFilter{
            "Audio",
            "mp3;ogg;wav;flac;aac;m4a;opus;wma"
        },
        SDL_DialogFileFilter{
            "Video",
            "mp4;mkv;mov;avi;wmv;webm;m4v"
        },
    };
    return choose_source_path(
        menu,
        false,
        std::span<const SDL_DialogFileFilter>{filters},
        "AUTOCHART  //  SELECT MEDIA",
        "Choose one audio or video file. PulseForge extracts and analyses its audio automatically.",
        error
    );
}

[[nodiscard]] std::string autochart_difficulty_label(
    const std::vector<std::string>& difficulties
) {
    if (difficulties == std::vector<std::string>{"expert"}) {
        return "Expert";
    }
    if (difficulties == std::vector<std::string>{"easy", "normal", "hard", "expert"}) {
        return "Easy + Normal + Hard + Expert";
    }
    if (difficulties == std::vector<std::string>{
            "easy", "normal", "hard", "expert", "insane"}) {
        return "All (Easy -> Insane)";
    }
    std::string label;
    for (const auto& difficulty : difficulties) {
        if (!label.empty()) {
            label += ", ";
        }
        label += difficulty;
    }
    return label;
}

[[nodiscard]] bool configure_autochart_gui(
    MenuSession& menu,
    AutoChartGuiConfig& config
) {
    std::size_t selected_row = 0U;
    while (true) {
        const std::array choices{
            std::string{"Generate chart"},
            std::string{"Analysis quality: "} + std::string(to_string(config.mode)),
            std::string{"ML: "} + std::string(to_string(config.ml_mode)),
            std::string{"Video assist: "} + std::string(to_string(config.video_mode)),
            std::string{"Keys: "} + std::to_string(config.key_count) + "K",
            std::string{"Difficulties: "} + autochart_difficulty_label(config.difficulties),
            std::string{"Variable tempo: "} + on_off(config.variable_tempo),
            std::string{"Back"},
        };
        const auto selected = browse_choices(
            menu,
            "AUTOCHART  //  CONFIGURE",
            choices,
            "ENTER change/start   Accurate + ML Auto is recommended",
            selected_row
        );
        if (!selected.has_value() || *selected == choices.size() - 1U) {
            return false;
        }
        selected_row = *selected;
        switch (*selected) {
        case 0U:
            return true;
        case 1U:
            config.mode = config.mode == AutoChartMode::fast
                ? AutoChartMode::accurate
                : config.mode == AutoChartMode::accurate
                    ? AutoChartMode::maximum
                    : AutoChartMode::fast;
            break;
        case 2U:
            config.ml_mode = config.ml_mode == AutoChartMlMode::automatic
                ? AutoChartMlMode::off
                : config.ml_mode == AutoChartMlMode::off
                    ? AutoChartMlMode::on
                    : AutoChartMlMode::automatic;
            break;
        case 3U:
            config.video_mode = config.video_mode == AutoChartVideoMode::automatic
                ? AutoChartVideoMode::off
                : config.video_mode == AutoChartVideoMode::off
                    ? AutoChartVideoMode::on
                    : AutoChartVideoMode::automatic;
            break;
        case 4U: {
            std::vector<std::string> key_choices;
            key_choices.reserve(maximum_supported_key_count);
            std::size_t initial = 0U;
            for (std::uint16_t keys = 1U; keys <= maximum_supported_key_count; ++keys) {
                key_choices.push_back(std::to_string(keys) + "K");
                if (keys == config.key_count) {
                    initial = static_cast<std::size_t>(keys - 1U);
                }
            }
            const auto picked = browse_choices(
                menu,
                "AUTOCHART  //  KEY MODE",
                key_choices,
                "Select the number of lanes for the generated chart",
                initial
            );
            if (picked.has_value()) {
                config.key_count = static_cast<std::uint16_t>(*picked + 1U);
            }
            break;
        }
        case 5U: {
            const std::array presets{
                std::string{"Expert only"},
                std::string{"Easy + Normal + Hard + Expert"},
                std::string{"All (Easy + Normal + Hard + Expert + Insane)"},
            };
            const auto picked = browse_choices(
                menu,
                "AUTOCHART  //  DIFFICULTIES",
                presets,
                "The expensive audio/ML analysis is shared across all generated difficulties"
            );
            if (picked == 0U) {
                config.difficulties = std::vector<std::string>{"expert"};
            } else if (picked == 1U) {
                config.difficulties = std::vector<std::string>{"easy", "normal", "hard", "expert"};
            } else if (picked == 2U) {
                config.difficulties = std::vector<std::string>{
                    "easy", "normal", "hard", "expert", "insane"
                };
            }
            break;
        }
        case 6U:
            config.variable_tempo = !config.variable_tempo;
            break;
        default:
            break;
        }
    }
}

struct AutoChartGenerationShared final {
    std::mutex mutex;
    AutoChartProgress progress;
    AutoChartResult result;
    std::atomic<bool> done{};
    std::atomic<bool> cancel{};
};

void draw_autochart_progress(
    MenuSession& menu,
    const AutoChartProgress& progress,
    const bool cancellation_requested
) {
    auto* const renderer = menu.renderer();
    const auto palette = palette_for(
        menu.theme(),
        SDL_Color{78, 220, 230, 255}
    );
    draw_menu_background(renderer, SDL_GetTicks(), menu.theme());
    const SDL_FRect panel_rect{145.0F, 165.0F, 990.0F, 385.0F};
    fill_rect(renderer, panel_rect, palette.panel);
    outline_rect(renderer, panel_rect, palette.accent);
    draw_text(
        renderer,
        205.0F,
        220.0F,
        "AUTOCHART  //  ANALYSING MEDIA",
        palette.title,
        1.9F
    );
    draw_text(
        renderer,
        205.0F,
        278.0F,
        std::string{"Stage: "} + std::string(to_string(progress.stage)),
        palette.text,
        1.2F
    );
    std::string detail = progress.detail;
    if (!progress.difficulty.empty()) {
        detail += "  [" + progress.difficulty + ']';
    }
    draw_text(renderer, 205.0F, 318.0F, detail, palette.muted, 1.05F, 105U);
    const double bounded = std::clamp(progress.fraction, 0.0, 1.0);
    fill_rect(renderer, {205.0F, 388.0F, 870.0F, 14.0F}, palette.header);
    fill_rect(
        renderer,
        {205.0F, 388.0F, 870.0F * static_cast<float>(bounded), 14.0F},
        palette.accent
    );
    draw_text(
        renderer,
        205.0F,
        424.0F,
        std::to_string(static_cast<int>(std::lround(bounded * 100.0))) + "%",
        palette.title,
        1.35F
    );
    draw_text(
        renderer,
        205.0F,
        475.0F,
        cancellation_requested
            ? "Cancellation requested - waiting for the current bounded decoder/model step to return..."
            : "ESC cancels safely. The source media is never modified.",
        cancellation_requested ? palette.accent : palette.muted,
        1.0F,
        105U
    );
    SDL_RenderPresent(renderer);
}

[[nodiscard]] AutoChartResult run_autochart_generation_screen(
    MenuSession& menu,
    const std::filesystem::path& media,
    AutoChartOptions options
) {
    AutoChartGenerationShared shared;
    shared.progress = {
        AutoChartProgressStage::validating,
        0.0,
	0.0,
        "Preparing AutoChart worker",
        {},
    };
    menu.publish_presence(
        RuntimeActivityKind::autochart,
        "Preparing AutoChart worker",
        {},
        {},
        0U,
        0.0
    );
    options.progress_callback = [&](const AutoChartProgress& progress) {
        const std::scoped_lock lock(shared.mutex);
        shared.progress = progress;
    };
    options.cancel_requested = [&]() {
        return shared.cancel.load(std::memory_order_acquire);
    };

    std::thread worker([&]() {
        auto result = generate_autochart_mod(media, options);
        {
            const std::scoped_lock lock(shared.mutex);
            shared.result = std::move(result);
        }
        shared.done.store(true, std::memory_order_release);
    });

    while (!shared.done.load(std::memory_order_acquire)) {
        menu.pump_presence();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            SDL_ConvertEventToRenderCoordinates(menu.renderer(), &event);
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                menu.request_close();
                shared.cancel.store(true, std::memory_order_release);
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (menu.handle_audio_action(event.key)) {
                    continue;
                }
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    shared.cancel.store(true, std::memory_order_release);
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                static_cast<void>(menu.handle_audio_action(event.gbutton));
            }
        }
        AutoChartProgress progress;
        {
            const std::scoped_lock lock(shared.mutex);
            progress = shared.progress;
        }
        std::string presence_status = std::string(to_string(progress.stage));
        if (!progress.detail.empty()) {
            presence_status += " - " + progress.detail;
        }
        menu.publish_presence(
            RuntimeActivityKind::autochart,
            std::move(presence_status),
            {},
            progress.difficulty,
            0U,
            std::clamp(progress.fraction, 0.0, 1.0)
        );
        draw_autochart_progress(
            menu,
            progress,
            shared.cancel.load(std::memory_order_acquire)
        );
        SDL_Delay(8U);
    }
    worker.join();
    const std::scoped_lock lock(shared.mutex);
    return std::move(shared.result);
}

[[nodiscard]] std::optional<std::size_t> choose_generated_difficulty(
    MenuSession& menu,
    const AutoChartResult& generated,
    const std::string_view title
) {
    if (generated.difficulties.empty()) {
        return std::nullopt;
    }
    if (generated.difficulties.size() == 1U) {
        return 0U;
    }
    std::vector<std::string> choices;
    choices.reserve(generated.difficulties.size());
    for (const auto& difficulty : generated.difficulties) {
        choices.push_back(
            difficulty.difficulty + "  //  " + std::to_string(difficulty.note_count)
            + " notes  //  review "
            + std::to_string(difficulty.high_priority_review_count) + " high"
        );
    }
    return browse_choices(menu, title, choices);
}

[[nodiscard]] EditorUiOutcome review_generated_autochart(
    MenuSession& menu,
    const AutoChartResult& generated,
    const AppLaunchOptions& base_options
) {
    const auto difficulty_index = choose_generated_difficulty(
        menu,
        generated,
        "AUTOCHART  //  REVIEW DIFFICULTY"
    );
    if (!difficulty_index.has_value()) {
        return {};
    }
    const auto& difficulty = generated.difficulties[*difficulty_index];
    auto load_options = base_options.chart_options;
    load_options.difficulty = difficulty.difficulty;
    load_options.difficulty_explicit = true;
    const auto loaded = ChartLoader::load(difficulty.chart_path, load_options);
    if (!loaded) {
        return {
            EditorUiExit::closed,
            false,
            false,
            "Generated chart could not be opened for review: " + loaded.error,
        };
    }

    try {
        EditorStorage storage(generated.mod_root);
        if (!storage.ready()) {
            return {
                EditorUiExit::closed,
                false,
                false,
                "Generated mod review storage is unavailable: "
                    + storage.initialization_error(),
            };
        }
        ChartEditor editor(std::move(*loaded.chart));
        const auto stem = editor_slug(difficulty.difficulty);
        ChartEditorUiOptions ui;
        ui.storage = &storage;
        ui.project_path = std::filesystem::path{"review/editor"}
            / (stem + ".pfchart.json");
        std::error_code relative_error;
        ui.psych_chart_path = std::filesystem::relative(
            difficulty.chart_path,
            generated.mod_root,
            relative_error
        );
        if (relative_error || ui.psych_chart_path.empty()
            || ui.psych_chart_path.is_absolute()) {
            return {
                EditorUiExit::closed,
                false,
                false,
                "Generated chart path cannot be made relative to its staged mod",
            };
        }
        ui.autosave_path = std::filesystem::path{"review/editor"}
            / (stem + ".autosave.pfchart.json");
        ui.audio_manifest.instrumental = generated.audio_path;
        ui.audio_settings = base_options.settings.audio;
        ui.audio_search_roots.push_back(generated.mod_root);
        ui.choice_discovery_roots.push_back(generated.mod_root);
        for (const auto& root : base_options.content_roots) {
            if (!root.empty()
                && std::find(
                       ui.choice_discovery_roots.begin(),
                       ui.choice_discovery_roots.end(),
                       root
                   ) == ui.choice_discovery_roots.end()) {
                ui.choice_discovery_roots.push_back(root);
            }
        }
        ui.autochart_review_index_path = generated.review_index_path;
        ui.autochart_review_difficulty = difficulty.difficulty;
        ui.autochart_review_overlay = true;
        ui.autochart_review_queue_limit = 5'000U;
        return run_chart_editor_ui(
            menu.window(),
            menu.renderer(),
            editor,
            ui
        );
    } catch (const std::exception& exception) {
        return {
            EditorUiExit::closed,
            false,
            false,
            std::string{"AutoChart review editor failed: "} + exception.what(),
        };
    }
}

[[nodiscard]] int preview_generated_autochart(
    MenuSession& menu,
    const AutoChartResult& generated,
    const AppLaunchOptions& base_options
) {
    const auto difficulty_index = choose_generated_difficulty(
        menu,
        generated,
        "AUTOCHART  //  PREVIEW DIFFICULTY"
    );
    if (!difficulty_index.has_value()) {
        return runner_return_to_launcher;
    }
    const auto& difficulty = generated.difficulties[*difficulty_index];
    auto preview = base_options;
    preview.chart_path = difficulty.chart_path;
    preview.chart_options.difficulty = difficulty.difficulty;
    preview.chart_options.difficulty_explicit = true;
    preview.catalog_song.reset();
    preview.instrumental_override = generated.audio_path;
    preview.vocal_overrides.clear();
    preview.selected_content_root = generated.mod_root;
    preview.selected_mod_root = generated.mod_root;
    preview.show_launcher = false;
    preview.return_to_launcher = true;
    if (std::find(
            preview.content_roots.begin(),
            preview.content_roots.end(),
            generated.mod_root
        ) == preview.content_roots.end()) {
        preview.content_roots.insert(preview.content_roots.begin(), generated.mod_root);
    }

    menu.suspend_music();
    show_loading_screen(
        menu,
        "AUTOCHART  //  PREVIEW",
        difficulty.difficulty + "  //  loading staged chart"
    );
    auto platform = menu.release_platform();
    TransferredPlatform returned_platform;
    auto gameplay = make_gameplay_application(
        std::move(preview),
        std::move(platform),
        &returned_platform,
        menu.discord_session()
    );
    const int result = gameplay->run();
    gameplay.reset();
    if (!menu.adopt_platform(std::move(returned_platform))) {
        return EXIT_FAILURE;
    }
    return result;
}

[[nodiscard]] AutoChartWorkflowResult run_autochart_workflow(
    MenuSession& menu,
    const AppLaunchOptions& base_options
) {
    AutoChartWorkflowResult workflow;
    std::string dialog_error;
    const auto media = choose_autochart_media(menu, dialog_error);
    if (!media.has_value()) {
        if (!dialog_error.empty()) {
            const std::array<std::string, 1> back{"Back"};
            static_cast<void>(browse_choices(
                menu,
                "AUTOCHART  //  MEDIA ERROR",
                back,
                dialog_error
            ));
        }
        return workflow;
    }

    AutoChartGuiConfig config;
    if (!configure_autochart_gui(menu, config)) {
        return workflow;
    }

    const auto mods_root = choose_mods_root(base_options);
    const auto staging_parent = mods_root / ".autochart-staging";
    std::error_code directory_error;
    std::filesystem::create_directories(staging_parent, directory_error);
    if (directory_error) {
        const std::array<std::string, 1> back{"Back"};
        static_cast<void>(browse_choices(
            menu,
            "AUTOCHART  //  STAGING ERROR",
            back,
            "Could not create .autochart-staging: " + directory_error.message()
        ));
        return workflow;
    }
    const auto staging = staging_parent / editor_slug(path_as_utf8(media->stem()));
    std::error_code exists_error;
    if (std::filesystem::exists(staging, exists_error) && !exists_error) {
        std::string discard_error;
        if (!discard_autochart_staging(staging, &discard_error)) {
            const std::array<std::string, 1> back{"Back"};
            static_cast<void>(browse_choices(
                menu,
                "AUTOCHART  //  STAGING ERROR",
                back,
                discard_error
            ));
            return workflow;
        }
    } else if (exists_error) {
        const std::array<std::string, 1> back{"Back"};
        static_cast<void>(browse_choices(
            menu,
            "AUTOCHART  //  STAGING ERROR",
            back,
            "Could not inspect AutoChart staging path: " + exists_error.message()
        ));
        return workflow;
    }

    AutoChartOptions generate_options;
    generate_options.mode = config.mode;
    generate_options.ml_mode = config.ml_mode;
    generate_options.video_mode = config.video_mode;
    generate_options.key_count = config.key_count;
    generate_options.difficulties = config.difficulties;
    generate_options.variable_tempo = config.variable_tempo;
    generate_options.output_root = staging;
    generate_options.mods_root = mods_root;
    generate_options.add_to_mods = false;
    generate_options.overwrite = true;
    generate_options.review_artifacts = true;
    generate_options.review_queue_limit = 5'000U;

    menu.suspend_music();
    auto generated = run_autochart_generation_screen(
        menu,
        *media,
        std::move(generate_options)
    );
    if (menu.close_requested()) {
        workflow.quit_requested = true;
        return workflow;
    }
    menu.resume_music();

    if (!generated.ok) {
        std::string discard_error;
        static_cast<void>(discard_autochart_staging(staging, &discard_error));
        const std::array<std::string, 1> back{"Back"};
        static_cast<void>(browse_choices(
            menu,
            generated.cancelled
                ? "AUTOCHART  //  CANCELLED"
                : "AUTOCHART  //  GENERATION FAILED",
            back,
            generated.cancelled
                ? "Generation was cancelled safely; staged output was discarded."
                : generated.error
        ));
        return workflow;
    }

    std::size_t selected_row = 0U;
    while (true) {
        const std::array actions{
            std::string{"Review / edit generated chart"},
            std::string{"Preview generated chart"},
            std::string{"Accept and add to Mods"},
            std::string{"Discard generated chart"},
            std::string{"Back (keep staged result)"},
        };
        std::ostringstream footer;
        footer << std::fixed << std::setprecision(2)
               << "BPM " << generated.detected_bpm
               << "  //  " << generated.difficulties.size() << " difficulty(s)"
               << "  //  review high " << generated.high_priority_review_count
               << "  //  ML " << (generated.ml_used ? "used" : "fallback/off");
        const auto selected = browse_choices(
            menu,
            "AUTOCHART  //  GENERATED",
            actions,
            footer.str(),
            selected_row
        );
        if (!selected.has_value() || *selected == 4U) {
            return workflow;
        }
        selected_row = *selected;
        if (*selected == 0U) {
            menu.suspend_music();
            auto outcome = review_generated_autochart(
                menu,
                generated,
                base_options
            );
            if (outcome.exit == EditorUiExit::quit_requested) {
                workflow.quit_requested = true;
                return workflow;
            }
            if (!play_return_transition(menu, base_options)) {
                workflow.quit_requested = true;
                return workflow;
            }
            menu.resume_music();
            show_editor_outcome(menu, "AUTOCHART REVIEW", outcome);
        } else if (*selected == 1U) {
            const int preview = preview_generated_autochart(
                menu,
                generated,
                base_options
            );
            if (preview == EXIT_FAILURE || preview == runner_chart_load_failed) {
                menu.resume_music();
                const std::array<std::string, 1> back{"Back"};
                static_cast<void>(browse_choices(
                    menu,
                    "AUTOCHART  //  PREVIEW FAILED",
                    back,
                    "The staged chart could not be previewed; the generated files are still intact."
                ));
            } else if (preview == runner_quit_engine) {
                workflow.quit_requested = true;
                return workflow;
            } else {
                if (!play_return_transition(menu, base_options)) {
                    workflow.quit_requested = true;
                    return workflow;
                }
                menu.resume_music();
            }
        } else if (*selected == 2U) {
            const auto install_target = mods_root / generated.mod_root.filename();
            bool overwrite = false;
            std::error_code exists_error2;
            if (std::filesystem::exists(install_target, exists_error2)
                && !exists_error2) {
                const std::array replace{
                    std::string{"Replace existing mod"},
                    std::string{"Cancel"},
                };
                const auto choice = browse_choices(
                    menu,
                    "AUTOCHART  //  MOD ALREADY EXISTS",
                    replace,
                    "Replacement is atomic and uses a rollback directory until modsList.txt is updated"
                );
                if (choice != 0U) {
                    continue;
                }
                overwrite = true;
            } else if (exists_error2) {
                const std::array<std::string, 1> back{"Back"};
                static_cast<void>(browse_choices(
                    menu,
                    "AUTOCHART  //  INSTALL ERROR",
                    back,
                    "Could not inspect the destination mod: " + exists_error2.message()
                ));
                continue;
            }
            const auto installed = install_autochart_mod(
                generated.mod_root,
                mods_root,
                overwrite
            );
            if (!installed.ok) {
                const std::array<std::string, 1> back{"Back"};
                static_cast<void>(browse_choices(
                    menu,
                    "AUTOCHART  //  INSTALL FAILED",
                    back,
                    installed.error
                ));
                continue;
            }
            workflow.content_changed = true;
            const std::array<std::string, 1> done{"Return to Editors"};
            static_cast<void>(browse_choices(
                menu,
                "AUTOCHART  //  INSTALLED",
                done,
                "The generated mod is enabled in modsList.txt and will be visible after the catalog refresh."
            ));
            return workflow;
        } else if (*selected == 3U) {
            std::string discard_error;
            if (!discard_autochart_staging(generated.mod_root, &discard_error)) {
                const std::array<std::string, 1> back{"Back"};
                static_cast<void>(browse_choices(
                    menu,
                    "AUTOCHART  //  DISCARD FAILED",
                    back,
                    discard_error
                ));
                continue;
            }
            const std::array<std::string, 1> done{"Return to Editors"};
            static_cast<void>(browse_choices(
                menu,
                "AUTOCHART  //  DISCARDED",
                done,
                "The staged AutoChart was removed. The original media file was not changed."
            ));
            return workflow;
        }
    }
}

[[nodiscard]] EditorUiOutcome run_new_chart_editor(
    MenuSession& menu,
    const EditorStorage& storage,
    const AppLaunchOptions& options
) {
    const auto chart_path = available_editor_path(
        storage,
        "charts/untitled",
        "untitled",
        ".json"
    );
    const auto stem = chart_path.stem().string();
    Chart chart;
    chart.title = "Untitled";
    chart.source_format = ChartFormat::psych;
    chart.player_character = "bf";
    chart.opponent_character = "dad";
    chart.girlfriend_character = "gf";
    chart.tempos.push_back({0.0, 120.0, 4U, 4U});
    ChartEditor editor(std::move(chart));
    ChartEditorUiOptions ui;
    ui.storage = &storage;
    ui.project_path = std::filesystem::path{"projects"}
        / (stem + ".pfchart.json");
    ui.psych_chart_path = chart_path;
    ui.autosave_path = std::filesystem::path{"autosaves"}
        / (stem + ".autosave.pfchart.json");
    ui.choice_discovery_roots = options.content_roots;
    return run_chart_editor_ui(
        menu.window(),
        menu.renderer(),
        editor,
        ui
    );
}

[[nodiscard]] EditorUiOutcome run_existing_chart_editor(
    MenuSession& menu,
    const EditorStorage& storage,
    const SongCatalogEntry& entry,
    const AppLaunchOptions& options
) {
    auto load_options = options.chart_options;
    load_options.difficulty = entry.difficulty;
    load_options.difficulty_explicit = true;
    if (entry.metadata_path.has_value()) {
        load_options.metadata_path = entry.metadata_path;
    }
    std::vector<std::string> scripts;
    for (const auto& script : entry.script_paths) {
        if (entry.mod_root.empty()) {
            continue;
        }
        const auto relative = script.lexically_relative(entry.mod_root);
        if (relative.empty() || relative.is_absolute()) {
            continue;
        }
        const auto first = relative.begin();
        if (first != relative.end() && *first == std::filesystem::path{".."}) {
            continue;
        }
        scripts.push_back(path_as_utf8(relative));
    }
    const auto configure_editor_audio = [&](ChartEditorUiOptions& ui,
                                            AudioManifest manifest) {
        ui.audio_manifest = std::move(manifest);
        ui.audio_settings = options.settings.audio;
        const auto append_root = [&](const std::filesystem::path& root) {
            if (root.empty()
                || std::find(
                    ui.audio_search_roots.begin(),
                    ui.audio_search_roots.end(),
                    root
                ) != ui.audio_search_roots.end()) {
                return;
            }
            ui.audio_search_roots.push_back(root);
        };
        append_root(entry.chart_path.parent_path());
        append_root(entry.content_root);
        append_root(entry.mod_root);
        for (const auto& root : options.content_roots) {
            append_root(root);
        }
    };
    auto loaded = ChartLoader::load(entry.chart_path, load_options);
    if (!loaded) {
        std::error_code size_error;
        const auto source_bytes = std::filesystem::file_size(
            entry.chart_path,
            size_error
        );
        const bool requires_streaming = (!size_error
                && source_bytes > maximum_chart_json_bytes)
            || loaded.error.find("5000000") != std::string::npos
            || loaded.error.find("too many notes") != std::string::npos
            || loaded.error.find("note limit") != std::string::npos
            || loaded.error.find("configured safety limits")
                != std::string::npos;
        if (!requires_streaming) {
            return {EditorUiExit::closed, false, false, loaded.error};
        }

        StreamingChartCacheOptions cache_options;
        cache_options.difficulty = entry.difficulty;
        cache_options.difficulty_explicit = true;
        auto cached = prepare_streaming_chart_cache(
            entry.chart_path,
            cache_options
        );
        if (!cached) {
            return {
                EditorUiExit::closed,
                false,
                false,
                "Materialized editor rejected the chart: " + loaded.error
                    + "\nStreaming editor cache failed: " + cached.error,
            };
        }

        try {
            const auto song = editor_slug(entry.song_id);
            const auto difficulty = editor_slug(entry.difficulty);
            const auto display_stem = difficulty == "normal"
                ? song
                : song + '-' + difficulty;
            auto editor_audio = cached.chart_metadata.audio;
            cached.chart_metadata.audio = editor_logical_audio_manifest(
                editor_audio,
                entry
            );
            const auto source_identity = hexadecimal_u64(
                cached.source_fingerprint
            );
            const auto file_stem = display_stem + '-' + source_identity;
            StreamingChartEditor editor(
                std::move(*cached.reader),
                std::move(cached.chart_metadata),
                entry.chart_path,
                cached.content_end_us,
                scripts,
                cached.source_fingerprint
            );
            ChartEditorUiOptions ui;
            ui.storage = &storage;
            ui.project_path = std::filesystem::path{"projects"}
                / (file_stem + ".pfpatch.json");
            ui.psych_chart_path = std::filesystem::path{"charts"}
                / song / (file_stem + ".json");
            configure_editor_audio(ui, std::move(editor_audio));
            const auto append_choice_root = [&](const std::filesystem::path& root) {
                if (!root.empty()
                    && std::find(
                           ui.choice_discovery_roots.begin(),
                           ui.choice_discovery_roots.end(),
                           root
                       ) == ui.choice_discovery_roots.end()) {
                    ui.choice_discovery_roots.push_back(root);
                }
            };
            append_choice_root(entry.content_root);
            append_choice_root(entry.mod_root);
            for (const auto& root : options.content_roots) {
                append_choice_root(root);
            }
            std::error_code patch_error;
            const bool patch_exists = std::filesystem::is_regular_file(
                    storage.root() / ui.project_path,
                    patch_error
                );
            if (patch_error) {
                return {
                    EditorUiExit::closed,
                    false,
                    false,
                    "Existing streaming patch could not be inspected: "
                        + patch_error.message(),
                };
            }
            if (patch_exists) {
                const auto patch = editor.load_patch(
                    storage,
                    ui.project_path
                );
                if (!patch) {
                    return {
                        EditorUiExit::closed,
                        false,
                        false,
                        "Existing streaming patch could not be loaded: "
                            + patch.message,
                    };
                }
            }
            return run_streaming_chart_editor_ui(
                menu.window(),
                menu.renderer(),
                editor,
                ui
            );
        } catch (const std::exception& exception) {
            return {
                EditorUiExit::closed,
                false,
                false,
                std::string{"Streaming editor failed: "} + exception.what(),
            };
        }
    }
    // The editor workspace must stay relocatable. The source paths remain in
    // the catalog and are not rewritten into absolute drive-specific paths.
    try {
        auto editor_audio = loaded.chart->audio;
        loaded.chart->audio = editor_logical_audio_manifest(
            editor_audio,
            entry
        );
        ChartEditor editor(std::move(*loaded.chart), std::move(scripts));
        const auto song = editor_slug(entry.song_id);
        const auto difficulty = editor_slug(entry.difficulty);
        const auto display_stem = difficulty == "normal"
            ? song
            : song + '-' + difficulty;
        const auto file_stem = display_stem + '-'
            + hexadecimal_u64(path_fingerprint(entry.chart_path));
        ChartEditorUiOptions ui;
        ui.storage = &storage;
        ui.project_path = std::filesystem::path{"projects"}
            / (file_stem + ".pfchart.json");
        ui.psych_chart_path = std::filesystem::path{"charts"}
            / song / (file_stem + ".json");
        ui.autosave_path = std::filesystem::path{"autosaves"}
            / (file_stem + ".autosave.pfchart.json");
        configure_editor_audio(ui, std::move(editor_audio));
        // Put the selected pack first so its characters, stages, scripts and
        // note types are immediately available, then merge the common roots.
        // Discovery itself de-duplicates values and remains bounded.
        if (!entry.content_root.empty()) {
            ui.choice_discovery_roots.push_back(entry.content_root);
        }
        if (!entry.mod_root.empty()
            && entry.mod_root != entry.content_root) {
            ui.choice_discovery_roots.push_back(entry.mod_root);
        }
        for (const auto& root : options.content_roots) {
            if (std::find(
                    ui.choice_discovery_roots.begin(),
                    ui.choice_discovery_roots.end(),
                    root
                ) == ui.choice_discovery_roots.end()) {
                ui.choice_discovery_roots.push_back(root);
            }
        }
        return run_chart_editor_ui(
            menu.window(),
            menu.renderer(),
            editor,
            ui
        );
    } catch (const std::exception& exception) {
        return {EditorUiExit::closed, false, false, exception.what()};
    }
}

[[nodiscard]] EditorUiOutcome run_new_character_editor(
    MenuSession& menu,
    const EditorStorage& storage
) {
    const auto path = available_editor_path(
        storage,
        "characters",
        "new-character",
        ".json"
    );
    CharacterDescriptor descriptor;
    descriptor.id = path.stem().string();
    descriptor.image = "characters/" + descriptor.id;
    descriptor.animations.push_back({"idle", "idle", 24, true, {}, {}});
    CharacterEditor editor(std::move(descriptor));
    DescriptorEditorUiOptions ui;
    ui.storage = &storage;
    ui.psych_json_path = path;
    return run_character_editor_ui(
        menu.window(),
        menu.renderer(),
        editor,
        ui
    );
}

[[nodiscard]] EditorUiOutcome run_new_week_editor(
    MenuSession& menu,
    const EditorStorage& storage
) {
    const auto path = available_editor_path(
        storage,
        "weeks",
        "new-week",
        ".json"
    );
    WeekDescriptor descriptor;
    descriptor.id = path.stem().string();
    descriptor.story_name = "New Week";
    descriptor.display_name = "New Week";
    descriptor.songs.push_back({"New Song", "dad", {146, 113, 253}, {}});
    descriptor.difficulties.clear();
    descriptor.difficulties.emplace_back("easy");
    descriptor.difficulties.emplace_back("normal");
    descriptor.difficulties.emplace_back("hard");
    WeekEditor editor(std::move(descriptor));
    DescriptorEditorUiOptions ui;
    ui.storage = &storage;
    ui.psych_json_path = path;
    return run_week_editor_ui(
        menu.window(),
        menu.renderer(),
        editor,
        ui
    );
}

[[nodiscard]] EditorMenuResult show_editors(
    MenuSession& menu,
    const ContentCatalog& catalog,
    const AppLaunchOptions& options
) {
    EditorMenuResult result;
    const auto workspace = choose_mods_root(options) / "pulseforge-created";
    EditorStorage storage(workspace);
    if (!storage.ready()) {
        const std::array<std::string, 1> back{"Back"};
        static_cast<void>(browse_choices(
            menu,
            "EDITOR STORAGE ERROR",
            back,
            storage.initialization_error()
        ));
        return result;
    }

    std::size_t selected_row = 0U;
    while (true) {
        menu.publish_presence(RuntimeActivityKind::editor, "Choosing an editor");
        const std::array choices{
            std::string("Create a new chart"),
            std::string("Edit an installed chart"),
            std::string("AutoChart from audio / video"),
            std::string("Create a character"),
            std::string("Create a week"),
            std::string("Back"),
        };
        const auto selected = browse_choices(
            menu,
            "PULSEFORGE  //  EDITORS",
            choices,
            "Editors reuse this window   Ctrl+S saves atomically   Esc returns",
            selected_row
        );
        if (!selected.has_value() || *selected == choices.size() - 1U) {
            return result;
        }
        selected_row = *selected;
        if (*selected == 2U) {
            const auto autochart = run_autochart_workflow(menu, options);
            if (autochart.quit_requested) {
                result.quit_requested = true;
                return result;
            }
            result.content_changed = result.content_changed
                || autochart.content_changed;
            continue;
        }
        EditorUiOutcome outcome;
        // Editor timelines and descriptor workspaces must remain silent so
        // chart audio and precise editing feedback are never masked.
        menu.suspend_music();
        if (*selected == 0U) {
            outcome = run_new_chart_editor(menu, storage, options);
        } else if (*selected == 1U) {
            const auto chart = browse_catalog(menu, catalog);
            if (!chart.has_value()) {
                menu.resume_music();
                continue;
            }
            show_loading_screen(
                menu,
                "OPENING CHART EDITOR",
                catalog.entries()[*chart].title + "  //  resolving chart and audio"
            );
            outcome = run_existing_chart_editor(
                menu,
                storage,
                catalog.entries()[*chart],
                options
            );
        } else if (*selected == 3U) {
            outcome = run_new_character_editor(menu, storage);
        } else {
            outcome = run_new_week_editor(menu, storage);
        }
        if (outcome.exit == EditorUiExit::quit_requested) {
            result.quit_requested = true;
            return result;
        }
        if (!play_return_transition(menu, options)) {
            result.quit_requested = true;
            return result;
        }
        menu.resume_music();
        result.content_changed = result.content_changed
            || outcome.compatible_json_saved;
        show_editor_outcome(menu, "EDITOR", outcome);
    }
}

[[nodiscard]] std::optional<std::uint32_t> choose_musical_ppqn(
    MenuSession& menu,
    const std::uint32_t initial = 960U
) {
    constexpr std::array<std::uint32_t, 9> values{
        96U, 192U, 240U, 480U, 960U, 1'920U, 3'840U, 9'600U, 15'360U
    };
    std::vector<std::string> choices;
    choices.reserve(values.size());
    std::size_t selected{};
    for (std::size_t index = 0U; index < values.size(); ++index) {
        choices.push_back(std::to_string(values[index]) + " PPQN");
        if (values[index] == initial) selected = index;
    }
    const auto picked = browse_choices(
        menu,
        "MUSICAL CHART  //  PPQN",
        choices,
        "Higher PPQN preserves finer musical subdivisions.",
        selected
    );
    return picked.has_value()
        ? std::optional<std::uint32_t>{values[*picked]}
        : std::nullopt;
}

[[nodiscard]] std::filesystem::path musical_metadata_sidecar(
    const std::filesystem::path& source
) {
    auto filename = path_as_utf8(source.filename());
    const auto lower = lower_ascii(filename);
    constexpr std::string_view pfm_source_suffix{".pfm.json"};
    if (lower.ends_with(pfm_source_suffix)) {
        filename.resize(filename.size() - pfm_source_suffix.size());
        return source.parent_path() / (filename + ".pfmeta.json");
    }
    auto result = source;
    result.replace_extension(".pfmeta.json");
    return result;
}

[[nodiscard]] std::filesystem::path musical_conversion_root(
    const AppLaunchOptions& options
) {
    return choose_mods_root(options)
        / "pulseforge-created" / "musical-conversions";
}

[[nodiscard]] std::filesystem::path available_musical_output(
    const std::filesystem::path& root,
    std::string stem,
    const std::string_view suffix
) {
    stem = editor_slug(stem);
    if (stem.empty()) stem = "chart";
    auto candidate = root / (stem + std::string(suffix));
    for (std::uint32_t copy = 2U;
         std::filesystem::exists(candidate) && copy < 100'000U;
         ++copy) {
        candidate = root
            / (stem + "-" + std::to_string(copy) + std::string(suffix));
    }
    return candidate;
}

void show_musical_tool_message(
    MenuSession& menu,
    const std::string_view title,
    const std::string& message
) {
    const std::array<std::string, 1> back{"Continue"};
    static_cast<void>(browse_choices(menu, title, back, message));
}

void remove_musical_temporary(
    const std::filesystem::path& pfc
) noexcept {
    std::error_code error;
    std::filesystem::remove(pfc, error);
    auto pvd = pfc;
    pvd.replace_extension(".pvd");
    error.clear();
    std::filesystem::remove(pvd, error);
}

[[nodiscard]] bool import_musical_chart_source(
    MenuSession& menu,
    const AppLaunchOptions& options
) {
    constexpr std::array filters{
        SDL_DialogFileFilter{
            "MIDI / PulseForge Musical Charts",
            "mid;midi;pfm;json"
        },
    };
    std::string dialog_error;
    const auto source = choose_source_path(
        menu,
        false,
        filters,
        "IMPORT MIDI / PFM CHART",
        "Select .mid/.midi, .pfm, or a declarative .pfm.json source.",
        dialog_error
    );
    if (!source.has_value()) {
        if (!dialog_error.empty()) {
            show_musical_tool_message(menu, "MUSICAL IMPORT ERROR", dialog_error);
        }
        return false;
    }
    if (!is_midi_chart_path(*source)
        && !is_pfm_chart_path(*source)
        && !is_pfm_source_path(*source)) {
        show_musical_tool_message(
            menu,
            "MUSICAL IMPORT REJECTED",
            "The selected file is not MIDI, PFM, or .pfm.json."
        );
        return false;
    }

    const auto mods_root = choose_mods_root(options);
    auto source_stem = path_as_utf8(source->filename());
    constexpr std::string_view pfm_source_suffix{".pfm.json"};
    if (lower_ascii(source_stem).ends_with(pfm_source_suffix)) {
        source_stem.resize(source_stem.size() - pfm_source_suffix.size());
    } else {
        source_stem = path_as_utf8(source->stem());
    }
    const auto song_slug = editor_slug(source_stem);
    const auto destination_root = mods_root
        / "pulseforge-musical-imports"
        / (song_slug.empty() ? "chart" : song_slug)
        / "charts";
    std::error_code error;
    std::filesystem::create_directories(destination_root, error);
    if (error) {
        show_musical_tool_message(
            menu,
            "MUSICAL IMPORT ERROR",
            "Cannot create the musical-chart import directory."
        );
        return false;
    }
    const auto destination = destination_root / source->filename();
    std::filesystem::copy_file(
        *source,
        destination,
        std::filesystem::copy_options::overwrite_existing,
        error
    );
    if (error) {
        show_musical_tool_message(
            menu,
            "MUSICAL IMPORT ERROR",
            "Cannot copy the selected chart into mods."
        );
        return false;
    }

    const auto sidecar = musical_metadata_sidecar(*source);
    if (std::filesystem::is_regular_file(sidecar, error) && !error) {
        error.clear();
        const auto sidecar_destination = musical_metadata_sidecar(destination);
        std::filesystem::copy_file(
            sidecar,
            sidecar_destination,
            std::filesystem::copy_options::overwrite_existing,
            error
        );
    }
    show_musical_tool_message(
        menu,
        "MUSICAL CHART INSTALLED",
        path_as_utf8(destination)
    );
    return true;
}

[[nodiscard]] bool convert_json_to_midi(
    MenuSession& menu,
    const AppLaunchOptions& options
) {
    constexpr std::array filters{
        SDL_DialogFileFilter{"JSON rhythm charts", "json"},
    };
    std::string dialog_error;
    const auto source = choose_source_path(
        menu, false, filters,
        "CONVERT JSON TO MIDI",
        "Select a materializable JSON chart. Extreme charts should use JSON to PFM.",
        dialog_error
    );
    if (!source.has_value()) return false;
    const auto ppqn = choose_musical_ppqn(menu);
    if (!ppqn.has_value()) return false;

    const auto loaded = ChartLoader::load(*source);
    if (!loaded) {
        show_musical_tool_message(
            menu,
            "JSON TO MIDI FAILED",
            loaded.error
                + "  //  Standard MIDI is event-linear; use JSON to PFM for huge charts."
        );
        return false;
    }
    auto root = musical_conversion_root(options);
    std::error_code fs_error;
    std::filesystem::create_directories(root, fs_error);
    if (fs_error) return false;
    const auto output = available_musical_output(
        root,
        path_as_utf8(source->stem()),
        ".mid"
    );
    MidiChartOptions midi;
    midi.ppqn = *ppqn;
    std::string error;
    if (!export_chart_to_midi(*loaded.chart, output, midi, &error)) {
        show_musical_tool_message(menu, "JSON TO MIDI FAILED", error);
        return false;
    }
    show_musical_tool_message(
        menu,
        "MIDI EXPORTED",
        path_as_utf8(output) + "  //  PPQN " + std::to_string(*ppqn)
    );
    return true;
}

[[nodiscard]] bool convert_json_to_pfm(
    MenuSession& menu,
    const AppLaunchOptions& options
) {
    constexpr std::array filters{
        SDL_DialogFileFilter{"JSON rhythm charts", "json"},
    };
    std::string dialog_error;
    const auto source = choose_source_path(
        menu, false, filters,
        "CONVERT JSON TO PFM",
        "The bounded JSON compiler will preserve detected PatternRuns instead of expanding them.",
        dialog_error
    );
    if (!source.has_value()) return false;
    const auto ppqn = choose_musical_ppqn(menu);
    if (!ppqn.has_value()) return false;

    auto root = musical_conversion_root(options);
    std::error_code fs_error;
    std::filesystem::create_directories(root, fs_error);
    if (fs_error) return false;
    const auto output = available_musical_output(
        root, path_as_utf8(source->stem()), ".pfm"
    );
    auto temporary = output;
    temporary += ".compile.pfc";

    StreamingChartImportOptions import_options;
    const auto compiled = compile_streaming_json_chart_to_pfc(
        *source,
        temporary,
        import_options
    );
    if (!compiled) {
        remove_musical_temporary(temporary);
        show_musical_tool_message(menu, "JSON TO PFM FAILED", compiled.error);
        return false;
    }
    std::string reader_error;
    auto reader = PackedChartReader::open(
        temporary,
        &reader_error,
        import_options.packed_limits
    );
    if (!reader.has_value()) {
        remove_musical_temporary(temporary);
        show_musical_tool_message(menu, "JSON TO PFM FAILED", reader_error);
        return false;
    }
    PfmChartOptions pfm;
    pfm.ppqn = *ppqn;
    std::string error;
    const bool exported = export_packed_chart_to_pfm(
        *reader,
        compiled.chart_metadata,
        output,
        pfm,
        &error
    );
    remove_musical_temporary(temporary);
    if (!exported) {
        show_musical_tool_message(menu, "JSON TO PFM FAILED", error);
        return false;
    }
    show_musical_tool_message(
        menu,
        "PFM EXPORTED",
        path_as_utf8(output) + "  //  logical notes "
            + std::to_string(compiled.logical_note_count)
    );
    return true;
}

[[nodiscard]] bool convert_midi_to_pfm(
    MenuSession& menu,
    const AppLaunchOptions& options
) {
    constexpr std::array filters{
        SDL_DialogFileFilter{"Standard MIDI Files", "mid;midi"},
    };
    std::string dialog_error;
    const auto source = choose_source_path(
        menu, false, filters,
        "CONVERT MIDI TO PFM",
        "MIDI is parsed with PPQN timing and compiled through bounded PFC1 streaming.",
        dialog_error
    );
    if (!source.has_value()) return false;
    const auto ppqn = choose_musical_ppqn(menu);
    if (!ppqn.has_value()) return false;

    auto root = musical_conversion_root(options);
    std::error_code fs_error;
    std::filesystem::create_directories(root, fs_error);
    if (fs_error) return false;
    const auto output = available_musical_output(
        root, path_as_utf8(source->stem()), ".pfm"
    );
    auto temporary = output;
    temporary += ".compile.pfc";

    MidiChartOptions midi;
    const auto compiled = compile_midi_chart_to_pfc(*source, temporary, midi);
    if (!compiled) {
        remove_musical_temporary(temporary);
        show_musical_tool_message(menu, "MIDI TO PFM FAILED", compiled.error);
        return false;
    }
    std::string reader_error;
    auto reader = PackedChartReader::open(temporary, &reader_error);
    if (!reader.has_value()) {
        remove_musical_temporary(temporary);
        show_musical_tool_message(menu, "MIDI TO PFM FAILED", reader_error);
        return false;
    }
    PfmChartOptions pfm;
    pfm.ppqn = *ppqn;
    std::string error;
    const bool exported = export_packed_chart_to_pfm(
        *reader,
        compiled.chart_metadata,
        output,
        pfm,
        &error
    );
    remove_musical_temporary(temporary);
    if (!exported) {
        show_musical_tool_message(menu, "MIDI TO PFM FAILED", error);
        return false;
    }
    show_musical_tool_message(
        menu,
        "PFM EXPORTED",
        path_as_utf8(output)
    );
    return true;
}

[[nodiscard]] bool compile_pfm_source_to_binary(
    MenuSession& menu,
    const AppLaunchOptions& options
) {
    constexpr std::array filters{
        SDL_DialogFileFilter{"PulseForge PFM source JSON", "json"},
    };
    std::string dialog_error;
    const auto source = choose_source_path(
        menu, false, filters,
        "COMPILE PFM SOURCE TO BINARY PFM",
        "A declarative .pfm.json can describe trillions of notes with RUN/REPEAT blocks.",
        dialog_error
    );
    if (!source.has_value()) return false;
    if (!is_pfm_source_path(*source)) {
        show_musical_tool_message(
            menu,
            "PFM SOURCE REJECTED",
            "The selected file must end in .pfm.json."
        );
        return false;
    }
    const auto ppqn = choose_musical_ppqn(menu);
    if (!ppqn.has_value()) return false;
    auto root = musical_conversion_root(options);
    std::error_code fs_error;
    std::filesystem::create_directories(root, fs_error);
    if (fs_error) return false;

    auto source_stem = path_as_utf8(source->filename());
    constexpr std::string_view source_suffix{".pfm.json"};
    if (source_stem.size() > source_suffix.size()) {
        source_stem.resize(source_stem.size() - source_suffix.size());
    }
    const auto output = available_musical_output(root, source_stem, ".pfm");
    auto temporary = output;
    temporary += ".compile.pfc";
    PfmChartOptions pfm;
    pfm.ppqn = *ppqn;
    const auto compiled = compile_pfm_source_to_pfc(*source, temporary, pfm);
    if (!compiled) {
        remove_musical_temporary(temporary);
        show_musical_tool_message(menu, "PFM SOURCE COMPILE FAILED", compiled.error);
        return false;
    }
    std::string reader_error;
    auto reader = PackedChartReader::open(temporary, &reader_error);
    if (!reader.has_value()) {
        remove_musical_temporary(temporary);
        show_musical_tool_message(menu, "PFM SOURCE COMPILE FAILED", reader_error);
        return false;
    }
    std::string error;
    const bool exported = export_packed_chart_to_pfm(
        *reader,
        compiled.chart_metadata,
        output,
        pfm,
        &error
    );
    remove_musical_temporary(temporary);
    if (!exported) {
        show_musical_tool_message(menu, "PFM SOURCE COMPILE FAILED", error);
        return false;
    }
    show_musical_tool_message(
        menu,
        "BINARY PFM CREATED",
        path_as_utf8(output) + "  //  logical notes "
            + std::to_string(compiled.logical_note_count)
    );
    return true;
}

[[nodiscard]] bool create_pfm_trillion_template(
    MenuSession& menu,
    const AppLaunchOptions& options
) {
    const auto ppqn = choose_musical_ppqn(menu);
    if (!ppqn.has_value()) return false;
    auto root = musical_conversion_root(options);
    std::error_code fs_error;
    std::filesystem::create_directories(root, fs_error);
    if (fs_error) return false;
    const auto output = available_musical_output(
        root,
        "trillion-note-template",
        ".pfm.json"
    );
    std::string error;
    if (!write_pfm_source_template(output, *ppqn, &error)) {
        show_musical_tool_message(menu, "PFM TEMPLATE FAILED", error);
        return false;
    }
    show_musical_tool_message(
        menu,
        "PFM SOURCE TEMPLATE CREATED",
        path_as_utf8(output)
            + "  //  contains a one-trillion-note arithmetic RUN"
    );
    return true;
}

[[nodiscard]] bool show_musical_chart_tools(
    MenuSession& menu,
    const AppLaunchOptions& options
) {
    std::size_t selected{};
    while (true) {
        const std::array choices{
            std::string{"Import MIDI / PFM chart into Mods..."},
            std::string{"Convert JSON -> MIDI..."},
            std::string{"Convert JSON -> PFM..."},
            std::string{"Convert MIDI -> PFM..."},
            std::string{"Compile .pfm.json -> binary PFM..."},
            std::string{"Create trillion-note PFM source template..."},
            std::string{"Back"},
        };
        const auto action = browse_choices(
            menu,
            "PULSEFORGE  //  MIDI + PFM CHART TOOLS",
            choices,
            "MIDI = interchange/DAW; PFM = PPQN + varints + constant-storage PatternRuns.",
            selected
        );
        if (!action.has_value() || *action == choices.size() - 1U) {
            return false;
        }
        selected = *action;
        switch (*action) {
        case 0U:
            if (import_musical_chart_source(menu, options)) return true;
            break;
        case 1U:
            if (convert_json_to_midi(menu, options)) return true;
            break;
        case 2U:
            if (convert_json_to_pfm(menu, options)) return true;
            break;
        case 3U:
            if (convert_midi_to_pfm(menu, options)) return true;
            break;
        case 4U:
            if (compile_pfm_source_to_binary(menu, options)) return true;
            break;
        case 5U:
            if (create_pfm_trillion_template(menu, options)) return true;
            break;
        default:
            break;
        }
    }
}

[[nodiscard]] bool show_mod_browser(
    MenuSession& menu,
    const ContentCatalog& catalog,
    const AppLaunchOptions& options
) {
    std::size_t selected_row = 0U;
    while (true) {
        menu.publish_presence(
            RuntimeActivityKind::mods,
            std::to_string(catalog.mods().size()) + " installed mods"
        );
        std::vector<std::string> choices;
        choices.reserve(catalog.mods().size() + 6U);
        for (const auto& mod : catalog.mods()) {
            const auto chart_count = static_cast<std::size_t>(std::count_if(
                catalog.entries().begin(),
                catalog.entries().end(),
                [&](const SongCatalogEntry& entry) { return entry.mod_id == mod.id; }
            ));
            choices.push_back(
                std::string(mod.enabled ? "[ENABLED] " : "[DISABLED] ")
                + mod.name + "  //  " + std::string(to_string(mod.profile))
                + "  //  " + std::to_string(chart_count) + " charts"
            );
        }
        const auto mod_count = choices.size();
        choices.emplace_back("Install a mod archive (ZIP / 7z / RAR / TAR)...");
        choices.emplace_back("Install an unpacked mod folder...");
        choices.emplace_back("Install an FLP chart (SNIFF-RUSTED)...");
        choices.emplace_back("MIDI / PFM chart tools...");
        choices.emplace_back("Rescan installed content");
        choices.emplace_back("Back");
        const auto selected = browse_choices(
            menu,
            "PULSEFORGE  //  MOD MANAGER",
            choices,
            "ENTER action   MIDI/PFM tools support PPQN and constant-storage PatternRuns",
            selected_row
        );
        if (!selected.has_value() || *selected == choices.size() - 1U) {
            return false;
        }
        selected_row = *selected;
        if (*selected < mod_count) {
            const auto& mod = catalog.mods()[*selected];
            const std::array<std::string, 1> back{"Back to installed mods"};
            static_cast<void>(browse_choices(
                menu,
                std::string{"PULSEFORGE  //  MODS  //  "} + mod.name,
                back,
                std::string(mod.enabled ? "Enabled" : "Disabled")
                    + "  //  " + mod.root.string()
            ));
            continue;
        }
        if (*selected == mod_count + 4U) {
            return true;
        }
        if (*selected == mod_count + 3U) {
            if (show_musical_chart_tools(menu, options)) {
                return true;
            }
            continue;
        }
        if (*selected == mod_count + 2U) {
            const auto imported = install_flp_chart(menu, options);
            if (imported.installed) {
                const std::array<std::string, 1> back{"Continue"};
                static_cast<void>(browse_choices(
                    menu,
                    "FLP CHART INSTALLED",
                    back,
                    imported.message
                ));
                return true;
            }
            if (!imported.message.empty()) {
                const std::array<std::string, 1> back{"Back"};
                static_cast<void>(browse_choices(
                    menu,
                    "FLP IMPORT NOT COMPLETED",
                    back,
                    imported.message
                ));
            }
            continue;
        }
        std::string dialog_error;
        const bool directory = *selected == mod_count + 1U;
        const auto source = choose_mod_source(menu, directory, dialog_error);
        if (!source.has_value()) {
            if (!dialog_error.empty()) {
                const std::array<std::string, 1> back{"Back"};
                static_cast<void>(browse_choices(
                    menu,
                    "MOD INSTALLATION ERROR",
                    back,
                    dialog_error
                ));
            }
            continue;
        }
        const auto installed = install_mod(
            *source,
            choose_mods_root(options)
        );
        const std::array<std::string, 1> back{"Continue"};
        if (!installed) {
            static_cast<void>(browse_choices(
                menu,
                "MOD INSTALLATION REJECTED",
                back,
                installed.error
            ));
            continue;
        }
        static_cast<void>(browse_choices(
            menu,
            "MOD INSTALLED",
            back,
            installed.mod_id + "  //  "
                + std::to_string(installed.installed_files) + " files  //  "
                + format_bytes(installed.installed_bytes)
        ));
        return true;
    }
}

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const noexcept {
        SDL_DestroyTexture(texture);
    }
};

using TexturePtr = std::unique_ptr<SDL_Texture, TextureDeleter>;

struct CreditProfileTexture {
    TexturePtr texture;
    float width{};
    float height{};
};

struct CreditEntry {
    std::string name;
    std::string detail;
    std::filesystem::path profile_name;
    std::string url;
    bool selectable{};
};

[[nodiscard]] std::filesystem::path resolve_credit_profile(
    const AppLaunchOptions& options,
    const std::filesystem::path& filename
) {
    std::error_code error;
    for (const auto& root : options.content_roots) {
        const auto candidate = root / "credits/profiles" / filename;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate;
        }
        error.clear();
    }
    return {};
}

[[nodiscard]] CreditProfileTexture load_credit_profile(
    SDL_Renderer* renderer,
    const std::filesystem::path& path
) {
    CreditProfileTexture result;
    constexpr std::uintmax_t maximum_profile_bytes = 8U * 1024U * 1024U;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return result;
    }
    const auto end = input.tellg();
    if (end <= std::streampos{0}
        || static_cast<std::uintmax_t>(end) > maximum_profile_bytes
        || static_cast<std::uintmax_t>(end)
            > static_cast<std::uintmax_t>(std::numeric_limits<int>::max())) {
        return result;
    }
    std::vector<stbi_uc> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    if (!input) {
        return result;
    }
    int width{};
    int height{};
    int components{};
    stbi_uc* pixels = stbi_load_from_memory(
        bytes.data(),
        static_cast<int>(bytes.size()),
        &width,
        &height,
        &components,
        STBI_rgb_alpha
    );
    if (pixels == nullptr || width <= 0 || height <= 0
        || width > 4'096 || height > 4'096) {
        stbi_image_free(pixels);
        return result;
    }
    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        width,
        height,
        SDL_PIXELFORMAT_RGBA32,
        pixels,
        width * 4
    );
    SDL_Texture* texture = surface != nullptr
        ? SDL_CreateTextureFromSurface(renderer, surface)
        : nullptr;
    SDL_DestroySurface(surface);
    stbi_image_free(pixels);
    if (texture == nullptr) {
        return result;
    }
    static_cast<void>(SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR));
    result.texture.reset(texture);
    result.width = static_cast<float>(width);
    result.height = static_cast<float>(height);
    return result;
}

void show_credits(
    MenuSession& menu,
    const AppLaunchOptions& options
) {
    const std::vector<CreditEntry> credits{
        {"PULSEFORGE CREATORS", "Profiles supplied by the creators", {}, {}, false},
        {"Shash / Shashangabush", "Creator",
         "shashangabush.png", {}, true},
        {"That One Brudda From The Booshes", "Creator // Shash alias",
         {}, {}, true},
        {"xX D3T0X L1NT3R Xx", "Creator", "xxdet0xl1nt3rxx.png", {}, true},
        {"Xeno Philer", "Creator", "xeno_philer.png", {}, true},
        {"Ph3n0 Typ3", "Creator", "ph3n0_typ3.png", {}, true},
        {"Codex / OpenAI", "Engineering assistance", {}, {}, false},
        {"pulseforge-cli.exe",
         "Console tools: validate, benchmark, PFC, render and CI; not another game build",
         {}, {}, false},
        {"REFERENCE PROJECTS", "ENTER opens a confirmed HTTPS project page", {}, {}, false},
        {"Funkin Crew", "Friday Night Funkin'",
         {}, "https://github.com/FunkinCrew/funkin", true},
        {"ShadowMario", "FNF Psych Engine",
         {}, "https://github.com/ShadowMario/FNF-PsychEngine", true},
        {"Psych-Slice", "P-Slice",
         {}, "https://github.com/Psych-Slice/P-Slice", true},
        {"DendyGG / HRK-EXEX", "H-Slice",
         {}, "https://github.com/DendyGG/H-Slice", true},
        {"HaxePixel / HRK-EXEX", "H-Slice",
         {}, "https://github.com/HaxePixel/H-Slice", true},
        {"JordanSantiagoYT", "FNF JS Engine",
         {}, "https://github.com/JordanSantiagoYT/FNF-JS-Engine", true},
        {"acc0untz0138 / justAMZ", "DenpaEx",
         {}, "https://github.com/acc0untz0138/DenpaEx", true},
        {"JordanSantiagoYT", "Mods for JS Engine",
         {}, "https://github.com/JordanSantiagoYT/Mods-for-JS-Engine", true},
        {"TECHNOLOGY", "C++20, SDL, Lua, JSON, GLSL, FFmpeg, CMake, Haxe/HScript heritage",
         {}, {}, false},
        {"FULL ATTRIBUTION", "docs/CREDITS.md contains contributors and licenses",
         {}, {}, false},
    };
    std::vector<CreditProfileTexture> profiles(credits.size());
    for (std::size_t index = 0U; index < credits.size(); ++index) {
        if (!credits[index].profile_name.empty()) {
            profiles[index] = load_credit_profile(
                menu.renderer(),
                resolve_credit_profile(options, credits[index].profile_name)
            );
        }
    }

    const auto next_selectable = [&credits](
        const std::size_t current,
        const int direction
    ) {
        auto candidate = current;
        for (std::size_t count = 0U; count < credits.size(); ++count) {
            if (direction < 0) {
                candidate = candidate == 0U ? credits.size() - 1U : candidate - 1U;
            } else {
                candidate = (candidate + 1U) % credits.size();
            }
            if (credits[candidate].selectable) {
                return candidate;
            }
        }
        return current;
    };

    std::size_t selected = 1U;
    std::string status = "Creators without a confirmed public profile are intentionally not linked";
    bool running = true;
    menu.set_title("PulseForge - Credits");
    while (running) {
        menu.update_music();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            SDL_ConvertEventToRenderCoordinates(menu.renderer(), &event);
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                menu.request_close();
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (menu.handle_audio_action(event.key)) {
                    continue;
                }
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    running = false;
                } else if (event.key.scancode == SDL_SCANCODE_UP) {
                    selected = next_selectable(selected, -1);
                } else if (event.key.scancode == SDL_SCANCODE_DOWN) {
                    selected = next_selectable(selected, 1);
                } else if (event.key.scancode == SDL_SCANCODE_RETURN
                    || event.key.scancode == SDL_SCANCODE_KP_ENTER) {
                    const auto& url = credits[selected].url;
                    if (url.empty()) {
                        status = "No verified public URL for " + credits[selected].name;
                    } else if (!url.starts_with("https://")) {
                        status = "Blocked a non-HTTPS credit URL";
                    } else if (!SDL_OpenURL(url.c_str())) {
                        status = "Could not open the browser: "
                            + std::string(SDL_GetError());
                    } else {
                        status = "Opened " + credits[selected].name
                            + "; Credits remains open";
                    }
                }
            } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                static_cast<void>(menu.handle_audio_action(event.gbutton));
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                && event.button.button == SDL_BUTTON_LEFT) {
                const auto range = menu_visible_range(
                    credits.size(),
                    selected,
                    8U
                );
                if (event.button.x >= 120.0F && event.button.x <= 1'160.0F
                    && event.button.y >= 134.0F) {
                    const auto row = static_cast<std::size_t>(
                        (event.button.y - 134.0F) / 64.0F
                    );
                    const auto index = range.first + row;
                    if (row < range.last - range.first
                        && index < credits.size()
                        && credits[index].selectable) {
                        selected = index;
                    }
                }
            }
            if (!running) {
                break;
            }
        }

        const auto ticks = SDL_GetTicks();
        const auto palette = palette_for(menu.theme());
        draw_menu_background(menu.renderer(), ticks, menu.theme());
        draw_text(
            menu.renderer(),
            44.0F,
            30.0F,
            "PULSEFORGE  //  CREDITS",
            palette.title,
            2.1F
        );
        draw_text(
            menu.renderer(),
            44.0F,
            73.0F,
            "UP/DOWN select   ENTER open verified link   ESC back",
            palette.muted,
            1.1F
        );
        fill_rect(menu.renderer(), {100.0F, 118.0F, 1'080.0F, 536.0F}, palette.panel);
        outline_rect(
            menu.renderer(),
            {100.0F, 118.0F, 1'080.0F, 536.0F},
            {palette.accent.r, palette.accent.g, palette.accent.b, 105}
        );
        const auto range = menu_visible_range(credits.size(), selected, 8U);
        float y = 134.0F;
        for (std::size_t index = range.first; index < range.last; ++index) {
            const auto& credit = credits[index];
            const bool active = index == selected;
            if (active) {
                fill_rect(
                    menu.renderer(),
                    {120.0F, y, 1'040.0F, 57.0F},
                    palette.selected
                );
                outline_rect(
                    menu.renderer(),
                    {120.0F, y, 1'040.0F, 57.0F},
                    palette.accent
                );
            }
            const auto& profile = profiles[index];
            if (profile.texture) {
                constexpr float box = 48.0F;
                const float scale = std::min(
                    box / profile.width,
                    box / profile.height
                );
                const SDL_FRect destination{
                    130.0F + (box - profile.width * scale) * 0.5F,
                    y + 4.0F + (box - profile.height * scale) * 0.5F,
                    profile.width * scale,
                    profile.height * scale,
                };
                static_cast<void>(SDL_RenderTexture(
                    menu.renderer(),
                    profile.texture.get(),
                    nullptr,
                    &destination
                ));
            } else if (credit.selectable) {
                outline_rect(
                    menu.renderer(),
                    {130.0F, y + 4.0F, 48.0F, 48.0F},
                    palette.muted
                );
            }
            draw_text(
                menu.renderer(),
                198.0F,
                y + 13.0F,
                credit.name,
                active ? palette.title : palette.text,
                credit.selectable ? 1.15F : 1.0F
            );
            draw_text(
                menu.renderer(),
                590.0F,
                y + 15.0F,
                printable_text(credit.detail, 70U),
                credit.url.empty() ? palette.muted : palette.accent,
                0.9F
            );
            y += 64.0F;
        }
        draw_text(
            menu.renderer(),
            112.0F,
            681.0F,
            printable_text(status, 116U),
            palette.muted,
            0.9F
        );
        SDL_RenderPresent(menu.renderer());
        SDL_Delay(1);
    }
}

void show_discord_options(MenuSession& menu, AppLaunchOptions& options) {
    std::size_t selected_row = 0U;
    while (true) {
        auto& discord = options.settings.discord;
        menu.publish_presence(
            RuntimeActivityKind::options,
            "Configuring Discord Rich Presence"
        );
        const std::string privacy_label = discord.privacy == DiscordPresencePrivacy::full
            ? "Full details (least private)"
            : discord.privacy == DiscordPresencePrivacy::reduced
                ? "Reduced details"
                : "Minimal details (most private)";
        const auto link_state = menu.discord_account_link_state();
        const auto link_message = menu.discord_account_link_message();
        const auto effective_application_id = discord_effective_application_id(discord);
        const auto application_id_label = effective_application_id == 0U
            ? std::string{"not configured"}
            : std::to_string(effective_application_id)
                + (discord.application_id == 0U ? " (compiled default)" : " (settings)");
        const auto redirect_label = discord.oauth_redirect_uri.empty()
            ? std::string{"platform default"}
            : printable_text(discord.oauth_redirect_uri, 56U);
        const std::vector<std::string> choices{
            "Enable Rich Presence: " + on_off(discord.enabled),
            "Shared detail level: " + privacy_label,
            "Show chart name: " + on_off(discord.show_chart_name),
            "Show difficulty / mania: " + on_off(discord.show_difficulty_mania),
            "Show progress: " + on_off(discord.show_progress),
            "Show note counter: " + on_off(discord.show_note_counter),
            "Show gameplay stats: " + on_off(discord.show_gameplay_stats),
            "Show BOTPLAY: " + on_off(discord.show_botplay),
            "Show remaining time: " + on_off(discord.show_remaining_time),
            "Show mod name: " + on_off(discord.show_mod_name),
            "Advanced templates: " + on_off(discord.advanced_customization),
            "Retry failed updates: " + on_off(discord.retry_failed_updates),
            "Custom button 1: " + on_off(discord.button1.enabled),
            "Custom button 2: " + on_off(discord.button2.enabled),
            "Publish interval: " + std::to_string(discord.publish_interval_ms / 1'000U) + " s",
            "Discord account: " + std::string(
                discord_account_link_state_name(link_state)
            ),
            "Open Discord Connected Games",
            "Application ID: " + application_id_label,
            "OAuth redirect: " + redirect_label,
            "Open Discord Developer Portal",
            "Template / button fields: settings.json",
            "Back",
        };
        std::string footer{
            "ENTER toggles/cycles or links/unlinks account   ESC back   Full details shows enabled gameplay fields"
        };
        if (!link_message.empty()) {
            footer += "   ";
            footer += printable_text(link_message, 58U);
        }
        const auto selected = browse_choices(
            menu,
            "DISCORD RICH PRESENCE",
            choices,
            footer,
            selected_row
        );
        if (!selected.has_value() || *selected == choices.size() - 1U) return;
        selected_row = *selected;
        switch (*selected) {
        case 0U: discord.enabled = !discord.enabled; break;
        case 1U:
            discord.privacy = discord.privacy == DiscordPresencePrivacy::full
                ? DiscordPresencePrivacy::reduced
                : discord.privacy == DiscordPresencePrivacy::reduced
                    ? DiscordPresencePrivacy::minimal
                    : DiscordPresencePrivacy::full;
            break;
        case 2U: discord.show_chart_name = !discord.show_chart_name; break;
        case 3U: discord.show_difficulty_mania = !discord.show_difficulty_mania; break;
        case 4U: discord.show_progress = !discord.show_progress; break;
        case 5U: discord.show_note_counter = !discord.show_note_counter; break;
        case 6U: discord.show_gameplay_stats = !discord.show_gameplay_stats; break;
        case 7U: discord.show_botplay = !discord.show_botplay; break;
        case 8U: discord.show_remaining_time = !discord.show_remaining_time; break;
        case 9U: discord.show_mod_name = !discord.show_mod_name; break;
        case 10U: discord.advanced_customization = !discord.advanced_customization; break;
        case 11U: discord.retry_failed_updates = !discord.retry_failed_updates; break;
        case 12U: discord.button1.enabled = !discord.button1.enabled; break;
        case 13U: discord.button2.enabled = !discord.button2.enabled; break;
        case 14U:
            discord.publish_interval_ms = discord.publish_interval_ms >= 5'000U
                ? 2'000U
                : discord.publish_interval_ms + 1'000U;
            break;
        case 15U: {
            if (link_state == DiscordAccountLinkState::linked) {
                menu.unlink_discord_account();
            } else if (link_state == DiscordAccountLinkState::unavailable) {
                const std::array<std::string, 2U> info{
                    "This build has the safe no-op Discord backend",
                    "Package the official Discord Social SDK to enable account linking",
                };
                static_cast<void>(browse_choices(
                    menu,
                    "DISCORD ACCOUNT LINK",
                    info,
                    "Rich Presence remains fail-open; no webhook or token is requested"
                ));
            } else {
                menu.request_discord_account_link();
            }
            break;
        }
        case 16U: {
            if (!menu.open_discord_connected_games()) {
                const std::array<std::string, 2U> info{
                    "Connected Games requires a linked Discord account",
                    "Link the account above and ensure this build includes the Social SDK",
                };
                static_cast<void>(browse_choices(
                    menu,
                    "DISCORD CONNECTED GAMES",
                    info,
                    "PulseForge does not open or emulate account settings without Discord"
                ));
            }
            break;
        }
        case 17U: {
            const std::array<std::string, 2U> back{
                "settings.json applicationId overrides the compiled distribution ID",
                "Distributors can set PULSEFORGE_DISCORD_APPLICATION_ID at CMake configure time",
            };
            static_cast<void>(browse_choices(
                menu,
                "DISCORD APPLICATION ID",
                back,
                effective_application_id == 0U
                    ? "No effective Discord Application ID is configured"
                    : "Effective ID: " + std::to_string(effective_application_id)
            ));
            break;
        }
        case 18U: {
            const std::array<std::string, 3U> info{
                "Empty uses the official platform default",
                "Desktop: http://127.0.0.1/callback",
                "Android: discord-APP_ID:/authorize/callback",
            };
            static_cast<void>(browse_choices(
                menu,
                "DISCORD OAUTH REDIRECT",
                info,
                "Only the validated loopback/mobile scheme surface is accepted"
            ));
            break;
        }
        case 19U: {
            if (!SDL_OpenURL("https://discord.com/developers/applications")) {
                const std::array<std::string, 1U> info{
                    "Could not open the browser; visit discord.com/developers/applications"
                };
                static_cast<void>(browse_choices(
                    menu,
                    "DISCORD DEVELOPER PORTAL",
                    info,
                    SDL_GetError()
                ));
            }
            break;
        }
        case 20U: {
            const std::array<std::string, 3U> info{
                "Templates support chart/gameplay/render/media placeholders",
                "External HTTPS image URLs and up to two buttons are supported",
                "Minimal privacy ignores advanced templates entirely",
            };
            static_cast<void>(browse_choices(
                menu,
                "ADVANCED DISCORD FIELDS",
                info,
                "Edit template strings, image URLs and button fields in assets/settings.json"
            ));
            break;
        }
        default: return;
        }
    }
}

void show_options(MenuSession& menu, AppLaunchOptions& options) {
    std::size_t selected_row = 0U;
    while (true) {
        auto& gameplay = options.settings.gameplay;
        auto& visual = options.settings.visual;
        auto& audio = options.settings.audio;
        auto& performance = options.settings.performance;
        menu.publish_presence(RuntimeActivityKind::options, "Configuring performance");
        const auto visualizer_image_label = [&]() -> std::string_view {
            switch (visual.audio_visualizer_image) {
            case AudioVisualizerImage::star_of_david:
                return "Star of David";
            case AudioVisualizerImage::circular_symbol:
                return "Circular symbol";
            case AudioVisualizerImage::hammer_sickle_star:
                return "Hammer & Sickle star";
            case AudioVisualizerImage::custom:
                return "Custom image";
            }
            return "Star of David";
        }();
        const auto note_skin_label =
            note_skin_selection_display_name(visual.note_skin_selection);
        const auto selected_music = [&]() {
            if (audio.menu_music_selection == "custom") {
                if (audio.custom_menu_music_path.empty()) {
                    return std::string{"Custom (not selected)"};
                }
                return std::string{"Custom: "} + printable_text(
                    path_as_utf8(std::filesystem::path(
                        audio.custom_menu_music_path
                    ).stem()),
                    44U
                );
            }
            if (audio.menu_music_selection == "randomized") {
                return std::string{"Randomized"};
            }
            const auto selected = std::ranges::find_if(
                menu.music_paths(),
                [&audio](const std::filesystem::path& path) {
                    return path.filename().string()
                        == audio.menu_music_selection;
                }
            );
            return selected == menu.music_paths().end()
                ? std::string{"Randomized (saved track unavailable)"}
                : printable_text(selected->stem().string(), 52U);
        }();
        const std::vector<std::string> choices{
            "Downscroll: " + on_off(gameplay.downscroll),
            "Middlescroll: " + on_off(gameplay.middle_scroll),
            "Ghost tapping: " + on_off(gameplay.ghost_tapping),
            "Practice mode: " + on_off(gameplay.practice),
            "No fail: " + on_off(gameplay.no_fail),
            "Botplay: " + on_off(gameplay.autoplay),
            "Mirror lanes: " + on_off(gameplay.mirror),
            "Randomized lanes: " + on_off(gameplay.randomize_lanes),
            "Hide opponent notes: " + on_off(gameplay.hide_opponent_notes),
            "Scroll speed: " + std::to_string(gameplay.scroll_speed).substr(0, 4) + "x",
            "Input offset: " + std::to_string(gameplay.input_offset_ms).substr(0, 6) + " ms",
            "Visual offset: " + std::to_string(gameplay.visual_offset_ms).substr(0, 6) + " ms",
            "Low quality: " + on_off(visual.low_quality),
            "Reduced motion: " + on_off(visual.reduced_motion),
            "Note splashes: " + on_off(visual.note_splashes),
            "Flashing lights: " + on_off(visual.flashing_lights),
            "VSync: " + on_off(visual.vsync),
            "FPS cap: " + std::string(visual.fps_cap == 0
                ? "unlimited"
                : std::to_string(visual.fps_cap)),
            "Master volume: " + std::to_string(master_volume_percent(audio)) + "%",
            "Master mute: " + on_off(audio.muted),
            "Playback rate: " + std::to_string(audio.playback_rate).substr(0, 4) + "x",
            "Audio buffer: " + std::to_string(audio.buffer_frames) + " frames",
            "Fullscreen: " + on_off(visual.fullscreen),
            "Interface theme: " + std::string(
                presentation_theme_name(visual.theme)
            ),
            "Post effect: " + std::string(post_effect_display_name(
                visual.post_effect
            )) + (performance.maximum_performance_mode
                ? " (maximum-performance bypass)"
                : ""),
            "Menu music: " + selected_music,
            "Menu music playback: " + std::string(
                audio.menu_music_loop_selected
                    ? "Loop selected"
                    : "Randomized"
            ),
            "Menu music mute: " + on_off(audio.menu_music_muted),
            "Choose custom menu music...",
            "Skip startup intro: " + on_off(visual.skip_intro),
            "Maximum performance mode: "
                + on_off(performance.maximum_performance_mode),
            "Ultra-low latency mode: " + on_off(
                performance.ultra_low_latency
            ),
            "Audio visualizer opacity: "
                + std::to_string(static_cast<int>(std::lround(
                    std::clamp(
                        visual.audio_visualizer_background_opacity,
                        0.0F,
                        1.0F
                    ) * 100.0F
                ))) + "%",
            "Audio visualizer image: "
                + std::string(visualizer_image_label),
            "Note skin: " + note_skin_label,
            "Discord Rich Presence...",
            "Controls...",
            "Back",
        };
        const auto selected = browse_choices(
            menu,
            "PULSEFORGE  //  OPTIONS",
            choices,
            "ENTER toggles/cycles   +/- volume   changes save immediately   ESC back",
            selected_row
        );
        if (!selected.has_value() || *selected == choices.size() - 1U) {
            return;
        }
        selected_row = *selected;
        bool menu_music_changed = false;
        if (performance.maximum_performance_mode
            && ((*selected >= 12U && *selected <= 17U)
                || *selected == 21U)) {
            // A manual edit to an overridden field exits the parallel profile,
            // restores the normal snapshot, then applies the requested edit.
            set_maximum_performance_mode(options.settings, false);
        }
        switch (*selected) {
        case 0: gameplay.downscroll = !gameplay.downscroll; break;
        case 1: gameplay.middle_scroll = !gameplay.middle_scroll; break;
        case 2: gameplay.ghost_tapping = !gameplay.ghost_tapping; break;
        case 3: gameplay.practice = !gameplay.practice; break;
        case 4: gameplay.no_fail = !gameplay.no_fail; break;
        case 5: gameplay.autoplay = !gameplay.autoplay; break;
        case 6: gameplay.mirror = !gameplay.mirror; break;
        case 7: gameplay.randomize_lanes = !gameplay.randomize_lanes; break;
        case 8: gameplay.hide_opponent_notes = !gameplay.hide_opponent_notes; break;
        case 9: {
            constexpr std::array speeds{0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0};
            const auto current = std::ranges::find(speeds, gameplay.scroll_speed);
            gameplay.scroll_speed = current == speeds.end() || current + 1 == speeds.end()
                ? speeds.front()
                : *(current + 1);
            break;
        }
        case 10:
        case 11: {
            constexpr std::array offsets{-100.0, -50.0, -25.0, 0.0, 25.0, 50.0, 100.0};
            auto& value = *selected == 10U
                ? gameplay.input_offset_ms
                : gameplay.visual_offset_ms;
            const auto current = std::ranges::find(offsets, value);
            value = current == offsets.end() || current + 1 == offsets.end()
                ? offsets.front()
                : *(current + 1);
            break;
        }
        case 12: visual.low_quality = !visual.low_quality; break;
        case 13: visual.reduced_motion = !visual.reduced_motion; break;
        case 14: visual.note_splashes = !visual.note_splashes; break;
        case 15: visual.flashing_lights = !visual.flashing_lights; break;
        case 16: visual.vsync = !visual.vsync; break;
        case 17: {
            constexpr std::array caps{
                60, 120, 144, 240, 360, 480, 720, 1'000, 0
            };
            const auto current = std::ranges::find(caps, visual.fps_cap);
            const auto next = current == caps.end() || current + 1 == caps.end()
                ? caps.begin()
                : current + 1;
            visual.fps_cap = *next;
            break;
        }
        case 18:
            if (master_volume_percent(audio) >= 100) {
                audio.master_volume = 0.0F;
            } else {
                static_cast<void>(adjust_master_volume(audio, 1));
            }
            break;
        case 19:
            toggle_master_mute(audio);
            break;
        case 20: {
            constexpr std::array rates{0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
            const auto current = std::ranges::find(rates, audio.playback_rate);
            audio.playback_rate = current == rates.end() || current + 1 == rates.end()
                ? rates.front()
                : *(current + 1);
            break;
        }
        case 21: {
            constexpr std::array<std::uint32_t, 5> buffers{
                64U, 128U, 256U, 512U, 1024U
            };
            const auto current = std::ranges::find(buffers, audio.buffer_frames);
            const auto next = current == buffers.end() || current + 1 == buffers.end()
                ? buffers.begin()
                : current + 1;
            audio.buffer_frames = *next;
            break;
        }
        case 22:
            visual.fullscreen = !visual.fullscreen;
            break;
        case 23:
            switch (visual.theme) {
            case PresentationTheme::pulseforge:
                visual.theme = PresentationTheme::watch_dogs;
                break;
            case PresentationTheme::watch_dogs:
                visual.theme = PresentationTheme::ps2;
                break;
            case PresentationTheme::ps2:
                visual.theme = PresentationTheme::beverly_hills_90210;
                break;
            case PresentationTheme::beverly_hills_90210:
                visual.theme = PresentationTheme::just_cause_3;
                break;
            case PresentationTheme::just_cause_3:
                visual.theme = PresentationTheme::just_cause_4;
                break;
            case PresentationTheme::just_cause_4:
                visual.theme = PresentationTheme::xbox_original;
                break;
            case PresentationTheme::xbox_original:
                visual.theme = PresentationTheme::xbox_360;
                break;
            case PresentationTheme::xbox_360:
                visual.theme = PresentationTheme::pulseforge;
                break;
            }
            break;
        case 24: {
            visual.post_effect = next_post_effect(visual.post_effect);
            break;
        }
        case 25: {
            std::vector<std::string> selections{"randomized"};
            selections.reserve(menu.music_paths().size() + 2U);
            for (const auto& path : menu.music_paths()) {
                selections.push_back(path.filename().string());
            }
            if (!audio.custom_menu_music_path.empty()) {
                selections.push_back("custom");
            }
            const auto current = std::ranges::find(
                selections,
                audio.menu_music_selection
            );
            const auto next = current == selections.end()
                    || current + 1 == selections.end()
                ? selections.begin()
                : current + 1;
            audio.menu_music_selection = *next;
            audio.menu_music_loop_selected = *next != "randomized";
            menu_music_changed = true;
            break;
        }
        case 26:
            if (audio.menu_music_loop_selected) {
                audio.menu_music_loop_selected = false;
            } else if (audio.menu_music_selection == "custom"
                       && !audio.custom_menu_music_path.empty()) {
                audio.menu_music_loop_selected = true;
            } else if (!menu.music_paths().empty()) {
                if (audio.menu_music_selection == "randomized"
                    || std::ranges::none_of(
                        menu.music_paths(),
                        [&audio](const std::filesystem::path& path) {
                            return path.filename().string()
                                == audio.menu_music_selection;
                        }
                    )) {
                    audio.menu_music_selection =
                        menu.music_paths().front().filename().string();
                }
                audio.menu_music_loop_selected = true;
            }
            menu_music_changed = true;
            break;
        case 27:
            audio.menu_music_muted = !audio.menu_music_muted;
            menu_music_changed = true;
            break;
        case 28: {
            constexpr std::array audio_filters{
                SDL_DialogFileFilter{"Audio", "mp3;ogg;wav;flac"},
            };
            std::string dialog_error;
            const auto selected_audio = choose_source_path(
                menu,
                false,
                std::span<const SDL_DialogFileFilter>{audio_filters},
                "CUSTOM MENU MUSIC",
                "Select MP3, OGG, WAV or FLAC",
                dialog_error
            );
            if (selected_audio.has_value()) {
                audio.custom_menu_music_path = path_as_utf8(*selected_audio);
                audio.menu_music_selection = "custom";
                audio.menu_music_loop_selected = true;
                audio.menu_music_muted = false;
                menu_music_changed = true;
            } else if (!dialog_error.empty()) {
                const std::array<std::string, 1> back{"Back"};
                static_cast<void>(browse_choices(
                    menu,
                    "CUSTOM MENU MUSIC",
                    back,
                    dialog_error
                ));
            }
            break;
        }
        case 29:
            visual.skip_intro = !visual.skip_intro;
            break;
        case 30:
            set_maximum_performance_mode(
                options.settings,
                !performance.maximum_performance_mode
            );
            break;
        case 31:
            performance.ultra_low_latency = !performance.ultra_low_latency;
            if (performance.ultra_low_latency) {
                visual.vsync = false;
                audio.buffer_frames = 64U;
            }
            break;
        case 32: {
            std::vector<std::string> opacity_choices;
            opacity_choices.reserve(101U);
            for (int percent = 0; percent <= 100; ++percent) {
                opacity_choices.push_back(std::to_string(percent) + "%");
            }
            const auto current_percent = static_cast<std::size_t>(
                std::clamp(
                    static_cast<int>(std::lround(
                        visual.audio_visualizer_background_opacity * 100.0F
                    )),
                    0,
                    100
                )
            );
            const auto selected_opacity = browse_choices(
                menu,
                "AUDIO VISUALIZER OPACITY",
                opacity_choices,
                "UP/DOWN adjusts by 1%   ENTER set   ESC cancel",
                current_percent
            );
            if (selected_opacity.has_value()) {
                visual.audio_visualizer_background_opacity =
                    static_cast<float>(*selected_opacity) / 100.0F;
            }
            break;
        }
        case 33: {
            const std::vector<std::string> image_choices{
                "Star of David",
                "Circular symbol",
                "Hammer & Sickle star",
                "Custom image...",
            };
            const std::size_t current_image =
                visual.audio_visualizer_image == AudioVisualizerImage::circular_symbol
                    ? 1U
                    : visual.audio_visualizer_image
                            == AudioVisualizerImage::hammer_sickle_star
                        ? 2U
                        : visual.audio_visualizer_image == AudioVisualizerImage::custom
                            ? 3U
                            : 0U;
            const auto selected_image = browse_choices(
                menu,
                "AUDIO VISUALIZER IMAGE",
                image_choices,
                "UP/DOWN chooses image   ENTER set   ESC cancel",
                current_image
            );
            if (selected_image.has_value()) {
                switch (*selected_image) {
                case 1U:
                    visual.audio_visualizer_image =
                        AudioVisualizerImage::circular_symbol;
                    break;
                case 2U:
                    visual.audio_visualizer_image =
                        AudioVisualizerImage::hammer_sickle_star;
                    break;
                case 3U: {
                    constexpr std::array image_filters{
                        SDL_DialogFileFilter{"Images", "png;jpg;jpeg;bmp"},
                    };
                    std::string dialog_error;
                    const auto custom_image = choose_source_path(
                        menu,
                        false,
                        std::span<const SDL_DialogFileFilter>{image_filters},
                        "CUSTOM VISUALIZER IMAGE",
                        "Select PNG, JPG/JPEG or BMP",
                        dialog_error
                    );
                    if (custom_image.has_value()) {
                        visual.audio_visualizer_custom_image_path =
                            path_as_utf8(*custom_image);
                        visual.audio_visualizer_image = AudioVisualizerImage::custom;
                    } else if (!dialog_error.empty()) {
                        const std::array<std::string, 1> back{"Back"};
                        static_cast<void>(browse_choices(
                            menu,
                            "CUSTOM VISUALIZER IMAGE",
                            back,
                            dialog_error
                        ));
                    }
                    break;
                }
                default:
                    visual.audio_visualizer_image =
                        AudioVisualizerImage::star_of_david;
                    break;
                }
            }
            break;
        }
        case 34: {
            std::vector<std::filesystem::path> discovery_roots =
                options.content_roots;
            const auto append_discovery_root =
                [&](const std::filesystem::path& root) {
                    if (root.empty()) return;
                    if (std::find(
                            discovery_roots.begin(),
                            discovery_roots.end(),
                            root
                        ) == discovery_roots.end()) {
                        discovery_roots.push_back(root);
                    }
                };
            append_discovery_root(options.selected_content_root);
            append_discovery_root(options.selected_mod_root);

            const auto catalog = discover_note_skins(discovery_roots);

            std::vector<std::string> note_skin_choices{
                "Chart default",
                "Classic (NOTE_assets)",
                "Pixel (arrows-pixels)",
            };
            std::vector<std::string> note_skin_selections{
                "chart-default",
                "atlas:NOTE_assets",
                "pixel:arrows-pixels",
            };
            note_skin_choices.reserve(catalog.size() + 4U);
            note_skin_selections.reserve(catalog.size() + 4U);

            for (const auto& entry : catalog) {
                const auto parsed = parse_note_skin_selection(entry.selection);
                const bool compatibility_alias =
                    (!parsed.pixel
                        && note_skin_catalog_detail::equals_ascii_insensitive(
                            parsed.style,
                            "NOTE_assets"
                        ))
                    || (parsed.pixel
                        && note_skin_catalog_detail::equals_ascii_insensitive(
                            parsed.style,
                            "arrows-pixels"
                        ));
                if (compatibility_alias) continue;
                note_skin_choices.push_back(entry.display_name);
                note_skin_selections.push_back(entry.selection);
            }

            auto current = std::find(
                note_skin_selections.begin(),
                note_skin_selections.end(),
                visual.note_skin_selection
            );
            if (current == note_skin_selections.end()) {
                const auto normalized = normalize_note_skin_selection(
                    visual.note_skin_selection
                );
                current = std::find(
                    note_skin_selections.begin(),
                    note_skin_selections.end(),
                    normalized
                );
            }

            // Keep a saved skin visible even if its mod/assets are temporarily
            // unavailable, rather than silently changing the user's setting.
            if (current == note_skin_selections.end()
                && visual.note_skin_selection != "chart-default") {
                note_skin_choices.push_back(
                    "Unavailable: "
                    + note_skin_selection_display_name(
                        visual.note_skin_selection
                    )
                );
                note_skin_selections.push_back(
                    visual.note_skin_selection
                );
                current = note_skin_selections.end() - 1;
            }

            const std::size_t current_note_skin =
                current == note_skin_selections.end()
                    ? 0U
                    : static_cast<std::size_t>(
                        std::distance(note_skin_selections.begin(), current)
                    );

            const auto selected_note_skin = browse_choices(
                menu,
                "NOTE SKINS  //  INSTALLED",
                note_skin_choices,
                "Auto-detected from assets/mods   UP/DOWN choose   ENTER set",
                current_note_skin
            );
            if (selected_note_skin.has_value()
                && *selected_note_skin < note_skin_selections.size()) {
                visual.note_skin_selection =
                    note_skin_selections[*selected_note_skin];
            }
            break;
        }
        case 35:
            show_discord_options(menu, options);
            break;
        case 36:
            show_controls_editor(
                menu.window(),
                menu.renderer(),
                options.settings,
                options.settings_path
            );
            break;
        default: return;
        }
        menu.apply_visual_settings(options.settings.visual);
        if (menu_music_changed) {
            menu.reconfigure_music();
        } else {
            menu.resume_music();
        }
        if (!options.settings_path.empty()) {
            std::string save_error;
            if (!save_settings(
                    options.settings_path,
                    options.settings,
                    &save_error
                )) {
                const std::array<std::string, 1> back{"Continue without saving"};
                static_cast<void>(browse_choices(
                    menu,
                    "SETTINGS COULD NOT BE SAVED",
                    back,
                    save_error
                ));
            }
        }
    }
}

class LauncherApplication final : public ApplicationRunner {
public:
    explicit LauncherApplication(AppLaunchOptions options)
        : options_(std::move(options)),
          discord_session_(std::make_shared<DiscordPresenceSession>()) {}

    [[nodiscard]] int run() override {
        if (!options_.chart_path.empty() && !options_.show_launcher
            && !options_.catalog_song.has_value()) {
            auto direct = options_;
            const int result = make_gameplay_application(
                std::move(direct),
                discord_session_
            )->run();
            return finish_direct_launch(result);
        }

        ContentCatalogOptions scan_options;
        scan_options.roots = options_.content_roots;
        auto catalog = ContentCatalog::scan(scan_options);
        for (const auto& diagnostic : catalog.diagnostics()) {
            std::cerr << "Content warning: " << diagnostic.message << '\n';
        }
        if (catalog.entries().empty()) {
            std::cerr
                << "No playable charts were found in the configured content roots; "
                   "Editors and AutoChart remain available\n";
        }

        if (options_.catalog_song.has_value()) {
            const auto difficulty = options_.chart_options.difficulty_explicit
                ? std::string_view(options_.chart_options.difficulty)
                : std::string_view{};
            const auto* entry = catalog.find(*options_.catalog_song, difficulty);
            if (entry == nullptr) {
                std::cerr << "Song not found in content catalog: "
                          << *options_.catalog_song << '\n';
                return EXIT_FAILURE;
            }
            auto selected = options_for_entry(options_, *entry, false);
            const int result = make_gameplay_application(
                std::move(selected),
                discord_session_
            )->run();
            return finish_direct_launch(result);
        }

        auto stories = build_story_collections(catalog);
        const std::vector<std::string> main_choices{
            "Story Mode",
            "Freeplay",
            "Editors",
            "Mods",
            "Rendering Mode",
            "Options",
            "Credits",
            "Quit",
        };
        std::unique_ptr<MenuSession> menu;
        if (!open_menu(menu, false)) {
            return EXIT_FAILURE;
        }
        if (!options_.settings.visual.skip_intro) {
            std::filesystem::path startup_movie;
            if (options_.settings.visual.theme == PresentationTheme::ps2) {
                startup_movie = ps2_startup_movie_path(options_);
            } else {
                const auto intros = startup_movie_paths(options_);
                if (!intros.empty()) {
                    startup_movie = choose_startup_movie(intros);
                }
            }
            if (!startup_movie.empty()) {
                StartupIntroResult intro;
                if (options_.settings.visual.theme == PresentationTheme::ps2) {
                    StartupIntroPlaybackOptions playback;
                    playback.allow_decoded_derivative = true;
                    playback.allow_native_movie = true;
                    playback.allow_procedural_fallback = true;
                    playback.loop_until_skip = false;
                    playback.show_skip_hint = false;
                    playback.overlay = &draw_ps2_startup_overlay;
                    intro = play_startup_intro_ex(
                        menu->window(),
                        menu->renderer(),
                        startup_movie,
                        options_.settings.audio,
                        playback
                    );
                } else {
                    intro = play_startup_intro(
                        menu->window(),
                        menu->renderer(),
                        startup_movie,
                        options_.settings.audio
                    );
                }
                if (!intro.diagnostic.empty()) {
                    std::cerr << "Startup intro warning: "
                              << intro.diagnostic << '\n';
                }
                if (intro.status == StartupIntroStatus::quit_requested) {
                    return EXIT_SUCCESS;
                }
            }
        }
        menu->resume_music();
        std::size_t main_selected = 0U;
        while (true) {
            if (menu->close_requested()) {
                return EXIT_SUCCESS;
            }
            const auto action = browse_choices(
                *menu,
                "PULSEFORGE",
                main_choices,
                "UP/DOWN select   ENTER accept   ESC quit   PulseForge v"
                    PULSEFORGE_VERSION,
                main_selected,
                false
            );
            if (!action.has_value()) {
                return EXIT_SUCCESS;
            }
            if (*action == 7U) {
                play_explicit_exit_video(*menu, options_);
                return EXIT_SUCCESS;
            }
            main_selected = *action;
            if (*action == 0U) {
                const int result = run_story(menu, catalog, stories);
                if (result == runner_quit_engine) {
                    if (!open_menu(menu, false)) {
                        return EXIT_FAILURE;
                    }
                    play_explicit_exit_video(*menu, options_);
                    return EXIT_SUCCESS;
                }
                if (result == EXIT_FAILURE
                    || result == runner_chart_load_failed) {
                    if (!open_menu(menu)) {
                        return EXIT_FAILURE;
                    }
                    const std::array<std::string, 1> back{"Back to main menu"};
                    static_cast<void>(browse_choices(
                        *menu,
                        "CHART COULD NOT BE LOADED",
                        back,
                        "PulseForge rejected invalid chart/audio/runtime data; see the diagnostic log"
                    ));
                    continue;
                }
                if (result != runner_return_to_launcher) {
                    return result;
                }
            } else if (*action == 1U) {
                const auto selected_index = browse_catalog(*menu, catalog);
                if (!selected_index.has_value()) {
                    continue;
                }
                auto selected = options_for_entry(
                    options_,
                    catalog.entries()[*selected_index],
                    true
                );
                menu->suspend_music();
                show_loading_screen(
                    *menu,
                    "PREPARING CHART",
                    catalog.entries()[*selected_index].title + "  ["
                        + catalog.entries()[*selected_index].difficulty + ']'
                );
                auto platform = menu->release_platform();
                TransferredPlatform returned_platform;
                auto gameplay = make_gameplay_application(
                    std::move(selected),
                    std::move(platform),
                    &returned_platform,
                    discord_session_
                );
                const int result = gameplay->run();
                gameplay.reset();
                if (!restore_menu_platform(
                        menu,
                        std::move(returned_platform),
                        false
                    )) {
                    return EXIT_FAILURE;
                }
                if (result == runner_quit_engine) {
                    play_explicit_exit_video(*menu, options_);
                    return EXIT_SUCCESS;
                }
                if (result == EXIT_FAILURE
                    || result == runner_chart_load_failed) {
                    menu->resume_music();
                    const std::array<std::string, 1> back{"Back to Freeplay"};
                    static_cast<void>(browse_choices(
                        *menu,
                        "CHART COULD NOT BE LOADED",
                        back,
                        "PulseForge rejected invalid chart/audio/runtime data; see the diagnostic log"
                    ));
                    continue;
                }
                if (result != runner_return_to_launcher) {
                    return result;
                }
                if (!play_return_transition(*menu, options_)) {
                    return EXIT_SUCCESS;
                }
                menu->resume_music();
            } else if (*action == 2U) {
                const auto editor_result = show_editors(
                    *menu,
                    catalog,
                    options_
                );
                if (editor_result.quit_requested) {
                    return EXIT_SUCCESS;
                }
                if (editor_result.content_changed) {
                    catalog = ContentCatalog::scan(scan_options);
                    stories = build_story_collections(catalog);
                }
            } else if (*action == 3U) {
                if (show_mod_browser(*menu, catalog, options_)) {
                    catalog = ContentCatalog::scan(scan_options);
                    stories = build_story_collections(catalog);
                }
            } else if (*action == 4U) {
                const auto selected_index = browse_catalog(*menu, catalog);
                if (!selected_index.has_value()) {
                    continue;
                }
                const auto render_config = configure_render(*menu);
                if (!render_config.has_value()) {
                    continue;
                }
                auto selected = options_for_entry(
                    options_,
                    catalog.entries()[*selected_index],
                    true
                );
                selected.offline_render = *render_config;
                menu->suspend_music();
                show_loading_screen(
                    *menu,
                    "PREPARING OFFLINE RENDER",
                    catalog.entries()[*selected_index].title + "  //  "
                        + std::to_string(render_config->width) + 'x'
                        + std::to_string(render_config->height) + "  //  "
                        + std::to_string(render_config->fps) + " FPS"
                );
                auto platform = menu->release_platform();
                TransferredPlatform returned_platform;
                auto gameplay = make_gameplay_application(
                    std::move(selected),
                    std::move(platform),
                    &returned_platform,
                    discord_session_
                );
                const int result = gameplay->run();
                gameplay.reset();
                if (!restore_menu_platform(
                        menu,
                        std::move(returned_platform),
                        false
                    )) {
                    return EXIT_FAILURE;
                }
                if (result == runner_quit_engine) {
                    play_explicit_exit_video(*menu, options_);
                    return EXIT_SUCCESS;
                }
                if (result == runner_chart_load_failed) {
                    menu->resume_music();
                    const std::array<std::string, 1> back{
                        "Back to Rendering Mode",
                    };
                    static_cast<void>(browse_choices(
                        *menu,
                        "RENDER CHART COULD NOT BE LOADED",
                        back,
                        "The complete load error remains in the diagnostic log"
                    ));
                    continue;
                }
                if (result != EXIT_SUCCESS
                    && result != runner_return_to_launcher) {
                    return result;
                }
                if (result == runner_return_to_launcher
                    && !play_return_transition(*menu, options_)) {
                    return EXIT_SUCCESS;
                }
                menu->resume_music();
            } else if (*action == 5U) {
                show_options(*menu, options_);
            } else if (*action == 6U) {
                show_credits(*menu, options_);
            }
        }
    }

private:
    [[nodiscard]] int finish_direct_launch(const int result) {
        if (result != runner_quit_engine) {
            return result;
        }
        std::unique_ptr<MenuSession> exit_session;
        if (!open_menu(exit_session, false)) {
            return EXIT_FAILURE;
        }
        play_explicit_exit_video(*exit_session, options_);
        return EXIT_SUCCESS;
    }

    [[nodiscard]] bool restore_menu_platform(
        std::unique_ptr<MenuSession>& destination,
        TransferredPlatform platform,
        const bool start_music = true
    ) {
        if (!destination) {
            std::cerr << "Menu session was lost during platform transfer\n";
            return false;
        }
        refresh_settings_from_disk();
        if (!destination->adopt_platform(std::move(platform))) {
            std::cerr << destination->error() << '\n';
            return false;
        }
        if (start_music) {
            destination->resume_music();
        }
        return true;
    }

    [[nodiscard]] bool open_menu(
        std::unique_ptr<MenuSession>& destination,
        const bool start_music = true
    ) {
        if (destination && !static_cast<bool>(*destination)) {
            destination.reset();
        }
        if (destination) {
            if (start_music) {
                destination->resume_music();
            }
            return true;
        }
        refresh_settings_from_disk();
        auto menu = std::make_unique<MenuSession>(
            options_.settings,
            options_.settings_path,
            menu_music_paths(options_),
            discord_session_
        );
        if (!*menu) {
            std::cerr << menu->error() << '\n';
            return false;
        }
        destination = std::move(menu);
        if (start_music) {
            destination->resume_music();
        }
        return true;
    }

    void refresh_settings_from_disk() {
        if (options_.settings_path.empty()) {
            return;
        }
        const auto loaded = load_settings(options_.settings_path);
        if (loaded) {
            options_.settings = *loaded.settings;
        }
    }

    [[nodiscard]] int run_story(
        std::unique_ptr<MenuSession>& menu,
        const ContentCatalog& catalog,
        const std::vector<StoryCollection>& stories
    ) {
        menu->publish_presence(
            RuntimeActivityKind::story,
            std::to_string(stories.size()) + " story collections"
        );
        if (stories.empty()) {
            const std::array<std::string, 1> back{"Back"};
            static_cast<void>(browse_choices(
                *menu,
                "STORY MODE",
                back,
                "No story or song collections were found"
            ));
            return runner_return_to_launcher;
        }

        std::vector<std::string> labels;
        labels.reserve(stories.size());
        for (const auto& story : stories) {
            std::set<std::string> song_ids;
            for (const auto* chart : story.charts) {
                song_ids.insert(lower_ascii(chart->song_id));
            }
            labels.push_back(
                story.label + "  //  " + std::to_string(song_ids.size())
                + (song_ids.size() == 1U ? " song" : " songs")
            );
        }
        const auto story_index = browse_choices(
            *menu,
            "PULSEFORGE  //  STORY MODE",
            labels,
            "Choose a week/collection   ENTER continue   ESC back"
        );
        if (!story_index.has_value()) {
            return runner_return_to_launcher;
        }
        const auto& story = stories[*story_index];
        menu->publish_presence(RuntimeActivityKind::story, story.label);
        auto difficulties = collection_difficulties(story);
        if (difficulties.empty()) {
            difficulties.emplace_back("normal");
        }
        std::size_t difficulty_index = 0;
        if (difficulties.size() > 1U) {
            const auto selected_difficulty = browse_choices(
                *menu,
                std::string{"PULSEFORGE  //  STORY MODE  //  "}
                    + story.label,
                difficulties,
                "Choose difficulty   missing per-song charts fall back to normal"
            );
            if (!selected_difficulty.has_value()) {
                return runner_return_to_launcher;
            }
            difficulty_index = *selected_difficulty;
        }
        const auto playlist = build_story_playlist(
            story,
            difficulties[difficulty_index]
        );
        bool seeking_next_playable = false;
        std::size_t skipped_invalid = 0U;
        for (std::size_t index = 0; index < playlist.size(); ++index) {
            auto selected = options_for_entry(options_, *playlist[index], true);
            selected.campaign_mode = true;
            selected.suppress_load_error_acknowledgement = seeking_next_playable;
            selected.chart_options.difficulty = playlist[index]->difficulty;
            selected.chart_options.difficulty_explicit = true;
            std::cout << "Story " << (index + 1U) << '/' << playlist.size()
                      << ": " << playlist[index]->title << " ["
                      << playlist[index]->difficulty << "]\n";
            menu->suspend_music();
            show_loading_screen(
                *menu,
                "PREPARING STORY CHART",
                playlist[index]->title + "  ["
                    + playlist[index]->difficulty + ']'
            );
            auto platform = menu->release_platform();
            TransferredPlatform returned_platform;
            auto gameplay = make_gameplay_application(
                std::move(selected),
                std::move(platform),
                &returned_platform,
                discord_session_
            );
            const int result = gameplay->run();
            gameplay.reset();
            if (!restore_menu_platform(
                    menu,
                    std::move(returned_platform),
                    false
                )) {
                return EXIT_FAILURE;
            }
            if (result == runner_song_completed) {
                seeking_next_playable = false;
                continue;
            }
            if (result == runner_chart_load_failed) {
                ++skipped_invalid;
                bool skip = seeking_next_playable;
                if (!seeking_next_playable) {
                    menu->resume_music();
                    const std::array<std::string, 2> actions{
                        "Skip to next playable song",
                        "Return to main menu",
                    };
                    const auto action = browse_choices(
                        *menu,
                        "STORY CHART COULD NOT BE LOADED",
                        actions,
                        playlist[index]->title
                            + " was rejected; the full error remains in the diagnostic log"
                    );
                    skip = action.has_value() && *action == 0U;
                }
                if (!skip) {
                    return runner_return_to_launcher;
                }
                seeking_next_playable = true;
                if (index + 1U >= playlist.size()) {
                    menu->resume_music();
                    const std::array<std::string, 1> back{
                        "Return to main menu",
                    };
                    static_cast<void>(browse_choices(
                        *menu,
                        "NO PLAYABLE STORY SONG REMAINS",
                        back,
                        std::to_string(skipped_invalid)
                            + " invalid chart(s) were skipped; diagnostics were preserved"
                    ));
                    return runner_return_to_launcher;
                }
                continue;
            }
            if (result == runner_song_failed) {
                menu->resume_music();
                const std::array<std::string, 1> back{"Return to main menu"};
                static_cast<void>(browse_choices(
                    *menu,
                    "STORY FAILED",
                    back,
                    "The campaign state was discarded; individual scores remain valid"
                ));
                return runner_return_to_launcher;
            }
            if (result == runner_return_to_launcher) {
                if (!play_return_transition(*menu, options_)) {
                    return runner_return_to_launcher;
                }
                menu->resume_music();
                return runner_return_to_launcher;
            }
            return result;
        }
        if (!open_menu(menu)) {
            return EXIT_FAILURE;
        }
        const std::array<std::string, 1> back{"Return to main menu"};
        static_cast<void>(browse_choices(
            *menu,
            "STORY COMPLETE",
            back,
            story.label + " cleared"
        ));
        static_cast<void>(catalog);
        return runner_return_to_launcher;
    }

    AppLaunchOptions options_;
    std::shared_ptr<DiscordPresenceSession> discord_session_;
};

}  // namespace

std::unique_ptr<ApplicationRunner> make_application(AppLaunchOptions options) {
    return std::make_unique<LauncherApplication>(std::move(options));
}

}  // namespace pulseforge::detail
