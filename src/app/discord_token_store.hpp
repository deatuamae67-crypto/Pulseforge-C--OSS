#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pulseforge::detail {

// Secure, platform-owned storage for the Discord OAuth refresh token. The
// access token is intentionally never persisted. Failures are fail-open: the
// caller can keep the current in-memory Discord session and surface the error
// as a "session only" warning.
[[nodiscard]] bool discord_secure_token_store_available() noexcept;

[[nodiscard]] std::optional<std::string> discord_secure_token_load(
    std::uint64_t application_id,
    std::string* error = nullptr
) noexcept;

[[nodiscard]] bool discord_secure_token_save(
    std::uint64_t application_id,
    std::string_view refresh_token,
    std::string* error = nullptr
) noexcept;

[[nodiscard]] bool discord_secure_token_erase(
    std::uint64_t application_id,
    std::string* error = nullptr
) noexcept;

}  // namespace pulseforge::detail
