# PulseForge 1.0.0 Complete

PulseForge 1.0.0 Complete is the **current PulseForge engine runtime plus the Complete content manifest**. It is distributed in the same practical form as the normal 1.0.0 engine builds: one compiled runtime per supported platform. The heavy mod corpus is **not duplicated inside every release package**.

## What the Complete edition contains

Every Complete build is produced from one exact `main` commit for:

- Windows x86_64;
- Linux x86_64;
- macOS arm64;
- macOS x86_64;
- Android arm64.

The packages contain the current engine, its normal runtime assets and `assets/complete-runtime-manifest.json`. That manifest describes the 30 approved Complete content collections and their canonical Google Drive sources.

The release packages do **not** embed a multi-gigabyte `mods/` tree and do not contain separate per-mod ZIP bundles.

## Runtime Complete content

When `complete-runtime-manifest.json` is present, PulseForge checks the local Complete state at startup. If content is missing or outdated, the engine asks whether it should download the pending content from the canonical Drive folders.

If the user accepts, PulseForge downloads only the required files, supports interrupted-transfer resume through `.part` files, validates the complete staged tree, and installs through the normal bounded/atomic mod-install pipeline. The state is committed only after installation succeeds.

If the user declines, is offline, or Google Drive is temporarily unavailable, the engine continues to launch normally. The Complete downloader is deliberately fail-open.

See `docs/complete/RUNTIME_INSTALL.md` for the full runtime contract and security invariants.

## Content source and provenance

`docs/COMPLETE_CONTENT_1.0.0.json`, `docs/MOD_CORPUS_1.0.0.json` and `docs/complete/mods/*.json` retain the Complete content inventory, historical source thresholds, current recoverable materialization thresholds and Google Drive provenance.

Historical source counts are retained separately when the currently recoverable public Drive snapshot contains fewer files but preserves the verified payload represented by the corpus inventory. Runtime generation validates against the current materialized threshold rather than pretending that unavailable historical files are still present.

## Runtime/package layout

Complete follows the normal PulseForge runtime layout rather than shipping a second embedded game tree:

```text
PulseForge/
  bin/pulseforge[.exe]
  bin/assets/...
  # no bundled multi-gigabyte mods/ corpus
```

On Android, Complete is distributed as the installable arm64 APK. The APK contains the Complete runtime manifest and downloads missing Complete content only after the user accepts the prompt.

Desktop releases use the same user-facing archive formats as the normal engine distribution: `.zip` on Windows and `.tar.gz` on Linux/macOS, with SHA-256 checksum files. Android is published directly as an `.apk` plus its checksum.

## Engine state represented by the release

`v1.0.0-complete` is updated to the exact tested `main` commit used to compile the five runtimes. The release body records that source SHA, so the Complete tag and published binaries represent the current verified engine snapshot rather than the older built-in-content package.

This includes the current Android storage/file-picker path: files selected through Android's system picker are materialized into app-accessible storage before native importers process them, allowing mod ZIPs selected from locations such as Downloads to pass the normal installer validation.

## Gameplay compatibility

- SC:R / SCReboot gameplay-start freeze handling remains present.
- Overkill/Timeless compatibility retains the Psych-compatible Lua/tween/animation behavior added for 1.0.0.
- Huge-chart PFC1/streaming behavior remains available for very large charts.

## Distribution boundary

Independently authored content retains its applicable notices and terms; inclusion in the Complete manifest does not silently relicense it under Apache-2.0.

The Complete distribution does **not** include proprietary Discord Social SDK binaries, private signing keys, tokens, credentials, build caches or development backups. Android CI builds remain test-signed unless a production signing identity is supplied; macOS CI builds remain ad-hoc signed unless production signing/notarization is supplied.
