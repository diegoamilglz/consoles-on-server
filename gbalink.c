/* gbalink: varias GBA (mGBA) unidas por cable link dentro de un mismo proceso.
 * Es la misma lógica de MultiplayerController.cpp de mGBA (lockstep), pero sin interfaz:
 * cada consola corre en su hilo, entrega su imagen y su sonido por tuberías FIFO y
 * recibe las teclas por stdin. Como todas las consolas están en la misma máquina, el
 * cable no sufre el retardo de internet: lo que viaja por la red es imagen y teclas.
 *
 * Uso: gbalink <rom> <dir_datos> <jugadores 2-4> [dir_fifos]
 * Órdenes por stdin:
 *   k <jugador> <máscara>     teclas pulsadas (bit 0 A, 1 B, 2 Select, 3 Start, 4 Der,
 *                             5 Izq, 6 Arriba, 7 Abajo, 8 R, 9 L)
 *   shot <jugador> <fichero>  guarda la imagen actual en PPM
 *   turbo <0|1>               sin límite de velocidad (para pruebas)
 *   vel <factor>              velocidad de todas las consolas (1 = normal, 1.5, 2...)
 *   pausa <0|1>               detiene o reanuda todas las consolas
 *   ss <jugador> <fichero>    guarda un estado   /   ls <jugador> <fichero>  lo carga
 *   sav <jugador>             vuelve a leer jugadorN.sav (partida subida) y reinicia esa consola
 *   stat                     imprime fotogramas por segundo de cada consola
 *   quit                      guarda y sale
 *
 * Se compila con dos versiones de mGBA:
 *   - 0.10.5 (gbalink): el cable es la lógica de MultiplayerController.cpp, copiada aquí.
 *   - 0.11 de mgba-splitscreen (gbalink-fs, con -DMGBA_011): el cable lo coordina la propia
 *     libmgba (GBASIOLockstepCoordinator). Esa versión tiene arreglos en el cable que hacen
 *     falta para Zelda Four Swords, que con la 0.10.5 se queda en "Conectando las consolas".
 */
#include <mgba/core/core.h>
#include <mgba/core/lockstep.h>
#include <mgba/core/serialize.h>
#include <mgba/core/thread.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba-util/vfs.h>
#ifdef MGBA_011
#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>
#include <mgba-util/image.h>
typedef mColor color_t; /* en la 0.11 el tipo del píxel cambió de nombre */
#else
#include <mgba/core/blip_buf.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ANCHO 240
#define ALTO 160
#define FRECUENCIA_AUDIO 16384
#define NS_POR_FOTOGRAMA 16742706LL /* 280896 ciclos a 16,777216 MHz */

struct Jugador {
	int id;
	struct mCore* core;
	struct mCoreThread hilo;
	color_t* buffer;

#ifdef MGBA_011
	struct GBASIOLockstepDriver cable;
	struct mLockstepUser usuario; /* el cable duerme y despierta la consola con esto */
	/* Hilo propio de la consola (no mCoreThread): ver hiloConsola */
	pthread_t hiloPropio;
	pthread_mutex_t control;
	pthread_cond_t cambio;
	bool dormida;   /* el cable le ha pedido esperar */
	int detenida;   /* órdenes (pausa, velocidad, partida...): se puede anidar */
	bool corriendo; /* dentro de runLoop */
	bool parar;
	struct mAudioResampler remuestreo;
	struct mAudioBuffer sonido; /* ya a FRECUENCIA_AUDIO, en estéreo */
#else
	struct GBASIOLockstepNode nodo;
	/* Lockstep: los mismos campos que MultiplayerController::Player */
	int awake;
	int32_t cyclesPosted;
	unsigned waitMask;
#endif
	volatile bool sync;

	long long siguiente; /* siguiente fotograma, en ns */
	volatile long fotogramas;
	long fotogramasPrevios;

