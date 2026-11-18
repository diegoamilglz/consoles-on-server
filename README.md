# Consolas en el servidor

Juegos de GBA y N64 que **corren en el servidor**: el navegador de cada jugador solo recibe vídeo y sonido, y manda sus teclas. Nadie tiene que tener el juego ni un emulador; quien monta el servidor pone el juego y los demás entran con un enlace.

Sirve para lo que un emulador en el navegador no puede hacer:

- **Cable link de GBA entre casas.** `gbalink` une de 2 a 4 GBA con el cable de mGBA en el mismo proceso, así que van en sincronía perfecta, sin netcode. Cada jugador ve su pantalla.
- **Una consola compartida.** Juegos de dos en una sola GBA: todos ven la misma pantalla y las teclas de todos llegan a la consola.
- **Juegos que necesitan un emulador exacto.** `lrlink` ejecuta un núcleo de libretro (probado con parallel-n64 y el dibujo exacto *angrylion*) en un servidor sin pantalla, para juegos que fallan con los emuladores ligeros del navegador.

## Cómo funciona

```
navegador ──WebSocket──> servidor.py ──tuberías──> gbalink / lrlink (las consolas)
     <── vídeo H.264 (WebCodecs) y sonido PCM ──┘        └──> ffmpeg (x264)
```

- `servidor.py` (Python, `websockets`) lleva las **salas**: arrancan cuando entra el primero y se apagan un minuto después de que salga el último. Limita las consolas encendidas a la vez (`--max-consolas`).
- Cada consola manda sus fotogramas a `ffmpeg`, que los codifica en H.264 (`ultrafast`, `zerolatency`) para el navegador. El navegador los decodifica con WebCodecs en cuanto llegan, sin búfer.
- Las **partidas** son de cada navegador: este guarda la suya (IndexedDB) y la manda al entrar. Cuando el juego guarda, el servidor devuelve el `.sav` nuevo a su dueño y guarda una copia.
- La página (`gba-online.html`) trae pausa, velocidad x1 / x1,5 / x2 (de toda la sala), volumen, controles de teclado y mando editables, descargar y subir la partida, captura, pantalla completa e información de la conexión.

Medido en un portátil con una AMD A6-5200 (4 núcleos, 2013):

| | CPU | Datos por jugador |
|---|---|---|
| 2 GBA unidas | 0,75 núcleos | 330-700 kbit/s |
| 4 GBA unidas | 1,4 núcleos | |
| N64 con angrylion | ~2 núcleos (49-50 fps; baja en escenas muy cargadas) | |

Probado entre dos casas a unos 30 km (21 ms de ping): sin retraso apreciable.

## Requisitos

- Linux con systemd (probado en Debian 13).
- `python3`, `python3-websockets` (13 o más nueva) y `ffmpeg` con `libx264`.
- Para compilar: `gcc`, `cmake`, `zlib1g-dev`.

## Compilar

