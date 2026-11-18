#!/bin/bash
# Instala el servicio gba-link (consolas en el servidor) en Debian o parecidos.
# Uso: sudo bash instalar.sh [carpeta de las ROMs]
#   La carpeta de las ROMs es donde están los ficheros del campo "origen" de juegos-online.json
#   (de serie, ./roms). Se copian a /var/lib/gba-link, porque el servicio no lee nada más.
# Programas, si están junto a este script (si no, se queda el que ya hubiera instalado):
#   gbalink                              GBA (compilar.sh)
#   gbalink-fs                           GBA con mGBA 0.11 (compilar-fs.sh), opcional
#   lrlink y parallel_n64_libretro.so    N64 (compilar-n64.sh), opcional
set -e
O=$(cd "$(dirname "$0")" && pwd)
ROMS=$(cd "${1:-$O/roms}" && pwd)

id gbalink >/dev/null 2>&1 || useradd --system --home-dir /var/lib/gba-link --no-create-home --shell /usr/sbin/nologin gbalink

install -d -m 755 /opt/gba-link
for f in gbalink gbalink-fs lrlink parallel_n64_libretro.so; do
  if [ -f "$O/$f" ]; then
    install -m 755 "$O/$f" /opt/gba-link/$f
    echo "programa: $f"
  fi
done
[ -x /opt/gba-link/gbalink ] || { echo "falta gbalink: compílalo antes con compilar.sh" >&2; exit 1; }
install -m 644 "$O/servidor.py" "$O/gba-online.html" "$O/juegos-online.json" "$O/retro.css" /opt/gba-link/

install -d -o gbalink -g gbalink -m 750 /var/lib/gba-link /var/lib/gba-link/partidas
python3 - "$O/juegos-online.json" "$ROMS" <<'PY' | while IFS=$'\t' read -r origen destino; do
import json, sys
for g in json.load(open(sys.argv[1], encoding='utf-8')):
    print(f"{sys.argv[2]}/{g['origen']}\t{g['rom']}")
PY
  if [ -f "$origen" ]; then
    install -o gbalink -g gbalink -m 640 "$origen" "/var/lib/gba-link/$destino"
    echo "ROM: $destino"
  else
    echo "aviso: no encuentro $origen (ese juego no saldrá)" >&2
  fi
done

install -m 644 "$O/gba-link.service" /etc/systemd/system/gba-link.service
systemctl daemon-reload
systemctl enable --now gba-link
systemctl restart gba-link
sleep 2
systemctl is-active gba-link || true  # solo informativo: con "set -e" cortaría el script
journalctl -u gba-link -n 8 --no-pager | grep 'no está disponible' || true
echo "Escucha en http://127.0.0.1:9081/ (ver el README para publicarlo con Apache o nginx)"
