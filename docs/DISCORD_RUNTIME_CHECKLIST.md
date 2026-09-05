# Discord-enabled release checklist

Before publishing any PulseForge binary with the Discord Social SDK enabled:

- [ ] SDK obtained from the Discord Developer Portal by an authorized developer.
- [ ] `discordpp.h` and `cdiscord.h` detected during configure.
- [ ] Platform import/shared library detected.
- [ ] `PULSEFORGE_REQUIRE_DISCORD_SOCIAL_SDK=ON` used for the release build.
- [ ] Windows package contains `discord_partner_sdk.dll` beside the executable.
- [ ] Linux package contains `libdiscord_partner_sdk.so` and resolves it through `$ORIGIN` or an equivalent package-local path.
- [ ] macOS package uses a runtime supplied by the same authorized SDK version. The currently validated 1.10.19337 path contains `libdiscord_partner_sdk.dylib` and resolves it through the bundle-local runtime path configured by PulseForge.
- [ ] If the optional framework path from issue #42 is used, the complete `discord_partner_sdk.framework` is embedded in `pulseforge.app/Contents/Frameworks`, including its nested dependencies, and resolves through `@rpath`.
- [ ] Android package was built with `discord_partner_sdk.aar` through Gradle/Prefab.
- [ ] Final packaged binary is checked for unresolved dynamic dependencies.
- [ ] Application starts with Discord closed/unreachable without affecting gameplay.
- [ ] Authentication/link failures do not terminate the application.
- [ ] No OAuth access token, refresh token, client secret, signing key or private SDK URL is present in the package.
- [ ] Raw SDK archive/framework/xcframework or standalone SDK runtime is not uploaded as a public artifact.
