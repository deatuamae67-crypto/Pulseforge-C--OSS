# PulseForge 1.0.0

PulseForge 1.0.0 is the definitive stable release following the 0.9.7 pre-release.

## Highlights

- Compatibility validated against a complete 29-mod historical corpus; the Drive-root audit adds `taimuresu spam amplified` and `charts feitos no flp` beyond the historical 27-entry `modsList.txt`, while third-party payload redistribution remains separate from the OSS engine release.
- SC:R / SCReboot gameplay-start freeze fixed by isolating executable script discovery to the selected mod/content roots.
- Overkill/Timeless compatibility fixed end-to-end: glitch helpers no longer abort stage setup; `setScrollFactor`/`setLuaSpriteScrollFactor` follow Psych semantics; decimal beat/step plus resolved `mustHitSection` globals drive the modchart; the animation probe runs through bounded literal `string.find` plus an allow-listed `getAnimationName` bridge; and Psych easing names are matched case-insensitively with quad/cube/quart/quint/sine/circ families so the original fades and strum tweens do not silently fall back to linear.
- The redistributable DevCore asset tier is restored in the OSS tree: the original CC0 `Neon Circuit` demo plus the original Apache-2.0 PulseForge shader presets ship with the engine.
- Menu/pause music, startup-intro, credits-portrait and visualizer-media integration remain fully supported as local media extension points. The historical commercial/third-party media library is intentionally not relicensed or bundled by the Apache-2.0 GitHub release; clean OSS checkouts now exercise deterministic fallbacks instead of depending on those private files.
- Public `assets/settings.json` stays minimal by design so a release does not inherit the developer workspace's BOTPLAY/practice/performance/theme/media preferences; the complete settings schema and code-defined defaults remain in the engine.
- Existing cross-platform engine targets retained: Windows x86_64, Linux x86_64, macOS x86_64, macOS arm64 and Android arm64.
- Huge-chart PFC1/streaming behavior retained; large source charts are not forced through materializing loaders or Git blobs.
- Discord Social SDK remains an optional private dependency; proprietary SDK files are not redistributed in the OSS repository or public no-SDK builds.

## Distribution

The public release builds the core platform packages from the final 1.0.0 source/provenance chain. The external 29-mod corpus is represented by a deterministic compatibility manifest, not automatically bundled into the OSS binaries. The historical local media corpus is likewise external unless an individual asset has an explicit redistributable license. Android CI output remains test-signed and macOS CI output remains ad-hoc signed unless separate production identities are supplied.
