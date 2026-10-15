<p align="center">
  <img src="docs/banner.svg" alt="Clip: your best plays, saved. Your frames, untouched." width="100%">
</p>

<p align="center">
  <a href="https://github.com/luca-bv/clip/releases/latest"><img alt="Download" src="https://img.shields.io/badge/download-Windows%2011-e83030?style=for-the-badge&logo=windows11&logoColor=white"></a>
  <a href="https://github.com/luca-bv/clip/actions/workflows/build.yml"><img alt="Build" src="https://img.shields.io/github/actions/workflow/status/luca-bv/clip/build.yml?branch=main&style=for-the-badge&label=build"></a>
  <a href="LICENSE"><img alt="MIT licence" src="https://img.shields.io/badge/licence-MIT-2a2f3a?style=for-the-badge"></a>
</p>

<p align="center">
  <b>Clip</b> records your game in the background and saves the last 30 seconds the moment you press <kbd>F8</kbd>.<br>
  Then trim it, crop it for TikTok, and paste it straight into Discord. Free, open source, and built so your game keeps every frame.
</p>

---

## Why Clip

<table>
<tr><td width="30%">🎯 <b>Never miss the play</b></td><td>Recording starts by itself when a game goes fullscreen. Press <kbd>F8</kbd> (or double-tap <kbd>Right Ctrl</kbd>) and the last 15 seconds to 5 minutes are saved.</td></tr>
<tr><td width="30%">⚡ <b>Your frames stay yours</b></td><td>Capture, colour conversion and encoding all stay on the GPU's video hardware (NVENC, AMF or Quick Sync) at the lowest GPU priority. Clip's own thread spends about half a millisecond per frame.</td></tr>
<tr><td width="30%">✂️ <b>An editor that gets out of the way</b></td><td>One scrubber with a waveform and filmstrip: drag the red edges to trim, double-click to split, cut out the boring middle, scroll to zoom.</td></tr>
<tr><td width="30%">📱 <b>Ready for every app</b></td><td>Export as is, under 10 MB for Discord, 1080p or 720p, or cropped to 9:16 for TikTok, Shorts and Reels. Keep all audio, just the game, just your mic, or none.</td></tr>
<tr><td width="30%">📋 <b>One paste into Discord</b></td><td>Hover a clip and hit copy, or let Clip copy each new clip for you. One <kbd>Ctrl</kbd>+<kbd>V</kbd> in Discord attaches the video.</td></tr>
<tr><td width="30%">🎧 <b>Every sound on its own track</b></td><td>Game, microphone and any app playing sound (Spotify, Discord…) are separate tracks, plus one mix, so editors can balance them later.</td></tr>
<tr><td width="30%">⌨️ <b>Show your inputs</b></td><td>An optional overlay draws your keys and mouse movement into the clip, with a layout editor.</td></tr>
<tr><td width="30%">🟣 <b>Discord activity</b></td><td>Friends see <i>Clipping Roblox with Clip</i> with the game's icon, just like the big clippers.</td></tr>
</table>

## Get it

Download **Clip.zip** from the [latest release](https://github.com/luca-bv/clip/releases/latest), unzip it anywhere and run `Clip.exe`. Clip adds itself to the Start menu, waits in the tray and starts with Windows.

Or install and update from Git Bash:

```bash
curl -fsSL https://raw.githubusercontent.com/luca-bv/clip/main/install.sh | bash
```

Add `-s -- --uninstall` after `bash` to remove it. Your clips and settings stay.

**Needs** Windows 11 and a GPU with a hardware H.264 encoder (any NVIDIA, AMD or Intel GPU from the last several years).

## How it works

1. **Play.** Clip starts recording when a fullscreen or borderless game has focus, and stops when it closes.
2. **Press <kbd>F8</kbd>.** A chime plays and the clip lands in `Videos\Clip\<game>\`, named after the game and the time.
3. **Share.** Open Clip from the tray or Start menu: your clips are there, ready to trim, export or copy.

| Shortcut | What it does |
| --- | --- |
| <kbd>F8</kbd> | Save the last 30 seconds (change it in Settings) |
| <kbd>Right Ctrl</kbd> <kbd>Right Ctrl</kbd> | Same, as a double tap (any key, or off) |
| <kbd>Space</kbd> | Play or pause in the editor |
| <kbd>S</kbd> | Split at the playhead |
| <kbd>←</kbd> <kbd>→</kbd> | One frame back or forward (hold <kbd>Shift</kbd> for one second) |

## Privacy

Clips never leave your PC unless you share them. Clip has no accounts, no uploads and no telemetry. It talks to the internet for two things only: Discord's public list of games, so it can call `robloxplayerbeta.exe` "Roblox" (cached for a week), and Discord itself, when the Discord activity is on.

<details>
<summary><b>Settings, files and logs</b></summary>

<br>

Everything is in the settings window. Behind it:

| | |
| --- | --- |
| Settings | `%APPDATA%\Clip\config.toml` (edits apply while Clip runs) |
| Clips | `Videos\Clip\<game>\`, exports in `Videos\Clip\Exports\` |
| Log | `%LOCALAPPDATA%\Clip\clip.log` |

Defaults: 30 second clips, 60 fps, up to 1080p, 20 Mbit/s. Frame rate, resolution and bitrate apply from the next game.

Under *Settings → General → Credit* you can turn off the small "Clipped with Clip" badge on shared copies, the `#ClippedWithClip` paste text and the Discord activity. Saved clips never have a badge.

</details>

<details>
<summary><b>Build it yourself</b></summary>

<br>

CI builds every push ([workflow](.github/workflows/build.yml)). Locally you need Visual Studio 2022 or newer, CMake, and an [LGPL shared FFmpeg build](https://github.com/BtbN/FFmpeg-Builds/releases) unzipped to `ffmpeg/`. The WebView2 SDK is fetched by CMake.

```bash
cmake -B build -A x64
cmake --build build --config Release
```

`cmake --build build --config Release --target clip_bench` builds a bench that measures what recording costs a game, and checks that recording and exporting work on your GPU (`clip_bench smoke out.mp4`).

</details>

## Built with

[FFmpeg](https://ffmpeg.org) (LGPL, linked dynamically) · [wavesurfer.js](https://wavesurfer.xyz) (BSD-3) · [toml++](https://github.com/marzer/tomlplusplus) (MIT) · [WebView2](https://developer.microsoft.com/microsoft-edge/webview2/) · Windows Graphics Capture

Clip is [MIT licensed](LICENSE).
