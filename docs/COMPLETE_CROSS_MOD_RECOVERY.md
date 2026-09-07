# Complete cross-mod recovery policy

Some historical Complete Edition source folders are partial recoveries rather than clean, self-contained mods. A folder can contain a chart without its `Inst`/`Voices`, an audio stem without its chart data, or chart metadata without some directly referenced runtime resources.

PulseForge may reconstruct those payloads from other already-vetted Complete mods, but only when the relationship can be demonstrated rather than guessed.

## Identity rules

The automatic recovery stage accepts two primary identity proofs:

1. **Chart -> donor**: an exact gameplay-chart fingerprint. The fingerprint is derived from timing/gameplay data (`bpm`, notes/sections and events) and deliberately ignores presentation-only naming such as the displayed song name, characters and stage.
2. **Audio -> donor**: exact content identity. Ordinary files use SHA-256; canonical Git LFS pointers use their content-addressed SHA-256 OID.

A filename, folder name, song title or loose textual similarity is never sufficient by itself.

If several donor mods match, automatic recovery is allowed only when their relevant closure is equivalent. If the candidates disagree, the recovery remains unresolved and the existing fail-closed payload auditor decides the result.

## Runtime layout

Recovered files are normalized into the conventional layout already understood by PulseForge/Psych-style discovery:

- chart data: `data/<song>/...` or the matching `assets[/preload|shared]/data/<song>/...` root;
- song audio: `songs/<song>/Inst.*`, `Voices.*` and supported split vocal stems, under the root corresponding to the target chart/audio;
- direct character, stage, custom note and custom event resources retain the donor's already-valid relative runtime path.

Existing target files always win. Recovery never overwrites an authoritative file already present in the target mod.

## Dependency closure and provenance

For a chart match, the recovery stage can supply missing `Inst`, supported `Voices*` stems, standalone `events.json`, directly referenced character definitions/images/atlas metadata, stage definitions and available custom note/event scripts from the matched donor.

For an audio match, it can recover the donor song's chart directory and the missing sibling audio stems, then apply the same direct-reference recovery.

This is a **pre-audit materialization step**, not a relaxation of validation. After recovery, `scripts/audit_complete_mod_payload.py` still performs the normal Complete functional-dependency closure audit. Missing nested Lua assets, incompatible resources or any other unresolved dependency therefore remain hard failures unless an explicitly documented archival policy applies.

Every staging attempt writes a machine-readable recovery report containing source mod, source path, normalized destination, recovery reason and identity evidence for each copied file. Ambiguous candidates are recorded but left untouched.

## OSS boundary

Cross-mod recovery only draws from sibling mods already present in the OSS checkout. It must not import private Discord Social SDK binaries, signing material, secrets or any other content outside the existing redistribution boundary. The normal OSS boundary and Complete gates remain authoritative after reconstruction.
