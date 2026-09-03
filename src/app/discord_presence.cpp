#include "discord_presence.hpp"
#include "discord_token_store.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
#define DISCORDPP_IMPLEMENTATION
#include <discordpp.h>
#endif

namespace pulseforge::detail {
namespace {

[[nodiscard]] std::uint64_t unix_time_ms() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return milliseconds < 0 ? 0U : static_cast<std::uint64_t>(milliseconds);
}

[[nodiscard]] std::uint64_t monotonic_time_ms() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return milliseconds < 0 ? 0U : static_cast<std::uint64_t>(milliseconds);
}

[[nodiscard]] bool interval_elapsed(
    const std::uint64_t now,
    const std::uint64_t previous,
    const std::uint32_t interval
) noexcept {
    return now < previous || now - previous >= interval;
}

#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
[[nodiscard]] std::optional<std::string> discord_optional(
    const std::string& text,
    const std::size_t minimum_size = 1U
) {
    return text.size() >= minimum_size
        ? std::optional<std::string>{text}
        : std::nullopt;
}

[[nodiscard]] std::uint32_t discord_result_retry_after_ms(
    const discordpp::ClientResult& result
) noexcept {
    const float retry_after_seconds = result.RetryAfter();
    if (!std::isfinite(retry_after_seconds) || retry_after_seconds <= 0.0F) {
        return 0U;
    }
    const long double milliseconds =
        static_cast<long double>(retry_after_seconds) * 1'000.0L;
    return static_cast<std::uint32_t>(std::clamp(
        milliseconds,
        0.0L,
        static_cast<long double>(60'000U)
    ));
}

[[nodiscard]] bool discord_result_retryable(
    const discordpp::ClientResult& result
) noexcept {
    return result.Retryable()
        || result.Type() == discordpp::ErrorType::NetworkError
        || result.Type() == discordpp::ErrorType::ClientNotReady
        || result.Type() == discordpp::ErrorType::Disabled;
}

struct DiscordCallbackFeedback final {
    std::atomic<std::uint64_t> active_generation{};
    std::atomic<std::uint64_t> successful_generation{};
    std::atomic<std::uint64_t> failed_generation{};
    std::atomic<bool> retryable{};
    std::atomic<std::uint32_t> retry_after_ms{};
};

struct DiscordAuthFeedback final {
    std::atomic<DiscordAccountLinkState> state{DiscordAccountLinkState::unlinked};
    std::atomic<std::uint64_t> authorization_generation{};
    std::atomic<std::uint64_t> client_generation{};
    std::atomic<std::uint64_t> connection_epoch{};
    std::atomic<bool> sdk_ready{};
    std::atomic<bool> restore_retry_requested{};
    std::atomic<std::uint32_t> restore_retry_after_ms{};
    mutable std::mutex mutex;
    std::string error;
    std::string persistence_warning;
    std::string presence_diagnostic;
    std::string access_token;
    std::string refresh_token;
    std::uint64_t application_id{};
};

void set_auth_state(
    const std::shared_ptr<DiscordAuthFeedback>& feedback,
    const DiscordAccountLinkState state,
    std::string error = {}
) noexcept {
    try {
        {
            const std::scoped_lock lock(feedback->mutex);
            feedback->error = std::move(error);
        }
        feedback->state.store(state, std::memory_order_release);
    } catch (...) {
        feedback->state.store(DiscordAccountLinkState::failed, std::memory_order_release);
    }
}

void set_persistence_warning(
    const std::shared_ptr<DiscordAuthFeedback>& feedback,
    std::string warning
) noexcept {
    try {
        const std::scoped_lock lock(feedback->mutex);
        feedback->persistence_warning = std::move(warning);
    } catch (...) {
    }
}

void set_presence_diagnostic(
    const std::shared_ptr<DiscordAuthFeedback>& feedback,
    std::string message
) noexcept {
    try {
        const std::scoped_lock lock(feedback->mutex);
        feedback->presence_diagnostic = std::move(message);
    } catch (...) {
    }
}

void schedule_saved_login_retry(
    const std::shared_ptr<DiscordAuthFeedback>& feedback,
    const discordpp::ClientResult& result,
    const std::string_view context
) noexcept {
    try {
        if (!discord_result_retryable(result)) {
            set_auth_state(
                feedback,
                DiscordAccountLinkState::failed,
                std::string(context) + ": " + result.ToString()
            );
            return;
        }
        feedback->restore_retry_after_ms.store(
            discord_result_retry_after_ms(result),
            std::memory_order_relaxed
        );
        feedback->restore_retry_requested.store(true, std::memory_order_release);
        set_auth_state(
            feedback,
            DiscordAccountLinkState::connecting,
            std::string(context)
                + "; waiting for Discord/network and retrying automatically"
        );
    } catch (...) {
        feedback->restore_retry_requested.store(true, std::memory_order_release);
        set_auth_state(feedback, DiscordAccountLinkState::connecting);
    }
}

void persist_refresh_token(
    const std::shared_ptr<DiscordAuthFeedback>& feedback,
    const std::uint64_t application_id,
    const std::string_view refresh_token
) noexcept {
    if (refresh_token.empty() || application_id == 0U) return;
    std::string error;
    if (discord_secure_token_save(application_id, refresh_token, &error)) {
        set_persistence_warning(feedback, {});
    } else {
        set_persistence_warning(
            feedback,
            error.empty()
                ? "Discord linked for this launch only; secure credential storage is unavailable"
                : "Discord linked for this launch only: " + error
        );
    }
}

void best_effort_secure_clear(std::string& value) noexcept {
    // std::string::clear() alone does not overwrite retained capacity. This is
    // best-effort only (SSO/copies may still exist). Durable refresh tokens are
    // stored separately in an OS-owned credential vault; access tokens are never
    // persisted by PulseForge.
    if (!value.empty()) {
        std::fill(value.begin(), value.end(), '\0');
    }
    value.clear();
}

[[nodiscard]] bool current_authorization(
    const std::shared_ptr<DiscordAuthFeedback>& feedback,
    const std::uint64_t application_id,
    const std::uint64_t authorization_generation,
    const std::uint64_t client_generation
) noexcept {
    if (feedback->authorization_generation.load(std::memory_order_acquire)
            != authorization_generation
        || feedback->client_generation.load(std::memory_order_acquire)
            != client_generation) {
        return false;
    }
    try {
        const std::scoped_lock lock(feedback->mutex);
        return feedback->application_id == application_id;
    } catch (...) {
        return false;
    }
}

#if defined(__ANDROID__)
[[nodiscard]] std::string mobile_redirect_scheme(const std::string_view uri) {
    constexpr std::string_view suffix = ":/authorize/callback";
    return uri.ends_with(suffix)
        ? std::string(uri.substr(0U, uri.size() - suffix.size()))
        : std::string{};
}
#endif
#endif

}  // namespace

