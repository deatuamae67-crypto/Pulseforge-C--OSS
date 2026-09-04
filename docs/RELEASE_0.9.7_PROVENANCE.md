# PulseForge 0.9.7 release provenance

This document records the build, validation and publication provenance for the first public PulseForge 0.9.7 OSS release candidate.

## Source state

- Integration PR: #25
- Merge commit on `main`: `fe100c6f5ff5c69aff85ecf760696d5f0c5fd8d5`
- Last engine/build-source head with both PR validation matrices completed successfully: `fbb9ee22350a939071595721f5d6a35909d41a6d`
- Cross-platform build validation run: `33884590462` — success
- Deterministic core tests run: `33884590622` — success
- Release-workflow commit used for the preferred package set: `6e49caed8f7abbe5f2104632d48bdc93f4baa3b1`

The changes between the validated source head and the merge were limited to repository documentation and `.github` maintenance/CI files; there were no changes to `src/`, `include/`, `tests/`, `CMakeLists.txt`, `CMakePresets.json`, `platform/android/`, `assets/`, or `third_party/`.

The release-workflow commit changes only the macOS Intel runner label from `macos-15-intel` to `macos-26-intel`; it does not change PulseForge engine or build inputs. Documentation, governance and release-publication workflow commits after that point likewise do not alter the generated engine binaries.

## Preferred `main` release-artifact run

Release-artifact run `33891158742` targets commit `6e49caed8f7abbe5f2104632d48bdc93f4baa3b1` on `main` and completed successfully for all five supported release targets.

Each package-level SHA-256 file was independently rechecked after downloading the GitHub Actions artifact:

| Target | Package | SHA-256 | Status |
| --- | --- | --- | --- |
| Windows x86_64 | `PulseForge-v0.9.7-Windows-x86_64.zip` | `b70d5adf35fd2b071b54b0711e91215985feb41fd1bf78b5b8f5d16d444291df` | verified |
| Linux x86_64 | `PulseForge-v0.9.7-Linux-x86_64.tar.gz` | `e9063dda68e7f6e5c1a5692d5718883b458d1855819fe5a7815f7928483a36ce` | verified |
| macOS x86_64 | `PulseForge-v0.9.7-macOS-x86_64.tar.gz` | `de7a93432316b12f6e60a21398c4adec51dc43527863ce1c1d2ecbcff25c5b1d` | verified |
| macOS arm64 | `PulseForge-v0.9.7-macOS-arm64.tar.gz` | `aebfea49ac288c93343776df759bfacb71d837057bf5deefd5263df74562204e` | verified |
| Android arm64 | `PulseForge-v0.9.7-Android-arm64-test-signed.apk` | `848e0b42ee581ac801260eb81828d6f17ec5d5925b154546cb7fb906f41a2a18` | verified |

Independent package evidence also confirms:

- Windows contains both `bin/pulseforge.exe` and `bin/pulseforge-cli.exe`.
- Linux is an x86-64 ELF executable and the recorded `ldd` output contains no unresolved shared libraries.
- macOS x86_64 is a Mach-O 64-bit `x86_64` executable and passed the workflow's ad-hoc signing/codesign verification.
- macOS arm64 is a Mach-O 64-bit `arm64` executable and passed the workflow's ad-hoc signing/codesign verification.
- Android identifies as `org.pulseforge.engine`, `versionCode=90700`, `versionName=0.9.7`, `minSdkVersion=21`, `targetSdkVersion=35`, with native code `arm64-v8a`; the CI artifact is deliberately test-signed.

The preferred current package set therefore no longer depends on the earlier macOS x86_64 fallback.

## Durable asset-integrity record

GitHub Actions artifact retention is finite. The five workflow artifacts from run `33891158742` are currently retained until 4 October 2026; after that point the workflow ZIPs may no longer be downloadable even though the reviewed GitHub Release assets remain intact.

`docs/RELEASE_0.9.7_ASSET_MANIFEST.json` is therefore the durable integrity record for the reviewed `v0.9.7` package set. It records:

- release ID `382859979`;
- release target `b1e7b048a63118299c5fad795f2b4c607f931a86`;
- source run `33891158742` and its workflow/source SHA;
- the exact ten reviewed Release asset names;
- each asset's exact byte size;
- each asset's GitHub SHA-256 digest.

Before this manifest was committed, all ten current GitHub Release assets were independently cross-checked against the files extracted directly from run `33891158742`: names, byte sizes and SHA-256 values matched 10/10.

The read-only `.github/workflows/release-integrity-validation.yml` workflow continuously validates the durable manifest, source-run provenance and current GitHub Release asset metadata. It has only `actions: read` and `contents: read` permissions and cannot publish or mutate a release.

The controlled publisher still downloads and re-hashes the Actions artifacts while all five remain retained. After those artifacts expire, it may use the reviewed durable manifest only to validate an already-existing release whose ten assets still match exactly. The manifest is verification evidence only: it is never used to reconstruct package bytes. If the existing draft is deleted after the Actions artifacts become unavailable, the publisher refuses to recreate it.

## Historical macOS x86_64 fallback

Before `macos-26-intel` capacity was used successfully, release-artifact run `33883728198` provided a validated macOS x86_64 fallback built from commit `4063a05110be16f044809c58048ea9baae74f590` with unchanged engine/build inputs.

That historical fallback remains documented for auditability only:

- package: `PulseForge-v0.9.7-macOS-x86_64.tar.gz`
- SHA-256: `a0c2a48dab31f9cafd0e2f60c48a682a994fe64967468eef8aac986b91d7a0c4`
- architecture: Mach-O 64-bit `x86_64`
- workflow result: successful build, ad-hoc signing, codesign verification and artifact upload

It should not be used for the final release when the complete preferred package set from run `33891158742` is available.

## Distribution boundary

These CI packages are public OSS/no-SDK builds unless the Discord Social SDK and its required platform runtime are explicitly supplied. They must not be represented as final Discord-enabled packages unless `docs/RELEASE_REQUIREMENTS.md` is satisfied.

The current macOS packages do not link `discord_partner_sdk` in this OSS/no-SDK package set.

The Android package is test-signed and the macOS packages are ad-hoc signed; production distribution requires the appropriate production signing/notarization process.

## Publication status

A GitHub Release draft named `PulseForge 0.9.7` exists as release ID `382859979` with reserved tag name `v0.9.7` and target commit `b1e7b048a63118299c5fad795f2b4c607f931a86`. It contains the five reviewed platform packages plus the five corresponding `SHA256SUMS.txt` files.

The release remains a draft and has not been published publicly. The actual Git ref `refs/tags/v0.9.7` is not materialized while the release remains in this draft state.

Reusable GitHub Release text is stored in `docs/RELEASE_NOTES_0.9.7.md`. Public publication is controlled by `.github/workflows/publish-release.yml`, which is owner-only, must be dispatched from `main`, validates the approved release-artifact run provenance, validates the durable asset manifest, and refuses direct public creation without a reviewed draft.
