# PulseForge 1.0.0 built-in mod/content corpus

PulseForge 1.0.0 Complete carries the approved historical/current content tree **inside the engine repository and runtime layout**. The curated set contains **30 top-level collections** under `mods/`: the 29 runnable/compatibility roots identified by the historical audit plus `Hellbreaker Remake FLP`, which is retained as project source/content material rather than treated as a runnable mod.

Twenty-seven runnable roots are recorded in the historical `modsList.txt`; the Drive-root audit also found `taimuresu spam amplified` (which contains Overkill) and `charts feitos no flp`. The historical runnable corpus alone contains 19,585 files and 15,019,576,859 unpacked bytes; the Complete tree additionally retains the later/current curated folders represented under `docs/complete/mods/`.

## Repository/runtime model

The Drive tree is an import/provenance source, not a runtime dependency. The actual files are versioned in the PulseForge repository:

- `mods/<collection>/...` contains the real mod/content files;
- `mods/modsList.txt` contains the historical/default enablement list used by the engine catalog;
- large/binary objects are stored through Git LFS while textual charts/scripts/configuration remain ordinary Git files where practical.

`docs/MOD_CORPUS_1.0.0.json` and `docs/complete/mods/*.json` are audit metadata for the real tree. They do not replace the corresponding files under `mods/`.

`.autochart-staging` remains excluded because it is transient AutoChart staging data, not engine content. `Hellbreaker Remake FLP` is included in the Complete tree but is not advertised as a runnable mod because it contains production/source material such as FLP/MIDI/audio.

SCReboot is included as `drive-pack-screboot-demo`; executable Lua is scoped to the selected mod in 1.0.0 so sibling mods cannot block its countdown. Overkill is included inside `taimuresu spam amplified` and has a dedicated Lua compatibility regression in the 1.0.0 engine.

## Distribution boundary

The selected Complete content set is part of PulseForge and is shipped with Complete engine builds. Independently authored components/assets retain their applicable notices and terms; inclusion in the repository does not silently relicense them under Apache-2.0.

Private signing material, secrets, build caches and proprietary Discord Social SDK binaries remain excluded from the repository and release artifacts.
