# Clip

Records your game in the background and saves the last 30 seconds when you press **Alt+F9**. Windows 11, any GPU with a hardware H.264 encoder.

## Use

1. In Git Bash on Windows 11, run:
   ```bash
   curl -fsSL https://raw.githubusercontent.com/luca-bv/clip/main/install.sh | bash
   ```
   Run it again to update. Add `-s -- --uninstall` after `bash` to remove Clip (your clips and settings stay). Or download `Clip.zip` from [the latest release](../../releases/latest), unzip it anywhere and run `Clip.exe`. Clip lives in the tray and starts with Windows.
2. Launch a game. Clip starts recording when a fullscreen or borderless window has focus, and stops when the game exits.
3. Press **Alt+F9**. A chime means the clip was saved to `Videos\Clip\<game>\`.

The tray menu changes the shortcut (press the keys you want), sets clip length, picks which audio is recorded (game, microphone, and any app that's playing sound, like Spotify or Discord), and has *Always record* / *Never record* for apps the fullscreen check gets wrong.

*Show keys and mouse in clips* draws the keys you press and a mouse-movement pad (a trail that wraps at the edges, or pans) into the video. It's off by default and only reads input while the game has focus. Tray → *Input overlay* switches between ready-made layouts, and *Edit layout and look…* opens an editor: drag keys to move them, drag the corner handle to resize, add or remove keys, and change colours, size, roundness, font and the mouse trail, with a live preview. Everything is also under `[overlay]` in the settings file.

Every clip has the video, a **Mix** track with all enabled audio, and one track per audio source, for editors that can split them.

## Settings

`%APPDATA%\Clip\config.toml`, opened from *Edit settings*. Changes apply while Clip runs; `fps`, `max_height` and `bitrate_mbps` apply from the next game. Defaults: 30 s clips, 60 fps, up to 1080p, 20 Mbit/s.

Log: `%LOCALAPPDATA%\Clip\clip.log`.

## Build

CI builds on every push (see `.github/workflows/build.yml`). Locally, with Visual Studio 2022+ and an [LGPL shared FFmpeg build](https://github.com/BtbN/FFmpeg-Builds/releases) unzipped to `ffmpeg/`:

```
cmake -B build -A x64
cmake --build build --config Release
```

MIT licensed. FFmpeg is LGPL and dynamically linked.
