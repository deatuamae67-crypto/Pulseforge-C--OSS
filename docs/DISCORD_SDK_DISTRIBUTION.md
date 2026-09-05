# Discord Social SDK distribution policy

PulseForge integrates Discord Rich Presence and account linking through Discord's official Social SDK.

## Source repository boundary

The public source repository does **not** mirror or publish Discord's SDK archive as a standalone dependency. The raw SDK remains governed by Discord's Social SDK Terms and must be obtained by an authorized developer from the Discord Developer Portal.

The project-local SDK directory remains ignored by Git:

```text
third_party/discord_social_sdk/
```

Before staging a private SDK archive, audit it in place with the standard-library-only inspector:

```bash
python3 scripts/inspect-discord-social-sdk.py \
  --sdk /path/to/DiscordSocialSdk-1.10.19337.zip \
  --expect-version 1.10.19337 \
  --require all \
  --deep \
  --hash
```

The auditor reads ZIP/TAR archives or extracted directories without copying SDK contents into the repository. It reports the detected version, headers, platform inputs, PE/ELF/Mach-O architectures, Android Prefab/ABI metadata and SHA-256 values for matched SDK artifacts. Omit `--require all` when auditing a platform-specific download; use repeated `--require windows`, `--require linux`, `--require macos` or `--require android` gates instead.

Developers can then stage an authorized SDK archive with:

```powershell
.\scripts\setup-discord-social-sdk.ps1 -SdkPath C:\Downloads\discord_social_sdk.zip
```

or:

```bash
./scripts/setup-discord-social-sdk.sh --sdk ~/Downloads/discord_social_sdk.zip
```

## Shipping the game

Discord's C++ integration documentation requires the Social SDK runtime to ship with the game when the SDK is linked.

PulseForge release packages therefore treat the runtime as a required integrated application dependency:

- Windows: `discord_partner_sdk.dll` beside `pulseforge.exe` and `pulseforge-cli.exe`.
- Linux: `libdiscord_partner_sdk.so` beside the executable, with an `$ORIGIN` runtime search path.
- macOS: the inspected Social SDK 1.10.19337 package contains **both** a universal `libdiscord_partner_sdk.dylib` and `discord_partner_sdk.framework` (`x86_64 + arm64`). PulseForge's currently validated automatic CMake path uses the packaged `.dylib`; its install name is `@rpath/libdiscord_partner_sdk.dylib` and PulseForge supplies a bundle-local runtime search path. Native framework linking/embedding remains tracked separately in issue #42 as a packaging enhancement, not as a prerequisite for using the current authorized 1.10.19337 SDK on macOS.
- Android: `discord_partner_sdk.aar` is consumed through Gradle/Prefab and its native runtime is packaged into the APK.

If the framework path is deliberately selected after #42 is implemented, the complete `discord_partner_sdk.framework` bundle must be preserved inside `pulseforge.app/Contents/Frameworks` and resolved through `@rpath`. Copying only the framework executable is invalid because the inspected 1.10.19337 framework contains its own `Frameworks/libdiscord_krisp.dylib` dependency.

A Discord-enabled release must never be published if the corresponding runtime dependency is missing.

Raw Apple SDK bundles such as `discord_partner_sdk.framework` or an SDK-provided `.xcframework` are private SDK inputs just like the raw DLL/SO/AAR and must not be committed to the public source repository.

## CI and release secrets

Public CI may compile the no-op backend without access to proprietary SDK files. Release CI that produces Discord-enabled distributable binaries must receive an authorized SDK archive through a repository secret or another private authenticated source.

Recommended secret name:

```text
PULSEFORGE_DISCORD_SDK_ARCHIVE_URL
```

The downloaded SDK archive must only be used as an input to the build. Do not upload the unmodified SDK archive as a public artifact. Only publish the final PulseForge application package containing the runtime integrated as required by Discord.

## Fail-open runtime behavior

Discord connectivity, account state, authentication failures, rate limits and Discord being closed must never terminate gameplay or prevent PulseForge from starting.

This is distinct from a missing loader dependency: if PulseForge is linked against the Discord shared library but the platform runtime was omitted from the application package, the operating-system loader can fail before PulseForge's own fail-open code executes. Packaging validation therefore prevents such a build from being released.

## Official references

- Discord Social SDK Terms: https://support-dev.discord.com/hc/en-us/articles/30225844245271-Discord-Social-SDK-Terms
- Discord Social SDK installation: https://discord.com/developers/docs/social-sdk/installation.html
- Discord Social SDK release notes: https://discord.com/developers/docs/social-sdk/release_notes.html
- Discord Social SDK documentation: https://discord.com/developers/docs/social-sdk/index.html
