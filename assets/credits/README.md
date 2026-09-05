# Credit portraits

The complete private DevCore contains contributor/profile portraits supplied to the project owner for the PulseForge credits screen. The public OSS source keeps the credits code and asset lookup contract (`assets/credits/profiles/...`) but does not automatically relicense those portraits under Apache-2.0.

When the corresponding portrait files are installed locally, the launcher uses them. If they are absent, credits remain functional without inventing external profile URLs. This separation keeps the engine redistributable without changing the credits feature.
