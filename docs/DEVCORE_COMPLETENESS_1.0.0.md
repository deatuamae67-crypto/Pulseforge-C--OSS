# DevCore → OSS completeness audit for PulseForge 1.0.0

This document records the final release-freeze comparison between the canonical `PulseForge_DevCore_2026-08-31_20-58-49` workspace and the public `Pulseforge-C--OSS` 1.0.0 branch.

## Source and runtime

The canonical DevCore and the public 1.0.0 branch contain the same active application architecture: launcher/application runtime, editors, streaming/PFC1 gameplay, content and mod loading, Lua compatibility layer, media hub/routes, startup intro support, PS2 theme, post effects, note skins, AutoChart integration, Discord presence integration, mobile touch controls and cross-platform platform code. Several public-branch application files are newer than the canonical DevCore snapshot because the 1.0.0 compatibility fixes were developed after that snapshot. The release therefore does **not** bulk-copy older Drive sources over the newer GitHub sources.

Historical `*.pre_*`, backup directories, generated build outputs, local caches and superseded source snapshots are evidence/reference material rather than active release source and are not copied into the public tree.

## Redistributable assets restored

The public tree restores the active DevCore assets whose redistribution basis is explicit:

- `assets/demo/` — original `Neon Circuit` procedural demo, declared CC0-1.0;
- `assets/shaders/` — original PulseForge GLSL-source presets distributed under Apache-2.0;
- media-extension README contracts for menu/pause music, credits portraits and optional UI/visualizer media.

The asset directory is installed by the existing CMake packaging rules, so these files ship in desktop release packages and follow the existing Android asset-packaging path.

## Media intentionally external

The complete historical workspace also contains local media used during development. It is not automatically part of the Apache-2.0 source grant. In particular, the historical menu playlist includes commercial/third-party recordings and the historical intro material includes Watch Dogs/ctOS-derived media. Credit/profile images and some optional UI binaries also lack an explicit Apache-2.0 redistribution grant in the audited DevCore metadata.

PulseForge keeps the runtime support for these local files, but the public release does not silently relicense or bundle them. Users can install legally obtained media in the documented locations or select external files through the existing settings. Missing optional media must fall back safely and cannot block startup, menus or gameplay.

## Settings policy

The canonical DevCore `assets/settings.json` is a developer-state file containing choices such as autoplay/BOTPLAY, practice mode, downscroll, quality/performance options, theme choices and a specific local menu track. Copying it verbatim would change fresh-install behavior. The OSS release therefore keeps a minimal settings seed and relies on the complete code-defined settings defaults/schema.

## Mod corpus

The 1.0.0 compatibility audit covers 29 historical mods (19,585 files; 15,019,576,859 bytes in the audited corpus). The public repository stores a deterministic corpus manifest and regression fixtures, not the approximately 15 GB of third-party mod payloads. Engine fixes are generic compatibility fixes; copyrighted mod payloads remain external unless their individual redistribution rights are established.

## Release rule

A file being present in the private/historical workspace is not, by itself, sufficient reason to publish it. The 1.0.0 public release includes active source and explicitly redistributable project assets; proprietary SDKs, credentials/signing material, commercial media, unlicensed third-party mod payloads, backups and generated artifacts remain outside Git history. This boundary preserves the complete engine functionality without turning the OSS release into an unlicensed mirror of the development machine.
