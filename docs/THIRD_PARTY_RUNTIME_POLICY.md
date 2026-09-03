# Third-party runtime policy

PulseForge distinguishes between source dependencies and redistributable runtime dependencies.

1. Open-source dependencies may be fetched/pinned by CMake and are documented in `THIRD_PARTY.md` / `THIRD_PARTY_LICENSES/` as applicable.
2. Proprietary SDK archives are not mirrored as standalone public source artifacts unless their license explicitly permits that mode of redistribution.
3. A proprietary runtime that its vendor authorizes to be distributed as an integrated part of PulseForge may be included in final application packages.
4. CI must fail rather than publish a binary whose dynamic-loader dependency is missing.
5. Tokens, client secrets, signing keys, private SDK download URLs and other credentials are never included in release packages or committed to source control.

The Discord Social SDK follows rule 3: its runtime is distributed with Discord-enabled PulseForge application packages, while the raw SDK archive remains a private build input.