std::uint64_t discord_effective_application_id(
    const DiscordPresenceSettings& settings
) noexcept {
    if (settings.application_id != 0U) return settings.application_id;
#if defined(PULSEFORGE_DISCORD_APPLICATION_ID)
    return static_cast<std::uint64_t>(PULSEFORGE_DISCORD_APPLICATION_ID);
#else
    return 0U;
#endif
}

class DiscordPresenceSession::Impl final {
public:
    void publish(
        const RuntimeTelemetrySnapshot& snapshot,
        const DiscordPresenceSettings& settings
    ) noexcept {
        const auto monotonic_ms = monotonic_time_ms();
#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
        pump_callbacks(monotonic_ms);
        consume_callback_feedback(monotonic_ms, settings);
#endif
        const auto application_id = discord_effective_application_id(settings);
        if (!settings.enabled || application_id == 0U) {
            if (published_ || last_payload_.has_value()) clear();
            return;
        }

        DiscordPresencePayload payload;
        try {
            payload = build_discord_presence_payload(
                snapshot,
                settings,
                unix_time_ms()
            );
        } catch (...) {
            return;
        }

        bool configuration_changed = last_settings_.has_value()
            && discord_presence_configuration_change(*last_settings_, settings);
#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
        const auto connection_epoch = auth_feedback_->connection_epoch.load(
            std::memory_order_acquire
        );
        configuration_changed = configuration_changed
            || connection_epoch != last_connection_epoch_;
#endif
        const bool structural_changed = !last_snapshot_.has_value()
            || !last_settings_.has_value()
            || discord_presence_structural_change(*last_snapshot_, snapshot);
        const bool payload_equal = last_payload_.has_value()
            && *last_payload_ == payload;

#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
        if ((retry_suppressed_ || retry_waiting(monotonic_ms))
            && !structural_changed && !configuration_changed) {
            return;
        }
        if (structural_changed || configuration_changed) {
            retry_due_ms_ = 0U;
            retry_suppressed_ = false;
        }
#endif

        const bool publish_interval_passed = interval_elapsed(
            monotonic_ms,
            last_publish_ms_,
            settings.publish_interval_ms
        );
        if (!discord_presence_should_publish(
                published_,
                payload_equal,
                structural_changed,
                configuration_changed,
                publish_interval_passed
            )) {
            return;
        }

#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
        if (!ensure_client(application_id)) {
            remember_attempt(snapshot, settings, payload, monotonic_ms, false);
            schedule_retry(monotonic_ms, settings, true, 0U);
            return;
        }
        // The Social SDK is only safe for backend-backed operations once the
        // websocket has reached Ready. Queue the latest payload instead of
        // relying on the desktop-only unauthenticated RPC path, which can make
        // presence appear briefly and then vanish when OAuth connects.
        if (!auth_feedback_->sdk_ready.load(std::memory_order_acquire)
            || client_->GetStatus() != discordpp::Client::Status::Ready) {
            remember_attempt(snapshot, settings, payload, monotonic_ms, false);
            return;
        }
        try {
            discordpp::Activity activity{};
            activity.SetType(discordpp::ActivityTypes::Playing);
            // Discord can choose Name, State or Details for the compact user
            // status line. Use Details by default so chart/difficulty/progress
            // and BOTPLAY are visible without opening the full profile card.
            // The expanded Rich Presence card still receives both details and
            // state, where note counters and gameplay statistics are shown.
            activity.SetStatusDisplayType(discordpp::StatusDisplayTypes::Details);
            activity.SetDetails(discord_optional(payload.details, 2U));
            activity.SetState(discord_optional(payload.state, 2U));

            if (settings.advanced_customization) {
                if (!payload.activity_name.empty()) {
                    activity.SetName(payload.activity_name);
                }
                activity.SetDetailsUrl(discord_optional(payload.details_url, 2U));
                activity.SetStateUrl(discord_optional(payload.state_url, 2U));
            }

            if (settings.advanced_customization
                && (!payload.large_image_key.empty()
                    || !payload.small_image_key.empty()
                    || !payload.large_image_url.empty()
                    || !payload.small_image_url.empty())) {
                discordpp::ActivityAssets assets{};
                if (!payload.large_image_key.empty()) {
                    assets.SetLargeImage(discord_optional(payload.large_image_key));
                    assets.SetLargeText(discord_optional(payload.large_image_text, 2U));
                }
                if (!payload.large_image_url.empty()) {
                    assets.SetLargeUrl(discord_optional(payload.large_image_url));
                }
                if (!payload.small_image_key.empty()) {
                    assets.SetSmallImage(discord_optional(payload.small_image_key));
                    assets.SetSmallText(discord_optional(payload.small_image_text, 2U));
                }
                if (!payload.small_image_url.empty()) {
                    assets.SetSmallUrl(discord_optional(payload.small_image_url));
                }
                activity.SetAssets(std::move(assets));
            }

            if (settings.advanced_customization) {
                for (std::size_t index = 0U; index < payload.button_count; ++index) {
                    discordpp::ActivityButton button{};
                    button.SetLabel(payload.buttons[index].label);
                    button.SetUrl(payload.buttons[index].url);
                    activity.AddButton(std::move(button));
                }
            }

            if (payload.start_unix_ms.has_value()
                || payload.end_unix_ms.has_value()) {
                discordpp::ActivityTimestamps timestamps{};
                if (payload.start_unix_ms.has_value()) {
                    // Discord's public Activity API uses Unix seconds. The
                    // backend-neutral payload retains millisecond precision.
                    timestamps.SetStart(*payload.start_unix_ms / 1'000U);
                }
                if (payload.end_unix_ms.has_value()) {
                    timestamps.SetEnd(*payload.end_unix_ms / 1'000U);
                }
                activity.SetTimestamps(std::move(timestamps));
            }

            const auto generation = next_generation();
            callback_feedback_->active_generation.store(
                generation,
                std::memory_order_release
            );
            const auto feedback = callback_feedback_;
            const auto auth_feedback = auth_feedback_;
            client_->UpdateRichPresence(
                std::move(activity),
                [feedback, auth_feedback, generation](discordpp::ClientResult result) {
                    if (feedback->active_generation.load(std::memory_order_acquire)
                        != generation) {
                        return;
                    }
                    if (result.Successful()) {
                        set_presence_diagnostic(
                            auth_feedback,
                            "Rich Presence accepted by Discord"
                        );
                        feedback->successful_generation.store(
                            generation,
                            std::memory_order_release
                        );
                        return;
                    }
                    try {
                        std::string message;
                        if (result.Type() == discordpp::ErrorType::ValidationError) {
                            // ValidationError carries its useful explanation in
                            // Error(); put it first so narrow in-game UI does not
                            // hide it behind unrelated overlays.
                            message = "RP validation failed: ";
                            message += result.Error();
                        } else {
                            message = "Rich Presence update failed";
                            const auto result_text = result.ToString();
                            if (!result_text.empty()) {
                                message += ": ";
                                message += result_text;
                            }
                        }
                        if (result.ErrorCode() != 0) {
                            message += " [code ";
                            message += std::to_string(result.ErrorCode());
                            message += ']';
                        }
                        const auto response = result.ResponseBody();
                        if (!response.empty()) {
                            constexpr std::size_t response_limit = 220U;
                            message += " - ";
                            message += response.substr(
                                0U,
                                std::min(response.size(), response_limit)
                            );
                        }
                        set_presence_diagnostic(auth_feedback, std::move(message));
                    } catch (...) {
                        set_presence_diagnostic(
                            auth_feedback,
                            "Rich Presence update failed"
                        );
                    }
                    const bool retryable = discord_result_retryable(result);
                    feedback->retryable.store(retryable, std::memory_order_relaxed);
                    feedback->retry_after_ms.store(
                        discord_result_retry_after_ms(result),
                        std::memory_order_relaxed
                    );
                    feedback->failed_generation.store(
                        generation,
                        std::memory_order_release
                    );
                }
            );
            remember_attempt(snapshot, settings, payload, monotonic_ms, true);
        } catch (...) {
            remember_attempt(snapshot, settings, payload, monotonic_ms, false);
            invalidate_callbacks();
            client_.reset();
            application_id_ = 0U;
            auth_feedback_->sdk_ready.store(false, std::memory_order_release);
            set_auth_state(
                auth_feedback_,
                DiscordAccountLinkState::connecting,
                "Discord presence transport reset; retrying automatically"
            );
            schedule_retry(monotonic_ms, settings, true, 0U);
            return;
        }
#else
        // No SDK: retain no fake success state. Payload generation remains
        // available for tests/telemetry, while the engine behaves normally.
        return;
#endif
    }

    void pump() noexcept {
#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
        const auto now = monotonic_time_ms();
        pump_callbacks(now);
        if (last_settings_.has_value()) {
            consume_callback_feedback(now, *last_settings_);
        }
        service_saved_login_retry(now);
        republish_after_connection_transition();
        service_presence_retry(now);
#endif
    }

    void clear() noexcept {
#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
        invalidate_callbacks();
        try {
            if (client_ != nullptr) client_->ClearRichPresence();
        } catch (...) {
        }
        retry_due_ms_ = 0U;
        retry_suppressed_ = false;
        consecutive_failures_ = 0U;
        set_presence_diagnostic(auth_feedback_, {});
#endif
        published_ = false;
        last_snapshot_.reset();
        last_settings_.reset();
        last_payload_.reset();
        last_publish_ms_ = 0U;
    }

    void request_account_link(const DiscordPresenceSettings& settings) noexcept {
#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
        pump();
        const auto application_id = discord_effective_application_id(settings);
        if (application_id == 0U) {
            set_auth_state(
                auth_feedback_,
                DiscordAccountLinkState::failed,
                "Configure a Discord Application ID first"
            );
            return;
        }
#if defined(__ANDROID__)
        constexpr bool mobile_platform = true;
#else
        constexpr bool mobile_platform = false;
#endif
        if (!discord_oauth_redirect_uri_compatible(
                settings.oauth_redirect_uri,
                mobile_platform
            )) {
            set_auth_state(
                auth_feedback_,
                DiscordAccountLinkState::failed,
                mobile_platform
                    ? "Android OAuth requires an application URI scheme ending in :/authorize/callback"
                    : "Desktop OAuth requires http://127.0.0.1/callback"
            );
            return;
        }
        const auto current = auth_feedback_->state.load(std::memory_order_acquire);
        if (current != DiscordAccountLinkState::linked) {
            set_presence_diagnostic(auth_feedback_, {});
        }
        if (current == DiscordAccountLinkState::authorizing
            || current == DiscordAccountLinkState::exchanging_token
            || current == DiscordAccountLinkState::connecting
            || current == DiscordAccountLinkState::linked) {
            return;
        }
        if (!ensure_client(application_id)) {
            set_auth_state(
                auth_feedback_,
                DiscordAccountLinkState::failed,
                "Discord Social SDK could not initialize"
            );
            return;
        }
        try {
            const auto client = client_;
            const auto feedback = auth_feedback_;
            auto verifier = client->CreateAuthorizationCodeVerifier();
            discordpp::AuthorizationArgs args{};
            args.SetClientId(application_id);
            args.SetScopes(discordpp::Client::GetDefaultPresenceScopes());
            args.SetCodeChallenge(verifier.Challenge());
#if defined(__ANDROID__)
            args.SetCustomSchemeParam(
                settings.oauth_redirect_uri.empty()
                    ? std::string{"discord-"} + std::to_string(application_id)
                    : mobile_redirect_scheme(settings.oauth_redirect_uri)
            );
#endif
            {
                const std::scoped_lock lock(feedback->mutex);
                feedback->application_id = application_id;
                best_effort_secure_clear(feedback->access_token);
                best_effort_secure_clear(feedback->refresh_token);
            }
            const auto client_generation = feedback->client_generation.load(
                std::memory_order_acquire
            );
            const auto authorization_generation =
                feedback->authorization_generation.fetch_add(
                    1U, std::memory_order_acq_rel
                ) + 1U;
            set_auth_state(feedback, DiscordAccountLinkState::authorizing);
            client->Authorize(
                std::move(args),
                [client, feedback, verifier, application_id,
                 authorization_generation, client_generation](
                    discordpp::ClientResult result,
                    std::string code,
                    std::string redirect_uri
                ) mutable {
                    if (!current_authorization(
                            feedback, application_id, authorization_generation,
                            client_generation
                        )) {
                        return;
                    }
                    if (!result.Successful()) {
                        set_auth_state(
                            feedback,
                            DiscordAccountLinkState::failed,
                            result.ToString()
                        );
                        return;
                    }
                    set_auth_state(
                        feedback,
                        DiscordAccountLinkState::exchanging_token
                    );
                    client->GetToken(
                        application_id,
                        code,
                        verifier.Verifier(),
                        redirect_uri,
                        [client, feedback, application_id,
                         authorization_generation, client_generation](
                            discordpp::ClientResult token_result,
                            std::string access_token,
                            std::string refresh_token,
                            discordpp::AuthorizationTokenType token_type,
                            std::int32_t,
                            std::string
                        ) {
                            if (!current_authorization(
                                    feedback, application_id,
                                    authorization_generation, client_generation
                                )) {
                                return;
                            }
                            if (!token_result.Successful()) {
                                set_auth_state(
                                    feedback,
                                    DiscordAccountLinkState::failed,
                                    token_result.ToString()
                                );
                                return;
                            }
                            std::string refresh_for_store;
                            {
                                const std::scoped_lock lock(feedback->mutex);
                                feedback->access_token = access_token;
                                feedback->refresh_token = std::move(refresh_token);
                                refresh_for_store = feedback->refresh_token;
                            }
                            persist_refresh_token(
                                feedback,
                                application_id,
                                refresh_for_store
                            );
                            best_effort_secure_clear(refresh_for_store);
                            set_auth_state(
                                feedback,
                                DiscordAccountLinkState::connecting
                            );
                            client->UpdateToken(
                                token_type,
                                std::move(access_token),
                                [client, feedback, application_id,
                                 authorization_generation, client_generation](
                                    discordpp::ClientResult update_result
                                ) {
                                    if (!current_authorization(
                                            feedback, application_id,
                                            authorization_generation, client_generation
                                        )) {
                                        return;
                                    }
                                    if (!update_result.Successful()) {
                                        set_auth_state(
                                            feedback,
                                            DiscordAccountLinkState::failed,
                                            update_result.ToString()
                                        );
                                        return;
                                    }
                                    client->Connect();
                                }
                            );
                        }
                    );
                }
            );
        } catch (...) {
            set_auth_state(
                auth_feedback_,
                DiscordAccountLinkState::failed,
                "Discord authorization could not be started"
            );
        }
#else
        static_cast<void>(settings);
#endif
    }

    void unlink_account(const DiscordPresenceSettings& settings) noexcept {
        const auto effective_application_id = discord_effective_application_id(settings);
        std::string persistence_error;
        const bool erased_persisted_login = discord_secure_token_erase(
            effective_application_id,
            &persistence_error
        );
#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
        const auto publish_erase_result = [&]() noexcept {
            if (!erased_persisted_login) {
                set_persistence_warning(
                    auth_feedback_,
                    persistence_error.empty()
                        ? "Discord disconnected, but the saved login could not be erased"
                        : "Discord disconnected, but the saved login could not be erased: "
                            + persistence_error
                );
            } else {
                set_persistence_warning(auth_feedback_, {});
            }
        };
        try {
            if (client_ == nullptr) {
                set_auth_state(auth_feedback_, DiscordAccountLinkState::unlinked);
                publish_erase_result();
                return;
            }
            const auto feedback = auth_feedback_;
            const auto client_generation = feedback->client_generation.load(
                std::memory_order_acquire
            );
            const auto authorization_generation =
                feedback->authorization_generation.fetch_add(
                    1U, std::memory_order_acq_rel
                ) + 1U;
            if (feedback->state.load(std::memory_order_acquire)
                == DiscordAccountLinkState::authorizing) {
                client_->AbortAuthorize();
            }
            std::string token;
            std::uint64_t application_id = 0U;
            {
                const std::scoped_lock lock(auth_feedback_->mutex);
                token = auth_feedback_->refresh_token.empty()
                    ? auth_feedback_->access_token
                    : auth_feedback_->refresh_token;
                application_id = auth_feedback_->application_id;
                best_effort_secure_clear(auth_feedback_->access_token);
                best_effort_secure_clear(auth_feedback_->refresh_token);
                auth_feedback_->error.clear();
                auth_feedback_->presence_diagnostic.clear();
            }
            const auto client = client_;
            if (!token.empty() && application_id != 0U) {
                client->RevokeToken(
                    application_id,
                    token,
                    [client, feedback, authorization_generation, client_generation](
                        discordpp::ClientResult
                    ) {
                        if (feedback->authorization_generation.load(
                                std::memory_order_acquire
                            ) != authorization_generation
                            || feedback->client_generation.load(
                                std::memory_order_acquire
                            ) != client_generation) {
                            return;
                        }
                        client->Disconnect();
                        feedback->connection_epoch.fetch_add(
                            1U, std::memory_order_acq_rel
                        );
                    }
                );
                best_effort_secure_clear(token);
            } else {
                client->Disconnect();
                feedback->connection_epoch.fetch_add(
                    1U, std::memory_order_acq_rel
                );
            }
            auth_feedback_->state.store(
                DiscordAccountLinkState::unlinked,
                std::memory_order_release
            );
            publish_erase_result();
        } catch (...) {
            set_auth_state(
                auth_feedback_,
                DiscordAccountLinkState::failed,
                "Discord account unlink failed"
            );
        }
#else
        static_cast<void>(erased_persisted_login);
        static_cast<void>(persistence_error);
#endif
    }

    [[nodiscard]] bool open_connected_games_settings(
        const DiscordPresenceSettings& settings
    ) noexcept {
#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
        if (discord_effective_application_id(settings) == 0U
            || auth_feedback_->state.load(std::memory_order_acquire)
                != DiscordAccountLinkState::linked
            || client_ == nullptr) {
            return false;
        }
        try {
            client_->OpenConnectedGamesSettingsInDiscord(
                [](discordpp::ClientResult) {}
            );
            return true;
        } catch (...) {
            return false;
        }
#else
        static_cast<void>(settings);
        return false;
#endif
    }

    [[nodiscard]] DiscordAccountLinkState account_link_state() const noexcept {
#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
        return auth_feedback_->state.load(std::memory_order_acquire);
#else
        return DiscordAccountLinkState::unavailable;
#endif
    }

    [[nodiscard]] std::string account_link_message() const {
#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
        const std::scoped_lock lock(auth_feedback_->mutex);
        if (!auth_feedback_->error.empty()) return auth_feedback_->error;

        // Presence diagnostics are the most useful live signal when account
        // linking has already succeeded. Put them first so the bounded menu
        // footer does not hide UpdateRichPresence() success/failure behind a
        // longer credential-persistence warning.
        if (!auth_feedback_->presence_diagnostic.empty()) {
            if (auth_feedback_->persistence_warning.empty()) {
                return auth_feedback_->presence_diagnostic;
            }
            return auth_feedback_->presence_diagnostic
                + " | " + auth_feedback_->persistence_warning;
        }
        if (!auth_feedback_->persistence_warning.empty()) {
            return auth_feedback_->persistence_warning;
        }
        return {};
#else
        return "Discord Social SDK is not included in this build";
#endif
    }

    [[nodiscard]] bool backend_available() const noexcept {
#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
        return client_ != nullptr;
#else
        return false;
#endif
    }

    void shutdown() noexcept {
#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
        invalidate_callbacks();
        const auto feedback = auth_feedback_;
        feedback->authorization_generation.fetch_add(1U, std::memory_order_acq_rel);
        feedback->client_generation.fetch_add(1U, std::memory_order_acq_rel);
        try {
            if (client_ != nullptr) {
                try { client_->AbortAuthorize(); } catch (...) {}
                try { client_->ClearRichPresence(); } catch (...) {}
                try { client_->Disconnect(); } catch (...) {}
            }
        } catch (...) {
        }
        {
            try {
                const std::scoped_lock lock(feedback->mutex);
                best_effort_secure_clear(feedback->access_token);
                best_effort_secure_clear(feedback->refresh_token);
                feedback->error.clear();
                feedback->persistence_warning.clear();
                feedback->presence_diagnostic.clear();
                feedback->application_id = 0U;
            } catch (...) {
            }
        }
        feedback->state.store(DiscordAccountLinkState::unlinked, std::memory_order_release);
        feedback->sdk_ready.store(false, std::memory_order_release);
        feedback->restore_retry_requested.store(false, std::memory_order_release);
        feedback->restore_retry_after_ms.store(0U, std::memory_order_relaxed);
        feedback->connection_epoch.fetch_add(1U, std::memory_order_acq_rel);
        client_.reset();
        application_id_ = 0U;
        auth_retry_due_ms_ = 0U;
        auth_retry_failures_ = 0U;
        retry_due_ms_ = 0U;
        retry_suppressed_ = false;
        consecutive_failures_ = 0U;
#endif
        published_ = false;
        last_snapshot_.reset();
        last_settings_.reset();
        last_payload_.reset();
        last_publish_ms_ = 0U;
    }

private:
#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
    [[nodiscard]] std::uint64_t next_generation() noexcept {
        if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
            generation_ = 1U;
        } else {
            ++generation_;
            if (generation_ == 0U) generation_ = 1U;
        }
        return generation_;
    }

    void invalidate_callbacks() noexcept {
        callback_feedback_->active_generation.store(
            next_generation(),
            std::memory_order_release
        );
        callback_feedback_->successful_generation.store(0U, std::memory_order_relaxed);
        callback_feedback_->failed_generation.store(0U, std::memory_order_relaxed);
    }

    void restore_persisted_account(
        const std::uint64_t application_id,
        const std::uint64_t client_generation
    ) noexcept {
        if (client_ == nullptr || application_id == 0U) return;
        std::string storage_error;
        std::optional<std::string> stored_refresh;
        try {
            const std::scoped_lock lock(auth_feedback_->mutex);
            if (!auth_feedback_->refresh_token.empty()) {
                stored_refresh = auth_feedback_->refresh_token;
            }
        } catch (...) {
        }
        if (!stored_refresh.has_value() || stored_refresh->empty()) {
            stored_refresh = discord_secure_token_load(
                application_id,
                &storage_error
            );
        }
        if (!stored_refresh.has_value() || stored_refresh->empty()) {
            if (!storage_error.empty()) {
                set_persistence_warning(
                    auth_feedback_,
                    "Discord saved login is unavailable: " + storage_error
                );
            }
            return;
        }

        const auto client = client_;
        const auto feedback = auth_feedback_;
        const auto authorization_generation =
            feedback->authorization_generation.fetch_add(
                1U, std::memory_order_acq_rel
            ) + 1U;
        try {
            {
                const std::scoped_lock lock(feedback->mutex);
                best_effort_secure_clear(feedback->refresh_token);
                feedback->refresh_token = *stored_refresh;
            }
            set_auth_state(
                feedback,
                DiscordAccountLinkState::exchanging_token
            );
            client->RefreshToken(
                application_id,
                *stored_refresh,
                [client, feedback, application_id,
                 authorization_generation, client_generation](
                    discordpp::ClientResult result,
                    std::string access_token,
                    std::string refresh_token_new,
                    discordpp::AuthorizationTokenType token_type,
                    std::int32_t,
                    std::string
                ) {
                    if (!current_authorization(
                            feedback,
                            application_id,
                            authorization_generation,
                            client_generation
                        )) {
                        return;
                    }
                    if (!result.Successful()) {
                        schedule_saved_login_retry(
                            feedback,
                            result,
                            "Saved Discord login could not be refreshed"
                        );
                        return;
                    }
                    std::string refresh_for_store;
                    {
                        const std::scoped_lock lock(feedback->mutex);
                        best_effort_secure_clear(feedback->access_token);
                        feedback->access_token = access_token;
                        if (!refresh_token_new.empty()) {
                            best_effort_secure_clear(feedback->refresh_token);
                            feedback->refresh_token = std::move(refresh_token_new);
                        }
                        refresh_for_store = feedback->refresh_token;
                    }
                    persist_refresh_token(
                        feedback,
                        application_id,
                        refresh_for_store
                    );
                    best_effort_secure_clear(refresh_for_store);
                    set_auth_state(
                        feedback,
                        DiscordAccountLinkState::connecting
                    );
                    client->UpdateToken(
                        token_type,
                        std::move(access_token),
                        [client, feedback, application_id,
                         authorization_generation, client_generation](
                            discordpp::ClientResult update_result
                        ) {
                            if (!current_authorization(
                                    feedback,
                                    application_id,
                                    authorization_generation,
                                    client_generation
                                )) {
                                return;
                            }
                            if (!update_result.Successful()) {
                                schedule_saved_login_retry(
                                    feedback,
                                    update_result,
                                    "Discord token could not be installed"
                                );
                                return;
                            }
                            client->Connect();
                        }
                    );
                }
            );
        } catch (...) {
            set_auth_state(
                feedback,
                DiscordAccountLinkState::failed,
                "Saved Discord login could not be restored"
            );
        }
        best_effort_secure_clear(*stored_refresh);
    }

    [[nodiscard]] bool ensure_client(const std::uint64_t application_id) noexcept {
        if (client_ != nullptr && application_id_ == application_id) return true;
        invalidate_callbacks();
        const auto feedback = auth_feedback_;
        feedback->authorization_generation.fetch_add(1U, std::memory_order_acq_rel);
        const auto client_generation = feedback->client_generation.fetch_add(
            1U, std::memory_order_acq_rel
        ) + 1U;
        try {
            if (client_ != nullptr) {
                try { client_->AbortAuthorize(); } catch (...) {}
                try { client_->Disconnect(); } catch (...) {}
            }
            {
                const std::scoped_lock lock(feedback->mutex);
                best_effort_secure_clear(feedback->access_token);
                best_effort_secure_clear(feedback->refresh_token);
                feedback->error.clear();
                feedback->persistence_warning.clear();
                feedback->presence_diagnostic.clear();
                feedback->application_id = application_id;
            }
            feedback->state.store(
                DiscordAccountLinkState::unlinked,
                std::memory_order_release
            );
            feedback->sdk_ready.store(false, std::memory_order_release);
            feedback->restore_retry_requested.store(false, std::memory_order_release);
            feedback->restore_retry_after_ms.store(0U, std::memory_order_relaxed);
            auth_retry_due_ms_ = 0U;
            auth_retry_failures_ = 0U;
            client_ = std::make_shared<discordpp::Client>();
            client_->SetApplicationId(application_id);
            const auto client = client_;
            client_->SetStatusChangedCallback(
                [feedback, client_generation](
                    const discordpp::Client::Status status,
                    const discordpp::Client::Error error,
                    std::int32_t
                ) {
                    if (feedback->client_generation.load(std::memory_order_acquire)
                        != client_generation) {
                        return;
                    }
                    feedback->sdk_ready.store(
                        status == discordpp::Client::Status::Ready,
                        std::memory_order_release
                    );
                    if (status == discordpp::Client::Status::Ready) {
                        feedback->restore_retry_requested.store(
                            false, std::memory_order_release
                        );
                        feedback->restore_retry_after_ms.store(
                            0U, std::memory_order_relaxed
                        );
                        feedback->connection_epoch.fetch_add(
                            1U, std::memory_order_acq_rel
                        );
                        set_presence_diagnostic(
                            feedback,
                            "Discord connected; publishing Rich Presence"
                        );
                        set_auth_state(feedback, DiscordAccountLinkState::linked);
                        return;
                    }

                    const auto link_state = feedback->state.load(
                        std::memory_order_acquire
                    );
                    if (link_state == DiscordAccountLinkState::unlinked
                        || link_state == DiscordAccountLinkState::authorizing
                        || link_state == DiscordAccountLinkState::exchanging_token) {
                        return;
                    }

                    switch (status) {
                    case discordpp::Client::Status::Connecting:
                    case discordpp::Client::Status::Connected:
                    case discordpp::Client::Status::Reconnecting:
                    case discordpp::Client::Status::HttpWait:
                        set_auth_state(
                            feedback,
                            DiscordAccountLinkState::connecting,
                            status == discordpp::Client::Status::Reconnecting
                                ? std::string{"Discord connection interrupted; reconnecting automatically"}
                                : std::string{}
                        );
                        break;
                    case discordpp::Client::Status::Disconnected:
                        feedback->restore_retry_after_ms.store(
                            0U, std::memory_order_relaxed
                        );
                        feedback->restore_retry_requested.store(
                            true, std::memory_order_release
                        );
                        set_auth_state(
                            feedback,
                            DiscordAccountLinkState::connecting,
                            error == discordpp::Client::Error::None
                                ? std::string{"Discord disconnected; retrying automatically"}
                                : std::string{"Discord is temporarily unavailable; retrying automatically ("}
                                    + discordpp::Client::ErrorToString(error) + ")"
                        );
                        break;
                    case discordpp::Client::Status::Disconnecting:
                    case discordpp::Client::Status::Ready:
                        break;
                    }
                }
            );
            const std::weak_ptr<discordpp::Client> weak_client{client_};
            client_->SetTokenExpirationCallback(
                [weak_client, feedback, client_generation]() {
                    const auto expiring_client = weak_client.lock();
                    if (expiring_client == nullptr
                        || feedback->client_generation.load(std::memory_order_acquire)
                            != client_generation) {
                        return;
                    }
                    std::string refresh_token;
                    std::uint64_t linked_application_id = 0U;
                    {
                        const std::scoped_lock lock(feedback->mutex);
                        refresh_token = feedback->refresh_token;
                        linked_application_id = feedback->application_id;
                    }
                    if (refresh_token.empty() || linked_application_id == 0U) {
                        set_auth_state(
                            feedback,
                            DiscordAccountLinkState::failed,
                            "Discord session expired; link the account again"
                        );
                        return;
                    }
                    const auto authorization_generation =
                        feedback->authorization_generation.fetch_add(
                            1U, std::memory_order_acq_rel
                        ) + 1U;
                    set_auth_state(feedback, DiscordAccountLinkState::exchanging_token);
                    expiring_client->RefreshToken(
                        linked_application_id,
                        refresh_token,
                        [client = expiring_client, feedback, client_generation,
                         application_id = linked_application_id,
                         authorization_generation](
                            discordpp::ClientResult result,
                            std::string access_token,
                            std::string refresh_token_new,
                            discordpp::AuthorizationTokenType token_type,
                            std::int32_t,
                            std::string
                        ) {
                            if (!current_authorization(
                                    feedback, application_id,
                                    authorization_generation, client_generation
                                )) {
                                return;
                            }
                            if (!result.Successful()) {
                                schedule_saved_login_retry(
                                    feedback,
                                    result,
                                    "Discord session refresh failed"
                                );
                                return;
                            }
                            std::string refresh_for_store;
                            {
                                const std::scoped_lock lock(feedback->mutex);
                                best_effort_secure_clear(feedback->access_token);
                                feedback->access_token = access_token;
                                if (!refresh_token_new.empty()) {
                                    best_effort_secure_clear(feedback->refresh_token);
                                    feedback->refresh_token = std::move(refresh_token_new);
                                }
                                refresh_for_store = feedback->refresh_token;
                            }
                            persist_refresh_token(
                                feedback,
                                application_id,
                                refresh_for_store
                            );
                            best_effort_secure_clear(refresh_for_store);
                            set_auth_state(feedback, DiscordAccountLinkState::connecting);
                            client->UpdateToken(
                                token_type,
                                std::move(access_token),
                                [client, feedback, application_id,
                                 authorization_generation, client_generation](
                                    discordpp::ClientResult update_result
                                ) {
                                    if (!current_authorization(
                                            feedback, application_id,
                                            authorization_generation, client_generation
                                        )) {
                                        return;
                                    }
                                    if (!update_result.Successful()) {
                                        schedule_saved_login_retry(
                                            feedback,
                                            update_result,
                                            "Discord refreshed token could not be installed"
                                        );
                                        return;
                                    }
                                    client->Connect();
                                }
                            );
                        }
                    );
                }
            );
            application_id_ = application_id;
            published_ = false;
            restore_persisted_account(application_id, client_generation);
            return true;
        } catch (...) {
            client_.reset();
            application_id_ = 0U;
            return false;
        }
    }

    void service_saved_login_retry(const std::uint64_t now) noexcept {
        if (auth_feedback_->restore_retry_requested.exchange(
                false, std::memory_order_acq_rel
            )) {
            if (auth_retry_failures_ != std::numeric_limits<std::uint32_t>::max()) {
                ++auth_retry_failures_;
            }
            const auto server_delay = auth_feedback_->restore_retry_after_ms.exchange(
                0U, std::memory_order_acq_rel
            );
            const auto exponent = std::min<std::uint32_t>(
                auth_retry_failures_ > 0U ? auth_retry_failures_ - 1U : 0U,
                5U
            );
            const auto exponential_delay = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(1'000ULL << exponent, 30'000ULL)
            );
            const auto delay = std::max(exponential_delay, server_delay);
            auth_retry_due_ms_ = now > std::numeric_limits<std::uint64_t>::max() - delay
                ? std::numeric_limits<std::uint64_t>::max()
                : now + delay;
        }

        if (auth_feedback_->sdk_ready.load(std::memory_order_acquire)) {
            auth_retry_due_ms_ = 0U;
            auth_retry_failures_ = 0U;
            return;
        }
        if (auth_retry_due_ms_ == 0U || now < auth_retry_due_ms_
            || client_ == nullptr || application_id_ == 0U) {
            return;
        }

        const auto state = auth_feedback_->state.load(std::memory_order_acquire);
        if (state == DiscordAccountLinkState::authorizing
            || state == DiscordAccountLinkState::exchanging_token) {
            return;
        }
        auth_retry_due_ms_ = 0U;
        restore_persisted_account(
            application_id_,
            auth_feedback_->client_generation.load(std::memory_order_acquire)
        );
    }

