# PulseForge 1.0.0 mod corpus

The definitive 1.0.0 distribution tracks **all 27 entries** in the engine's authoritative `modsList.txt`, including the two entries that are disabled by default. The corpus contains **19,547 mod files** and **14,429,581,335 unpacked bytes**; `modsList.txt` itself is distributed alongside the packs.

The public Git repository stores this deterministic inventory rather than committing roughly 14.4 GB of binary/static mod content into Git history. The GitHub 1.0.0 release packages every listed mod as a separately checksummed content asset sourced from the public read-only Drive corpus. `.autochart-staging` and auxiliary FLP/source folders are not engine mods and are not part of authoritative `modsList.txt`.

`docs/MOD_CORPUS_1.0.0.json` records the Drive folder ID, default enablement, file count, unpacked byte count and SHA-256 of the sorted `path<TAB>size` inventory for every pack. Release-package SHA-256 values are generated from the actual archives and are independent of these inventory hashes.

SCReboot is included as `drive-pack-screboot-demo`; executable Lua is scoped to the selected mod in 1.0.0 so sibling mods cannot block its countdown.
