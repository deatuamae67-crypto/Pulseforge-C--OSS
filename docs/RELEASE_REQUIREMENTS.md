# PulseForge release requirements

A public PulseForge release is allowed only after the corresponding source commit has passed the required validation matrix and every runtime dependency that the binaries link against is present in the package.

## Discord-enabled builds

If `PULSEFORGE_DISCORD_SOCIAL_SDK_ACTIVE` is enabled, the package must contain the Social SDK runtime required by that target platform. A build that links Discord but omits the runtime is invalid even if compilation itself succeeds, because the operating-system loader may terminate the application before PulseForge can enter its fail-open Discord path.

Release CI must configure with:

```text
-DPULSEFORGE_REQUIRE_DISCORD_SOCIAL_SDK=ON
```

and must verify the packaged runtime before upload.

Expected integrated runtime for currently validated or explicitly gated paths:

- Windows: `discord_partner_sdk.dll`
- Linux: `libdiscord_partner_sdk.so`
- macOS 1.10 production: complete `discord_partner_sdk.framework` bundle in `pulseforge.app/Contents/Frameworks`, resolved through `@rpath`
- Android: Discord AAR/Prefab native runtime embedded by Gradle

The inspected Social SDK 1.10.19337 macOS package also contains a universal `libdiscord_partner_sdk.dylib`. PulseForge's current CMake can use that dylib for private compatibility/build smoke tests, but Discord's 1.10 release notes define the framework as the macOS distribution path with the updated signing/notarization pipeline and bundled Krisp. Therefore the dylib compatibility path must not be used as evidence that a **production** macOS 1.10 package is complete.

Production macOS 1.10 release readiness remains gated by issue #42. The complete framework must be embedded, not only its main executable: the real 1.10.19337 bundle includes `Frameworks/libdiscord_krisp.dylib` and Krisp model resources under `Resources/Krisp`.

The raw Discord SDK archive, framework, xcframework, DLL, SO, dylib or AAR must not be uploaded as a standalone public artifact.

## No-SDK development builds

Development and public source CI may deliberately compile the no-op Discord backend. Those builds must remain launchable and gameplay-safe without Discord installed. They are distinct from Discord-enabled release packages and must not be mislabeled as final Discord-enabled binaries.
