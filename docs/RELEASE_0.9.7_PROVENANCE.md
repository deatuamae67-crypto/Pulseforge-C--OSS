# PulseForge 0.9.7 release provenance

This document records the final build, validation and publication provenance for the public PulseForge 0.9.7 OSS release.

## Final source state

- Final release commit on `main`: `8e603b240c8c11945da70b03a42e93b7babf763f`
- Git tag: `v0.9.7`, resolving to that exact commit
- GitHub Release ID: `382859979`
- GitHub Release target: `8e603b240c8c11945da70b03a42e93b7babf763f`
- Publication state: regular public release (`draft=false`, `prerelease=false`)
- Published: 5 September 2026

The final source includes the merged production macOS Discord-framework packaging work (#45) and the public synthetic Discord SDK integration validation (#46). No proprietary Discord SDK binary, AAR, framework, credential, token or keystore is committed to the public repository.

## Validation gates

The final head of #46 passed the required pull-request validation matrix before merge:

- PulseForge cross-platform build validation — success
- PulseForge deterministic core tests — success
- PulseForge OSS boundary validation — success
- PulseForge synthetic Discord SDK integration validation — success

After #45 and #46 were merged, the final `main` commit was built again by the release-artifact workflow.

## Final release-artifact run

Release-artifact run `33989617582` targets commit `8e603b240c8c11945da70b03a42e93b7babf763f` on `main` and completed successfully for all five supported release targets.

The five GitHub Actions artifact containers are retained until 5 October 2026. The published GitHub Release assets and `docs/RELEASE_0.9.7_ASSET_MANIFEST.json` are the durable distribution/integrity record.

Each package-level `SHA256SUMS.txt` was rechecked against its package before publication, and the live published Release was then revalidated by asset name, byte size and GitHub SHA-256 digest.

| Target | Package | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| Windows x86_64 | `PulseForge-v0.9.7-Windows-x86_64.zip` | 7,495,270 | `f6e31ca373e1bcd33584b3893b7fd17522fcc56d064ca329a2ef4e9df1c4b61c` |
| Linux x86_64 | `PulseForge-v0.9.7-Linux-x86_64.tar.gz` | 4,931,763 | `a6ecc06d9a3a89d17a6976dcdc239be344f1108ba2e2d6791edd4aad5da34bec` |
| macOS x86_64 | `PulseForge-v0.9.7-macOS-x86_64.tar.gz` | 4,154,575 | `1a0126735fb7d4e867afa9693d1eb8e2a8866013d3629035b03c52830de1c7b5` |
| macOS arm64 | `PulseForge-v0.9.7-macOS-arm64.tar.gz` | 3,635,767 | `90c570528d3d8b5c3e40793c3bcb3f8aeb8a88d5ca35c10bad200e17fc0bc797` |
| Android arm64, test-signed | `PulseForge-v0.9.7-Android-arm64-test-signed.apk` | 4,560,637 | `5b19402892265fafea3feadfd7b08458edd9638f4bd535103ce843d1ed78009a` |

The corresponding checksum-file digests are recorded in `docs/RELEASE_0.9.7_ASSET_MANIFEST.json` together with all ten exact asset sizes.

## Platform evidence

- Windows packages the PulseForge executables produced by the final MSVC release build.
- Linux is built as the x86_64 release package and its packaging validation checks the installed executable and runtime dependencies.
- macOS x86_64 and arm64 packages are produced on their native architecture runners and go through the repository's packaging/signing validation. The public OSS package remains usable without redistributing Discord's proprietary SDK.
- Android is `org.pulseforge.engine`, `versionCode=90700`, `versionName=0.9.7`, `targetSdk=35`, `compileSdk=36`, native ABI `arm64-v8a`. The public no-SDK build uses `minSdk=21`; a Discord-SDK-enabled private build requires `minSdk=24`. The published CI APK is deliberately test-signed.

## Discord SDK boundary

The release source supports optional Discord Social SDK integration but the public repository and ordinary OSS release artifacts do not redistribute the proprietary SDK.

Public synthetic integration tests exercise the SDK-enabled build paths without shipping Discord SDK code. Real private-SDK validation remains tracked separately for:

- Android AAR integration (#30)
- macOS framework integration (#42)
- Windows/Linux desktop runtime integration (#43)

Those private validation tickets are not publication provenance for the no-SDK OSS package and therefore remain open until tested against the privately supplied SDK package.

## Durable integrity record

`docs/RELEASE_0.9.7_ASSET_MANIFEST.json` records:

- release ID `382859979`;
- release target `8e603b240c8c11945da70b03a42e93b7babf763f`;
- source release-artifact run `33989617582`;
- final source SHA `8e603b240c8c11945da70b03a42e93b7babf763f`;
- all ten published asset names, exact byte sizes and GitHub SHA-256 digests.

`.github/workflows/release-integrity-validation.yml` is the read-only integrity verifier for this published state. It must not mutate the release.

## Publication sequence

Publication followed the required guarded sequence: final PR checks green → merge → final `main` → final release-artifact run → verified package/checksum set → `v0.9.7` at the final source SHA → replacement of the stale draft assets with the final run assets → live draft digest verification → publication → post-publication tag/target/asset verification.

The first one-shot publication attempt failed safely before any mutation because the GitHub CLI token environment was not exported. After adding the workflow token, the second run (`33990522815`) completed all provenance, package-hash, draft-asset, publication and post-publication checks successfully.

## Historical pre-final package set

Earlier release candidate provenance used release-artifact run `33891158742` and source `6e49caed8f7abbe5f2104632d48bdc93f4baa3b1`. That package set was superseded after later merges and must not be used as the final 0.9.7 distribution.

The older macOS x86_64 fallback from run `33883728198` is likewise historical evidence only.

## Distribution notes

The Android package is test-signed and the macOS packages use the repository's CI signing path; production store/notarized distribution may require platform-specific production credentials and signing infrastructure that are intentionally outside the OSS repository.

Reusable release text is stored in `docs/RELEASE_NOTES_0.9.7.md`.
