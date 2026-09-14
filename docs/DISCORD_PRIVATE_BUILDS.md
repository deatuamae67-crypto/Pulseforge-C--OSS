# Private Discord-enabled builds

PulseForge keeps Discord's proprietary Social SDK out of the public OSS tree. The source integration, account-link flow, Rich Presence backend and Android deep-link wiring are public, but a build only contains the real SDK when an authorized SDK input is hydrated in the CI runner.

## Required GitHub secrets

The manual **PulseForge private Discord-enabled builds** workflow never falls back to a no-SDK build. Configure either the generic secret below or the platform-specific override for every platform you intend to build:

- `PULSEFORGE_DISCORD_SDK_ARCHIVE_URL` — generic private/authenticated/short-lived SDK URL used as the fallback on every platform.
- `PULSEFORGE_DISCORD_WINDOWS_SDK_ARCHIVE_URL` — optional Windows SDK archive override.
- `PULSEFORGE_DISCORD_LINUX_SDK_ARCHIVE_URL` — optional Linux SDK archive override.
- `PULSEFORGE_DISCORD_MACOS_SDK_ARCHIVE_URL` — optional macOS SDK archive override.
- `PULSEFORGE_DISCORD_ANDROID_SDK_ARCHIVE_URL` — optional Android override. It may point directly to the authorized `discord_partner_sdk.aar` or to a ZIP/TAR containing that AAR.

The platform-specific secret wins over the generic one. This is useful when Discord distributes different authorized inputs per target. Desktop hydration validates only the platform currently being built; an archive therefore does not need to contain unrelated operating-system runtimes. The Android hydrator validates the AAR's Prefab module, C/C++ headers and `arm64-v8a` native runtime before Gradle sees it.

Never put these URLs, SDK archives/AARs, OAuth tokens or credentials in the repository, workflow inputs, issue comments or build logs.

## What the workflow guarantees

For every requested target the workflow:

1. fails immediately when no private SDK URL is configured;
2. hydrates the authorized SDK only into the ephemeral runner workspace;
3. configures PulseForge with `PULSEFORGE_REQUIRE_DISCORD_SOCIAL_SDK=ON`;
4. builds and installs/packages the target;
5. checks that the required Discord runtime is actually present in the output;
6. uploads only the built PulseForge package, never the unmodified SDK input.

Android additionally requires the official private AAR and verifies that the APK contains `lib/arm64-v8a/libdiscord_partner_sdk.so`. Its OAuth callback remains `discord-<application-id>:/authorize/callback`, registered through `com.discord.socialsdk.AuthenticationActivity`.

The ordinary public release workflow intentionally remains capable of building the OSS/no-SDK variant. Use the private workflow when the artifact must support Discord account linking and Social SDK Rich Presence.

## Validation without proprietary inputs

`PulseForge private Discord SDK hydrator validation` runs on pull requests without secrets. It syntax-checks the Bash and PowerShell hydration scripts and builds a synthetic Prefab AAR to test both direct-AAR and wrapped-AAR staging paths. This validates the plumbing without redistributing Discord's SDK.
