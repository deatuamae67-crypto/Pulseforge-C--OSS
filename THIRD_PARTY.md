# Third-party notices

PulseForge downloads pinned source archives during CMake configuration. Each
archive is verified with the SHA-256 recorded in `CMakeLists.txt` before it is
used. The corresponding license texts shipped with a source or binary package
are kept in `THIRD_PARTY_LICENSES/`.

| Component | Pinned version | License |
|---|---:|---|
| SDL | 3.4.12 | zlib |
| miniaudio | 0.11.25 | MIT-0 or public domain |
| stb | commit `31c1ad3` | MIT or public domain |
| Lua | 5.4.8 | MIT |
| nlohmann/json | 3.12.0 | MIT |
| simdjson | 4.6.4 | Apache-2.0 or MIT |
| miniz | commit `77d0dce` | MIT |
| libarchive | 3.8.9 | BSD-style |
| XZ Utils / liblzma | 5.8.3 | 0BSD |

FFmpeg is an optional external program used by Rendering Mode. It is not
linked into the PulseForge core. Anyone redistributing an FFmpeg binary must
ship its matching license and comply with that binary's exact build options;
PulseForge does not download or publish an FFmpeg binary from this repository.

The Discord Social SDK is also optional and obtained separately from Discord.
Its headers, libraries, Android AAR and redistributable runtime are deliberately
excluded from this public repository. Without them, PulseForge builds and runs
with its fail-open no-op Discord backend.

The upstream FNF engines consulted by this project are references for formats,
mod conventions and observable behaviour. They are not vendored dependencies.
A source-code license does not automatically grant permission to redistribute
music, video, characters, fonts, artwork or third-party mods found alongside
that source. For this reason the public OSS repository excludes the private
workspace's imported `mods/` and media library.

See `docs/OSS_SCOPE.md`, `docs/THIRD_PARTY_RUNTIME_POLICY.md` and
`docs/DISCORD_SDK_DISTRIBUTION.md` for the publication boundary used by the
official builds.