    void republish_after_connection_transition() noexcept {
        if (!auth_feedback_->sdk_ready.load(std::memory_order_acquire)
            || client_ == nullptr
            || last_snapshot_ == std::nullopt
            || last_settings_ == std::nullopt) {
            return;
        }
        const auto epoch = auth_feedback_->connection_epoch.load(
            std::memory_order_acquire
        );
        if (epoch == last_connection_epoch_) return;

        try {
            const auto snapshot = *last_snapshot_;
            const auto settings = *last_settings_;
            published_ = false;
            retry_due_ms_ = 0U;
            retry_suppressed_ = false;
            publish(snapshot, settings);
        } catch (...) {
            published_ = false;
        }
    }

    void service_presence_retry(const std::uint64_t now) noexcept {
        if (retry_suppressed_ || retry_due_ms_ == 0U || now < retry_due_ms_
            || !last_snapshot_.has_value() || !last_settings_.has_value()) {
            return;
        }
        try {
            const auto snapshot = *last_snapshot_;
            const auto settings = *last_settings_;
            // Clear the due timestamp before replaying. publish() will install a
            // new delay if the SDK/client is still unavailable.
            retry_due_ms_ = 0U;
            published_ = false;
            publish(snapshot, settings);
        } catch (...) {
            published_ = false;
        }
    }

