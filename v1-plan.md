# Clip v1 plan

Settled 2026-10-10/11 from the functional spec (kept out of this repo). Where this and the spec disagree, this wins for v1.

## What v1 is

One native Windows 11 process (C++20) that lives in the tray, records the foreground game automatically, and saves the last N seconds to an MP4 on a hotkey. Open source, MIT.

## Decisions

| Area | v1 | Later |
| --- | --- | --- |
| Process model | One process: recorder + tray. No UI process, no pipe, no RPC | Settings window, library |
| UI | Tray menu + `config.toml` (reloaded on change) | |
| Capture | WGC only, borderless, SDR (8-bit BGRA) | DXGI duplication, HDR |
| Engine | Our own: WGC → D3D11 video processor (NV12) → FFmpeg hardware encoder (D3D11 textures) → RAM packet ring → in-process MP4 mux | Segment ring on disk |
| Encoders | `h264_nvenc`, `h264_amf`, `h264_qsv` (NVENC tested first). No software encoder: exit with a message | HEVC/AV1 |
| Defaults | ≤1080p, 60 fps, H.264 CBR 20 Mbit/s, 1 s keyframes, encoder-default B-frames, 30 s clips | |
| Clip length | Tray presets 15/30/60/120/300 s or any 15–300 in config, one global value | |
| Audio | Sources toggled independently: Game (auto), Mic, any app by exe (Spotify, Discord…). Each enabled source gets its own AAC track, plus track 1 = mix of all. First run: Game + Mic on, apps off. Toggle applies from the click on | Noise suppression, push-to-talk |
| Detection | Event-driven: foreground-change hook + location hook on the foreground process; record a foreground window that fills its monitor, minus a denylist; tray "always / never record this app" | Game catalogue |
| Hotkey | `RegisterHotKey`, keyboard only, default `Alt+F9` | Controllers |
| Feedback | Sound cue; tray balloon for errors. No overlay | |
| Elevated games | One-time balloon: run Clip as admin | |
| Files | `<Videos>/Clip/<game exe>/<game> yyyy-mm-dd hh-mm-ss.mp4`; refuse to save below 2 GB free; no cleanup | |
| OS | Windows 11 only; refuse to start on 10 | |
| Licence | MIT; FFmpeg LGPL build, dynamically linked | |
| Build | GitHub Actions `windows-latest`, zip artifact; tested on an NVIDIA PC | Velopack installer, SignPath signing |
| Cut from v1 | Cloud, accounts, auto-clips, dev SDK, editor, library, player, screenshots, session recording, telemetry, overlay | |

## Done when

On the NVIDIA test PC: Clip starts at login into the tray; launching a game starts recording; Alt+F9 saves a playable MP4 of the set length within 2 s, with the enabled audio sources on separate tracks plus a mix; toggling sources and clip length works from the tray; exiting the game stops recording and frees the buffer; PresentMon shows under 3% average fps loss.

## Clean room

The implementer (Claude) works only from `spec.md` and this plan, and never opens the original product's binaries or the analysis notes.
