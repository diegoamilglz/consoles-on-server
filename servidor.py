#!/usr/bin/env python3
"""GBA online: une gbalink, ffmpeg y los navegadores, para varios juegos.

Las consolas corren aquí. Cada jugador recibe por WebSocket su imagen (H.264) y su sonido, y
manda sus teclas. Cada juego (juegos-online.json) tiene sus salas, que arrancan al entrar el
primer jugador y se apagan cuando llevan un minuto vacías (las partidas quedan en disco, en
<datos>/<juego>/salaN/jugadorM.sav). Hay dos modos:
  - "cable": una consola por jugador, unidas por el cable link (2 a 4 consolas).
  - "compartida": una sola consola que ven todos, y a la que llegan las teclas de todos
    juntas, como cuando dos personas comparten la misma GBA (WarioWare).
Entre todas las salas de todos los juegos no puede haber más de --max-consolas encendidas.

Partidas por navegador (con cable): cada navegador tiene un identificador al azar ("yo") y guarda su
partida de cada juego. Al entrar, la manda, y su consola la carga, sea cual sea el puesto. Cuando el
juego guarda, el .sav cambia, y el servidor se lo devuelve al navegador y guarda una copia en
<datos>/<juego>/personas/<yo>.sav. Un navegador sin partida elige una de las que había en los puestos
antes de este cambio (copiadas una vez en <datos>/<juego>/originales) o empieza una nueva.
"""
import argparse
import asyncio
import hashlib
import http
import json
import os
import re
import shutil
import struct
import tempfile
import time
import urllib.parse
from pathlib import Path

from websockets.asyncio.server import serve
from websockets.datastructures import Headers
from websockets.http11 import Response

AQUI = Path(__file__).resolve().parent
ESPERA_VACIA = 60  # segundos con la sala vacía antes de cerrarla
AUD = b"\x00\x00\x00\x01\x09"
SUBIDA = b"SAV1"  # cabecera del mensaje con una partida subida
MIA = b"MIA1"  # cabecera de la partida que manda el navegador al entrar
PARTIDA_MAX = 256 * 1024
VIGILAR = 2  # segundos entre cada vistazo a los .sav de las consolas
ID_VALIDO = re.compile(r"[0-9a-f]{16,32}")


def huella(datos):
    return hashlib.sha1(datos).hexdigest() if datos is not None else None


def escribir(ruta, datos):
    """Escribe sin dejar nunca el fichero a medias."""
    nueva = ruta.with_name(ruta.name + ".nueva")
    nueva.write_bytes(datos)
    os.replace(nueva, ruta)


def entrenador(datos):
    """Nombre del entrenador de una partida de Pokémon de 3.ª generación (Quetzal es un hack de
    Esmeralda), o None si no lo es. La partida tiene dos copias de 14 secciones de 4 KiB; se mira la
    sección 0 (datos del entrenador) más reciente."""
    mejor = None
    for base in (0, 0xE000):
        for i in range(14):
            inicio = base + i * 0x1000
            if inicio + 0x1000 > len(datos):
                break
            sid, = struct.unpack_from("<H", datos, inicio + 0xFF4)
            firma, indice = struct.unpack_from("<II", datos, inicio + 0xFF8)
            if sid == 0 and firma == 0x08012025 and (mejor is None or indice > mejor[0]):
                mejor = (indice, inicio)
    if mejor is None:
        return None
    nombre = ""
    for c in datos[mejor[1]:mejor[1] + 7]:
        if c == 0xFF:
            break
        if 0xBB <= c <= 0xD4:
            nombre += chr(65 + c - 0xBB)
        elif 0xD5 <= c <= 0xEE:
            nombre += chr(97 + c - 0xD5)
        elif 0xA1 <= c <= 0xAA:
            nombre += chr(48 + c - 0xA1)
        elif c == 0:
            nombre += " "
    return nombre.strip() or None
VELOCIDADES = (1.0, 1.5, 2.0)
MAX_JUGADORES = 4  # el cable link de la GBA une hasta 4 consolas


class Cliente:
    def __init__(self, ws, yo=None):
        self.ws = ws
        self.yo = yo  # identificador del navegador, para su partida
        self.cola = asyncio.Queue(maxsize=40)
        self.esperando_clave = True

    def enviar_video(self, unidad, clave):
        if self.esperando_clave and not clave:
            return
        if self.cola.full():
            self.esperando_clave = True  # va lento: saltar hasta la siguiente imagen completa
            return
        self.esperando_clave = False
        self.cola.put_nowait(bytes([1, 1 if clave else 0]) + unidad)

    def enviar_audio(self, datos):
        if not self.cola.full():
            self.cola.put_nowait(b"\x02" + datos)

    def enviar_texto(self, texto):
        if not self.cola.full():
            self.cola.put_nowait(texto)

    def enviar_partida(self, datos):
        """La partida no puede perderse aunque la cola vaya llena: se manda aparte."""
        mensaje = b"\x03" + datos if datos is not None else "sinpartida"
        asyncio.create_task(self._mandar(mensaje))

    async def _mandar(self, mensaje):
        try:
            await self.ws.send(mensaje)
        except Exception:
            pass