    void pump_callbacks(const std::uint64_t monotonic_ms) noexcept {
        constexpr std::uint64_t callback_interval_ms = 100U;
        if (last_callback_pump_ms_ != 0U
            && !interval_elapsed(
                monotonic_ms,
                last_callback_pump_ms_,
                static_cast<std::uint32_t>(callback_interval_ms)
            )) {
            return;
        }
        try {
            discordpp::RunCallbacks();
        } catch (...) {
        }
        last_callback_pump_ms_ = monotonic_ms;
    }

    [[nodiscard]] bool retry_waiting(const std::uint64_t now) const noexcept {
        return retry_due_ms_ != 0U && now < retry_due_ms_;
    }

    void schedule_retry(
        const std::uint64_t now,
        const DiscordPresenceSettings& settings,
        const bool retryable,
        const std::uint32_t server_retry_after_ms
    ) noexcept {
        published_ = false;
        if (!retryable || !settings.retry_failed_updates) {
            retry_due_ms_ = 0U;
            retry_suppressed_ = true;
            return;
        }
        retry_suppressed_ = false;
        if (consecutive_failures_ != std::numeric_limits<std::uint32_t>::max()) {
            ++consecutive_failures_;
        }
        const auto delay = discord_presence_retry_delay_ms(
            consecutive_failures_,
            server_retry_after_ms
        );
        retry_due_ms_ = now > std::numeric_limits<std::uint64_t>::max() - delay
            ? std::numeric_limits<std::uint64_t>::max()
            : now + delay;
    }

