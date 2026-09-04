# PulseForge 0.9.7 release provenance

This document records the build and validation provenance for the first public PulseForge 0.9.7 OSS release candidate.

## Source state

- Integration PR: #25
- Merge commit on `main`: `fe100c6f5ff5c69aff85ecf760696d5f0c5fd8d5`
- Last engine/build-source head with both PR validation matrices completed successfully: `fbb9ee22350a939071595721f5d6a35909d41a6d`
- Cross-platform build validation run: `33884590462` — success
- Deterministic core tests run: `33884590622` — success

The changes between the validated source head and the merge were limited to repository documentation and `.github` maintenance/CI files; there were no changes to `src/`, `include/`, `tests/`, `CMakeLists.txt`, `CMakePresets.json`, `platform/android/`, `assets/`, or `third_party/`.

## Current `main` release-artifact run

Release-artifact run `33888451183` targets merge commit `fe100c6f5ff5c69aff85ecf760696d5f0c5fd8d5`.

The following packages completed successfully and their package-level SHA-256 files were independently rechecked after downloading the GitHub Actions artifacts:

| Target | Package | SHA-256 |
| --- | --- | --- |
| Windows x86_64 | `PulseForge-v0.9.7-Windows-x86_64.zip` | `4917a088ee09ddb528285c117627278972f5aaa77ecac36dc399fe7125be0577` |
| Linux x86_64 | `PulseForge-v0.9.7-Linux-x86_64.tar.gz` | `2bdf7a117e1c8ded2f88ed993980c728ec02ef70393d927bb791c6123b45d72a` |
| macOS arm64 | `PulseForge-v0.9.7-macOS-arm64.tar.gz` | `681362b26e0b658bf991dd9a42ca9f57029638056bae8fd50fc328b72fcd37e0` |
| Android arm64 | `PulseForge-v0.9.7-Android-arm64-test-signed.apk` | `403a74af9d8223ff2fc693558c46f5921f58c76b2db1b0c93486a26d34938469` |

Independent package evidence also confirmed:

- Windows contains both `bin/pulseforge.exe` and `bin/pulseforge-cli.exe` plus the expected OSS documentation/runtime policy files.
- Linux is an x86-64 ELF executable and the recorded `ldd` output contains no unresolved shared libraries.
- macOS arm64 is a Mach-O 64-bit arm64 executable and passed the workflow's ad-hoc codesign verification.
- Android identifies as `org.pulseforge.engine`, `versionCode=90700`, `versionName=0.9.7`, `minSdkVersion=21`, `targetSdkVersion=35`, with native code `arm64-v8a`; the CI artifact is deliberately test-signed.

## macOS x86_64 runner fallback

The macOS x86_64 job in current `main` run `33888451183` remains queued because no `macos-15-intel` runner has been assigned.

A previous successful release-artifact run, `33883728198`, built macOS x86_64 from commit `4063a05110be16f044809c58048ea9baae74f590` using the same release workflow and identical build inputs. A commit comparison from that source to the merge commit shows only README, documentation and `.github` changes; no engine, CMake, platform, asset or third-party build input changed.

The independently checked fallback package is:

- `PulseForge-v0.9.7-macOS-x86_64.tar.gz`
- SHA-256: `a0c2a48dab31f9cafd0e2f60c48a682a994fe64967468eef8aac986b91d7a0c4`
- Architecture evidence: Mach-O 64-bit executable `x86_64`
- Workflow result: successful build, ad-hoc signing, codesign verification and artifact upload

This fallback is suitable as a provenance-preserving release candidate because the build inputs are unchanged. If the current `main` macOS x86_64 job later completes, its newly produced artifact should supersede this fallback in the final GitHub Release.

## Distribution boundary

These CI packages are public OSS/no-SDK builds unless the Discord Social SDK and its required platform runtime are explicitly supplied. They must not be represented as final Discord-enabled packages unless `docs/RELEASE_REQUIREMENTS.md` is satisfied.

The Android package is test-signed and the macOS packages are ad-hoc signed; production distribution requires the appropriate production signing/notarization process.

## Publication status

The source is merged and the release candidate set is validated as recorded above. A Git tag/GitHub Release named `v0.9.7` has not yet been created by this automation path; publication should use this provenance record and, where available, prefer artifacts produced directly from current `main` run `33888451183`.