class Sala:
    def __init__(self, args, juego, numero, todas):
        self.args = args
        self.juego = juego
        self.numero = numero
        self.todas = todas  # las salas de todos los juegos, para repartir la CPU
        self.datos = Path(args.datos) / juego["id"] / f"sala{numero}"
        self.datos.mkdir(parents=True, exist_ok=True)
        self.compartida = juego["modo"] == "compartida"
        # Consolas encendidas; en una sala con cable lo elige quien la enciende
        self.n = 1 if self.compartida else juego["consolas"][0]
        self.proc = None
        self.tareas = []
        self.fifos = None
        self.clientes = [set() for _ in range(MAX_JUGADORES)]
        self.teclas = [0] * MAX_JUGADORES  # en la consola compartida, las de cada puesto
        self.enviados = [0] * MAX_JUGADORES
        self.cierre = None
        self.bloqueo = asyncio.Lock()
        # Velocidad y pausa son de toda la sala: con el cable, las consolas van a la par
        self.velocidad = 1.0
        self.pausa = False
        self.fps_emulador = []
        # Partidas por navegador (solo con cable): de quién es la partida de cada consola y cómo
        # era su .sav la última vez que se miró
        self.personas = Path(args.datos) / juego["id"] / "personas"
        self.originales = Path(args.datos) / juego["id"] / "originales"
        self.duenos = [None] * MAX_JUGADORES
        self.huellas = [None] * MAX_JUGADORES

    @property
    def activa(self):
        return self.proc is not None and self.proc.returncode is None

    @property
    def puestos(self):
        """Jugadores que caben: uno por consola con cable, o los del juego si la comparten."""
        return self.juego["jugadores"] if self.compartida else self.n

    def resumen(self):
        return {"juego": self.juego["id"], "sala": self.numero, "activa": self.activa,
                "consolas": self.n if self.activa else None,
                "puestos": self.puestos if self.activa else None,
                "conectados": [len(c) for c in self.clientes[:self.puestos]] if self.activa else [],
                "velocidad": self.velocidad, "pausa": self.pausa}

    def registro(self, texto):
        print(f"{self.juego['id']} sala {self.numero}: {texto}", flush=True)

    async def entrar(self, j, n, c, inicial=("nada",)):
        """Mete al cliente en el puesto j. Si la sala está apagada, la enciende con n consolas
        (en la compartida, siempre una). inicial dice con qué partida entra (ver partida_de).
        Devuelve un texto de error, o None si ha entrado."""
        async with self.bloqueo:
            if self.activa and j >= self.puestos:
                return f"La sala {self.numero} está jugando con {self.puestos} jugadores: elige otro puesto."
            propia = not self.compartida and c.yo is not None and inicial[0] != "nada"
            if propia and any(otro.yo != c.yo for otro in self.clientes[j]):
                return f"El Jugador {j + 1} ya lo está usando otra persona: elige otro puesto."
            if propia and self.activa and self.duenos[j] == c.yo:
                # Vuelve a entrar en su consola (se cortó la conexión, o recargó la página): su partida
                # ya está ahí, y más al día que la de su navegador
                ruta = self.ruta_partida(j)
                c.enviar_partida(ruta.read_bytes() if ruta.exists() else None)
                propia = False
            if propia:
                datos = self.partida_de(c.yo, inicial)
                if datos is False:
                    return "Esa partida guardada no existe. Recarga la página y elige otra."
            if not self.activa:
                if self.compartida:
                    n = 1
                # Lo que gasta cada sala, en consolas de GBA (una N64 gasta como 3)
                otras = sum(s.n * s.juego.get("peso", 1) for s in self.todas if s is not self and s.activa)
                if otras + n * self.juego.get("peso", 1) > self.args.max_consolas:
                    libres = self.args.max_consolas - otras
                    consolas = "esta consola" if n == 1 else f"{n} consolas más"
                    peso = self.juego.get("peso", 1)
                    return (f"Ahora no hay CPU para {consolas}: entre todas las salas caben "
                            f"{self.args.max_consolas} consolas de GBA" + (f" (esta cuenta como {peso})" if peso > 1 else "")
                            + f", y quedan {max(libres, 0)}. Prueba cuando se apague otra sala.")
                self.n = n
                if propia:
                    # Con la sala apagada, la partida se deja en su sitio antes de encender: sin reinicios
                    await self._poner_partida(j, datos, c, reiniciar=False)
                    propia = False
                await self._arrancar()
            if j >= self.puestos:
                return f"La sala {self.numero} está jugando con {self.puestos} jugadores: elige otro puesto."
            if propia:
                await self._poner_partida(j, datos, c, reiniciar=True)
            if self.cierre:
                self.cierre.cancel()
                self.cierre = None
            self.clientes[j].add(c)
        self.avisar()
        return None

    def partida_de(self, yo, inicial):
        """La partida con la que entra un navegador: bytes, None (partida nueva) o False (no existe).
        inicial es ("mia", bytes): la de su navegador; ("usar", nombre): una de las de antes;
        ("copia",): la copia del servidor de ese navegador; ("nueva",): empezar de cero."""
        tipo = inicial[0]
        if tipo == "subida":  # con el botón "Subir una partida"
            return inicial[1]
        if tipo == "mia":
            # Manda la copia del servidor si la hay: todo lo que tiene el navegador le ha llegado de
            # aquí, y si cerró la pestaña justo después de guardar, solo el servidor tiene lo último
            copia = self.personas / f"{yo}.sav"
            return copia.read_bytes() if copia.is_file() else inicial[1]
        if tipo == "nueva":
            return None
        if tipo == "copia":
            ruta = self.personas / f"{yo}.sav"
            return ruta.read_bytes() if ruta.is_file() else False
        if tipo == "usar":
            nombre = inicial[1]
            ruta = self.originales / nombre
            if not re.fullmatch(r"sala\d+-jugador\d+\.sav", nombre) or not ruta.is_file():
                return False
            elegidas = leer_json(self.originales / "elegidas.json", {})
            if yo not in elegidas.setdefault(nombre, []):
                elegidas[nombre].append(yo)
                escribir(self.originales / "elegidas.json", json.dumps(elegidas, indent=1).encode())
            return ruta.read_bytes()
        return False

    async def _poner_partida(self, j, datos, c, reiniciar):
        """Pone en la consola j la partida del navegador de c (None: una nueva, vacía) y le apunta
        como dueño. La que había queda como jugadorN.sav.anterior."""
        ruta = self.ruta_partida(j)
        if datos is not None:
            tamano = ruta.stat().st_size if ruta.exists() else self.juego.get("partida_bytes", 0)
            if len(datos) < tamano:
                datos += b"\xff" * (tamano - len(datos))  # memoria vacía
            nueva = ruta.with_name(ruta.name + ".subida")
            nueva.write_bytes(datos)
        if ruta.exists():
            os.replace(ruta, ruta.with_name(ruta.name + ".anterior"))
        if datos is not None:
            os.replace(nueva, ruta)
        # Un navegador, una consola: si estaba en otra de esta sala, deja de serlo
        self.duenos = [None if d == c.yo else d for d in self.duenos]
        self.duenos[j] = c.yo
        self.huellas[j] = huella(datos)
        self.personas.mkdir(exist_ok=True)
        copia = self.personas / f"{c.yo}.sav"
        if datos is not None:
            escribir(copia, datos)
        elif copia.exists():
            os.replace(copia, copia.with_name(copia.name + ".anterior"))
        c.enviar_partida(datos)
        self.registro(f"jugador {j + 1}: partida del navegador {c.yo[:6]}… "
                      + (f"({len(datos)} bytes)" if datos is not None else "nueva"))
        if reiniciar and self.activa and j < self.n:
            # Con la sala encendida, la consola sigue con el fichero viejo (ya renombrado) hasta que
            # gbalink abre el nuevo con "sav" (si no existe, lo crea vacío)
            await self.orden(f"sav {j}")

    async def cambiar_partida(self, j, c, inicial):
        """Con la sala en marcha: el navegador cambia de partida (elige otra o empieza una nueva)."""
        if self.compartida or c.yo is None:
            return "partida sin cambiar: en este juego es la misma para todos."
        async with self.bloqueo:
            datos = self.partida_de(c.yo, inicial)
            if datos is False:
                return "partida sin cambiar: esa partida guardada no existe."
            await self._poner_partida(j, datos, c, reiniciar=True)
        return "partida cambiada"

    async def _vigilar_partidas(self):
        """Cuando el juego guarda, mGBA escribe el .sav: se manda al navegador de su dueño y se
        guarda su copia en el servidor."""
        while True:
            await asyncio.sleep(VIGILAR)
            self._repasar_partidas()

    def _repasar_partidas(self):
        if self.compartida:
            return
        for j in range(self.n):
            ruta = self.ruta_partida(j)
            try:
                datos = ruta.read_bytes()
            except OSError:
                continue
            h = huella(datos)
            if h == self.huellas[j]:
                continue
            self.huellas[j] = h
            yo = self.duenos[j]
            if yo is None:
                continue
            self.personas.mkdir(exist_ok=True)
            escribir(self.personas / f"{yo}.sav", datos)
            for c in self.clientes[j]:
                if c.yo == yo:
                    c.enviar_partida(datos)
            self.registro(f"jugador {j + 1}: partida guardada ({len(datos)} bytes), enviada a su navegador")

    async def _arrancar(self):
        self.fifos = tempfile.mkdtemp(prefix="gbalink-")
        for j in range(self.n):
            os.mkfifo(f"{self.fifos}/p{j}.video")
            os.mkfifo(f"{self.fifos}/p{j}.audio")
        binario = AQUI / self.juego.get("binario", "gbalink")
        rom = Path(self.args.roms) / self.juego["rom"]
        # lrlink (núcleos de libretro): qué núcleo carga y con qué opciones
        entorno = dict(os.environ)
        if self.juego.get("nucleo"):
            entorno["N64LINK_NUCLEO"] = str(AQUI / self.juego["nucleo"])
        if self.juego.get("opciones"):
            entorno["N64LINK_OPCIONES"] = self.juego["opciones"]
        self.proc = await asyncio.create_subprocess_exec(
            str(binario), str(rom), str(self.datos), str(self.n), self.fifos, env=entorno,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
        self.teclas = [0] * MAX_JUGADORES
        for k in range(self.n):
            if self.duenos[k] is None:  # las que no ha puesto nadie: no hay que mandarlas a ningún sitio
                ruta = self.ruta_partida(k)
                self.huellas[k] = huella(ruta.read_bytes()) if ruta.exists() else None
        self.tareas = [asyncio.create_task(self._leer_salida()), asyncio.create_task(self._medir())]
        if not self.compartida:
            self.tareas.append(asyncio.create_task(self._vigilar_partidas()))
        for j in range(self.n):
            self.tareas.append(asyncio.create_task(self._video(j)))
            self.tareas.append(asyncio.create_task(self._audio(j)))
        self.registro(f"abierta con {self.n} consolas")

    async def _leer_salida(self):
        while True:
            linea = await self.proc.stdout.readline()
            if not linea:
                return
            if linea.startswith(b"fps"):
                self.fps_emulador = [float(x) for x in linea.split()[1:]]
                self.avisar()
            elif linea.startswith((b"listo", b"ss", b"ls", b"sav", b"vel", b"pausa", b"mando")):
                self.registro(linea.decode(errors="replace").strip())

    async def _medir(self):
        vueltas = 0
        while True:
            await asyncio.sleep(5)
            await self.orden("stat")
            vueltas += 1
            if vueltas % 12 == 0:  # al registro, una vez por minuto
                self.registro(f"fps {self.fps_emulador} imagenes/s enviadas: "
                              f"{[round(n / 60) for n in self.enviados[:self.n]]}")
                self.enviados = [0] * MAX_JUGADORES

    def todos(self):
        return [c for grupo in self.clientes for c in grupo]

    def destinatarios(self, consola):
        """Quién recibe la imagen y el sonido de una consola: todos si es compartida."""
        return self.todos() if self.compartida else self.clientes[consola]

    def avisar(self):
        """Manda a todos el estado de la sala: quién está, velocidad, pausa y fps."""
        texto = "estado" + json.dumps(dict(self.resumen(), fps=self.fps_emulador))
        for c in self.todos():
            c.enviar_texto(texto)

    async def cambiar(self, velocidad=None, pausa=None):
        async with self.bloqueo:
            if velocidad in VELOCIDADES and velocidad != self.velocidad:
                self.velocidad = velocidad
                await self.orden(f"vel {velocidad}")
            if pausa is not None and pausa != self.pausa:
                self.pausa = pausa
                await self.orden(f"pausa {int(pausa)}")
        self.avisar()

    async def _video(self, j):
        v = self.juego["video"]
        # La GBA (240x160) se dobla con píxeles nítidos; la N64 ya sale a su tamaño
        escala = f"scale={v['salida'][0]}:{v['salida'][1]}:flags=neighbor," if v["salida"] != [v["ancho"], v["alto"]] else ""
        enc = await asyncio.create_subprocess_exec(
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
            "-f", "rawvideo", "-pixel_format", "rgb0", "-video_size", f"{v['ancho']}x{v['alto']}", "-framerate", str(v["fps"]),
            "-i", f"{self.fifos}/p{j}.video",
            "-vf", escala + "format=yuv420p",
            "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency", "-profile:v", "baseline",
            "-crf", str(self.args.crf), "-maxrate", self.args.maxrate, "-bufsize", self.args.bufsize,
            "-x264-params", f"aud=1:keyint={2 * v['fps']}:min-keyint={v['fps']}:scenecut=0:threads=1:sliced-threads=0:repeat-headers=1",
            "-f", "h264", "-flush_packets", "1", "-",
            stdout=asyncio.subprocess.PIPE)
        pendiente = b""
        try:
            while True:
                datos = await enc.stdout.read(65536)
                if not datos:
                    return
                pendiente += datos
                # Cada unidad de acceso empieza por un AUD: se envía al llegar la siguiente
                while True:
                    fin = pendiente.find(AUD, 1)
                    if fin < 0:
                        break
                    unidad, pendiente = pendiente[:fin], pendiente[fin:]
                    self._repartir_video(j, unidad)
        finally:
            if enc.returncode is None:
                enc.kill()

    def _repartir_video(self, j, unidad):
        # La imagen completa lleva SPS (tipo de NAL 7)
        clave = b"\x00\x00\x00\x01\x67" in unidad[:64] or b"\x00\x00\x01\x67" in unidad[:64]
        self.enviados[j] += 1
        for c in self.destinatarios(j):
            c.enviar_video(unidad, clave)

    async def _audio(self, j):
        loop = asyncio.get_running_loop()
        fd = os.open(f"{self.fifos}/p{j}.audio", os.O_RDONLY | os.O_NONBLOCK)
        cola = asyncio.Queue()
        loop.add_reader(fd, lambda: cola.put_nowait(None))
        resto = b""
        try:
            while True:
                await cola.get()
                try:
                    datos = os.read(fd, 8192)
                except BlockingIOError:
                    continue
                if not datos:
                    await asyncio.sleep(0.02)  # aún no hay escritor
                    continue
                # Muestras de 16 bits: nunca mandar medio número
                datos = resto + datos
                corte = len(datos) & ~1
                datos, resto = datos[:corte], datos[corte:]
                for c in self.destinatarios(j):
                    c.enviar_audio(datos)
        finally:
            loop.remove_reader(fd)
            os.close(fd)

    async def orden(self, texto):
        if self.activa:
            self.proc.stdin.write(texto.encode() + b"\n")
            await self.proc.stdin.drain()

    def consola(self, j):
        return 0 if self.compartida else j

    def ruta_partida(self, j):
        return self.datos / f"jugador{self.consola(j) + 1}.sav"

    async def subir_partida(self, j, datos):
        """Sustituye la partida del puesto j; la anterior queda como jugadorN.sav.anterior."""
        if not 0 < len(datos) <= PARTIDA_MAX:
            return "partida no válida: tamaño incorrecto"
        ruta = self.ruta_partida(j)
        # Algunos emuladores guardan menos de lo que ocupa la memoria del cartucho: se rellena
        # hasta el tamaño de la partida que había (cada juego tiene el suyo: 32 KiB WarioWare,
        # 128 KiB Esmeralda). Sin partida previa, el tamaño que diga juegos-online.json
        tamano = ruta.stat().st_size if ruta.exists() else self.juego.get("partida_bytes", 0)
        if len(datos) < tamano:
            datos += b"\xff" * (tamano - len(datos))  # memoria vacía
        nueva = ruta.with_name(ruta.name + ".subida")
        nueva.write_bytes(datos)
        async with self.bloqueo:
            # Con la sala encendida, la consola sigue usando el fichero viejo (ya renombrado)
            # hasta que gbalink abre el nuevo con "sav"
            if ruta.exists():
                os.replace(ruta, ruta.with_name(ruta.name + ".anterior"))
            os.replace(nueva, ruta)
            if self.activa and self.consola(j) < self.n:
                await self.orden(f"sav {self.consola(j)}")
        return "partida cargada"

    async def teclas_de(self, j, mascara, stick=None):
        if stick is not None:
            # N64: botones (16 bits) y el stick, de -80 a 80 en cada eje
            await self.orden(f"k {self.consola(j)} {mascara} {stick[0]} {stick[1]}")
            return
        if self.compartida:
            # Como dos personas con la misma GBA: lo que pulse cualquiera llega a la consola
            self.teclas[j] = mascara
            mascara = 0
            for m in self.teclas:
                mascara |= m
        await self.orden(f"k {self.consola(j)} {mascara}")

    async def baja(self, j, c):
        self.clientes[j].discard(c)
        self.avisar()
        if not self.clientes[j]:
            # que no se quede una tecla pulsada (ni el stick torcido)
            await self.teclas_de(j, 0, (0, 0) if self.juego["sistema"] == "n64" else None)
        if not any(self.clientes) and not self.cierre:
            self.cierre = asyncio.create_task(self._cerrar_luego())

    async def _cerrar_luego(self):
        await asyncio.sleep(ESPERA_VACIA)
        async with self.bloqueo:
            self.cierre = None
            if any(self.clientes):
                return
            await self.cerrar()

    async def reiniciar(self, quien):
        """Apaga la sala ahora y saca a todos, que vuelven a la pantalla de entrada."""
        async with self.bloqueo:
            if not self.activa:
                return
            self.registro(f"reiniciada por el jugador {quien + 1}")
            todos = self.todos()
            for c in todos:
                try:  # directo, sin cola: tiene que llegar antes de cerrar la conexión
                    await c.ws.send(f"cerrada El jugador {quien + 1} ha reiniciado la sala {self.numero}. "
                                    "Pulsa entrar para volver a encenderla.")
                except Exception:
                    pass
            await self.cerrar()
        for c in todos:  # sin esperar: entre ellas está la conexión que ha pedido reiniciar
            asyncio.create_task(c.ws.close())

    async def cerrar(self):
        if not self.activa:
            return
        self.registro("cerrada")
        await self.orden("quit")
        try:
            await asyncio.wait_for(self.proc.wait(), 3)
        except asyncio.TimeoutError:
            self.proc.kill()
        self._repasar_partidas()  # lo guardado en los últimos segundos, a la copia de su dueño
        for t in self.tareas:
            t.cancel()
        self.tareas = []
        self.velocidad, self.pausa, self.fps_emulador = 1.0, False, []
        self.duenos = [None] * MAX_JUGADORES
        self.huellas = [None] * MAX_JUGADORES
        if self.fifos:
            shutil.rmtree(self.fifos, ignore_errors=True)


def leer_json(ruta, si_no):
    try:
        return json.loads(Path(ruta).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return si_no


def guardar_originales(datos, juegos):
    """Una sola vez, al llegar las partidas por navegador: copia aparte las partidas que había en
    cada puesto, para que cada uno elija la suya aunque luego otros entren en ese puesto."""
    for g in juegos:
        if g["modo"] != "cable":
            continue
        carpeta = Path(datos) / g["id"]
        destino = carpeta / "originales"
        if destino.exists():
            continue
        destino.mkdir(parents=True)
        for f in sorted(carpeta.glob("sala*/jugador*.sav")):
            shutil.copy2(f, destino / f"{f.parent.name}-{f.stem}.sav")
        print(f"{g['id']}: partidas de antes copiadas a {destino}", flush=True)


def lista_originales(sala, yo):
    """Las partidas de antes de un juego, para elegir la propia: puesto, fecha, entrenador y si ya
    la ha elegido otro navegador."""
    elegidas = leer_json(sala.originales / "elegidas.json", {})
    lista = []
    for f in sorted(sala.originales.glob("sala*-jugador*.sav")):
        m = re.fullmatch(r"sala(\d+)-jugador(\d+)\.sav", f.name)
        if not m:
            continue
        datos = f.read_bytes()
        lista.append({"archivo": f.name, "sala": int(m[1]), "jugador": int(m[2]),
                      "fecha": time.strftime("%d-%m-%Y", time.localtime(f.stat().st_mtime)),
                      "entrenador": entrenador(datos),
                      "elegida": any(x != yo for x in elegidas.get(f.name, []))})
    return {"partidas": lista, "copia": bool(yo) and (sala.personas / f"{yo}.sav").is_file()}


def mudar_partidas(datos, primero):
    """Partidas de versiones anteriores, que solo tenían Pokémon Quetzal:
    - al principio había una sola sala, con las partidas sueltas en la carpeta;
    - luego hubo salas, en <datos>/salaN.
    Todo pasa a <datos>/<primer juego>/salaN."""
    datos = Path(datos)
    destino = datos / primero
    sueltas = list(datos.glob("jugador*.sav*"))
    if sueltas and not (datos / "sala1").exists():
        (datos / "sala1").mkdir()
        for f in sueltas:
            os.replace(f, datos / "sala1" / f.name)
    viejas = sorted(datos.glob("sala[0-9]*"))
    if viejas and not destino.exists():
        destino.mkdir()
        for d in viejas:
            os.replace(d, destino / d.name)
        print(f"partidas movidas a {destino}", flush=True)


def leer_juegos(ruta, roms):
    """La lista de juegos, sin los que no se pueden jugar aquí: falta su ROM o su gbalink
    (gbalink-fs, para Four Swords, hay que compilarlo aparte)."""
    juegos = []
    for g in json.loads(Path(ruta).read_text(encoding="utf-8")):
        hacen_falta = [Path(roms) / g["rom"], AQUI / g.get("binario", "gbalink")]
        if g.get("nucleo"):
            hacen_falta.append(AQUI / g["nucleo"])
        falta = [str(f) for f in hacen_falta if not f.is_file()]
        if falta:
            print(f"{g['id']}: no está disponible (falta {', '.join(falta)})", flush=True)
        else:
            juegos.append(g)
    for g in juegos:
        assert g["modo"] in ("cable", "compartida"), g["id"]
        # Sistema: "gba" (gbalink) o "n64" (lrlink con un núcleo de libretro, una sola consola).
        # Cómo es su imagen y su sonido
        g.setdefault("sistema", "gba")
        if g["sistema"] == "n64":
            g.setdefault("video", {"ancho": 640, "alto": 480, "fps": 50})
            g.setdefault("audio_hz", 22050)
        g.setdefault("video", {"ancho": 240, "alto": 160, "fps": 60, "salida": [480, 320]})
        g["video"].setdefault("salida", [g["video"]["ancho"], g["video"]["alto"]])
        g.setdefault("audio_hz", 16384)
        if g["modo"] == "cable":
            # Una consola por jugador; con una sola (la N64), cada uno juega en la suya
            g["consolas"] = sorted(n for n in g.get("consolas", [2, 3, 4]) if 1 <= n <= MAX_JUGADORES)
        else:
            g["jugadores"] = max(1, min(MAX_JUGADORES, g.get("jugadores", 2)))
        g.setdefault("salas", 1)
    return juegos


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--juegos", default=str(AQUI / "juegos-online.json"), help="lista de juegos")
    ap.add_argument("--roms", default="/var/lib/gba-link", help="carpeta de las ROMs de la lista")
    ap.add_argument("--datos", required=True)
    # Medido: 2 consolas gastan 0,75 núcleos, 3 consolas 1 y 4 consolas 1,4 (el doble a x2).
    # Ajústalo a los núcleos de la CPU (y a lo que necesiten otros servicios de la máquina)
    ap.add_argument("--max-consolas", type=int, default=6, help="entre todas las salas de todos los juegos")
    ap.add_argument("--puerto", type=int, default=9081)
    # Medido con un clip de menús y paseo: con crf 26 y búfer de 300k se emborronaban los
    # fundidos y los cambios de pantalla; crf 22 los deja limpios por un 30 % más de vídeo
    ap.add_argument("--crf", type=int, default=22)
    ap.add_argument("--maxrate", default="3000k")
    ap.add_argument("--bufsize", default="1500k")
    args = ap.parse_args()
    juegos = leer_juegos(args.juegos, args.roms)
    if not juegos:
        raise SystemExit("ningún juego disponible")
    mudar_partidas(args.datos, juegos[0]["id"])
    guardar_originales(args.datos, juegos)
    todas = []
    salas = {}  # id del juego -> sus salas
    for g in juegos:
        salas[g["id"]] = [Sala(args, g, k + 1, todas) for k in range(g["salas"])]
        todas.extend(salas[g["id"]])
    pagina = (AQUI / "gba-online.html").read_bytes()

    def consulta(ruta_completa):
        texto = ruta_completa.partition("?")[2]
        return dict(p.split("=", 1) for p in texto.split("&") if "=" in p)

    def parametros(ruta_completa):
        """Sala, puesto j (0-3) y consolas n de la consulta; None si no valen."""
        params = consulta(ruta_completa)
        lista = salas.get(params.get("juego", juegos[0]["id"]))
        if lista is None:
            return None
        g = lista[0].juego
        try:
            s = int(params.get("sala", "1"))
            j = int(params.get("j", "0"))
            n = int(params.get("n", "0"))
        except ValueError:
            return None
        if not (1 <= s <= len(lista) and 0 <= j < MAX_JUGADORES):
            return None
        if g["modo"] == "cable":
            # Las consolas que pide, si el juego las admite; si no, las mínimas para su puesto
            n = n if n in g["consolas"] and n > j else next((k for k in g["consolas"] if k > j), None)
            if n is None:
                return None
        elif j >= g["jugadores"]:
            return None
        return lista[s - 1], j, n

    def respuesta_json(datos):
        return Response(http.HTTPStatus.OK, "OK", Headers({
            "Content-Type": "application/json", "Cache-Control": "no-store"}),
            json.dumps(datos, ensure_ascii=False).encode())

    def http_estatico(conexion, peticion):
        ruta = peticion.path.split("?")[0]
        if ruta.endswith("/ws"):
            return None
        if ruta in ("/", "/gba-online.html"):
            return Response(http.HTTPStatus.OK, "OK", Headers({"Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-cache"}), pagina)
        if ruta.endswith("/retro.css") and (AQUI / "retro.css").exists():
            # La hoja de estilo va junto a la página (también detrás de un proxy en otra ruta)
            return Response(http.HTTPStatus.OK, "OK", Headers({"Content-Type": "text/css; charset=utf-8", "Cache-Control": "no-cache"}),
                            (AQUI / "retro.css").read_bytes())
        if ruta == "/juegos":
            # Para la página y la portada de juegos: qué se puede jugar y cómo
            publicos = ("id", "nombre", "imagen", "modo", "salas", "consolas", "jugadores", "descripcion", "ayuda",
                        "sistema", "video", "audio_hz",
                        # en_lista: no sale en la sección Online de la portada; lo abre la tarjeta
                        # del juego con ese id en la lista normal (juegos.json)
                        "en_lista")  # sin rutas
            return respuesta_json([{k: g[k] for k in publicos if k in g} for g in juegos])
        if ruta == "/salas":
            en_uso = sum(s.n * s.juego.get("peso", 1) for s in todas if s.activa)
            return respuesta_json({"juegos": {k: [s.resumen() for s in v] for k, v in salas.items()},
                                   "max_consolas": args.max_consolas, "consolas_en_uso": en_uso})
        if ruta == "/rom":
            # Descargar el juego (botón de la ventana Info)
            g = next((g for g in juegos if g["id"] == consulta(peticion.path).get("juego")), None)
            rom = Path(args.roms) / g["rom"] if g else None
            if rom and rom.is_file():
                nombre = "".join(ch for ch in g["nombre"] if ch.isalnum() or ch in " -_,.") + rom.suffix
                return Response(http.HTTPStatus.OK, "OK", Headers({
                    "Content-Type": "application/octet-stream", "Cache-Control": "no-store",
                    "Content-Disposition": "attachment; filename*=UTF-8''" + urllib.parse.quote(nombre)}),
                    rom.read_bytes())
        if ruta == "/partidas":
            # Las partidas de antes, para un navegador que aún no tiene la suya
            params = consulta(peticion.path)
            lista = salas.get(params.get("juego"))
            yo = params.get("yo", "")
            if lista and ID_VALIDO.fullmatch(yo):
                return respuesta_json(lista_originales(lista[0], yo))
        if ruta == "/partida":
            p = parametros(peticion.path)
            # Con la sala encendida el fichero está al día: mGBA lo tiene mapeado en memoria
            if p and p[0].ruta_partida(p[1]).exists():
                sala, j, _ = p
                nombre = f"{sala.juego['id']}-sala{sala.numero}" + ("" if sala.compartida else f"-jugador{j + 1}")
                return Response(http.HTTPStatus.OK, "OK", Headers({
                    "Content-Type": "application/octet-stream", "Cache-Control": "no-store",
                    "Content-Disposition": f'attachment; filename="{nombre}.sav"'}),
                    sala.ruta_partida(j).read_bytes())
        return Response(http.HTTPStatus.NOT_FOUND, "Not Found", Headers(), b"")

    async def atender(ws):
        p = parametros(ws.request.path)
        if p is None:
            await ws.send("errorEse puesto no existe en este juego.")
            return
        sala, j, n = p
        yo = consulta(ws.request.path).get("yo", "")
        c = Cliente(ws, yo if ID_VALIDO.fullmatch(yo) else None)
        # Lo primero que manda la página es con qué partida entra (ver Sala.partida_de). Una página
        # de antes no lo manda: se entra como antes, con la partida que tenga el puesto
        inicial = ("nada",)
        try:
            primero = await asyncio.wait_for(ws.recv(), 5)
        except asyncio.TimeoutError:
            primero = None
        if isinstance(primero, bytes) and primero.startswith(MIA) and 0 < len(primero) - len(MIA) <= PARTIDA_MAX:
            inicial = ("mia", primero[len(MIA):])
        elif isinstance(primero, str) and primero.startswith("usar "):
            inicial = ("usar", primero[5:])
        elif primero in ("nueva", "copia"):
            inicial = (primero,)
        error = await sala.entrar(j, n, c, inicial)
        if error:
            await ws.send("error" + error)
            return
        sala.registro(f"jugador {j + 1} conectado" + (f" (navegador {c.yo[:6]}…)" if c.yo else ""))

        async def emisor():
            while True:
                await ws.send(await c.cola.get())

        tarea = asyncio.create_task(emisor())
        try:
            async for msg in ws:
                if isinstance(msg, bytes) and len(msg) == 2:
                    await sala.teclas_de(j, int.from_bytes(msg, "little") & 0x3FF)
                elif isinstance(msg, bytes) and len(msg) == 4 and sala.juego["sistema"] == "n64":
                    # N64: botones (16 bits) y el stick (dos bytes con signo)
                    x, y = struct.unpack_from("<bb", msg, 2)
                    await sala.teclas_de(j, int.from_bytes(msg[:2], "little"), (max(-80, min(80, x)), max(-80, min(80, y))))
                elif isinstance(msg, bytes) and msg.startswith(SUBIDA):
                    datos = msg[len(SUBIDA):]
                    if sala.compartida or c.yo is None:
                        respuesta = await sala.subir_partida(j, datos)
                    elif not 0 < len(datos) <= PARTIDA_MAX:
                        respuesta = "partida no válida: tamaño incorrecto"
                    else:
                        respuesta = await sala.cambiar_partida(j, c, ("subida", datos))
                        respuesta = "partida cargada" if respuesta == "partida cambiada" else respuesta
                    sala.registro(f"jugador {j + 1}: {respuesta}")
                    await ws.send(respuesta)
                elif isinstance(msg, str) and (msg.startswith("usar ") or msg in ("nueva", "copia")):
                    inicial = ("usar", msg[5:]) if msg.startswith("usar ") else (msg,)
                    respuesta = await sala.cambiar_partida(j, c, inicial)
                    await ws.send(respuesta)
                elif isinstance(msg, str) and msg.startswith("ping"):
                    await ws.send("pong" + msg[4:])
                elif isinstance(msg, str) and msg.startswith("vel "):
                    try:
                        await sala.cambiar(velocidad=float(msg[4:]))
                    except ValueError:
                        pass
                elif msg in ("pausa 0", "pausa 1"):
                    await sala.cambiar(pausa=msg == "pausa 1")
                elif msg == "reiniciar":
                    await sala.reiniciar(j)
        finally:
            tarea.cancel()
            await sala.baja(j, c)
            sala.registro(f"jugador {j + 1} desconectado")

    async with serve(atender, "127.0.0.1", args.puerto, process_request=http_estatico, max_size=PARTIDA_MAX + 64, compression=None):
        print(f"escuchando en 127.0.0.1:{args.puerto} con {len(juegos)} juegos", flush=True)
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
