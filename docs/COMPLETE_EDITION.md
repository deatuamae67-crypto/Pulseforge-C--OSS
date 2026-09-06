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

## Authoritative content source

`docs/COMPLETE_CONTENT_1.0.0.json` records the exact Google Drive folders used to construct the 1.0.0 Complete distribution, the minimum historical file/byte counts used as regression guards, and the ten menu tracks with their expected byte sizes.

The release workflow downloads the current contents of those approved folders. `.autochart-staging` is excluded because it is a transient AutoChart work directory rather than a distributable mod.

The historical inventory predates three later content folders, so the workflow does not assume the old inventory is the complete current tree. It enumerates and hashes the live approved folders at packaging time while retaining the historical totals as minimum regression thresholds where available.

## Integrity

Every mod pack receives a JSON manifest containing each file path, byte size and SHA-256 digest. Every archive receives a SHA-256 checksum. If a ZIP would exceed the per-asset upload ceiling, the workflow splits the ZIP into ordered parts below that ceiling and publishes checksums for both the reconstructed ZIP and each part.

The final publish job verifies that every configured mod has a manifest, checksum and archive (or split archive), that the five platform packages and their checksums exist, and that all Release assets are fully uploaded before changing the draft into a public release.

## Installation

Complete platform packages already contain the ten background/menu tracks. To install all mods, run the supplied installer from the extracted engine directory:

```text
PowerShell:  .\install-complete-content.ps1
Linux/macOS: ./install-complete-content.sh
```

The installers download the public `v1.0.0-complete` content assets, verify checksums, reconstruct split packs if needed and extract them into the engine content root.

## Licensing and provenance boundary

The project owner has explicitly approved the content set recorded in the Complete manifest for distribution with PulseForge. This approval is specific to the selected Complete content set and does not change the license of independently authored components embedded inside a mod.

The Apache-2.0 license continues to cover PulseForge-authored source as described by `LICENSE` and `NOTICE`. Third-party components retain their own notices and terms where applicable.

The Complete release still excludes private signing material, secrets, build caches and the proprietary Discord Social SDK binaries. Discord remains an optional external integration.