**GBA (`gbalink`)**, contra la libmgba de [mGBA](https://github.com/mgba-emu/mgba) 0.10.5 estática:

```sh
# en la carpeta de las fuentes de mGBA 0.10.5
cmake -B build -DCMAKE_BUILD_TYPE=Release -DLIBMGBA_ONLY=ON -DBUILD_STATIC=ON -DBUILD_SHARED=OFF \
  -DM_CORE_GB=OFF -DUSE_EPOXY=OFF -DUSE_FFMPEG=OFF -DUSE_LIBZIP=OFF -DUSE_MINIZIP=OFF -DUSE_LZMA=OFF \
  -DUSE_SQLITE3=OFF -DUSE_ELF=OFF -DUSE_LUA=OFF -DUSE_JSON_C=OFF -DUSE_PNG=OFF -DENABLE_SCRIPTING=OFF
cmake --build build -j"$(nproc)"
# y aquí
bash compilar.sh <carpeta de las fuentes de mGBA>
```

`compilar-fs.sh` hace lo mismo contra mGBA 0.11 (por ejemplo, el fork [mgba-splitscreen](https://github.com/Spuds0588/mgba-splitscreen)) y da `gbalink-fs`. Es opcional y experimental.

**N64 (`lrlink` y el núcleo parallel-n64)**, opcional:

```sh
bash compilar-n64.sh
```

Baja parallel-n64 de GitHub y lo compila sin instalar nada en el sistema.

## Instalar

1. Pon tus juegos en una carpeta (de serie, `roms/` junto a este README) y apúntalos en `juegos-online.json` (ver abajo).
2. `sudo bash instalar.sh [carpeta de las ROMs]`

Crea el usuario `gbalink` (sin contraseña ni shell) y el servicio `gba-link`, que escucha en `127.0.0.1:9081`. El servicio solo puede escribir en `/var/lib/gba-link` y no puede salir a internet.

Para probarlo sin instalar nada: `python3 servidor.py --roms <carpeta> --datos <carpeta de partidas>` (con `gbalink` y `juegos-online.json` junto a él) y abre `http://localhost:9081/`.

### Publicarlo

Detrás de un proxy con HTTPS, en la ruta que quieras. Con Apache (`proxy`, `proxy_http` y `proxy_wstunnel`):

```apache
ProxyPass        /online/ws ws://127.0.0.1:9081/ws
ProxyPass        /online/   http://127.0.0.1:9081/
ProxyPassReverse /online/   http://127.0.0.1:9081/
```

El servicio no tiene usuarios: pon una contraseña delante (por ejemplo, autenticación básica en esa ruta).

## La lista de juegos (`juegos-online.json`)

| Campo | Qué es |
|---|---|
| `id`, `nombre` | Identificador (en la URL: `?juego=<id>`) y nombre visible. |
| `origen` | Fichero de la ROM, relativo a la carpeta que se pasa a `instalar.sh`. |
| `rom` | Nombre con que se copia a `/var/lib/gba-link`. |
| `modo` | `cable` (una consola por jugador) o `compartida` (una para todos). |
| `consolas` | Con `cable`: cuántas consolas se pueden elegir, por ejemplo `[2, 3, 4]`. |
| `jugadores` | Con `compartida`: cuántos jugadores. |
| `salas` | Cuántas salas tiene el juego. |
| `partida_bytes` | Tamaño de la partida, para rellenar las que se suben más pequeñas. |
| `descripcion`, `ayuda` | Textos de la página. |
| `binario` | Programa de la consola: `gbalink` (de serie), `gbalink-fs` o `lrlink`. |
| `sistema`, `nucleo`, `opciones` | Para `lrlink`: `n64`, el núcleo de libretro y sus opciones (`clave=valor;…`). |
| `peso` | Cuántas consolas de `--max-consolas` cuenta (una N64 con angrylion, 4). |
| `video`, `audio_hz` | Tamaño, fps y frecuencia de sonido, si no son los de la GBA. |
| `en_lista` | Opcional: el juego no sale en el selector, solo si se entra con su `?juego=`. |

Un juego cuya ROM o cuyo programa no está no aparece.

## Notas

- **Las ROMs y BIOS no van en este repositorio.** Usa juegos que tengas o homebrew.
- Con varias GBA unidas no hay estados guardados: romperían la sincronía del cable. Las partidas son las del propio juego.
- `lrlink` usa la gráfica (EGL/GBM sin escritorio) si puede abrirla, y si no, deja que el núcleo dibuje en memoria (angrylion no necesita gráfica). El servicio, de serie, no tiene acceso a `/dev/dri`.

## Licencia

GPL-3.0 (ver `LICENSE`). `gbalink` se enlaza con libmgba ([MPL-2.0](https://github.com/mgba-emu/mgba/blob/master/LICENSE)), y `lrlink` carga núcleos de libretro con su propia licencia (parallel-n64: GPL-2.0).
