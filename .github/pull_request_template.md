## Summary

Describe the problem and the change.

## Affected areas

- [ ] Core/gameplay
- [ ] PFC1/streaming
- [ ] Lua/NoteTypes/mod compatibility
- [ ] Launcher/UI
- [ ] AutoChart
- [ ] Offline render/FFmpeg
- [ ] Discord Social SDK
- [ ] Windows
- [ ] Linux
- [ ] macOS
- [ ] Android
- [ ] Build/CI/release
- [ ] Documentation only

## Validation

List exactly what you ran and where. Do not claim a platform or live Discord path was validated unless it was actually exercised.

- [ ] Deterministic core tests
- [ ] Windows MSVC x64 build
- [ ] Linux x86_64 build
- [ ] macOS arm64 build
- [ ] macOS x86_64 build
- [ ] Android arm64 build
- [ ] Materialized/PFC1 parity tests where relevant
- [ ] Live Discord integration where relevant

## Compatibility and boundedness

- [ ] No unintended O(note_count) mutable state was introduced.
- [ ] Large PFC1/PatternRun behavior remains bounded or the change explains why not.
- [ ] Materialized and streaming behavior remain equivalent where applicable.
- [ ] Gameplay timing/judgment remains independent of rendering FPS.

## OSS / licensing review

- [ ] No private mods, validation corpora, credentials, signing material or generated build outputs are included.
- [ ] No Discord Social SDK binary/AAR is committed.
- [ ] New third-party code/assets have a clear redistribution basis and notices were updated if required.

## Notes for reviewers

Call out migrations, known limitations, intentional compatibility changes or follow-up work.
