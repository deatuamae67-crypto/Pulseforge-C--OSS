# PulseForge 1.0.0

PulseForge 1.0.0 is the definitive stable release following the 0.9.7 pre-release.

## Highlights

- The approved historical/current mod corpus is integrated into the PulseForge engine tree rather than represented only by an external compatibility manifest. The Complete source contains the real `mods/<collection>/...` directories and `mods/modsList.txt`; large/binary objects are versioned through Git LFS.
- The ten approved background/menu tracks are integrated under `assets/menu/` as engine assets.
- SC:R / SCReboot gameplay-start freeze fixed by isolating executable script discovery to the selected mod/content roots.
- Overkill/Timeless compatibility fixed end-to-end: glitch helpers no longer abort stage setup; `setScrollFactor`/`setLuaSpriteScrollFactor` follow Psych semantics; decimal beat/step plus resolved `mustHitSection` globals drive the modchart; the animation probe runs through bounded literal `string.find` plus an allow-listed `getAnimationName` bridge; and Psych easing names are matched case-insensitively with quad/cube/quart/quint/sine/circ families so the original fades and strum tweens do not silently fall back to linear.
- The redistributable DevCore asset tier is restored in the OSS tree: the original CC0 `Neon Circuit` demo plus the original Apache-2.0 PulseForge shader presets ship with the engine.
- Menu/pause music, startup-intro, credits-portrait and visualizer-media integration remain supported. The specifically approved Complete menu tracks are built-in engine content; unrelated private/development media remains outside the public tree unless explicitly approved for inclusion.
- Public `assets/settings.json` stays minimal by design so a release does not inherit the developer workspace's BOTPLAY/practice/performance/theme preferences; the complete settings schema and code-defined defaults remain in the engine.
- Existing cross-platform engine targets retained: Windows x86_64, Linux x86_64, macOS x86_64, macOS arm64 and Android arm64.
- Huge-chart PFC1/streaming behavior retained; large source charts are not forced through materializing loaders.
- Discord Social SDK remains an optional private dependency; proprietary SDK files are not redistributed in the OSS repository or public no-SDK builds.

## Built-in content model

The Google Drive corpus and `docs/complete/mods/*.json` records are import/provenance inputs. They do not substitute for runtime files. The integrated source/runtime tree uses the native PulseForge layout:

```text
mods/modsList.txt
mods/<approved collection>/...
assets/menu/<approved track>.mp3
```

`CMakeLists.txt` installs `assets/` and `mods/` with the engine. Git LFS is used only as repository storage for large/binary built-in files; it is not an optional runtime content downloader.

## Distribution

Complete platform packages are built from the final source state that already contains the approved mod/menu tree. There are no separate per-mod installation packs in the corrected Complete architecture. Android CI output remains test-signed and macOS CI output remains ad-hoc signed unless separate production identities are supplied.

Independently authored assets/components retain their applicable notices and terms. Private signing material, credentials, build caches and proprietary Discord Social SDK binaries remain excluded.
