# PulseForge 1.0.0 Complete

PulseForge 1.0.0 Complete is the full-content companion distribution of PulseForge 1.0.0.

## What makes this the Complete edition

- all **10 approved background/menu tracks** are bundled into the Complete engine packages;
- all **30 approved current mod collections** are published as common versioned content packs;
- the historical mod corpus alone accounts for more than **13.4 GiB** and **19,500 files**, before three newer folders that were added after the historical inventory;
- each mod pack is enumerated at release time and receives a per-file SHA-256 manifest;
- large packs are automatically split into ordered Release assets when required, without changing the reconstructed ZIP checksum;
- Windows x86_64, Linux x86_64, macOS arm64, macOS x86_64 and Android arm64 packages are built from the same tagged source.

The engine packages do not duplicate the multi-gigabyte mod library five times. The mods are common Release content packs, and the included installer scripts place them into the correct `mods/` root. The menu music is embedded directly in each Complete platform package, including the Android APK asset bundle.

## Content source and validation

The approved content definition is recorded in `docs/COMPLETE_CONTENT_1.0.0.json`. The workflow downloads the live approved Drive folders, excludes only the transient `.autochart-staging` directory, validates historical minimum file/byte counts where available and hashes the files before publication.

The Complete release is fail-closed: it is created as a draft and becomes public only after the platform builds, menu content, mod packs, manifests and checksums are all present and verified.

## Installing the mods

After extracting/installing the platform package, run:

```text
# Linux / macOS
./install-complete-content.sh

# Windows PowerShell
.\install-complete-content.ps1
```

The scripts download the mod assets from this Release, validate SHA-256 checksums, reconstruct any split archives and extract the content into the engine directory.

## Distribution boundary

This Complete content set is published with the project owner's explicit authorization. Independently authored components retain their own applicable notices/terms; inclusion does not silently relicense them under Apache-2.0.

The public Complete distribution does **not** include the proprietary Discord Social SDK binaries, private signing keys, tokens, credentials, build caches or development backups. Android is test-signed by CI unless a separate production signing identity is supplied.
