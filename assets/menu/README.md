# Menu and pause music

PulseForge supports a discovered playlist for the launcher, normal menus and pause screen. The playlist is suspended during active gameplay, Chart Editor, Character Editor, Week Editor and offline rendering.

The PulseForge Complete engine includes the approved ten-track menu/background playlist directly in this directory. The recordings are built-in Complete engine assets; large media objects are versioned through Git LFS.

Additional legally distributable `.mp3`, `.ogg`, `.wav` or `.flac` files may also be placed here, and the custom-menu-music setting can select an explicit file. `Randomized` remains the default behavior; a missing selected track falls back to the discovered playlist rather than blocking the launcher.

Runtime contract:

- menu music always plays at `playbackRate = 1.0`;
- global volume/mute settings apply;
- randomized playback avoids immediate repetition when possible;
- an explicitly selected filename may loop;
- paths are validated as media and never become executable content roots.

Independently authored recordings retain their applicable provenance/terms; inclusion in the Complete tree does not silently relicense them under Apache-2.0.
