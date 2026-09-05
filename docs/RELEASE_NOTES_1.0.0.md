# PulseForge 1.0.0

PulseForge 1.0.0 is the definitive stable release following the 0.9.7 pre-release.

## Highlights

- Complete 29-mod runnable engine corpus distributed as checksummed GitHub release content assets; the Drive-root audit adds `taimuresu spam amplified` and `charts feitos no flp` beyond the historical 27-entry `modsList.txt`.
- SC:R / SCReboot gameplay-start freeze fixed by isolating executable script discovery to the selected mod/content roots.
- Overkill/Timeless compatibility fixed so unsupported glitch helpers no longer abort stage setup, single-argument `setScrollFactor` follows Psych semantics, and decimal beat/step globals drive its modchart motion.
- Existing cross-platform engine targets retained: Windows x86_64, Linux x86_64, macOS x86_64, macOS arm64 and Android arm64.
- Huge-chart PFC1/streaming behavior retained; large source charts are not forced through materializing loaders or Git blobs.
- Discord Social SDK remains an optional private dependency; proprietary SDK files are not redistributed in the OSS repository or public no-SDK builds.

## Distribution

The core platform packages and all mod-content packages are built/packaged from the final 1.0.0 source/provenance chain. Android CI output remains test-signed and macOS CI output remains ad-hoc signed unless separate production identities are supplied.
