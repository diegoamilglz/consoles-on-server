#!/bin/bash
# Compila gbalink-fs: gbalink con la libmgba 0.11 de mgba-splitscreen, que hace falta para
# Zelda Four Swords. Uso: compilar-fs.sh <carpeta con las fuentes de mgba-splitscreen>
# (https://github.com/Spuds0588/mgba-splitscreen; basta con include/, src/, res/,
# CMakeLists.txt y version.cmake). La primera vez tarda varios minutos.
set -e
M=$(cd "${1:?falta la carpeta de las fuentes}" && pwd)
AQUI=$(cd "$(dirname "$0")" && pwd)
if [ ! -f "$M/build/libmgba.a" ]; then
  cmake -S "$M" -B "$M/build" -DCMAKE_BUILD_TYPE=Release -DLIBMGBA_ONLY=ON -DBUILD_STATIC=ON -DBUILD_SHARED=OFF \
    -DM_CORE_GB=OFF -DUSE_EPOXY=OFF -DUSE_FFMPEG=OFF -DUSE_LIBZIP=OFF -DUSE_MINIZIP=OFF -DUSE_LZMA=OFF \
    -DUSE_SQLITE3=OFF -DUSE_ELF=OFF -DUSE_LUA=OFF -DUSE_JSON_C=OFF -DUSE_PNG=OFF -DENABLE_SCRIPTING=OFF
  cmake --build "$M/build" -j"$(nproc)"
fi
# Las mismas definiciones con que se compiló la libmgba, para que las estructuras coincidan
DEFINES=$(grep -rh "C_DEFINES = " "$M/build/CMakeFiles"/*/flags.make | head -1 | sed 's/^C_DEFINES = //')
# shellcheck disable=SC2086
gcc -O2 -pipe -std=gnu11 -pthread $DEFINES -DMGBA_011 -D_GNU_SOURCE \
  "$AQUI/gbalink.c" -o "$AQUI/gbalink-fs" -I"$M/include" -I"$M/build/include" -I"$M/src" "$M/build/libmgba.a" -lpthread -lm -lz
echo "hecho: $AQUI/gbalink-fs"