	/* Buzón: siempre el último fotograma; si el codificador va lento, se saltan */
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	uint8_t* buzon;
	bool buzonNuevo;
	char rutaVideo[512];
	char rutaAudio[512];
	int fdAudio;
	char rutaPartida[512];
};

static struct Jugador jugadores[MAX_GBAS];
static int numJugadores;
static volatile int turbo;
static volatile double velocidad = 1.0;
static int enPausa;
#ifdef MGBA_011
static struct GBASIOLockstepCoordinator coordinador;
#else
static pthread_mutex_t cerrojo = PTHREAD_MUTEX_INITIALIZER;
static struct GBASIOLockstep lockstep;
#endif

static long long ahoraNs(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* mGBA escribiría miles de líneas por segundo en stdout */
static void sinRegistro(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	UNUSED(category);
	UNUSED(level);
	UNUSED(format);
	UNUSED(args);
}
static struct mLogger registroMudo = { .log = sinRegistro };

#ifdef MGBA_011
/* Cada consola pide su puesto en el cable: la 0 es la maestra */
static int puestoPedido(struct mLockstepUser* u) {
	struct Jugador* p = (struct Jugador*) ((char*) u - offsetof(struct Jugador, usuario));
	return p->id;
}
#else
/* --- Callbacks de lockstep, traducidos de MultiplayerController.cpp --- */

static void lsLock(struct mLockstep* l) { UNUSED(l); pthread_mutex_lock(&cerrojo); }
static void lsUnlock(struct mLockstep* l) { UNUSED(l); pthread_mutex_unlock(&cerrojo); }

static bool lsSignal(struct mLockstep* l, unsigned mask) {
	UNUSED(l);
	struct Jugador* p = &jugadores[0];
	bool woke = false;
	p->waitMask &= ~mask;
	if (!p->waitMask && p->awake < 1) {
		mCoreThreadStopWaiting(&p->hilo);
		p->awake = 1;
		woke = true;
	}
	return woke;
}

static bool lsWait(struct mLockstep* l, unsigned mask) {
	UNUSED(l);
	struct Jugador* p = &jugadores[0];
	bool slept = false;
	p->waitMask |= mask;
	if (p->awake > 0) {
		mCoreThreadWaitFromThread(&p->hilo);
		p->awake = 0;
		slept = true;
	}
	p->sync = true;
	return slept;
}

static void lsAddCycles(struct mLockstep* l, int id, int32_t cycles) {
	UNUSED(l);
	if (cycles < 0) {
		abort();
	}
	struct Jugador* p = &jugadores[id];
	if (!id) {
		for (int i = 1; i < numJugadores; ++i) {
			p = &jugadores[i];
			if (p->nodo.d.p->mode > SIO_MULTI) {
				p->sync = true;
				continue;
			}
			p->sync = false;
			p->cyclesPosted += cycles;
			if (p->awake < 1) {
				p->nodo.nextEvent += p->cyclesPosted;
			}
			mCoreThreadStopWaiting(&p->hilo);
			p->awake = 1;
		}
	} else {
		p->sync = true;
		p->cyclesPosted += cycles;
	}
}

static int32_t lsUseCycles(struct mLockstep* l, int id, int32_t cycles) {
	UNUSED(l);
	struct Jugador* p = &jugadores[id];
	p->cyclesPosted -= cycles;
	if (p->cyclesPosted <= 0) {
		mCoreThreadWaitFromThread(&p->hilo);
		p->awake = 0;
	}
	return p->cyclesPosted;
}

static int32_t lsUnusedCycles(struct mLockstep* l, int id) {
	UNUSED(l);
	return jugadores[id].cyclesPosted;
}

static void lsUnload(struct mLockstep* l, int id) {
	if (id) {
		struct Jugador* p = &jugadores[id];
		p->sync = true;
		p->cyclesPosted = 0;
		/* Libera a la consola maestra si estaba esperando a esta */
		p = &jugadores[0];
		p->waitMask &= ~(1 << id);
		if (!p->waitMask && p->awake < 1) {
			mCoreThreadStopWaiting(&p->hilo);
			p->awake = 1;
		}
	} else {
		for (int i = 1; i < numJugadores; ++i) {
			struct Jugador* p = &jugadores[i];
			p->sync = true;
			p->cyclesPosted += ((struct GBASIOLockstep*) l)->players[0]->eventDiff;
			if (p->awake < 1) {
				p->nodo.nextEvent += p->cyclesPosted;
				mCoreThreadStopWaiting(&p->hilo);
				p->awake = 1;
			}
		}
	}
}
#endif

/* --- Salida de imagen y sonido --- */

static void* hiloVideo(void* arg) {
	struct Jugador* p = arg;
	uint8_t* copia = malloc(ANCHO * ALTO * 4);
	int fd = open(p->rutaVideo, O_WRONLY); /* se bloquea hasta que haya lector */
	if (fd < 0) {
		perror(p->rutaVideo);
		return NULL;
	}
	long long ultimo = 0;
	for (;;) {
		/* Como mucho unas 66 imágenes por segundo: acelerado, el resto se salta y el
		 * codificador y la red no trabajan de más */
		long long espera = ultimo + 15000000LL - ahoraNs();
		if (espera > 0) {
			struct timespec t = { .tv_sec = 0, .tv_nsec = espera };
			nanosleep(&t, NULL);
		}
		pthread_mutex_lock(&p->mutex);
		while (!p->buzonNuevo) {
			pthread_cond_wait(&p->cond, &p->mutex);
		}
		memcpy(copia, p->buzon, ANCHO * ALTO * 4);
		p->buzonNuevo = false;
		pthread_mutex_unlock(&p->mutex);
		ultimo = ahoraNs();
		size_t hecho = 0;
		while (hecho < ANCHO * ALTO * 4) {
			ssize_t n = write(fd, copia + hecho, ANCHO * ALTO * 4 - hecho);
			if (n <= 0) {
				if (n < 0 && errno == EINTR) {
					continue;
				}
				close(fd);
				return NULL;
			}
			hecho += n;
		}
	}
}

/* Sonido: mono, 16 bits, 16384 Hz (unos 260 kbit/s). Con 8 bits (ley mu) se oía siseo */
static void sacarAudio(struct Jugador* p) {
	int16_t muestras[4096];
	int16_t salida[2048];
#ifdef MGBA_011
	/* El núcleo deja el sonido a su frecuencia; el remuestreador lo pasa a la nuestra */
	mAudioResamplerSetSource(&p->remuestreo, p->core->getAudioBuffer(p->core), p->core->audioSampleRate(p->core), true);
	mAudioResamplerProcess(&p->remuestreo);
	int disponibles = (int) mAudioBufferRead(&p->sonido, muestras, 2048);
	if (disponibles <= 0) {
		return;
	}
#else
	struct blip_t* izq = p->core->getAudioChannel(p->core, 0);
	struct blip_t* der = p->core->getAudioChannel(p->core, 1);
	int disponibles = blip_samples_avail(izq);
	if (disponibles > 2048) {
		disponibles = 2048;
	}
	if (disponibles <= 0) {
		return;
	}
	blip_read_samples(izq, muestras, disponibles, 1);
	blip_read_samples(der, muestras + 1, disponibles, 1);
#endif
	if (p->fdAudio < 0 && p->rutaAudio[0]) {
		p->fdAudio = open(p->rutaAudio, O_WRONLY | O_NONBLOCK);
		if (p->fdAudio < 0) {
			return; /* aún no hay lector */
		}
	}
	if (p->fdAudio >= 0) {
		for (int i = 0; i < disponibles; ++i) {
			salida[i] = (int16_t) ((muestras[i * 2] + muestras[i * 2 + 1]) / 2);
		}
		/* Sin bloquear: mejor perder sonido que parar la consola. Bloques de menos de
		 * 4 KiB: la tubería los escribe enteros o no los escribe, así no se parten muestras */
		ssize_t n = write(p->fdAudio, salida, disponibles * 2);
		UNUSED(n);
	}
}

static void fotogramaHecho(struct Jugador* p) {
	++p->fotogramas;

	if (p->buzon) {
		pthread_mutex_lock(&p->mutex);
		memcpy(p->buzon, p->buffer, ANCHO * ALTO * 4);
		p->buzonNuevo = true;
		pthread_cond_signal(&p->cond);
		pthread_mutex_unlock(&p->mutex);
	}
	sacarAudio(p);

	if (turbo || !p->sync) {
		p->siguiente = 0;
		return;
	}
	long long ahora = ahoraNs();
	if (!p->siguiente || ahora > p->siguiente + 100000000LL) {
		p->siguiente = ahora; /* muy retrasados: no intentar recuperar */
	}
	/* Las consolas esclavas siguen a la maestra por el cable: basta con acelerar el reloj */
	p->siguiente += (long long) (NS_POR_FOTOGRAMA / velocidad);
	if (p->siguiente > ahora) {
		struct timespec t = { .tv_sec = (p->siguiente - ahora) / 1000000000LL, .tv_nsec = (p->siguiente - ahora) % 1000000000LL };
		nanosleep(&t, NULL);
	}
}

static void alFotograma(struct mCoreThread* hilo) {
	fotogramaHecho(hilo->userData);
}

/* Acelerado, el sonido sale más agudo pero al ritmo real: así no se acumula en el navegador */
static void ajustarAudio(struct Jugador* p) {
	double frecuencia = FRECUENCIA_AUDIO / velocidad;
#ifdef MGBA_011
	mAudioResamplerSetDestination(&p->remuestreo, &p->sonido, frecuencia);
#else
	blip_set_rates(p->core->getAudioChannel(p->core, 0), p->core->frequency(p->core), frecuencia);
	blip_set_rates(p->core->getAudioChannel(p->core, 1), p->core->frequency(p->core), frecuencia);
#endif
}

static void alArrancar(struct mCoreThread* hilo) {
	ajustarAudio(hilo->userData);
}

#ifdef MGBA_011
/* --- Hilos propios de las consolas ---
 * Como threaded_link de mgba-splitscreen, que es con lo que ese fork llega a jugar a Four
 * Swords: cada consola ejecuta runLoop en bucle, y cuando el cable le pide dormir, termina
 * el tramo y espera después (con mCoreThread se queda en "Conectando las consolas GBA") */
static struct Jugador* jugadorDe(struct mLockstepUser* u) {
	return (struct Jugador*) ((char*) u - offsetof(struct Jugador, usuario));
}

static void cableDuerme(struct mLockstepUser* u) {
	struct Jugador* p = jugadorDe(u);
	pthread_mutex_lock(&p->control);
	p->dormida = true;
	pthread_mutex_unlock(&p->control);
}

static void cableDespierta(struct mLockstepUser* u) {
	struct Jugador* p = jugadorDe(u);
	pthread_mutex_lock(&p->control);
	p->dormida = false;
	pthread_cond_broadcast(&p->cambio);
	pthread_mutex_unlock(&p->control);
}

static void* hiloConsola(void* arg) {
	struct Jugador* p = arg;
	uint32_t ultimo = p->core->frameCounter(p->core);
	for (;;) {
		pthread_mutex_lock(&p->control);
		while ((p->dormida || p->detenida) && !p->parar) {
			pthread_cond_wait(&p->cambio, &p->control);
		}
		if (p->parar) {
			pthread_mutex_unlock(&p->control);
			return NULL;
		}
		p->corriendo = true;
		pthread_mutex_unlock(&p->control);

		p->core->runLoop(p->core);
		uint32_t fc = p->core->frameCounter(p->core);
		if (fc != ultimo) {
			ultimo = fc;
			fotogramaHecho(p);
		}

		pthread_mutex_lock(&p->control);
		p->corriendo = false;
		pthread_cond_broadcast(&p->cambio);
		pthread_mutex_unlock(&p->control);
	}
}

/* --- Órdenes --- */

static void interrumpirTodas(void) {
	for (int i = 0; i < numJugadores; ++i) {
		struct Jugador* p = &jugadores[i];
		pthread_mutex_lock(&p->control);
		++p->detenida;
		while (p->corriendo) {
			pthread_cond_wait(&p->cambio, &p->control);
		}
		pthread_mutex_unlock(&p->control);
	}
}

static void continuarTodas(void) {
	for (int i = 0; i < numJugadores; ++i) {
		struct Jugador* p = &jugadores[i];
		pthread_mutex_lock(&p->control);
		if (p->detenida > 0) {
			--p->detenida;
		}
		pthread_cond_broadcast(&p->cambio);
		pthread_mutex_unlock(&p->control);
	}
}
#else
/* --- Órdenes --- */

static void interrumpirTodas(void) {
	for (int i = 0; i < numJugadores; ++i) {
		mCoreThreadInterrupt(&jugadores[i].hilo);
	}
}

static void continuarTodas(void) {
	for (int i = 0; i < numJugadores; ++i) {
		mCoreThreadContinue(&jugadores[i].hilo);
	}
}
#endif

static void captura(struct Jugador* p, const char* ruta) {
	FILE* f = fopen(ruta, "wb");
	if (!f) {
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", ANCHO, ALTO);
	const uint8_t* b = (const uint8_t*) p->buffer;
	for (int i = 0; i < ANCHO * ALTO; ++i) {
		fwrite(b + i * 4, 1, 3, f);
	}
	fclose(f);
}

int main(int argc, char** argv) {
	if (argc < 4) {
		fprintf(stderr, "uso: %s <rom> <dir_datos> <jugadores> [dir_fifos]\n", argv[0]);
		return 1;
	}
	const char* rom = argv[1];
	const char* datos = argv[2];
	numJugadores = atoi(argv[3]);
	const char* fifos = argc > 4 ? argv[4] : NULL;
	if (numJugadores < 1 || numJugadores > MAX_GBAS) {
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IOLBF, 0);
#ifdef MGBA_011
	/* El cable de mgba-splitscreen escribe diagnósticos muy a menudo: fuera */
	mLogSetDefaultLogger(&registroMudo);
#endif

#ifdef MGBA_011
	GBASIOLockstepCoordinatorInit(&coordinador);
#else
	memset(&lockstep, 0, sizeof(lockstep));
	mLockstepInit(&lockstep.d);
	GBASIOLockstepInit(&lockstep);
	lockstep.d.lock = lsLock;
	lockstep.d.unlock = lsUnlock;
	lockstep.d.signal = lsSignal;
	lockstep.d.wait = lsWait;
	lockstep.d.addCycles = lsAddCycles;
	lockstep.d.useCycles = lsUseCycles;
	lockstep.d.unusedCycles = lsUnusedCycles;
	lockstep.d.unload = lsUnload;
#endif

	mkdir(datos, 0755);
	for (int i = 0; i < numJugadores; ++i) {
		struct Jugador* p = &jugadores[i];
		p->id = i;
#ifndef MGBA_011
		p->awake = 1;
#endif
		p->sync = true;
		p->fdAudio = -1;
		pthread_mutex_init(&p->mutex, NULL);
		pthread_cond_init(&p->cond, NULL);

		p->core = mCoreFind(rom);
		if (!p->core || !p->core->init(p->core)) {
			fprintf(stderr, "no se pudo crear el núcleo\n");
			return 1;
		}
		mCoreInitConfig(p->core, NULL);
		p->buffer = calloc(ANCHO * ALTO, BYTES_PER_PIXEL);
		p->core->setVideoBuffer(p->core, p->buffer, ANCHO);
		if (!mCoreLoadFile(p->core, rom)) {
			fprintf(stderr, "no se pudo cargar la ROM\n");
			return 1;
		}
		snprintf(p->rutaPartida, sizeof(p->rutaPartida), "%s/jugador%d.sav", datos, i + 1);
		mCoreLoadSaveFile(p->core, p->rutaPartida, false);

		if (fifos) {
			p->buzon = malloc(ANCHO * ALTO * 4);
			snprintf(p->rutaVideo, sizeof(p->rutaVideo), "%s/p%d.video", fifos, i);
			snprintf(p->rutaAudio, sizeof(p->rutaAudio), "%s/p%d.audio", fifos, i);
			pthread_t t;
			pthread_create(&t, NULL, hiloVideo, p);
			pthread_detach(t);
		}

		p->hilo.core = p->core;
		p->hilo.userData = p;
		p->hilo.logger.d = (struct mLogger) { .log = sinRegistro };
		p->hilo.logger.logger = &registroMudo;
		p->hilo.frameCallback = alFotograma;
		p->hilo.startCallback = alArrancar;
#ifdef MGBA_011
		mAudioBufferInit(&p->sonido, 4096, 2);
		mAudioResamplerInit(&p->remuestreo, mINTERPOLATOR_COSINE);
		pthread_mutex_init(&p->control, NULL);
		pthread_cond_init(&p->cambio, NULL);
		GBASIOLockstepDriverCreate(&p->cable, &p->usuario);
		GBASIOLockstepCoordinatorAttach(&coordinador, &p->cable);
		p->usuario.sleep = cableDuerme;
		p->usuario.wake = cableDespierta;
		p->usuario.requestedId = puestoPedido;
#else
		GBASIOLockstepNodeCreate(&p->nodo);
#endif
	}

#ifdef MGBA_011
	/* El cable se enchufa antes de encender, y al encender (reset) cada consola se apunta en
	 * el coordinador: así el juego lo ve desde el arranque */
	for (int i = 0; i < numJugadores; ++i) {
		jugadores[i].core->setPeripheral(jugadores[i].core, mPERIPH_GBA_LINK_PORT, &jugadores[i].cable.d);
	}
	for (int i = 0; i < numJugadores; ++i) {
		jugadores[i].core->reset(jugadores[i].core);
		ajustarAudio(&jugadores[i]);
	}
	/* Solo 2 consolas: con 3 o 4, Four Swords se queda en "Conectando" (probado también
	 * con un solo hilo por turnos, como la versión web de mgba-splitscreen) */
	for (int i = 0; i < numJugadores; ++i) {
		if (pthread_create(&jugadores[i].hiloPropio, NULL, hiloConsola, &jugadores[i])) {
			fprintf(stderr, "no se pudo arrancar la consola %d\n", i);
			return 1;
		}
	}
#else
	/* Arrancar y conectar el cable con las consolas detenidas */
	for (int i = 0; i < numJugadores; ++i) {
		if (!mCoreThreadStart(&jugadores[i].hilo)) {
			fprintf(stderr, "no se pudo arrancar la consola %d\n", i);
			return 1;
		}
	}
	interrumpirTodas();
	for (int i = 0; i < numJugadores; ++i) {
		struct Jugador* p = &jugadores[i];
		struct GBA* gba = p->core->board;
		GBASIOLockstepAttachNode(&lockstep, &p->nodo);
		GBASIOSetDriver(&gba->sio, &p->nodo.d, SIO_MULTI);
		GBASIOSetDriver(&gba->sio, &p->nodo.d, SIO_NORMAL_32);
	}
	continuarTodas();
#endif
	printf("listo jugadores=%d\n", numJugadores);

	char linea[1024];
	long long ultimoStat = ahoraNs();
	while (fgets(linea, sizeof(linea), stdin)) {
		int j, m;
		char ruta[900];
		if (sscanf(linea, "k %d %d", &j, &m) == 2) {
			if (j >= 0 && j < numJugadores) {
				jugadores[j].core->setKeys(jugadores[j].core, (uint32_t) m & 0x3FF);
			}
		} else if (sscanf(linea, "shot %d %899s", &j, ruta) == 2) {
			if (j >= 0 && j < numJugadores) {
				captura(&jugadores[j], ruta);
				printf("shot %d\n", j);
			}
		} else if (sscanf(linea, "turbo %d", &m) == 1) {
			turbo = m;
		} else if (!strncmp(linea, "vel ", 4)) {
			double v = atof(linea + 4);
			if (v >= 0.5 && v <= 3) {
				interrumpirTodas();
				velocidad = v;
				for (int i = 0; i < numJugadores; ++i) {
					ajustarAudio(&jugadores[i]);
				}
				continuarTodas();
				printf("vel %.2f\n", velocidad);
			}
		} else if (sscanf(linea, "pausa %d", &m) == 1) {
			if (m && !enPausa) {
				interrumpirTodas();
			} else if (!m && enPausa) {
				continuarTodas();
			}
			enPausa = !!m;
			printf("pausa %d\n", enPausa);
		} else if (sscanf(linea, "ss %d %899s", &j, ruta) == 2 || sscanf(linea, "ls %d %899s", &j, ruta) == 2) {
			if (j >= 0 && j < numJugadores) {
				bool guardar = linea[0] == 's';
				struct VFile* vf = VFileOpen(ruta, guardar ? (O_CREAT | O_TRUNC | O_RDWR) : O_RDONLY);
				bool ok = false;
				if (vf) {
					interrumpirTodas();
					ok = guardar ? mCoreSaveStateNamed(jugadores[j].core, vf, SAVESTATE_SAVEDATA)
					             : mCoreLoadStateNamed(jugadores[j].core, vf, SAVESTATE_SAVEDATA);
					continuarTodas();
					vf->close(vf);
				}
				printf("%s %d %s\n", guardar ? "ss" : "ls", j, ok ? "ok" : "fallo");
			}
		} else if (sscanf(linea, "sav %d", &j) == 1) {
			/* El servidor ya ha dejado el fichero nuevo en jugadorN.sav (el viejo sigue abierto
			 * con otro nombre). Se cambia y se reinicia la consola, como al encenderla con otro cartucho */
			if (j >= 0 && j < numJugadores) {
				interrumpirTodas();
				bool ok = mCoreLoadSaveFile(jugadores[j].core, jugadores[j].rutaPartida, false);
				if (ok) {
					jugadores[j].core->reset(jugadores[j].core);
				}
				continuarTodas();
				printf("sav %d %s\n", j, ok ? "ok" : "fallo");
			}
		} else if (!strncmp(linea, "stat", 4)) {
			long long ahora = ahoraNs();
			double s = (ahora - ultimoStat) / 1e9;
			ultimoStat = ahora;
			printf("fps");
			for (int i = 0; i < numJugadores; ++i) {
				long f = jugadores[i].fotogramas;
				printf(" %.1f", (f - jugadores[i].fotogramasPrevios) / s);
				jugadores[i].fotogramasPrevios = f;
			}
			printf("\n");
		} else if (!strncmp(linea, "quit", 4)) {
			break;
		}
	}

#ifdef MGBA_011
	for (int i = 0; i < numJugadores; ++i) {
		struct Jugador* p = &jugadores[i];
		pthread_mutex_lock(&p->control);
		p->parar = true;
		pthread_cond_broadcast(&p->cambio);
		pthread_mutex_unlock(&p->control);
	}
#else
	if (enPausa) {
		continuarTodas();
	}
	interrumpirTodas();
	for (int i = 0; i < numJugadores; ++i) {
		struct mCoreThread* h = &jugadores[i].hilo;
		mCoreThreadContinue(h);
		mCoreThreadEnd(h);
	}
#endif
	/* Si alguna consola quedó esperando al cable, no se puede unir: salir igualmente.
	 * La partida ya está en disco: mGBA escribe el .sav al guardar dentro del juego. */
	usleep(300000);
	_exit(0);
}