    void consume_callback_feedback(
        const std::uint64_t now,
        const DiscordPresenceSettings& settings
    ) noexcept {
        const auto active = callback_feedback_->active_generation.load(
            std::memory_order_acquire
        );
        const auto successful = callback_feedback_->successful_generation.exchange(
            0U,
            std::memory_order_acq_rel
        );
        if (successful != 0U && successful == active) {
            consecutive_failures_ = 0U;
            retry_due_ms_ = 0U;
            retry_suppressed_ = false;
            published_ = true;
        }

        const auto failed = callback_feedback_->failed_generation.exchange(
            0U,
            std::memory_order_acq_rel
        );
        if (failed == 0U || failed != active) return;
        const bool retryable = callback_feedback_->retryable.load(
            std::memory_order_relaxed
        );
        const auto retry_after = callback_feedback_->retry_after_ms.load(
            std::memory_order_relaxed
        );
        schedule_retry(now, settings, retryable, retry_after);
    }

    void remember_attempt(
        const RuntimeTelemetrySnapshot& snapshot,
        const DiscordPresenceSettings& settings,
        const DiscordPresencePayload& payload,
        const std::uint64_t monotonic_ms,
        const bool submitted
    ) noexcept {
        try {
            last_snapshot_ = snapshot;
            last_settings_ = settings;
            last_payload_ = payload;
            last_publish_ms_ = monotonic_ms;
#if defined(PULSEFORGE_HAS_DISCORD_SOCIAL_SDK)
            last_connection_epoch_ = auth_feedback_->connection_epoch.load(
                std::memory_order_acquire
            );
#endif
            published_ = submitted;
        } catch (...) {
            published_ = false;
        }
    }

