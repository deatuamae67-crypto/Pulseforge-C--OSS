# Security Policy

## Supported version

Security fixes are developed against the current public `main` line and the active integration branch for the next PulseForge release. Older development snapshots, cumulative archives and private validation packages are not maintained as independently supported release lines.

## Reporting a vulnerability

Please do **not** open a public issue for a vulnerability that could expose users, credentials, signing material, local files, arbitrary code execution paths or other security-sensitive behavior.

Prefer GitHub's private vulnerability reporting/security-advisory flow when it is available for this repository. If private reporting is not available, contact the repository maintainer privately through their GitHub profile rather than publishing exploit details in an issue.

A useful report includes:

- affected PulseForge version/commit;
- operating system and architecture;
- affected component;
- minimal reproduction steps;
- expected versus observed behavior;
- security impact;
- logs or crash information with tokens, personal paths and private content removed;
- whether the issue reproduces with optional components such as Discord or FFmpeg disabled.

## Sensitive material

Never include any of the following in a public report, commit, artifact or log:

- Discord access or refresh tokens;
- OAuth authorization codes or PKCE verifiers;
- Android signing keys or keystore passwords;
- private certificates/keys;
- private mod/content corpora;
- credentials or secrets from local environment variables;
- the separately licensed Discord Social SDK binaries when redistribution is not authorized.

## Security-sensitive design expectations

PulseForge's public integration is expected to preserve these properties:

- Discord is optional and fail-open with respect to normal engine operation;
- access tokens remain process-local and are not persisted by PulseForge;
- persisted Discord refresh tokens use the platform credential service when supported and are never intentionally downgraded to plaintext storage;
- archive installation paths must remain traversal/ZIP-slip safe and bounded;
- third-party downloads and bundled binaries should remain pinned/verified where the build system supports verification;
- large chart handling should remain bounded rather than allowing attacker-controlled note counts to force unbounded materialization;
- public CI/release jobs must not require repository secrets merely to build the no-op Discord variant.

## Disclosure

Please allow time for the issue to be reproduced and fixed before publishing technical exploit details. Once a fix is available, a coordinated public description can be added to the repository or release notes as appropriate.
