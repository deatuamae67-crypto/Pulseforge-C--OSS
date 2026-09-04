# PulseForge 0.9.7

PulseForge 0.9.7 is the first public cross-platform OSS release candidate of the current engine line.

## Highlights

- Cross-platform C++20/CMake source for Windows x86_64, Linux x86_64, macOS x86_64, macOS arm64 and Android arm64.
- Deterministic core-test coverage integrated into GitHub Actions.
- Cross-platform build presets and release packaging.
- Current Discord Rich Presence integration boundary, with the Discord Social SDK remaining optional and fail-open.
- Public contribution, security, issue and pull-request templates plus Dependabot configuration.
- Removal of obsolete OSS-bootstrap import/patch bridge workflows.

## Release packages

- `PulseForge-v0.9.7-Windows-x86_64.zip`
- `PulseForge-v0.9.7-Linux-x86_64.tar.gz`
- `PulseForge-v0.9.7-macOS-x86_64.tar.gz`
- `PulseForge-v0.9.7-macOS-arm64.tar.gz`
- `PulseForge-v0.9.7-Android-arm64-test-signed.apk`

Package-level SHA-256 values and build provenance are recorded in `docs/RELEASE_0.9.7_PROVENANCE.md`.

## Validation

The engine/build source was validated by both the cross-platform build matrix and deterministic core-test matrix before merge. Release packaging is separately exercised by the release-artifact workflow for every supported target.

## Discord distribution boundary

The public source repository does not redistribute the Discord Social SDK. Public CI packages are OSS/no-SDK builds unless the official Discord Social SDK runtime is supplied explicitly. Do not describe a no-SDK package as a final Discord-enabled release. A Discord-enabled package must satisfy `docs/RELEASE_REQUIREMENTS.md`.

## Signing notes

- Android CI packages are test-signed and are not production-signed distribution artifacts.
- macOS CI packages use ad-hoc signing. Production distribution requires an appropriate Developer ID signing/notarization process.

## Source and compatibility

PulseForge 0.9.7 preserves the current engine architecture, including materialized and PFC1 chart paths, bounded-memory/runtime constraints, dynamic mania behavior, deterministic chart-total handling and the current optional Discord presence architecture.

For detailed public changes, see `CHANGELOG.md`.