    std::shared_ptr<discordpp::Client> client_;
    std::shared_ptr<DiscordCallbackFeedback> callback_feedback_{
        std::make_shared<DiscordCallbackFeedback>()
    };
    std::shared_ptr<DiscordAuthFeedback> auth_feedback_{
        std::make_shared<DiscordAuthFeedback>()
    };
    std::uint64_t application_id_{};
    std::uint64_t last_callback_pump_ms_{};
    std::uint64_t generation_{};
    std::uint64_t last_connection_epoch_{};
    std::uint64_t auth_retry_due_ms_{};
    std::uint32_t auth_retry_failures_{};
    std::uint64_t retry_due_ms_{};
    std::uint32_t consecutive_failures_{};
    bool retry_suppressed_{};
#endif
    std::optional<RuntimeTelemetrySnapshot> last_snapshot_;
    std::optional<DiscordPresenceSettings> last_settings_;
    std::optional<DiscordPresencePayload> last_payload_;
    std::uint64_t last_publish_ms_{};
    bool published_{};
};

DiscordPresenceSession::DiscordPresenceSession()
    : implementation_(std::make_unique<Impl>()) {}

DiscordPresenceSession::~DiscordPresenceSession() {
    shutdown();
}

void DiscordPresenceSession::shutdown() noexcept {
    if (implementation_ != nullptr) implementation_->shutdown();
}

