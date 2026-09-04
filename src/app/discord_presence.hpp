#pragma once

#include "pulseforge/runtime_telemetry.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace pulseforge::detail {

enum class DiscordAccountLinkState : std::uint8_t {
    unavailable,
    unlinked,
    authorizing,
    exchanging_token,
    connecting,
    linked,
    failed,
};

[[nodiscard]] std::string_view discord_account_link_state_name(
    DiscordAccountLinkState state
) noexcept;

// PULSEFORGE_P1_5_0F_DISCORD_PRESENCE_BACKEND_V1
// The implementation is always linkable. Without the optional Discord Social
// SDK it becomes a no-op, preserving startup/gameplay/shutdown behavior.
class DiscordPresenceSession final {
public:
    DiscordPresenceSession();
    ~DiscordPresenceSession();

    DiscordPresenceSession(const DiscordPresenceSession&) = delete;
    DiscordPresenceSession& operator=(const DiscordPresenceSession&) = delete;
    DiscordPresenceSession(DiscordPresenceSession&&) = delete;
    DiscordPresenceSession& operator=(DiscordPresenceSession&&) = delete;

    void shutdown() noexcept;

private:
    friend class DiscordPresencePublisher;
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

[[nodiscard]] std::uint64_t discord_effective_application_id(
    const DiscordPresenceSettings& settings
) noexcept;

class DiscordPresencePublisher final {
public:
    DiscordPresencePublisher();
    explicit DiscordPresencePublisher(
        std::shared_ptr<DiscordPresenceSession> session
    );
    ~DiscordPresencePublisher();
    DiscordPresencePublisher(DiscordPresencePublisher&&) noexcept;
    DiscordPresencePublisher& operator=(DiscordPresencePublisher&&) noexcept;

    DiscordPresencePublisher(const DiscordPresencePublisher&) = delete;
    DiscordPresencePublisher& operator=(const DiscordPresencePublisher&) = delete;

    void publish(
        const RuntimeTelemetrySnapshot& snapshot,
        const DiscordPresenceSettings& settings
    ) noexcept;
    void pump() noexcept;
    void clear() noexcept;
    void request_account_link(const DiscordPresenceSettings& settings) noexcept;
    void unlink_account(const DiscordPresenceSettings& settings) noexcept;
    [[nodiscard]] bool open_connected_games_settings(
        const DiscordPresenceSettings& settings
    ) noexcept;
    [[nodiscard]] DiscordAccountLinkState account_link_state() const noexcept;
    [[nodiscard]] std::string account_link_message() const;
    [[nodiscard]] bool backend_available() const noexcept;
    [[nodiscard]] std::shared_ptr<DiscordPresenceSession> session() const noexcept;

private:
    std::shared_ptr<DiscordPresenceSession> session_;
};

}  // namespace pulseforge::detail
