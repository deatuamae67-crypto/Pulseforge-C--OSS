# PulseForge 1.0.0 Complete

PulseForge 1.0.0 Complete is the full-content PulseForge distribution. The approved mods and menu music are not optional add-ons: **they are part of the engine tree itself**.

## What makes this the Complete edition

- all **10 approved background/menu tracks** are versioned under `assets/menu/` and ship with the engine;
- all **30 approved current mod/content collections** are versioned under `mods/` and ship with the engine;
- `mods/modsList.txt` is included as the engine's historical/default enablement list;
- large/binary source assets are stored through Git LFS while textual charts/scripts/configuration remain reviewable Git files where practical;
- Windows x86_64, Linux x86_64, macOS arm64, macOS x86_64 and Android arm64 builds are produced from the same source state containing those built-in assets.

There are no per-mod Complete ZIPs and no post-install content downloader. A Complete checkout/build already contains its Complete content.

## Content source and validation

`docs/COMPLETE_CONTENT_1.0.0.json`, `docs/MOD_CORPUS_1.0.0.json` and `docs/complete/mods/*.json` retain import/provenance and regression metadata. The corresponding Google Drive folders are controlled source inputs used to materialize the real repository tree, not runtime dependencies.

The built-in import validates historical minimum file/byte counts, imports the ten menu tracks and `modsList.txt`, rejects `.autochart-staging`, rejects private signing/Discord SDK material, routes large files through Git LFS and verifies that all 30 expected `mods/<name>` roots exist before integration.

## Runtime/package layout

The normal PulseForge installation layout is used directly:

```text
PulseForge/
  bin/pulseforge[.exe]
  bin/assets/menu/...
  bin/mods/modsList.txt
  bin/mods/<built-in collection>/...
```

On desktop platforms this follows the existing CMake install rules for `assets/` and `mods/`. Platform-specific packaging must preserve the same built-in content semantics.

## Gameplay compatibility

- SC:R / SCReboot gameplay-start freeze is fixed by isolating executable script discovery to the selected mod/content roots.
- Overkill/Timeless compatibility retains the Psych-compatible Lua/tween/animation behavior added for 1.0.0.
- Huge-chart PFC1/streaming behavior remains available for very large charts.

## Distribution boundary

The selected Complete content set is designated as part of PulseForge. Independently authored components/assets retain their applicable notices/terms; inclusion does not silently relicense them under Apache-2.0.

The Complete distribution does **not** include proprietary Discord Social SDK binaries, private signing keys, tokens, credentials, build caches or development backups. Android CI remains test-signed unless a separate production signing identity is supplied; macOS CI remains ad-hoc signed unless production signing/notarization is supplied.
