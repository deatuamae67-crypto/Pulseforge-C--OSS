# Runtime assets

The public tree ships the redistributable runtime assets that are part of PulseForge itself: a CC0 procedural demo and the original Apache-2.0 shader presets. `settings.json` stays intentionally minimal so new installs use the code-defined defaults; the complete settings schema remains implemented by the settings loader without forcing a developer workspace's personal gameplay/preferences onto every user.

The engine also supports local media packs for menu/pause music, startup intros, credit portraits and optional visualizer images. Commercial or otherwise separately licensed media from the historical development workspace is not silently relicensed or bundled into the OSS repository. Missing optional media uses the engine's documented fallback behavior and must never wedge startup, menus or gameplay.

See the README files in each asset subdirectory for the public/private media boundary.
