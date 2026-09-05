# PulseForge shader-source presets

These shader sources are original PulseForge presets distributed under the same Apache-2.0 license as the engine. They use the familiar HaxeFlixel/OpenFL `#pragma header` vocabulary so Psych/H-Slice tooling can inspect or reuse the sources without importing third-party shader code.

Built-in presets:

- `identity.frag` — unchanged source texture;
- `grayscale.frag` — adjustable monochrome blend;
- `watch-dogs-scanlines.frag` — PulseForge's original scanline/cool-tint preset;
- `rgb-split.frag` — bounded channel split.

The current SDL_Renderer backend catalogs and validates GLSL sources but does not execute arbitrary mod GLSL directly. Native equivalents are used where implemented, with deterministic fallback otherwise.
