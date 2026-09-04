# Third-party SDK staging

PulseForge keeps redistributable open-source dependency notices under `THIRD_PARTY.md` and `THIRD_PARTY_LICENSES/`.

The Discord Social SDK is different: the raw SDK archive is not mirrored in this public repository. Developers obtain it from the Discord Developer Portal and stage it locally under:

```text
third_party/discord_social_sdk/
```

That directory is intentionally ignored by Git. Discord-enabled **application release packages** do include the required platform runtime (DLL/SO/dylib/AAR integration) as part of PulseForge, in accordance with Discord's Social SDK documentation and terms.

See `docs/DISCORD_SDK_DISTRIBUTION.md`.
