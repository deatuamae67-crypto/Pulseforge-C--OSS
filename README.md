# PulseForge

> A deterministic, low-latency C++20 rhythm-game engine built for scalable charts, FNF/Psych-style content and cross-platform development.

[![Cross-platform build](https://github.com/deatuamae67-crypto/Pulseforge-C--OSS/actions/workflows/build-validation.yml/badge.svg?branch=main)](https://github.com/deatuamae67-crypto/Pulseforge-C--OSS/actions/workflows/build-validation.yml)
[![Deterministic core tests](https://github.com/deatuamae67-crypto/Pulseforge-C--OSS/actions/workflows/core-tests-validation.yml/badge.svg?branch=main)](https://github.com/deatuamae67-crypto/Pulseforge-C--OSS/actions/workflows/core-tests-validation.yml)
[![OSS boundary](https://github.com/deatuamae67-crypto/Pulseforge-C--OSS/actions/workflows/oss-boundary-validation.yml/badge.svg?branch=main)](https://github.com/deatuamae67-crypto/Pulseforge-C--OSS/actions/workflows/oss-boundary-validation.yml)
[![Release integrity](https://github.com/deatuamae67-crypto/Pulseforge-C--OSS/actions/workflows/release-integrity-validation.yml/badge.svg?branch=main)](https://github.com/deatuamae67-crypto/Pulseforge-C--OSS/actions/workflows/release-integrity-validation.yml)
[![Latest release](https://img.shields.io/github/v/release/deatuamae67-crypto/Pulseforge-C--OSS?sort=semver&label=release)](https://github.com/deatuamae67-crypto/Pulseforge-C--OSS/releases/latest)
[![License](https://img.shields.io/github/license/deatuamae67-crypto/Pulseforge-C--OSS)](LICENSE)

PulseForge is an open-source rhythm-game engine focused on precise timing,
responsive input and predictable gameplay. Judgement is kept independent from
rendering FPS, while charts are normalized into a canonical runtime model that
can be consumed either conventionally or through the PFC1 streaming pipeline.

This README describes the engine itself rather than a particular release.
Version-specific changes, compatibility snapshots and provenance records live
under [`docs/`](docs/) and on the [GitHub Releases](https://github.com/deatuamae67-crypto/Pulseforge-C--OSS/releases) page.

## Highlights

- **Deterministic gameplay** — timing, judgement and chart progression are
  designed to remain stable independently of rendering FPS.
- **Large-chart architecture** — PFC1 streaming, PatternRun support and
  monotonic gameplay cursors avoid requiring every logical note to be fully
  materialized in memory.
- **FNF / Psych compatibility** — imports common chart/content layouts and
  provides a bounded Lua 5.4 compatibility layer for supported mod behavior.
- **Selected-mod script isolation** — executable Lua discovery is scoped to the
  active content roots so unrelated sibling mods cannot inject gameplay code.
- **Editors and tooling** — chart, character and week editing paths live beside
  the runtime, together with validation and AutoChart integration.
- **Cross-platform** — Windows, Linux, macOS and Android are first-class build
  targets, with Android touch controls included in the public source.
- **Optional integrations** — Discord Social SDK support is fail-open and kept
  separate from the redistributable source tree when proprietary SDK inputs are
  unavailable.
- **Content-rich Complete distributions** — approved menu music and the full
  versioned mod library can be published as verified Release content without
  bloating the Git history.

## Platforms

| Platform | Architecture | Public CI package |
| --- | --- | --- |
| Windows | x86_64 | ZIP |
| Linux | x86_64 | `tar.gz` |
| macOS | arm64 | `tar.gz` app bundle |
| macOS | x86_64 | `tar.gz` app bundle |
| Android | arm64-v8a | test-signed APK |

Prebuilt packages and checksums are published on the
[Releases](https://github.com/deatuamae67-crypto/Pulseforge-C--OSS/releases)
page. Android CI packages use a testing certificate unless a separate
production signing identity is supplied; macOS CI packaging is similarly
distinct from production notarization.

## Content and mod support

PulseForge keeps imported content separate from engine source. The compatibility
layer covers chart parsing, tempo/event timelines, note ownership and lanes,
Lua callbacks, supported Psych-style helpers, media routing and gameplay
startup behavior without requiring multi-gigabyte content libraries to be
committed as ordinary Git blobs.

Historical compatibility corpora are tracked through deterministic manifests
and regression fixtures. Approved content-rich distributions are published as
versioned Release assets with checksums and per-pack manifests. See
[`docs/COMPLETE_EDITION.md`](docs/COMPLETE_EDITION.md) for the Complete
distribution model and [`docs/OSS_SCOPE.md`](docs/OSS_SCOPE.md) for the source
publication boundary.

Menu/pause music, startup intros, credit portraits and visualizer media remain
supported as extension points. Content included in a Complete release keeps its
own applicable provenance and terms; inclusion does not silently relicense it
under the PulseForge source license.

## Build from source

### Windows

Use the supported Visual Studio C++ toolchain and the repository CMake preset:

```text
cmake --preset windows-msvc
cmake --build --preset windows-release
```

### Linux

The Linux preset uses Ninja and the platform development libraries required by
SDL3:

```text
cmake --preset linux-release
cmake --build --preset linux-release
```

### macOS

```text
cmake --preset macos-release
cmake --build --preset macos-release
```

### Android

The Android project uses the Gradle wrapper under `platform/android/`. Required
SDK, NDK, CMake, Gradle and JDK versions are maintained in the Android project
and release requirements rather than duplicated here.

Dependencies used by the native build are pinned, and fetched archives are
hash-checked during configuration.

## Validation

The repository maintains separate CI gates for:

- cross-platform application builds;
- deterministic core tests;
- the public/private source boundary;
- release integrity;
- Complete-content manifests and packaging;
- synthetic Discord SDK integration and platform packaging paths where
  applicable.

Release publication is fail-closed: platform artifacts are built from the
selected `main` commit, checksums and the expected asset set are verified, and
only then is the GitHub Release published.

## Project structure

```text
include/             Public/native engine headers
src/                 Engine and application source
assets/              Redistributable runtime assets and extension contracts
platform/android/    Android frontend and Gradle project
tests/                Deterministic and compatibility regressions
docs/                 Architecture, release and compatibility documentation
third_party/          Redistributable dependency integration metadata
```

## Licensing and repository boundary

PulseForge-authored source is distributed under the Apache License 2.0 as
described by [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE). The procedural demo is
CC0-1.0. Optional or externally supplied SDKs, FFmpeg executables, music,
videos, mods and imported game assets retain their own applicable terms.

The Git source tree intentionally excludes secrets, signing identities,
proprietary Discord SDK packages, generated release packages, build caches and
historical backup payloads. Approved large media/mod collections may instead be
distributed as separately verified GitHub Release assets by a Complete release.

See [`THIRD_PARTY.md`](THIRD_PARTY.md), [`docs/OSS_SCOPE.md`](docs/OSS_SCOPE.md),
[`docs/COMPLETE_EDITION.md`](docs/COMPLETE_EDITION.md) and
[`docs/RELEASE_REQUIREMENTS.md`](docs/RELEASE_REQUIREMENTS.md) for the detailed
redistribution and release boundary.

## Releases and development

- **Downloads:** [GitHub Releases](https://github.com/deatuamae67-crypto/Pulseforge-C--OSS/releases)
- **Documentation:** [`docs/`](docs/)
- **Issues:** [GitHub Issues](https://github.com/deatuamae67-crypto/Pulseforge-C--OSS/issues)
- **Source:** [`main`](https://github.com/deatuamae67-crypto/Pulseforge-C--OSS/tree/main)

PulseForge is developed continuously. Version numbers belong to tagged releases
and release notes; the project identity and homepage remain version-independent.
