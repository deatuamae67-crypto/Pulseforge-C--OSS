# PulseForge Complete runtime content

`PulseForge 1.0.0 Complete` no longer needs to duplicate the heavy mod corpus in every platform package once the Drive-backed runtime path is enabled.

## Startup contract

A Complete package may contain `assets/complete-runtime-manifest.json`. Ordinary OSS/dev packages without that file do not run the Complete content gate.

When the manifest is present, PulseForge:

1. resolves the writable mods root (`PULSEFORGE_MOD_ROOT`, the selected mod root, or an existing `mods` content root);
2. compares the manifest's per-mod revisions with `.pulseforge-complete-state.json`;
3. prompts only when at least one mod is missing or outdated;
4. if the user chooses **Não**, continues normally and asks again on a later launch;
5. if the user chooses **Sim**, downloads only pending files into revision-scoped staging directories;
6. keeps interrupted transfers as `.part` files for resume;
7. validates the complete staged tree against the manifest before installation;
8. installs through the existing bounded `install_mod()` pipeline and commits Complete state only after success.

The downloader is fail-open: offline/Drive errors never prevent already-installed or local content from launching.

## Network backends

- Windows: WinHTTP from the operating system.
- Linux/macOS: the platform libcurl loaded dynamically; no curl executable is spawned.
- Android: `HttpURLConnection` through `CompleteDownloadBridge` inside the APK.

All initial manifest URLs must use Google Drive HTTPS origins. The transport follows Google redirects and the large-file confirmation flow with cookies and bounded confirmation-page parsing.

## Security invariants

The runtime manifest parser rejects unsafe/absolute paths, duplicate paths, case-collisions, invalid revisions, non-Drive initial URLs and invalid slugs. Download staging rejects symlinks and unsupported file types and must exactly match the manifest path set plus descriptor file/byte floors before the normal mod installer is invoked. Existing mods are rolled back if replacement or state commit fails.

The heavy Git/LFS payloads must remain in the repository until this path and Complete packaging are validated end-to-end on every release platform. Only then may the materialized corpus be removed from Git while the descriptors/manifest metadata remain.
