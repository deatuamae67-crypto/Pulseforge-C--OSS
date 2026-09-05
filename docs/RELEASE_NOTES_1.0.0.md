# PulseForge 1.0.0

PulseForge 1.0.0 is the definitive stable release following the 0.9.7 pre-release.

## Highlights

- Compatibility validated against a complete 29-mod historical corpus; the Drive-root audit adds `taimuresu spam amplified` and `charts feitos no flp` beyond the historical 27-entry `modsList.txt`, while third-party payload redistribution remains separate from the OSS engine release.
- SC:R / SCReboot gameplay-start freeze fixed by isolating executable script discovery to the selected mod/content roots.
- Overkill/Timeless compatibility fixed end-to-end: glitch helpers no longer abort stage setup; `setScrollFactor`/`setLuaSpriteScrollFactor` follow Psych semantics; decimal beat/step plus resolved `mustHitSection` globals drive the modchart; the animation probe runs through bounded literal `string.find` plus an allow-listed `getAnimationName` bridge; and Psych easing names are matched case-insensitively with quad/cube/quart/quint/sine/circ families so the original fades and strum tweens do not silently fall back to linear.
- Existing cross-platform engine targets retained: Windows x86_64, Linux x86_64, macOS x86_64, macOS arm64 and Android arm64.
- Huge-chart PFC1/streaming behavior retained; large source charts are not forced through materializing loaders or Git blobs.
- Discord Social SDK remains an optional private dependency; proprietary SDK files are not redistributed in the OSS repository or public no-SDK builds.

## Distribution

The public release builds the core platform packages from the final 1.0.0 source/provenance chain. The external 29-mod corpus is represented by a deterministic compatibility manifest, not automatically bundled into the OSS binaries. Android CI output remains test-signed and macOS CI output remains ad-hoc signed unless separate production identities are supplied.
