# PulseForge release requirements

A public PulseForge release is allowed only after the corresponding source commit has passed the required validation matrix and every runtime dependency that the binaries link against is present in the package.

## Discord-enabled builds

If `PULSEFORGE_DISCORD_SOCIAL_SDK_ACTIVE` is enabled, the package must contain the Social SDK runtime required by that target platform. A build that links Discord but omits the runtime is invalid even if compilation itself succeeds, because the operating-system loader may terminate the application before PulseForge can enter its fail-open Discord path.

Release CI must configure with:

```text
-DPULSEFORGE_REQUIRE_DISCORD_SOCIAL_SDK=ON
```

and must verify the packaged runtime before upload.

Expected integrated runtime for currently validated paths:

- Windows: `discord_partner_sdk.dll`
- Linux: `libdiscord_partner_sdk.so`
- macOS legacy SDK layout: `libdiscord_partner_sdk.dylib` only when deliberately building against an older authorized archive
- Android: Discord AAR/Prefab native runtime embedded by Gradle

Discord Social SDK 1.10 changed macOS distribution to `discord_partner_sdk.framework`. That framework-based PulseForge path is **not release-ready yet** and is tracked in issue #42. After #42 is completed with the authorized framework, the required integrated layout will be `discord_partner_sdk.framework` embedded in `PulseForge.app/Contents/Frameworks` and resolved through a bundle-local `@rpath`.

A framework-only macOS SDK must therefore not be treated as an activated/releasable Discord build merely because the setup script detects it.

The raw Discord SDK archive, framework, xcframework, DLL, SO, dylib or AAR must not be uploaded as a standalone public artifact.

## No-SDK development builds

Development and public source CI may deliberately compile the no-op Discord backend. Those builds must remain launchable and gameplay-safe without Discord installed. They are distinct from Discord-enabled release packages and must not be mislabeled as final Discord-enabled binaries.
