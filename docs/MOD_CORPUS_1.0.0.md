# PulseForge 1.0.0 mod corpus

The definitive 1.0.0 distribution contains **29 runnable mods**. Twenty-seven are recorded in the historical `modsList.txt`; a direct audit of the Drive `mods` root found two additional runnable roots that the list omitted: **`taimuresu spam amplified`** (which contains Overkill) and **`charts feitos no flp`**. The complete runnable corpus contains **19,585 files** and **15,019,576,859 unpacked bytes**.

The public Git repository stores a deterministic inventory rather than committing roughly 15.0 GB of static content into Git history. The GitHub 1.0.0 release distributes the runnable content as separately checksummed assets sourced from the public read-only Drive corpus.

Two direct children of the Drive `mods` root are intentionally excluded because they are not runnable engine mods: `.autochart-staging` is transient AutoChart staging data, and `Hellbreaker Remake FLP` contains production/source material such as FLP/MIDI/audio rather than an installable mod tree.

`docs/MOD_CORPUS_1.0.0.json` records the Drive folder ID, default enablement, file count, unpacked byte count and SHA-256 of the sorted `path<TAB>size` inventory for every runnable mod. Release-package SHA-256 values are generated from the actual archives and are independent of these inventory hashes.

SCReboot is included as `drive-pack-screboot-demo`; executable Lua is scoped to the selected mod in 1.0.0 so sibling mods cannot block its countdown. Overkill is included inside `taimuresu-spam-amplified` and has a dedicated Lua compatibility regression in the 1.0.0 engine.
