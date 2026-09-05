#!/usr/bin/env python3
"""Generate a public synthetic Discord-SDK-shaped fixture for PulseForge CI.

This is deliberately NOT a copy or reimplementation of Discord's proprietary
headers.  It declares only the tiny C++ surface that PulseForge itself calls,
with deterministic inline behavior, plus one external C marker symbol.  The
marker forces platform builds to link and load a generated DLL/SO/AAR runtime
instead of accidentally passing as a header-only compile.
"""
from __future__ import annotations

import argparse
from pathlib import Path


DISCORDPP_HEADER = r'''#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

extern "C" int discord_partner_sdk_fixture_marker(void);

namespace discordpp {

enum class ErrorType {
    None,
    NetworkError,
    ClientNotReady,
    Disabled,
    ValidationError,
};

enum class AuthorizationTokenType {
    Bearer,
};

enum class ActivityTypes {
    Playing,
};

enum class StatusDisplayTypes {
    Details,
};

class ClientResult {
public:
    ClientResult() = default;
    explicit ClientResult(bool successful) : successful_(successful) {}

    [[nodiscard]] bool Successful() const noexcept { return successful_; }
    [[nodiscard]] bool Retryable() const noexcept { return false; }
    [[nodiscard]] float RetryAfter() const noexcept { return 0.0F; }
    [[nodiscard]] ErrorType Type() const noexcept {
        return successful_ ? ErrorType::None : ErrorType::NetworkError;
    }
    [[nodiscard]] std::string ToString() const {
        return successful_ ? std::string{"synthetic success"}
                           : std::string{"synthetic failure"};
    }
    [[nodiscard]] std::string Error() const {
        return successful_ ? std::string{} : std::string{"synthetic failure"};
    }
    [[nodiscard]] std::int32_t ErrorCode() const noexcept {
        return successful_ ? 0 : 1;
    }
    [[nodiscard]] std::string ResponseBody() const { return {}; }

private:
    bool successful_{true};
};

class ActivityAssets {
public:
    void SetLargeImage(std::optional<std::string>) {}
    void SetLargeText(std::optional<std::string>) {}
    void SetLargeUrl(std::optional<std::string>) {}
    void SetSmallImage(std::optional<std::string>) {}
    void SetSmallText(std::optional<std::string>) {}
    void SetSmallUrl(std::optional<std::string>) {}
};

class ActivityButton {
public:
    void SetLabel(std::string) {}
    void SetUrl(std::string) {}
};

class ActivityTimestamps {
public:
    void SetStart(std::uint64_t) {}
    void SetEnd(std::uint64_t) {}
};

class Activity {
public:
    void SetType(ActivityTypes) {}
    void SetStatusDisplayType(StatusDisplayTypes) {}
    void SetDetails(std::optional<std::string>) {}
    void SetState(std::optional<std::string>) {}
    void SetName(std::string) {}
    void SetDetailsUrl(std::optional<std::string>) {}
    void SetStateUrl(std::optional<std::string>) {}
    void SetAssets(ActivityAssets) {}
    void AddButton(ActivityButton) {}
    void SetTimestamps(ActivityTimestamps) {}
};

class AuthorizationCodeVerifier {
public:
    [[nodiscard]] std::string Challenge() const { return "fixture-challenge"; }
    [[nodiscard]] std::string Verifier() const { return "fixture-verifier"; }
};

class AuthorizationArgs {
public:
    void SetClientId(std::uint64_t) {}

    template <typename T>
    void SetScopes(T&&) {}

    void SetCodeChallenge(std::string) {}
    void SetCustomSchemeParam(std::string) {}
};

class Client {
public:
    enum class Status {
        Disconnected,
        Connecting,
        Connected,
        Ready,
        Reconnecting,
        HttpWait,
        Disconnecting,
    };

    enum class Error {
        None,
        Synthetic,
    };

    using StatusCallback = std::function<void(Status, Error, std::int32_t)>;
    using TokenExpirationCallback = std::function<void()>;
    using ResultCallback = std::function<void(ClientResult)>;
    using AuthorizeCallback = std::function<void(
        ClientResult,
        std::string,
        std::string
    )>;
    using TokenCallback = std::function<void(
        ClientResult,
        std::string,
        std::string,
        AuthorizationTokenType,
        std::int32_t,
        std::string
    )>;

    Client() {
        // Forces every SDK-enabled PulseForge binary to retain a real dynamic
        // dependency on the generated fixture runtime.
        (void)discord_partner_sdk_fixture_marker();
    }

    static std::vector<std::string> GetDefaultPresenceScopes() {
        return {"identify", "rpc.activities.write"};
    }

    static std::string ErrorToString(Error error) {
        return error == Error::None ? "None" : "Synthetic";
    }

    void SetApplicationId(std::uint64_t application_id) noexcept {
        application_id_ = application_id;
    }

    void SetStatusChangedCallback(StatusCallback callback) {
        status_callback_ = std::move(callback);
    }

    void SetTokenExpirationCallback(TokenExpirationCallback callback) {
        expiration_callback_ = std::move(callback);
    }

    [[nodiscard]] AuthorizationCodeVerifier CreateAuthorizationCodeVerifier() const {
        return {};
    }

    void Authorize(AuthorizationArgs, AuthorizeCallback callback) {
        if (callback) {
            callback(
                ClientResult{true},
                "fixture-code",
                "http://127.0.0.1/callback"
            );
        }
    }

    void GetToken(
        std::uint64_t,
        std::string,
        std::string,
        std::string,
        TokenCallback callback
    ) {
        emit_tokens(std::move(callback));
    }

    void RefreshToken(
        std::uint64_t,
        std::string,
        TokenCallback callback
    ) {
        emit_tokens(std::move(callback));
    }

    void UpdateToken(
        AuthorizationTokenType,
        std::string,
        ResultCallback callback
    ) {
        if (callback) callback(ClientResult{true});
    }

    void RevokeToken(
        std::uint64_t,
        std::string,
        ResultCallback callback
    ) {
        if (callback) callback(ClientResult{true});
    }

    void Connect() {
        status_ = Status::Ready;
        if (status_callback_) {
            status_callback_(Status::Ready, Error::None, 0);
        }
    }

    void Disconnect() {
        status_ = Status::Disconnected;
    }

    void AbortAuthorize() {}

    [[nodiscard]] Status GetStatus() const noexcept { return status_; }

    void UpdateRichPresence(Activity, ResultCallback callback) {
        if (callback) callback(ClientResult{true});
    }

    void ClearRichPresence() {}

    void OpenConnectedGamesSettingsInDiscord(ResultCallback callback) {
        if (callback) callback(ClientResult{true});
    }

private:
    static void emit_tokens(TokenCallback callback) {
        if (callback) {
            callback(
                ClientResult{true},
                "fixture-access",
                "fixture-refresh",
                AuthorizationTokenType::Bearer,
                3600,
                "fixture"
            );
        }
    }

    std::uint64_t application_id_{};
    Status status_{Status::Disconnected};
    StatusCallback status_callback_{};
    TokenExpirationCallback expiration_callback_{};
};

inline void RunCallbacks() {
    (void)discord_partner_sdk_fixture_marker();
}

}  // namespace discordpp
'''

CDISCORD_HEADER = r'''#pragma once

#ifdef __cplusplus
extern "C" {
#endif
int discord_partner_sdk_fixture_marker(void);
#ifdef __cplusplus
}
#endif
'''

RUNTIME_C = r'''#if defined(_WIN32)
#define PULSEFORGE_FIXTURE_EXPORT __declspec(dllexport)
#else
#define PULSEFORGE_FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

PULSEFORGE_FIXTURE_EXPORT int discord_partner_sdk_fixture_marker(void) {
    return 19337;
}
'''


def write_fixture(root: Path) -> None:
    include = root / "include"
    include.mkdir(parents=True, exist_ok=True)
    (include / "discordpp.h").write_text(DISCORDPP_HEADER, encoding="utf-8")
    (include / "cdiscord.h").write_text(CDISCORD_HEADER, encoding="utf-8")
    (root / "fixture_runtime.c").write_text(RUNTIME_C, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    write_fixture(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
