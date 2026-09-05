# PulseForge 1.0.0 release provenance

This document records the final build, validation and publication provenance for the public PulseForge 1.0.0 OSS release.

## Final release source

- Release source commit on `main`: `c0ccce44f2ef31819294f127d0b41095d6042d86`
- Release PR: `#47` (`release: PulseForge 1.0.0`)
- Final PR head validated before merge: `37c439fef3680a503584c711e12280ceca3fd107`
- Git tag: `v1.0.0`, resolving to the release source commit
- GitHub Release ID: `383408852`
- GitHub Release target: `c0ccce44f2ef31819294f127d0b41095d6042d86`
- Publication state: regular public release (`draft=false`, `prerelease=false`)
- Published: 6 September 2026 in the project owner's Europe/Lisbon timezone (5 September 2026 23:53:59 UTC)

The later documentation-only homepage refresh is intentionally not part of the `v1.0.0` tag. The tag remains attached to the exact source commit from which the published binaries were rebuilt and verified.

## Pre-merge validation gates

The final head of PR #47 passed every release-freeze gate before merge:

- PulseForge cross-platform build validation — success
- PulseForge deterministic core tests — success
- PulseForge OSS boundary validation — success
- PulseForge synthetic Discord SDK integration validation — success
- PulseForge macOS Discord framework packaging validation — success

The synthetic Discord Linux validation was corrected before the final green run so it asserted the current CMake status string (`desktop shared-library enabled`) rather than the obsolete status wording. This changed CI validation, not the Discord runtime behavior.

## Final release-artifact run

Release-artifact run `33999373364` targeted `main` commit `c0ccce44f2ef31819294f127d0b41095d6042d86` and completed successfully for all five supported release targets:

- Windows x86_64 — success
- Linux x86_64 — success
- macOS arm64 — success
- macOS x86_64 — success
- Android arm64-v8a — success
- `Publish verified v1.0.0` — success

The publication job downloaded the artifacts from that same run, verified every package-level checksum, verified the exact ten-file public asset set, created a draft release, uploaded the assets, verified the uploaded state and only then published the release.

GitHub Actions artifact containers from this run expire on 5 October 2026. The published GitHub Release and `docs/RELEASE_1.0.0_ASSET_MANIFEST.json` are the durable distribution/integrity record.

## Published packages

| Target | Package | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| Windows x86_64 | `PulseForge-v1.0.0-Windows-x86_64.zip` | 7,513,984 | `561244c74cb5a43f45fa4f55f451138c9f9a2f7083af692bd68b1fa51be5bc6b` |
| Linux x86_64 | `PulseForge-v1.0.0-Linux-x86_64.tar.gz` | 4,943,551 | `691c8be9a52850321e74e46ed1967fce858f498049e9946318449ae0c0ddc8c3` |
| macOS arm64 | `PulseForge-v1.0.0-macOS-arm64.tar.gz` | 3,658,624 | `a6e403061e90297c31a39742b21d10528d66c68bef1a7308f3420423df3afde8` |
| macOS x86_64 | `PulseForge-v1.0.0-macOS-x86_64.tar.gz` | 4,175,820 | `f6b796e7fe5fa144373c0bd0bd2cd7d262ccf02bcb049abcaa1ad4a2449c7004` |
| Android arm64, test-signed | `PulseForge-v1.0.0-Android-arm64-test-signed.apk` | 4,569,811 | `95d91dbe40c094db990d6dab922a82800019adca679cb70ab895c8fffa0f7642` |

The corresponding five `SHA256SUMS.txt` files, their exact sizes and their own GitHub SHA-256 digests are recorded in `docs/RELEASE_1.0.0_ASSET_MANIFEST.json`.

## 1.0.0 compatibility scope

The release line contains the final compatibility work for the audited historical corpus, including the SC:R / SCReboot gameplay-start freeze fix and the Overkill/Timeless Psych-Lua compatibility work. The public repository stores deterministic manifests and regression fixtures rather than redistributing the third-party mod corpus.

The public asset tier also restores the original CC0 `Neon Circuit` procedural demo and PulseForge-authored Apache-2.0 shader presets. Commercial music, Watch Dogs-derived media, private mod payloads and other media without an explicit redistribution grant remain external while their engine integration points remain supported.

## Platform and signing notes

- Windows and Linux packages are ordinary public OSS CI packages.
- macOS packages use the repository CI signing path and are not represented as production-notarized App Store packages.
- Android uses application ID `org.pulseforge.engine`; the public APK is deliberately test-signed. Production/store updates require the project owner's persistent signing identity.
- The proprietary Discord Social SDK remains optional and is not redistributed by the OSS source tree or ordinary public release packages. Synthetic CI validates supported SDK-enabled integration paths without committing proprietary SDK binaries.

## Durable integrity record

`docs/RELEASE_1.0.0_ASSET_MANIFEST.json` records:

- release ID `383408852`;
- release target `c0ccce44f2ef31819294f127d0b41095d6042d86`;
- source release-artifact run `33999373364`;
- final release source SHA `c0ccce44f2ef31819294f127d0b41095d6042d86`;
- all ten published asset names, exact byte sizes and GitHub SHA-256 digests.

`.github/workflows/release-integrity-validation.yml` is the read-only verifier for this published state. It must not mutate the release.

## Publication sequence

Publication followed the guarded sequence:

final PR checks green → merge PR #47 → final release source on `main` → release-artifact run from that exact commit → five platform packages complete → ten-file package/checksum set verified → draft release created and populated → uploaded assets verified → `v1.0.0` published → live release/tag target verified.

Release-specific provenance remains immutable historical documentation even as `main` advances with later documentation and development commits.
