# Contributing to PulseForge

Thanks for contributing to PulseForge. This repository is the public OSS integration of the current PulseForge development tree, so changes must preserve the engine's deterministic, bounded and cross-platform behavior.

## Before opening a change

- Search existing issues and pull requests for related work.
- Keep changes focused; avoid mixing unrelated refactors with behavioral fixes.
- Do not commit private validation corpora, mods, music, video, artwork, signing material, generated builds, logs, caches or the Discord Social SDK itself.
- Review `docs/OSS_SCOPE.md` before adding third-party material.

## Build requirements

PulseForge uses C++20 and CMake 3.24+.

### Windows x64

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-release
```

### Linux x86_64

```bash
cmake --preset linux-release
cmake --build --preset linux-release
```

### macOS

```bash
cmake --preset macos-release
cmake --build --preset macos-release
```

### Deterministic core tests

A platform-independent core-only validation can be configured with:

```bash
cmake -S . -B out/build/core-tests -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPULSEFORGE_BUILD_APP=OFF \
  -DPULSEFORGE_BUILD_TESTS=ON \
  -DPULSEFORGE_BUILD_AUTOCHART=OFF \
  -DPULSEFORGE_ENABLE_LUA=ON \
  -DPULSEFORGE_BUNDLE_FFMPEG=OFF \
  -DPULSEFORGE_ENABLE_DISCORD_SOCIAL_SDK=OFF
cmake --build out/build/core-tests
ctest --test-dir out/build/core-tests --output-on-failure --no-tests=error
```

Windows CI uses the Visual Studio 2022 x64 generator instead of Ninja for this test matrix.

## Architectural constraints

Changes should preserve these project invariants unless a pull request explicitly proposes and justifies a redesign:

- both materialized and PFC1 streaming gameplay paths must remain supported;
- avoid new O(note_count) mutable runtime state for large charts;
- large PatternRun/PFC1 data should remain bounded and arithmetic rather than eagerly expanded;
- gameplay judgment must not depend on rendering FPS;
- Discord integration must remain optional and fail-open;
- missing Discord SDK/runtime must not prevent normal builds unless `PULSEFORGE_REQUIRE_DISCORD_SOCIAL_SDK=ON` is deliberately selected;
- third-party binaries and media require an explicit redistribution/licensing review;
- changes to dynamic lane projection, NoteTypes, multipliers, Third Strum or chart totals should keep materialized/streaming parity.

## Pull requests

A pull request should include:

- a concise explanation of the problem and solution;
- affected platforms and engine subsystems;
- tests added or updated;
- exact validation performed;
- compatibility or migration impact;
- third-party/license impact, if any.

Do not state that a platform or live Discord path works unless that exact path has actually been exercised.

Relevant pull requests targeting `main` automatically run cross-platform build validation and deterministic core tests.

## Source and dependency policy

Pinned dependencies must remain reproducible. Do not replace exact dependency commits/releases and SHA-256 hashes with floating `main`, `master` or latest-version references.

The official Discord Social SDK is an optional external dependency obtained separately through Discord and must not be committed to this repository.

## Style

- Follow the existing C++20 style in the file being changed.
- Prefer small, explicit helpers over hidden global state.
- Keep error paths fail-safe and bounded.
- Add regression coverage for behavioral fixes where practical.
- Avoid unnecessary formatting-only churn in unrelated code.
