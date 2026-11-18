# Consoles on server

*English · [Español](README.es.md)*

GBA and N64 games that **run on the server**: each player's browser only receives video and sound, and sends its key presses. Nobody needs the game or an emulator; whoever runs the server provides the game, and everyone else joins with a link.

It covers what an in-browser emulator can't do:

- **GBA link cable between homes.** `gbalink` connects 2 to 4 GBAs with mGBA's link cable inside one process, so they stay perfectly in sync with no netcode. Each player sees their own screen.
- **One shared console.** Two-player games on a single GBA: everyone sees the same screen and everyone's keys reach the console.
- **Games that need an accurate emulator.** `lrlink` runs a libretro core (tested with parallel-n64 and the accurate *angrylion* renderer) on a headless server, for games that break with the lightweight emulators in the browser.

## How it works

```
browser ──WebSocket──> servidor.py ──pipes──> gbalink / lrlink (the consoles)
    <── H.264 video (WebCodecs) and PCM sound ──┘      └──> ffmpeg (x264)
```

- `servidor.py` (Python, `websockets`) manages the **rooms**: a room starts when the first player joins and shuts down one minute after the last one leaves. It caps how many consoles run at once (`--max-consolas`).
- Each console sends its frames to `ffmpeg`, which encodes them as H.264 (`ultrafast`, `zerolatency`) for the browser. The browser decodes them with WebCodecs as soon as they arrive, with no buffering.
- **Saves** belong to each browser: it keeps its own (IndexedDB) and sends it when joining. When the game saves, the server sends the new `.sav` back to its owner and keeps a copy.
- The page (`gba-online.html`) has pause, x1 / x1.5 / x2 speed (for the whole room), volume, editable keyboard and gamepad controls, save download and upload, screenshot, fullscreen and connection info.

The code, its comments and the page are in Spanish.

Measured on a laptop with an AMD A6-5200 (4 cores, 2013):

| | CPU | Data per player |
|---|---|---|
| 2 linked GBAs | 0.75 cores | 330-700 kbit/s |
| 4 linked GBAs | 1.4 cores | |
| N64 with angrylion | ~2 cores (49-50 fps; drops in very busy scenes) | |

Tested between two homes about 30 km apart (21 ms ping): no noticeable lag.

**Tested games:** Pokémon Quetzal (a fan game based on Pokémon Emerald) works perfectly in 4-player co-op over the link cable, each player with their own save.

## Requirements

- Linux with systemd (tested on Debian 13).
- `python3`, `python3-websockets` (13 or newer) and `ffmpeg` with `libx264`.
- To build: `gcc`, `cmake`, `zlib1g-dev`.

## Building

**GBA (`gbalink`)**, against a static libmgba from [mGBA](https://github.com/mgba-emu/mgba) 0.10.5:

```sh
# in the mGBA 0.10.5 source folder
cmake -B build -DCMAKE_BUILD_TYPE=Release -DLIBMGBA_ONLY=ON -DBUILD_STATIC=ON -DBUILD_SHARED=OFF \
  -DM_CORE_GB=OFF -DUSE_EPOXY=OFF -DUSE_FFMPEG=OFF -DUSE_LIBZIP=OFF -DUSE_MINIZIP=OFF -DUSE_LZMA=OFF \
  -DUSE_SQLITE3=OFF -DUSE_ELF=OFF -DUSE_LUA=OFF -DUSE_JSON_C=OFF -DUSE_PNG=OFF -DENABLE_SCRIPTING=OFF
cmake --build build -j"$(nproc)"
# and here
bash compilar.sh <mGBA source folder>
```

`compilar-fs.sh` does the same against mGBA 0.11 (for example, the [mgba-splitscreen](https://github.com/Spuds0588/mgba-splitscreen) fork) and produces `gbalink-fs`. It is optional and experimental.

**N64 (`lrlink` and the parallel-n64 core)**, optional:

```sh
bash compilar-n64.sh
```

It downloads parallel-n64 from GitHub and builds it without installing anything system-wide.

## Installing

1. Put your games in a folder (by default `roms/`, next to this README) and list them in `juegos-online.json` (see below).
2. `sudo bash instalar.sh [ROM folder]`

It creates the `gbalink` user (no password, no shell) and the `gba-link` service, listening on `127.0.0.1:9081`. The service can only write to `/var/lib/gba-link` and cannot reach the internet.

To try it without installing: `python3 servidor.py --roms <folder> --datos <saves folder>` (with `gbalink` and `juegos-online.json` next to it), then open `http://localhost:9081/`.

### Publishing it

Behind an HTTPS proxy, at any path. With Apache (`proxy`, `proxy_http` and `proxy_wstunnel`):

```apache
ProxyPass        /online/ws ws://127.0.0.1:9081/ws
ProxyPass        /online/   http://127.0.0.1:9081/
ProxyPassReverse /online/   http://127.0.0.1:9081/
```

The service has no user accounts: put a password in front of it (for example, basic auth on that path).

## The game list (`juegos-online.json`)

| Field | Meaning |
|---|---|
| `id`, `nombre` | Identifier (in the URL: `?juego=<id>`) and display name. |
| `origen` | ROM file, relative to the folder passed to `instalar.sh`. |
| `rom` | Name it is copied as into `/var/lib/gba-link`. |
| `modo` | `cable` (one console per player) or `compartida` (one shared console). |
| `consolas` | With `cable`: how many consoles can be chosen, e.g. `[2, 3, 4]`. |
| `jugadores` | With `compartida`: how many players. |
| `salas` | How many rooms the game has. |
| `partida_bytes` | Save size, used to pad smaller uploaded saves. |
| `descripcion`, `ayuda` | Texts shown on the page. |
| `binario` | Console program: `gbalink` (default), `gbalink-fs` or `lrlink`. |
| `sistema`, `nucleo`, `opciones` | For `lrlink`: `n64`, the libretro core and its options (`key=value;…`). |
| `peso` | How many of `--max-consolas` it counts as (an N64 with angrylion: 4). |
| `video`, `audio_hz` | Size, fps and sound rate, if not the GBA ones. |
| `en_lista` | Optional: the game is not listed in the selector, only reachable with its `?juego=`. |

A game whose ROM or program is missing doesn't show up.

## Notes

- **ROMs and BIOS files are not part of this repository.** Use games you own or homebrew.
- With linked GBAs there are no save states: they would break the link cable sync. Saves are the game's own.
- `lrlink` uses the GPU (EGL/GBM, no desktop needed) if it can open it; otherwise it lets the core render in memory (angrylion doesn't need a GPU). By default the service has no access to `/dev/dri`.

## License

GPL-3.0 (see `LICENSE`). `gbalink` links against libmgba ([MPL-2.0](https://github.com/mgba-emu/mgba/blob/master/LICENSE)), and `lrlink` loads libretro cores under their own licenses (parallel-n64: GPL-2.0).
