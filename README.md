# Clip

Records your game in the background and saves the last 30 seconds when you press **Alt+F9**. Windows 11, any GPU with a hardware H.264 encoder.

## Use

1. Download the `Clip` artifact from the latest [build](../../actions), unzip it anywhere, and run `Clip.exe`. It lives in the tray and starts with Windows.
2. Launch a game. Clip starts recording when a fullscreen or borderless window has focus, and stops when the game exits.
3. Press **Alt+F9**. A chime means the clip was saved to `Videos\Clip\<game>\`.

The tray menu changes the shortcut (press the keys you want), sets clip length, picks which audio is recorded (game, microphone, and any app that's playing sound, like Spotify or Discord), and has *Always record* / *Never record* for apps the fullscreen check gets wrong.

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
