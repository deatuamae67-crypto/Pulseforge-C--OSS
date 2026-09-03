# Discord SDK source boundary

The PulseForge repository contains integration source code, setup scripts, packaging rules and validation for the Discord Social SDK. It intentionally does not publish Discord's original SDK archive as a standalone source-tree download.

Final Discord-enabled PulseForge application packages are expected to include the platform runtime required by Discord's documentation. This keeps the game launchable while respecting the distinction between distributing the SDK integrated into PulseForge and redistributing the SDK archive itself.
