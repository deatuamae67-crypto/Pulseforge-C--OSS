# Native custom note types

These files are clean-install fallbacks for note-type IDs observed in
PulseForge's Complete compatibility corpus. Resolution remains layered:
the selected/active mod wins when it provides the same ID; these files
are only the engine fallback.

`the note` includes its original BULLET skin and gunshot sound. Its
texture and miss penalty are declared in `the note.txt`, while its
hit animation/sound behavior lives in `the note.lua`.

`3rd Player` and `5th Player` are deliberately no-op identity files:
the source corpus contains empty scripts for those IDs, so PulseForge
preserves them without inventing semantics.
