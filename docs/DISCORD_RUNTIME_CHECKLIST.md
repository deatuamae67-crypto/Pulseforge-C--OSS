# Discord-enabled release checklist

Before publishing any PulseForge binary with the Discord Social SDK enabled:

- [ ] SDK obtained from the Discord Developer Portal by an authorized developer.
- [ ] `discordpp.h` and `cdiscord.h` detected during configure.
- [ ] Platform import/shared library detected.
- [ ] `PULSEFORGE_REQUIRE_DISCORD_SOCIAL_SDK=ON` used for the release build.
- [ ] Windows package contains `discord_partner_sdk.dll` beside the executable.
- [ ] Linux package contains `libdiscord_partner_sdk.so` and resolves it through `$ORIGIN` or an equivalent package-local path.
- [ ] If deliberately using the currently validated legacy macOS SDK layout, the package contains `libdiscord_partner_sdk.dylib` and resolves it through `@loader_path` or an equivalent bundle-local path.
- [ ] For macOS Social SDK 1.10+, issue #42 has been completed against the authorized `discord_partner_sdk.framework` before treating the build as Discord-enabled.
- [ ] After #42 is completed, the macOS framework is embedded in `PulseForge.app/Contents/Frameworks`, code-signed as part of the final app and resolved through a bundle-local `@rpath`.
- [ ] Android package was built with `discord_partner_sdk.aar` through Gradle/Prefab.
- [ ] Final packaged binary is checked for unresolved dynamic dependencies.
- [ ] Application starts with Discord closed/unreachable without affecting gameplay.
- [ ] Authentication/link failures do not terminate the application.
- [ ] No OAuth access token, refresh token, client secret, signing key or private SDK URL is present in the package.
- [ ] Raw SDK archive/framework/xcframework or standalone SDK runtime is not uploaded as a public artifact.
