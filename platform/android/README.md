# PulseForge Android

This directory is an SDL3/Gradle host for the production C++ runtime. The
release ABI is `arm64-v8a`, with Android API 21 as the minimum without Discord,
API 24 when the Discord Social SDK is bundled, and API 35 as the target. Gradle
builds `libSDL3.so` and PulseForge as `libmain.so`, then packages the Java
bootstrap, native libraries and portable engine assets into one APK. The
current delivery is `0.9.7` (`versionCode 90700`).

## Build

The Android project uses Android Gradle Plugin 9.4.0 with the Gradle 9.6.0
wrapper. Install JDK 17 plus Android SDK platform 35, Build Tools 36.0.0, CMake
3.31.6 and NDK 28.2.13676358. Then run from the repository root. On Windows:

```powershell
.\scripts\build-android-release.ps1
```

On Linux/macOS or GitHub Actions:

```bash
./scripts/build-android-release.sh
```

`JAVA_HOME` and `ANDROID_SDK_ROOT` are honoured. `-JavaHome`, `-AndroidSdk`,
`-BuildRoot` and `-OutputDirectory` can override them. The build script keeps
large intermediates outside the repository and writes the signed APK plus its
SHA-256 file to `Release/Android` by default.

For a public store release, provide a protected signing identity with
`-Keystore`, `-KeyAlias`, `-StorePassword` and `-KeyPassword`. Without one, the
script creates a machine-local development certificate. That APK is installable
for testing and direct distribution, but it is not a Play Store identity.

## Runtime storage

On first launch, `BootstrapActivity` extracts the packaged assets to app-private
storage on a worker thread. Existing `settings.json` is preserved across
upgrades. `PulseForgeActivity` exposes that directory to C++ through
`PULSEFORGE_ASSET_ROOT` and creates a writable app-specific external `mods`
directory through `PULSEFORGE_MOD_ROOT`. Android may remove both when the app is
uninstalled, so user-created mods and charts should be backed up separately.

The APK includes the demo, credits, menu audio, UI, shaders, Watch Dogs decoded
intro and the portable decoded PS2 startup/error sequences. Multi-gigabyte mod
libraries and Windows-only source/example MP4 files remain external.

## Touch input

No physical keyboard or mouse is required. SDL finger input drives a safe-area
aware virtual pad in launcher menus and pause, adaptive multi-touch lanes for
1K through 18K gameplay, and both direct interaction and editor shortcuts in
all bundled editors. Chords, holds, slides and releases are reference-counted;
focus loss/backgrounding cancels every held input to prevent stuck notes.

Configure labels, opacity, size, layout offsets, sensitivity, deadzone and lane
coverage in **Options > Touch controls**. These preferences live in the
`touch` object in the preserved `settings.json`. Disabling gameplay lanes does
not disable the menu pad, so an Android-only user cannot lock themselves out.