DiscordPresencePublisher::DiscordPresencePublisher()
    : session_(std::make_shared<DiscordPresenceSession>()) {}

DiscordPresencePublisher::DiscordPresencePublisher(
    std::shared_ptr<DiscordPresenceSession> session
) : session_(session != nullptr
        ? std::move(session)
        : std::make_shared<DiscordPresenceSession>()) {}

DiscordPresencePublisher::~DiscordPresencePublisher() = default;
DiscordPresencePublisher::DiscordPresencePublisher(DiscordPresencePublisher&&) noexcept = default;
DiscordPresencePublisher& DiscordPresencePublisher::operator=(DiscordPresencePublisher&&) noexcept = default;

void DiscordPresencePublisher::publish(
    const RuntimeTelemetrySnapshot& snapshot,
    const DiscordPresenceSettings& settings
) noexcept {
    session_->implementation_->publish(snapshot, settings);
}

void DiscordPresencePublisher::pump() noexcept {
    session_->implementation_->pump();
}

void DiscordPresencePublisher::clear() noexcept {
    session_->implementation_->clear();
}

bool DiscordPresencePublisher::backend_available() const noexcept {
    return session_->implementation_->backend_available();
}

std::shared_ptr<DiscordPresenceSession> DiscordPresencePublisher::session() const noexcept {
    return session_;
}

