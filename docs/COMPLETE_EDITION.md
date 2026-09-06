# PulseForge Complete edition

The Complete edition is the canonical content-rich PulseForge distribution. Its approved mods and menu music are **part of the engine itself**: they are versioned in the repository and installed with the normal engine tree rather than published as optional per-mod content packs.

## Built-in engine layout

The source and installed runtime use the existing PulseForge layout:

- `mods/<mod-name>/...` — the complete approved mod library;
- `mods/modsList.txt` — the engine's historical/default mod enablement list;
- `assets/menu/*.mp3` — the ten approved menu/background tracks;
- normal engine source, assets and platform support alongside those directories.

`CMakeLists.txt` already installs `assets/` to `bin/assets` and, when present, `mods/` to `bin/mods`. Complete platform builds therefore consume the same built-in directories instead of reconstructing them from separate downloads.

## Repository storage

Large and binary built-in assets are stored with Git LFS. Textual chart/script/configuration files remain ordinary Git files where practical so that they stay reviewable and diffable. Files that exceed the ordinary Git blob safety threshold are automatically routed through LFS during the controlled import.

Git LFS is storage for files that are still versioned as part of this repository; it is not a runtime content-pack system. A normal LFS-enabled checkout of the Complete source contains the real files in `mods/` and `assets/menu/`.

## Provenance descriptors

`docs/complete/mods/*.json` remain as provenance and regression metadata only. They record the approved source folder, expected historical minimum file/byte counts, slug and default-enabled state used to audit the import.

They are **not** substitutes for the mod directories and are not used by the runtime to fetch content. The import pipeline materializes the corresponding Drive content into the repository tree and validates it before merge.

`docs/COMPLETE_CONTENT_1.0.0.json` likewise remains an audit specification for the Complete content set and the ten menu tracks. The Google Drive folders are import/source-of-truth inputs, not a runtime dependency.

`.autochart-staging` is intentionally excluded because it is a transient AutoChart work directory rather than engine content.

## Integration and integrity

The built-in content import is fail-closed:

1. exactly 30 approved mod descriptors must be present;
2. every mod folder is downloaded into `mods/<name>` and checked against its historical minimum file/byte count;
3. `mods/modsList.txt` is imported and size-checked;
4. all ten menu tracks are imported into `assets/menu` and checked against the recorded aggregate size;
5. private/non-redistributable engine inputs such as Discord Social SDK binaries and signing containers are rejected;
6. binary/large files are committed through Git LFS and ordinary Git blobs are kept below the repository safety threshold;
7. the accumulated content branch is verified to contain every expected mod directory, the mod list and all ten menu tracks before it can be merged.

## Release model

A Complete release is built from the repository state that already contains the built-in content. The release process must not publish separate per-mod ZIPs or require an installer to reconstruct the engine after download.

Platform packaging must include the repository's `mods/` and `assets/menu/` trees together with the executable/runtime files. Release publication remains fail-closed until those embedded trees are present and validated.

## Licensing and provenance boundary

The project owner has explicitly designated the selected Complete content set as part of PulseForge. The Apache-2.0 license covers PulseForge-authored source as described by `LICENSE` and `NOTICE`; independently authored components or assets retain their own notices and terms where applicable.

The Complete tree continues to exclude private signing material, secrets, build caches and proprietary Discord Social SDK binaries. Discord remains an optional external integration.
