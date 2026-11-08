# jfmusic

A small music player for [Jellyfin](https://jellyfin.org), written for Windows 7 and later.
You can browse your library by artist and album, search it, and play tracks with a queue.

- **One 64-bit exe, nothing to install.** The C runtime is built in, and it only uses DLLs
  that ship with Windows (`WINHTTP`, `COMCTL32`, `UxTheme`, `USER32`, `GDI32`, `SHELL32`,
  `KERNEL32`).
- **Light.** It uses about 4 MB of RAM while playing. Tracks stream into a temporary file
  rather than into memory, and Windows deletes the file afterwards.
- **Plays your whole library.** MP3, FLAC and WAV are sent untouched. The server converts
  anything else (AAC/M4A, Opus, Vorbis, ALAC, ...) to MP3 on the fly.
- **Reports playback to the server,** so play counts, "recently played" and the dashboard's
  "now playing" stay up to date.

## Install

1. Download `jfmusic.exe` from the [latest release](https://github.com/Spooky098529/jfmusic/releases/latest).
2. Put it anywhere and run it.
3. Log in with your server address (for example `http://192.168.1.10:8096`) and your
   Jellyfin username and password.

The password is not stored. The client keeps the access token the server gives it, in
`jfmusic.ini` next to the exe. If that folder isn't writable (Program Files, a read-only
network share), it uses `%APPDATA%\jfmusic\jfmusic.ini` instead. **File → Log out** revokes
the token on the server.

## Using it

| To | Do |
|---|---|
| Browse | Click an artist, then an album. **(All albums)** at the top lists every album. |
| Play a track | Double-click it, or select it and press Enter. The rest of the list plays after it. |
| Play an album | Double-click the album. |
| Search | Type in the box at the top (**Ctrl+F**) and press Enter. This searches track titles. |
| Control playback | Prev / Play-Pause / Stop / Next buttons, the seek bar and the volume slider. |
| Use media keys | Play/Pause, Next, Previous and Stop work even when the window isn't focused. |

**Prev** goes back to the start of the track if more than 3 seconds have played, otherwise to
the previous track.

### System tray

Minimizing hides the window behind a tray icon. Left-click the icon to bring the window
back. Right-click it for Play/Pause, Next, Previous, Stop and Exit. Hovering shows the
current track. To minimize normally instead, turn off **File → Minimize to tray**.

## Limitations

- **Plain http only for now.** Use the server's LAN address, such as
  `http://192.168.1.10:8096`.
- **Music only.** Videos, playlists and album art aren't supported.
- **No remote control from other Jellyfin apps.** Jellyfin sends those commands over a
  WebSocket, and WinHTTP only supports WebSockets on Windows 8 and later.

## Building from source

The exe is cross-compiled on Linux with clang, lld-link and llvm-rc against Microsoft's CRT
and Windows SDK, which you download with [xwin](https://github.com/Jake-Shadle/xwin).
Running xwin accepts Microsoft's license.

```sh
xwin --accept-license --arch x86_64 splat --use-winsysroot-style --preserve-ms-arch-notation --output xwin-sdk
make                          # or: make WINSYSROOT=/path/to/xwin-sdk
make check                    # list imports
```

You can put `WINSYSROOT = /path/to/xwin-sdk` in a `config.mk` (git-ignored) instead of
passing it each time. `make check` lets you confirm the exe only imports functions that exist
on Windows 7.

The CRT is linked statically, because Windows 7 has no Universal CRT, and the exe is marked
for subsystem 6.01 (Windows 7).

### Layout

| File | What it does |
|---|---|
| `src/main.c` | Window, lists, login dialog, tray, playback queue |
| `src/api.c` | WinHTTP requests, Jellyfin API calls, background worker thread |
| `src/player.c` | Streaming to a temp file, decoding, audio output |
| `src/json.c` | Minimal JSON parser |
| `src/app.rc` | Menu, login dialog, manifest, icon, version info |
| `vendor/miniaudio.h` | [miniaudio](https://miniaud.io) 0.11.21 |

## Credits

- Audio decoding and output: [miniaudio](https://miniaud.io) by David Reid (public domain
  or MIT-0).
- Icon: the Jellyfin logo by the Jellyfin contributors, licensed
  [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/). See
  [src/ICON-LICENSE.txt](src/ICON-LICENSE.txt).
- The HTTP and JSON code started out in
  [lcdsmartie-jellyfin](https://github.com/Spooky098529/lcdsmartie-jellyfin).
