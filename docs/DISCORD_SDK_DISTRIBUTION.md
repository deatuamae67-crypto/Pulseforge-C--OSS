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
- macOS: Discord's 1.10 release line officially distributes the SDK as `discord_partner_sdk.framework`. The inspected 1.10.19337 package also contains a universal `libdiscord_partner_sdk.dylib` (`x86_64 + arm64`), and the current PulseForge CMake path can use that dylib for private compatibility/build smoke tests. **Do not treat that compatibility path as the canonical 1.10 production package.** Production macOS 1.10 packaging follows the framework distribution and is tracked in issue #42.
- Android: `discord_partner_sdk.aar` is consumed through Gradle/Prefab and its native runtime is packaged into the APK.

For the macOS 1.10 production path, preserve the complete `discord_partner_sdk.framework` bundle inside `pulseforge.app/Contents/Frameworks` and resolve it through `@rpath`. Copying only the framework executable is invalid: the inspected 1.10.19337 framework bundles `Frameworks/libdiscord_krisp.dylib` plus Krisp model resources under `Resources/Krisp`, and Discord's 1.10 release notes explicitly identify framework packaging as the new macOS signing/notarization path with Krisp bundled inside.

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
