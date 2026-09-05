# Menu and pause music

PulseForge supports a discovered playlist for the launcher, normal menus and pause screen. The playlist is suspended during active gameplay, Chart Editor, Character Editor, Week Editor and offline rendering.

The historical DevCore contained a local ten-track MP3 playlist. Those recordings are not covered by PulseForge's Apache-2.0 license and are therefore **not redistributed by the public OSS repository or GitHub release**.

To use menu music, place legally obtained `.mp3`, `.ogg`, `.wav` or `.flac` files directly in this directory, or choose an explicit external file through the custom-menu-music setting. `Randomized` is the public default; a missing selected track falls back to the discovered playlist rather than blocking the launcher.

Runtime contract:

- menu music always plays at `playbackRate = 1.0`;
- global volume/mute settings apply;
- randomized playback avoids immediate repetition when possible;
- an explicitly selected filename may loop;
- paths are validated as media and never become executable content roots.
