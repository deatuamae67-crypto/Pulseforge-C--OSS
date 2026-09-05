# Changelog

All notable public PulseForge changes are documented in this file.

## [1.0.0] - 2026-09-05

### Definitive release

- Promotes PulseForge to the stable `1.0.0` release line; `0.9.7` is retained as the preceding pre-release.
- Validates against a complete 29-mod historical compatibility corpus without bloating Git history with approximately 15.0 GB of static third-party content; a Drive-root audit recovered two runnable mods omitted from the historical `modsList.txt`. Public OSS release assets contain the engine/platform packages, while third-party mod redistribution remains separate and license-dependent.
- Adds a versioned mod-corpus manifest with Drive provenance, default enablement, file counts, unpacked byte counts and deterministic path/size inventory hashes.

### Fixed

- Adds Overkill/Timeless Lua compatibility: `addGlitchEffect` receives a bounded renderer-native fallback; `setScrollFactor(tag)` and legacy `setLuaSpriteScrollFactor` follow Psych semantics; fractional `curDecBeat`/`curDecStep` and resolved `mustHitSection` are available; a bounded literal-only `string.find` and allow-listed character `getAnimationName` bridge prevent the original scripts from aborting without exposing general Lua patterns or native reflection; Psych easing names are case-insensitive and include quad/cube/quart/quint/sine/circ families for modchart fidelity.
- Isolates executable Psych/Denpa/SC:R Lua discovery to the selected content/mod roots. Unrelated sibling mods can no longer inject `onStartCountdown()` / `Function_Stop` into the active SC:R chart and freeze gameplay at `t=0`.
- Keeps broad fallback discovery for non-executable content while explicitly disabling sibling stock-provider injection for executable script discovery.

### Validation

- Adds a deterministic regression that models the nested `drive-pack-screboot-demo/SCReboot_Demo/bin/assets/shared/data` layout and proves sibling script roots are excluded.
- Direct CLI launches without a catalog-selected mod retain the existing explicit-root behavior.

## [0.9.7] - 2026-09-04

### Added

- Cross-platform OSS source tree for Windows x86_64, Linux x86_64, macOS x86_64, macOS arm64 and Android arm64.
- Deterministic core-test coverage integrated into GitHub Actions.
- Cross-platform CMake presets and release packaging workflows.
- Discord Rich Presence integration boundary with an optional no-op backend when the Discord Social SDK is unavailable.
- Runtime and packaging checks for Discord-enabled builds.
- Public contribution and maintenance files, including `CONTRIBUTING.md`, `SECURITY.md`, pull-request and issue templates, and Dependabot configuration.

### Changed

- Public CI now validates the actual pull-request head instead of a fixed historical import branch.
- Pull-request validation runs on every PR targeting `main` so required checks remain reportable for documentation-only changes.
- CI validation uses concurrency cancellation so superseded runs do not continue consuming runners.
- Obsolete import/patch bridge workflows used during the OSS bootstrap were removed after their changes had been incorporated.
- Portability fixes were consolidated for Windows/MSVC, Linux, macOS/libc++ and Android/NDK builds.

### Release packaging

The release-artifact workflow produces:

- `PulseForge-v0.9.7-Windows-x86_64.zip`
- `PulseForge-v0.9.7-Linux-x86_64.tar.gz`
- `PulseForge-v0.9.7-macOS-x86_64.tar.gz`
- `PulseForge-v0.9.7-macOS-arm64.tar.gz`
- `PulseForge-v0.9.7-Android-arm64-test-signed.apk`

Each package is accompanied by SHA-256 metadata and platform-specific inspection output.

### Validation

The PulseForge engine/build source merged into `main` is byte-identical to the source at commit `fbb9ee22350a939071595721f5d6a35909d41a6d`, where both the cross-platform build-validation matrix and deterministic core-test matrix completed successfully.

The integration was merged through PR #25 into `main` at commit `fe100c6f5ff5c69aff85ecf760696d5f0c5fd8d5`. Changes after the validated source commit were limited to documentation and `.github` repository/CI maintenance files.

### Distribution notes

- The Discord Social SDK itself is not redistributed in the public source repository.
- Public no-SDK CI builds must not be described as final Discord-enabled releases.
- A Discord-enabled package must include the required platform runtime and satisfy `docs/RELEASE_REQUIREMENTS.md`.
- The Android CI artifact is test-signed and is not a production signing identity.
- macOS CI packages are ad-hoc signed unless a separate production signing/notarization process is applied.
