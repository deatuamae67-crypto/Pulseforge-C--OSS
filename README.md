# PulseForge C++ OSS 0.9.7

PulseForge is a C++20 rhythm-game engine focused on deterministic timing,
low-latency input and streaming charts whose note count is too large to
materialise in memory. It imports common FNF/Psych-style content into a
canonical model while keeping judgement independent from rendering FPS.

The public source includes the engine core, SDL3 application, Lua 5.4 sandbox,
PFC1 streaming pipeline, editors, deterministic tests, Android touch frontend
and reproducible CMake/Gradle builds. Private mod collections and third-party
media are intentionally not mirrored; see [the OSS scope](docs/OSS_SCOPE.md).

## Build targets

The `PulseForge release artifacts` workflow builds and uploads:

- Windows x86_64 (`zip` plus SHA-256 and packaged-file inventory);
- Linux x86_64 (`tar.gz`);
- macOS arm64 and Intel x86_64 (`tar.gz`);
- Android arm64-v8a (signed test APK plus SHA-256).

These public CI packages may use the no-op Discord backend when redistributable
Discord Social SDK inputs are not available. They must not be described as
final Discord-enabled packages unless the runtime requirements in
[release requirements](docs/RELEASE_REQUIREMENTS.md) are satisfied.

The Android APK produced without a repository signing secret uses an ephemeral
testing certificate. It is installable for testing, but production/store
updates must be signed with the project owner's persistent private keystore.

Local Windows builds use CMake 3.24+, Visual Studio 2022/2026 and the
`windows-msvc` preset. Linux and macOS use Ninja through their named presets.
Android requires JDK 17, SDK 35, Build Tools 35.0.0, NDK 28.2.13676358 and
CMake 3.31.6.

```text
cmake --preset windows-msvc
cmake --build --preset windows-release

cmake --preset linux-release
cmake --build --preset linux-release

cmake --preset macos-release
cmake --build --preset macos-release
```

Dependencies are pinned and archive hashes are checked during configuration.
See [third-party notices](THIRD_PARTY.md) and
[release requirements](docs/RELEASE_REQUIREMENTS.md).

## Repository policy

Apache-2.0 covers the PulseForge-authored core as described by `LICENSE` and
`NOTICE`. Optional Discord SDK binaries, FFmpeg executables, music, videos,
mods and imported game assets retain their own terms and are not relicensed by
this repository.
