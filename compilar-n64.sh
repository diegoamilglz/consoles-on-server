#!/bin/bash
# Baja y compila el núcleo parallel-n64 de libretro (sin Vulkan: OpenGL y angrylion) y lrlink.
# Uso: bash compilar-n64.sh [carpeta de trabajo, de serie /var/tmp/n64-par]
# No instala nada en el sistema: las cabeceras y nasm se bajan con apt-get download y se
# descomprimen con dpkg -x en la carpeta de trabajo. Deja lrlink y el núcleo junto a este script.
set -e
AQUI=$(cd "$(dirname "$0")" && pwd)
D=${1:-/var/tmp/n64-par}
mkdir -p "$D/debs" && cd "$D"
if [ ! -d fuente ]; then
  curl -sL -o par.tar.gz https://github.com/libretro/parallel-n64/archive/refs/heads/master.tar.gz
  mkdir fuente && tar xzf par.tar.gz -C fuente --strip-components=1
fi
# Cabeceras de OpenGL, EGL y GBM, y nasm (para el recompilador de x86-64)
if [ ! -f dev/usr/include/GL/gl.h ] || [ ! -f dev/usr/include/gbm.h ] || [ ! -x dev/usr/bin/nasm ]; then
  ( cd debs && apt-get download libgl-dev libglx-dev libopengl-dev mesa-common-dev libglvnd-dev libegl-dev libgles-dev libgbm-dev nasm 2>&1 | tail -2
    for f in *.deb; do dpkg -x "$f" "$D/dev"; done )
fi
export PATH=$D/dev/usr/bin:$PATH
# Por CPATH/LIBRARY_PATH: pasar CFLAGS al make pisaría las rutas del propio Makefile
export CPATH=$D/dev/usr/include LIBRARY_PATH=$D/dev/usr/lib/x86_64-linux-gnu
echo "--- compilando el núcleo (tarda unos minutos)"
( cd fuente && make -j"$(nproc)" platform=unix HAVE_PARALLEL=0 HAVE_PARALLEL_RSP=0 HAVE_THR_AL=1 > ../compilar.log 2>&1 ) \
  || { grep -B2 -E 'error|Error' compilar.log | head -30; exit 1; }
echo "--- compilando lrlink"
gcc -O2 -Wall -Wextra -Ifuente/libretro-common/include -o "$AQUI/lrlink" "$AQUI/lrlink.c" -ldl -lpthread -l:libEGL.so.1 -l:libgbm.so.1
cp fuente/parallel_n64_libretro.so "$AQUI/"
ls -la "$AQUI/lrlink" "$AQUI/parallel_n64_libretro.so"
