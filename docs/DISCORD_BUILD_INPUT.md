# Supplying the Discord Social SDK to CI

PulseForge release automation expects the authorized Discord Social SDK archive to be supplied privately. The recommended mechanism is a repository secret named `PULSEFORGE_DISCORD_SDK_ARCHIVE_URL` whose value is a private, authenticated or short-lived URL to the SDK archive controlled by the project owner.

The workflow must download the archive only into the ephemeral runner workspace, stage it with `scripts/setup-discord-social-sdk.*`, build PulseForge, validate the integrated runtime, and then discard the runner workspace.

Do not echo the secret URL, archive contents, OAuth tokens or credentials to logs. Do not upload the unmodified SDK archive as an artifact.
