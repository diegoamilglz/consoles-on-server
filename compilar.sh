#!/bin/bash
# Compila gbalink contra la libmgba 0.10.5 ya construida en $1 (carpeta de las fuentes de mGBA)
set -e
M=${1:?falta la carpeta de las fuentes de mGBA 0.10.5, ya compiladas (ver README)}
cd "$(dirname "$0")"
gcc -O2 -pipe -D_GNU_SOURCE -DBUILD_STATIC -DM_CORE_GBA -DUSE_PTHREADS -DUSE_ZLIB -DHAVE_STRLCPY -DHAVE_STRDUP -DHAVE_STRNDUP \
  gbalink.c -o gbalink -I"$M/include" -I"$M/build/include" -I"$M/src" "$M/build/libmgba.a" -lpthread -lm -lz
