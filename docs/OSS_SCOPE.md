# PulseForge public OSS scope

This document defines the publication boundary for `Pulseforge-C--OSS`.

## Source of truth

The public repository is derived selectively from the current PulseForge development tree. The development tree remains authoritative while the public mirror is being bootstrapped.

Older cumulative or hotfix archives must not be applied wholesale over a newer development tree. Changes are reconciled selectively against the current source.

## Included by default

Material authored for the PulseForge core may be published when its provenance and license are suitable for public redistribution, including:

- C++ source and public headers;
- deterministic tests;
- CMake/build configuration;
- build and validation scripts;
- platform wrappers authored for PulseForge;
- project documentation;
- project-owned metadata and notices.

Every import should be reviewed in the context of the current tree rather than copied from an obsolete package.

## Excluded by default

The following are not mirrored merely because they exist in the development workspace:

- user-provided or private validation corpora;
- local mods or imported mod libraries;
- third-party music, video, artwork, fonts or other media without a clear public redistribution basis;
- the separately obtained Discord Social SDK and Android AAR;
- Android signing material;
- generated executables, libraries, build trees and release archives;
- local settings, logs, crash dumps, replays and renders;
- AutoChart virtual environments, downloaded models and caches;
- privately hosted dependency archives;
- binary FFmpeg distributions unless the chosen public package satisfies the applicable redistribution requirements.

## Discord

The PulseForge Discord implementation may be published as source code. The official Discord Social SDK itself remains an optional external dependency and is not redistributed here.

The engine must remain buildable through the no-op Discord backend when the SDK is absent. Discord failures must remain fail-open with respect to normal engine operation.

## Assets and mods

A code license on an upstream engine does not automatically license the music, artwork, characters, fonts, videos or mods bundled with that engine.

Material used inside a private development workspace is not automatically treated as public-redistribution material.

Assets and mods therefore require a separate provenance and licensing decision before publication.

## Third-party notices

Apache-2.0 applies to the PulseForge-authored core as described by `LICENSE` and `NOTICE`. Third-party components retain their upstream licenses and notice requirements.

Before a public source or binary release, the third-party inventory and bundled content must be re-audited against the exact files being shipped.

## CI and release policy

Workflows should be enabled only when the source, scripts and platform files that they call are present in the public repository.

A passing validation result from a private development snapshot is useful historical evidence but is not a substitute for a clean build/test run from this public repository.

Platform status must be reported precisely; do not claim a Windows/MSVC, Android, macOS or live Discord validation unless that exact path was actually run successfully.
