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
- macOS: `libdiscord_partner_sdk.dylib` from the current authorized Social SDK 1.10.19337 package, with a bundle-local runtime search path
- Android: Discord AAR/Prefab native runtime embedded by Gradle

The inspected Social SDK 1.10.19337 macOS package also contains `discord_partner_sdk.framework`. Framework-native PulseForge linking/embedding is tracked in issue #42 as an optional packaging enhancement. It is **not** a prerequisite for a Discord-enabled macOS build while the same authorized SDK package provides the validated universal `.dylib` path.

If #42's framework path is used, the complete framework must be embedded in `pulseforge.app/Contents/Frameworks` and resolved through `@rpath`; do not copy only the main framework executable because the real 1.10.19337 bundle contains its own nested `Frameworks/libdiscord_krisp.dylib` dependency.

The raw Discord SDK archive, framework, xcframework, DLL, SO, dylib or AAR must not be uploaded as a standalone public artifact.

## No-SDK development builds

Development and public source CI may deliberately compile the no-op Discord backend. Those builds must remain launchable and gameplay-safe without Discord installed. They are distinct from Discord-enabled release packages and must not be mislabeled as final Discord-enabled binaries.
