#include "discord_presence.hpp"
#include "discord_token_store.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#ifndef PULSEFORGE_EXPECTED_COMPILED_DISCORD_APP_ID
#define PULSEFORGE_EXPECTED_COMPILED_DISCORD_APP_ID 0ULL
#endif

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

}  // namespace

int main() {
    try {
        std::string credential_error;
        require(
            !pulseforge::detail::discord_secure_token_save(
                0U, "not-a-real-token", &credential_error
            ),
            "secure credential storage must reject a zero Discord Application ID"
        );
        require(
            !credential_error.empty(),
            "invalid secure credential requests must return a diagnostic"
        );
        credential_error.clear();
        require(
            pulseforge::detail::discord_secure_token_erase(0U, &credential_error),
            "erasing a zero Application ID must be an idempotent no-op"
        );

        pulseforge::DiscordPresenceSettings settings;
        settings.application_id = 0U;
        require(
            pulseforge::detail::discord_effective_application_id(settings)
                == static_cast<std::uint64_t>(
                    PULSEFORGE_EXPECTED_COMPILED_DISCORD_APP_ID
                ),
            "compiled Discord Application ID fallback must be observable"
        );

        settings.application_id = 123456789012345678ULL;
        require(
            pulseforge::detail::discord_effective_application_id(settings)
                == settings.application_id,
            "settings Discord Application ID must override the compiled default"
        );

        auto shared = std::make_shared<pulseforge::detail::DiscordPresenceSession>();
        pulseforge::detail::DiscordPresencePublisher launcher(shared);
        pulseforge::detail::DiscordPresencePublisher gameplay(shared);
        require(
            launcher.session() == gameplay.session() && launcher.session() == shared,
            "launcher and gameplay publishers must share one explicit process session"
        );

        pulseforge::detail::DiscordPresencePublisher independent;
        require(
            independent.session() != shared,
            "a publisher can still own an independent session when explicitly needed"
        );

        shared->shutdown();
        shared->shutdown();
        launcher.pump();
        gameplay.pump();

        std::cout << "Discord presence policy tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Discord presence policy tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