void DiscordPresencePublisher::request_account_link(
    const DiscordPresenceSettings& settings
) noexcept {
    session_->implementation_->request_account_link(settings);
}

void DiscordPresencePublisher::unlink_account(
    const DiscordPresenceSettings& settings
) noexcept {
    session_->implementation_->unlink_account(settings);
}

bool DiscordPresencePublisher::open_connected_games_settings(
    const DiscordPresenceSettings& settings
) noexcept {
    return session_->implementation_->open_connected_games_settings(settings);
}

DiscordAccountLinkState DiscordPresencePublisher::account_link_state() const noexcept {
    return session_->implementation_->account_link_state();
}

std::string DiscordPresencePublisher::account_link_message() const {
    return session_->implementation_->account_link_message();
}

std::string_view discord_account_link_state_name(
    const DiscordAccountLinkState state
) noexcept {
    switch (state) {
    case DiscordAccountLinkState::unavailable: return "unavailable";
    case DiscordAccountLinkState::unlinked: return "not linked";
    case DiscordAccountLinkState::authorizing: return "authorizing in browser";
    case DiscordAccountLinkState::exchanging_token: return "finishing authorization";
    case DiscordAccountLinkState::connecting: return "connecting";
    case DiscordAccountLinkState::linked: return "linked";
    case DiscordAccountLinkState::failed: return "failed";
    }
    return "unavailable";
}

}  // namespace pulseforge::detail
