#pragma once

#include "pulseforge/application.hpp"

#include <memory>

struct SDL_Renderer;
struct SDL_Window;

namespace pulseforge::detail {

class DiscordPresenceSession;

inline constexpr int runner_return_to_launcher = 64;
inline constexpr int runner_song_completed = 65;
inline constexpr int runner_song_failed = 66;
// Explicit Quit action owned by launcher-level UI. Gameplay no longer exposes
// an engine-exit item in its pause menu.
inline constexpr int runner_quit_engine = 67;
// Loading/validation/platform/audio initialization failed before gameplay
// began. Story Mode uses this distinct result to seek the next loadable song;
// a voluntary Return to Menu remains runner_return_to_launcher.
inline constexpr int runner_chart_load_failed = 68;

struct TransferredPlatform {
    SDL_Window* window{};
    SDL_Renderer* renderer{};
    bool owns_sdl{};

    TransferredPlatform() noexcept = default;
    TransferredPlatform(
        SDL_Window* window_value,
        SDL_Renderer* renderer_value,
        bool owns_sdl_value
    ) noexcept;
    ~TransferredPlatform() noexcept;

    TransferredPlatform(const TransferredPlatform&) = delete;
    TransferredPlatform& operator=(const TransferredPlatform&) = delete;
    TransferredPlatform(TransferredPlatform&& other) noexcept;
    TransferredPlatform& operator=(TransferredPlatform&& other) noexcept;

    void adopt(
        SDL_Window* window_value,
        SDL_Renderer* renderer_value,
        bool owns_sdl_value
    ) noexcept;
    void detach(
        SDL_Window*& window_output,
        SDL_Renderer*& renderer_output,
        bool& owns_sdl_output
    ) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool complete() const noexcept;
};

// Private seam between the public CoreEngine lifecycle and the SDL runtime.
// It also keeps platform-heavy headers out of the public API.
class ApplicationRunner {
public:
    virtual ~ApplicationRunner() = default;

    [[nodiscard]] virtual int run() = 0;
};

[[nodiscard]] std::unique_ptr<ApplicationRunner> make_application(
    AppLaunchOptions options
);

[[nodiscard]] std::unique_ptr<ApplicationRunner> make_gameplay_application(
    AppLaunchOptions options
);

[[nodiscard]] std::unique_ptr<ApplicationRunner> make_gameplay_application(
    AppLaunchOptions options,
    std::shared_ptr<DiscordPresenceSession> discord_session
);

[[nodiscard]] std::unique_ptr<ApplicationRunner> make_gameplay_application(
    AppLaunchOptions options,
    TransferredPlatform platform
);

// return_platform must outlive the returned runner. When non-null, destroying
// the runner moves a still-valid SDL window/renderer/context back into it after
// gameplay-owned audio, Lua, gamepads and scene resources have been closed.
[[nodiscard]] std::unique_ptr<ApplicationRunner> make_gameplay_application(
    AppLaunchOptions options,
    TransferredPlatform platform,
    TransferredPlatform* return_platform
);

[[nodiscard]] std::unique_ptr<ApplicationRunner> make_gameplay_application(
    AppLaunchOptions options,
    TransferredPlatform platform,
    TransferredPlatform* return_platform,
    std::shared_ptr<DiscordPresenceSession> discord_session
);

}  // namespace pulseforge::detail
