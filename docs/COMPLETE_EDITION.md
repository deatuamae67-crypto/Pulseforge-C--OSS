# PulseForge Complete edition

The Complete edition is the content-rich distribution of PulseForge. It keeps the public source tree lean while publishing the project-approved menu music and mod library as versioned GitHub Release assets.

## Distribution model

A Complete release contains:

- the normal Windows, Linux, macOS arm64, macOS x86_64 and Android arm64 engine packages;
- all ten approved menu-music tracks inside every Complete platform build;
- the current approved `mods/` collections as common content packs, so the same multi-gigabyte mod library is not duplicated inside every platform archive;
- `modsList.txt`, per-pack file manifests and SHA-256 checksum files;
- installer scripts that reconstruct split packs when necessary and install the content into the engine's `mods/` and `assets/menu/` roots.

The Complete content is deliberately distributed through GitHub Releases instead of being committed as ordinary Git blobs. This avoids bloating repository history and keeps generated release payloads outside source control.

## One mod per pull request

The Complete mod library is integrated incrementally. Each mod has one descriptor under `docs/complete/mods/<slug>.json`, and each pull request is required to introduce exactly one new descriptor.

The single-mod CI path downloads only that mod, validates its minimum historical file/byte counts, hashes every file and proves that the ZIP can be reconstructed. A green PR can therefore be merged independently while other mod PRs continue validating in parallel.

After such a PR is merged into `main`, the same workflow downloads and packages only that mod and uploads its manifest, checksum and ZIP (or split ZIP parts) into the existing `v1.0.0-complete` draft Release. No previously integrated mod is rebuilt or re-uploaded.

The Complete Release remains a draft while this fan-out proceeds. Publication is attempted after every successful mod integration and succeeds only when all 30 expected descriptor files have landed and all 30 corresponding Release packs plus the platform/menu assets are present. The tag is then moved to the final `main` commit containing the complete registry before the Release becomes public.

## Authoritative content source

`docs/COMPLETE_CONTENT_1.0.0.json` records the shared Complete source, the ten menu tracks with their expected byte sizes and the target number of independently integrated mods. Each mod's exact Drive folder, default-enabled state and historical regression minima are stored in its own descriptor under `docs/complete/mods/`.

The release workflows download the current contents of those approved folders. `.autochart-staging` is excluded because it is a transient AutoChart work directory rather than a distributable mod.

The historical inventory predates three later content folders, so the workflow does not assume the old inventory is the complete current tree. It enumerates and hashes the live approved folders at packaging time while retaining the historical totals as minimum regression thresholds where available.

## Integrity

Every mod pack receives a JSON manifest containing each file path, byte size and SHA-256 digest. Every archive receives a SHA-256 checksum. If a ZIP would exceed the per-asset upload ceiling, the workflow splits the ZIP into ordered parts below that ceiling and publishes checksums for both the reconstructed ZIP and each part.

The final publication gate verifies that every configured mod has a manifest, checksum and archive (or split archive), that all five platform packages and the menu-music pack exist, and that the assets are fully uploaded before changing the draft into a public release.

## Installation

Complete platform packages already contain the ten background/menu tracks. To install all mods, run the supplied installer from the extracted engine directory:

```text
PowerShell:  .\install-complete-content.ps1
Linux/macOS: ./install-complete-content.sh
```

The installers download the public `v1.0.0-complete` content assets, verify checksums, reconstruct split packs if needed and extract them into the engine content root.

## Licensing and provenance boundary

The project owner has explicitly approved the Complete content set for distribution with PulseForge. This approval is specific to the selected Complete content set and does not change the license of independently authored components embedded inside a mod.

The Apache-2.0 license continues to cover PulseForge-authored source as described by `LICENSE` and `NOTICE`. Third-party components retain their own notices and terms where applicable.

The Complete release still excludes private signing material, secrets, build caches and the proprietary Discord Social SDK binaries. Discord remains an optional external integration.
