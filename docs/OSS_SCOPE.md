# PulseForge public OSS scope

This document defines the publication boundary for `Pulseforge-C--OSS`.

## Source of truth

The public repository is derived selectively from the current PulseForge development tree. The current reconciled engine tree is authoritative for public release work; older cumulative or hotfix archives must not be applied wholesale over a newer tree.

## Included by default

Material authored for the PulseForge core may be published when its provenance and license are suitable for public redistribution, including:

- C++ source and public headers;
- deterministic tests;
- CMake/build configuration;
- build and validation scripts;
- platform wrappers authored for PulseForge;
- project documentation;
- project-owned metadata and notices.

The approved PulseForge Complete content set is also explicitly included in the engine repository/runtime tree:

- `mods/<collection>/...` for the selected 30 current mod/content collections;
- `mods/modsList.txt` for historical/default enablement;
- `assets/menu/*.mp3` for the ten selected menu/background tracks.

Large/binary built-in assets are stored through Git LFS rather than ordinary Git blobs. Git LFS changes the storage mechanism, not the fact that these files are versioned engine content.

## Excluded from the Git source tree by default

The following are not mirrored merely because they exist in a development workspace:

- user-provided/private validation corpora that are not part of the approved Complete set;
- arbitrary local mods or imported libraries without an explicit distribution decision;
- unrelated third-party music, video, artwork, fonts or media without a public distribution decision;
- the separately obtained Discord Social SDK and Android AAR;
- Android/macOS/Windows signing material and credentials;
- generated executables, libraries, build trees and release archives except where an explicitly approved content collection itself legitimately contains runtime/source artifacts;
- local settings, logs, crash dumps, replays and renders;
- AutoChart virtual environments, downloaded models and caches;
- privately hosted dependency archives;
- binary FFmpeg distributions unless the chosen public package satisfies the applicable redistribution requirements.

`.autochart-staging` is always excluded from Complete content because it is transient AutoChart workspace data.

## Discord

The PulseForge Discord implementation may be published as source code. The official Discord Social SDK itself remains an optional external dependency and is not redistributed here.

The engine must remain buildable through the no-op Discord backend when the SDK is absent. Discord failures must remain fail-open with respect to normal engine operation.

## Assets and mods

A code license on an upstream engine does not automatically license music, artwork, characters, fonts, videos or mods bundled with that engine. Material used inside a private development workspace is therefore not automatically public.

For the PulseForge Complete content set recorded in `docs/COMPLETE_CONTENT_1.0.0.json` and `docs/complete/mods/*.json`, the project owner has explicitly designated the selected content for public distribution as part of PulseForge. The real files are integrated into `mods/` and `assets/menu/`; the descriptors remain provenance/regression metadata only.

This distribution decision does not silently relicense independently authored material under Apache-2.0. Existing notices, licenses and attribution requirements remain applicable to their respective files/components.

The controlled import must enumerate the approved sources, enforce historical regression minima, reject transient/private engine inputs, route large files through Git LFS and verify the resulting repository tree before merge.

## Third-party notices

Apache-2.0 applies to the PulseForge-authored core as described by `LICENSE` and `NOTICE`. Third-party components retain their upstream/applicable licenses and notice requirements.

Before a public source or binary release, the third-party inventory and bundled content must be re-audited against the exact files being shipped.

## CI and release policy

Workflows should be enabled only when the source, scripts and platform files that they call are present in the public repository.

A passing validation result from a private development snapshot is useful historical evidence but is not a substitute for a clean build/test run from this public repository.

Platform status must be reported precisely; do not claim a Windows/MSVC, Android, macOS or live Discord validation unless that exact path was actually run successfully.

Complete releases follow the same fail-closed principle: the release source must already contain the approved built-in `mods/` and `assets/menu/` trees, and platform packages must be built from that exact source state. Separate per-mod download packs are not the Complete engine architecture.
