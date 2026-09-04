# PulseForge 0.9.7 release provenance

This document records the build and validation provenance for the first public PulseForge 0.9.7 OSS release candidate.

## Source state

- Integration PR: #25
- Merge commit on `main`: `fe100c6f5ff5c69aff85ecf760696d5f0c5fd8d5`
- Last engine/build-source head with both PR validation matrices completed successfully: `fbb9ee22350a939071595721f5d6a35909d41a6d`
- Cross-platform build validation run: `33884590462` — success
- Deterministic core tests run: `33884590622` — success
- Current release-workflow commit: `6e49caed8f7abbe5f2104632d48bdc93f4baa3b1`

The changes between the validated source head and the merge were limited to repository documentation and `.github` maintenance/CI files; there were no changes to `src/`, `include/`, `tests/`, `CMakeLists.txt`, `CMakePresets.json`, `platform/android/`, `assets/`, or `third_party/`.

The release-workflow commit changes only the macOS Intel runner label from `macos-15-intel` to `macos-26-intel`; it does not change PulseForge engine or build inputs. Documentation-only commits after that workflow commit likewise do not alter the generated binaries.

## Preferred current `main` release-artifact run

Release-artifact run `33891158742` targets commit `6e49caed8f7abbe5f2104632d48bdc93f4baa3b1` on `main`.

The following packages have completed successfully in this run. Their package-level SHA-256 files were independently rechecked after downloading the GitHub Actions artifacts:

| Target | Package | SHA-256 | Status |
| --- | --- | --- | --- |
| Windows x86_64 | `PulseForge-v0.9.7-Windows-x86_64.zip` | `b70d5adf35fd2b071b54b0711e91215985feb41fd1bf78b5b8f5d16d444291df` | verified |
| Linux x86_64 | `PulseForge-v0.9.7-Linux-x86_64.tar.gz` | `e9063dda68e7f6e5c1a5692d5718883b458d1855819fe5a7815f7928483a36ce` | verified |
| macOS arm64 | `PulseForge-v0.9.7-macOS-arm64.tar.gz` | `aebfea49ac288c93343776df759bfacb71d837057bf5deefd5263df74562204e` | verified |
| Android arm64 | `PulseForge-v0.9.7-Android-arm64-test-signed.apk` | `848e0b42ee581ac801260eb81828d6f17ec5d5925b154546cb7fb906f41a2a18` | verified |
| macOS x86_64 | `PulseForge-v0.9.7-macOS-x86_64.tar.gz` | pending current-run package | building |

Independent package evidence for the four completed current-run packages also confirms:

- Windows contains both `bin/pulseforge.exe` and `bin/pulseforge-cli.exe`.
- Linux is an x86-64 ELF executable and the recorded `ldd` output contains no unresolved shared libraries.
- macOS arm64 is a Mach-O 64-bit arm64 executable and passed the workflow's ad-hoc codesign verification.
- Android identifies as `org.pulseforge.engine`, `versionCode=90700`, `versionName=0.9.7`, `minSdkVersion=21`, `targetSdkVersion=35`, with native code `arm64-v8a`; the CI artifact is deliberately test-signed.

## macOS x86_64 fallback provenance

The current run now uses GitHub's supported `macos-26-intel` runner label for x86_64 and its job is actively building.

Until that package completes, a previous successful release-artifact run, `33883728198`, provides a validated fallback built from commit `4063a05110be16f044809c58048ea9baae74f590`. A commit comparison from that source to the merged engine state showed no changes to engine, CMake, platform, asset or third-party build inputs.

The independently checked fallback package is:

- `PulseForge-v0.9.7-macOS-x86_64.tar.gz`
- SHA-256: `a0c2a48dab31f9cafd0e2f60c48a682a994fe64967468eef8aac986b91d7a0c4`
- Architecture evidence: Mach-O 64-bit executable `x86_64`
- Workflow result: successful build, ad-hoc signing, codesign verification and artifact upload

If run `33891158742` completes its macOS x86_64 job successfully, that newly produced artifact supersedes this fallback for the final GitHub Release.

## Distribution boundary

These CI packages are public OSS/no-SDK builds unless the Discord Social SDK and its required platform runtime are explicitly supplied. They must not be represented as final Discord-enabled packages unless `docs/RELEASE_REQUIREMENTS.md` is satisfied.

The Android package is test-signed and the macOS packages are ad-hoc signed; production distribution requires the appropriate production signing/notarization process.

## Publication status

The source is merged and four of five packages from the preferred current-main run are independently verified. The current macOS x86_64 package is still building, with a validated identical-input fallback available.

A Git tag/GitHub Release named `v0.9.7` has not yet been created through the available automation path. Publication should use this provenance record and, once available, the full artifact set from run `33891158742`.
