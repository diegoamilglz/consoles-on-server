/* lrlink: una consola de libretro (pensado para parallel-n64) sin pantalla, para jugar desde el
 * navegador como el GBA online. Hace lo mismo que n64link, pero con un núcleo de libretro en vez
 * de con mupen64plus y sus plugins.
 *
 * Uso: lrlink <rom> <carpeta de la partida> 1 <carpeta de las tuberías>
 *   (los mismos argumentos que gbalink y n64link, para que servidor.py lo lance igual)
 *
 * Imagen: el núcleo dibuja con OpenGL en un framebuffer nuestro (EGL sobre GBM, sin pantalla), o
 * en memoria si usa angrylion; se lee, se escala a ANCHO x ALTO y va a p0.video (RGBX).
 * Sonido: mono, 16 bits, a FRECUENCIA_SALIDA Hz, a p0.audio.
 * Partida: la flashram del núcleo <-> jugador1.sav (lo que usa Pokémon Snap). Se mira una vez por
 * segundo y, si el juego ha guardado, se escribe jugador1.sav.
 *
 * Órdenes por la entrada estándar (como n64link):
 *   k <jugador> <botones> <x> <y>   botones de la N64 (como BUTTONS de mupen64plus) y stick (-80..80)
 *   vel <factor> · pausa <0|1> · sav <jugador> · stat · quit
 *
 * Variables de entorno:
 *   N64LINK_NUCLEO     el núcleo (de serie, parallel_n64_libretro.so junto al programa)
 *   N64LINK_OPCIONES   opciones del núcleo: "parallel-n64-gfxplugin=angrylion;parallel-n64-cpucore=..."
 *   N64LINK_DRI        la gráfica (de serie, /dev/dri/renderD128)
 *   N64LINK_SIN_GL     no usar la gráfica aunque la haya (solo vale con angrylion)
 *   N64LINK_DEPURAR    con "stat" escribe también lo que lee el juego del mando
 *   N64LINK_ESTADO / N64LINK_GUARDAR_EN   para probar: estado a cargar al poco de arrancar, y dónde
 *                      guardarlo al recibir SIGUSR1
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>

#include "libretro.h"

#define ANCHO 640
#define ALTO 480
#define FRECUENCIA_SALIDA 22050

static char rutaPartida[1024], carpetaTemporal[1024], rutaVideo[1024], rutaAudio[1024];

static long long ahoraNs(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* --- El núcleo --- */
static void* nucleo;
static void (*r_init)(void);
static void (*r_deinit)(void);
static void (*r_set_environment)(retro_environment_t);
static void (*r_set_video_refresh)(retro_video_refresh_t);
static void (*r_set_audio_sample)(retro_audio_sample_t);
static void (*r_set_audio_sample_batch)(retro_audio_sample_batch_t);
static void (*r_set_input_poll)(retro_input_poll_t);
static void (*r_set_input_state)(retro_input_state_t);
static void (*r_get_system_av_info)(struct retro_system_av_info*);
static bool (*r_load_game)(const struct retro_game_info*);
static void (*r_unload_game)(void);
static void (*r_run)(void);
static void (*r_reset)(void);
static size_t (*r_serialize_size)(void);
static bool (*r_serialize)(void*, size_t);
static bool (*r_unserialize)(const void*, size_t);
static void* (*r_get_memory_data)(unsigned);
static size_t (*r_get_memory_size)(unsigned);

static void* simbolo(const char* nombre) {
	void* p = dlsym(nucleo, nombre);
	if (!p) {
		fprintf(stderr, "al núcleo le falta %s\n", nombre);
		exit(1);
	}
	return p;
}

/* --- Opciones: las de serie y las de N64LINK_OPCIONES --- */
#define MAX_OPCIONES 64
static char opClave[MAX_OPCIONES][96], opValor[MAX_OPCIONES][96];
static int nOpciones;

static void ponerOpcion(const char* clave, const char* valor) {
	for (int i = 0; i < nOpciones; ++i) {
		if (!strcmp(opClave[i], clave)) {
			snprintf(opValor[i], sizeof(opValor[i]), "%s", valor);
			return;
		}
	}
	if (nOpciones < MAX_OPCIONES) {
		snprintf(opClave[nOpciones], sizeof(opClave[0]), "%s", clave);
		snprintf(opValor[nOpciones], sizeof(opValor[0]), "%s", valor);
		nOpciones++;
	}
}

static void leerOpciones(void) {
	/* De serie: 640x480, Glide64 y sin zona muerta (ya la pone el navegador) */
	ponerOpcion("parallel-n64-screensize", "640x480");
	ponerOpcion("parallel-n64-gfxplugin", "glide64");
	ponerOpcion("parallel-n64-rspplugin", "hle");
	ponerOpcion("parallel-n64-astick-deadzone", "0");
	ponerOpcion("parallel-n64-astick-sensitivity", "100");
	ponerOpcion("parallel-n64-pak1", "none");
	const char* extra = getenv("N64LINK_OPCIONES");
	if (extra) {
		char copia[2048];
		snprintf(copia, sizeof(copia), "%s", extra);
		for (char* trozo = strtok(copia, ";"); trozo; trozo = strtok(NULL, ";")) {
			char* igual = strchr(trozo, '=');
			if (igual) {
				*igual = 0;
				ponerOpcion(trozo, igual + 1);
				printf("opcion %s = %s\n", trozo, igual + 1);
			}
		}
	}
}

/* --- Mando --- */
static _Atomic uint32_t botones;  /* bits como BUTTONS de mupen64plus */
static _Atomic int ejeX, ejeY;
static _Atomic long lecturasMando;

/* BUTTONS de mupen64plus -> mando de libretro, con el mapeo de serie de parallel-n64:
 * A = B, B = Y, Z = L2, L = L, R = R, Start, cruceta; stick = analógico izquierdo, C = derecho */
static int16_t botonLibretro(unsigned id) {
	uint32_t b = botones;
	switch (id) {
	case RETRO_DEVICE_ID_JOYPAD_B: return !!(b & (1 << 7));      /* A */
	case RETRO_DEVICE_ID_JOYPAD_Y: return !!(b & (1 << 6));      /* B */
	case RETRO_DEVICE_ID_JOYPAD_L2: return !!(b & (1 << 5));     /* Z */
	case RETRO_DEVICE_ID_JOYPAD_START: return !!(b & (1 << 4));
	case RETRO_DEVICE_ID_JOYPAD_UP: return !!(b & (1 << 3));
	case RETRO_DEVICE_ID_JOYPAD_DOWN: return !!(b & (1 << 2));
	case RETRO_DEVICE_ID_JOYPAD_LEFT: return !!(b & (1 << 1));
	case RETRO_DEVICE_ID_JOYPAD_RIGHT: return !!(b & (1 << 0));
	case RETRO_DEVICE_ID_JOYPAD_R: return !!(b & (1 << 12));
	case RETRO_DEVICE_ID_JOYPAD_L: return !!(b & (1 << 13));
	}
	return 0;
}

static void entradaPoll(void) {}

static int16_t entradaEstado(unsigned puerto, unsigned dispositivo, unsigned indice, unsigned id) {
	if (puerto != 0) {
		return 0;
	}
	lecturasMando++;
	if (dispositivo == RETRO_DEVICE_JOYPAD) {
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
			int16_t m = 0;
			for (unsigned i = 0; i <= RETRO_DEVICE_ID_JOYPAD_R3; ++i) {
				if (botonLibretro(i)) m |= 1 << i;
			}
			return m;
		}
		return botonLibretro(id);
	}
	if (dispositivo == RETRO_DEVICE_ANALOG) {
		uint32_t b = botones;
		if (indice == RETRO_DEVICE_INDEX_ANALOG_LEFT) {
			/* -80..80 de la N64 a -32767..32767; en libretro, Y hacia abajo es positivo */
			if (id == RETRO_DEVICE_ID_ANALOG_X) return (int16_t) (ejeX * 32767 / 80);
			if (id == RETRO_DEVICE_ID_ANALOG_Y) return (int16_t) (-ejeY * 32767 / 80);
		}
		if (indice == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
			/* Los botones C, como el stick derecho a tope */
			if (id == RETRO_DEVICE_ID_ANALOG_X) return (b & (1 << 8) ? 32767 : 0) - (b & (1 << 9) ? 32767 : 0);
			if (id == RETRO_DEVICE_ID_ANALOG_Y) return (b & (1 << 10) ? 32767 : 0) - (b & (1 << 11) ? 32767 : 0);
		}
	}
	return 0;
}

/* --- Sonido: estéreo a la frecuencia del núcleo -> mono a FRECUENCIA_SALIDA --- */
static int fdAudio = -1;
static double frecuenciaNucleo = 44100;
static _Atomic int factorVelocidad = 100;  /* en %, para no acumular retraso acelerado */
static double posicion;                     /* posición fraccionaria del remuestreo */
static int16_t ultimaMuestra;

static void escribirSonido(const int16_t* m, int n) {
	if (fdAudio < 0) {
		fdAudio = open(rutaAudio, O_WRONLY | O_NONBLOCK);
		if (fdAudio < 0) return;
	}
	for (int i = 0; i < n; i += 1024) {
		int k = n - i < 1024 ? n - i : 1024;
		ssize_t r = write(fdAudio, m + i, k * 2);
		(void) r;
	}
}

static size_t sonidoLote(const int16_t* datos, size_t marcos) {
	static int16_t salida[8192];
	int n = 0;
	double paso = frecuenciaNucleo * factorVelocidad / 100.0 / FRECUENCIA_SALIDA;
	while (posicion < marcos && n < 8192) {
		size_t i = (size_t) posicion;
		double f = posicion - i;
		int16_t a = i == 0 && f < 0 ? ultimaMuestra : (int16_t) ((datos[2 * i] + datos[2 * i + 1]) / 2);
		int16_t b = i + 1 < marcos ? (int16_t) ((datos[2 * i + 2] + datos[2 * i + 3]) / 2) : a;
		salida[n++] = (int16_t) (a + (b - a) * f);
		posicion += paso;
	}
	posicion -= marcos;
	if (marcos) ultimaMuestra = (int16_t) ((datos[2 * marcos - 2] + datos[2 * marcos - 1]) / 2);
	escribirSonido(salida, n);
	return marcos;
}

static void sonidoMuestra(int16_t i, int16_t d) {
	int16_t par[2] = { i, d };
	sonidoLote(par, 1);
}

/* --- Imagen --- */
static int fdDri = -1;
static struct gbm_device* gbm;
static EGLDisplay pantalla = EGL_NO_DISPLAY;
static EGLContext contexto = EGL_NO_CONTEXT;
static struct retro_hw_render_callback hw;
static bool usaGl;
static unsigned fbo, anchoMax = ANCHO, altoMax = ALTO;

static void (*glGenFramebuffersF)(int, unsigned*);
static void (*glBindFramebufferF)(unsigned, unsigned);
static void (*glGenRenderbuffersF)(int, unsigned*);
static void (*glBindRenderbufferF)(unsigned, unsigned);
static void (*glRenderbufferStorageF)(unsigned, unsigned, int, int);
static void (*glFramebufferRenderbufferF)(unsigned, unsigned, unsigned, unsigned);
static unsigned (*glCheckFramebufferStatusF)(unsigned);
static void (*glReadPixelsF)(int, int, int, int, unsigned, unsigned, void*);
static void (*glPixelStoreiF)(unsigned, int);

#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_RENDERBUFFER 0x8D41
#define GL_RGBA8 0x8058
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_PACK_ALIGNMENT 0x0D05

static uintptr_t fboActual(void) { return fbo; }
static retro_proc_address_t procGl(const char* nombre) { return (retro_proc_address_t) eglGetProcAddress(nombre); }

static bool abrirEgl(void) {
	const char* rutas[] = { getenv("N64LINK_DRI"), "/dev/dri/renderD128", "/dev/dri/card0" };
	for (unsigned i = 0; i < 3 && fdDri < 0; ++i) {
		if (rutas[i]) fdDri = open(rutas[i], O_RDWR | O_CLOEXEC);
	}
	if (fdDri < 0) {
		fprintf(stderr, "no se puede abrir la gráfica (/dev/dri)\n");
		return false;
	}
	gbm = gbm_create_device(fdDri);
	PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
	pantalla = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_GBM_MESA, gbm, NULL) : eglGetDisplay((EGLNativeDisplayType) gbm);
	EGLint mayor, menor;
	if (pantalla == EGL_NO_DISPLAY || !eglInitialize(pantalla, &mayor, &menor)) {
		fprintf(stderr, "EGL no arranca\n");
		return false;
	}
	return eglBindAPI(EGL_OPENGL_API);
}

/* ¿Se puede usar la gráfica? Si no (el servicio no la ve), se le niega el OpenGL al núcleo, que con
 * angrylion dibuja igual, en memoria. N64LINK_SIN_GL lo fuerza. */
static bool hayGrafica(void) {
	if (getenv("N64LINK_SIN_GL")) return false;
	const char* rutas[] = { getenv("N64LINK_DRI"), "/dev/dri/renderD128", "/dev/dri/card0" };
	for (unsigned i = 0; i < 3; ++i) {
		if (rutas[i] && access(rutas[i], R_OK | W_OK) == 0) return true;
	}
	return false;
}

/* El contexto, con lo que haya pedido el núcleo, y un framebuffer del tamaño máximo sin superficie */
static bool crearContexto(void) {
	EGLint atributos[16], n = 0;
	if (hw.context_type == RETRO_HW_CONTEXT_OPENGL_CORE) {
		atributos[n++] = EGL_CONTEXT_MAJOR_VERSION; atributos[n++] = hw.version_major ? (EGLint) hw.version_major : 3;
		atributos[n++] = EGL_CONTEXT_MINOR_VERSION; atributos[n++] = (EGLint) hw.version_minor;
		atributos[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK; atributos[n++] = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT;
	}
	atributos[n++] = EGL_NONE;
	contexto = eglCreateContext(pantalla, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, atributos);
	if (contexto == EGL_NO_CONTEXT || !eglMakeCurrent(pantalla, EGL_NO_SURFACE, EGL_NO_SURFACE, contexto)) {
		fprintf(stderr, "no se pudo crear el contexto OpenGL (error 0x%x)\n", eglGetError());
		return false;
	}
	glGenFramebuffersF = (void*) eglGetProcAddress("glGenFramebuffers");
	glBindFramebufferF = (void*) eglGetProcAddress("glBindFramebuffer");
	glGenRenderbuffersF = (void*) eglGetProcAddress("glGenRenderbuffers");
	glBindRenderbufferF = (void*) eglGetProcAddress("glBindRenderbuffer");
	glRenderbufferStorageF = (void*) eglGetProcAddress("glRenderbufferStorage");
	glFramebufferRenderbufferF = (void*) eglGetProcAddress("glFramebufferRenderbuffer");
	glCheckFramebufferStatusF = (void*) eglGetProcAddress("glCheckFramebufferStatus");
	glReadPixelsF = (void*) eglGetProcAddress("glReadPixels");
	glPixelStoreiF = (void*) eglGetProcAddress("glPixelStorei");
	unsigned rb[2];
	glGenFramebuffersF(1, &fbo);
	glBindFramebufferF(GL_FRAMEBUFFER, fbo);
	glGenRenderbuffersF(2, rb);
	glBindRenderbufferF(GL_RENDERBUFFER, rb[0]);
	glRenderbufferStorageF(GL_RENDERBUFFER, GL_RGBA8, (int) anchoMax, (int) altoMax);
	glFramebufferRenderbufferF(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
	glBindRenderbufferF(GL_RENDERBUFFER, rb[1]);
	glRenderbufferStorageF(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (int) anchoMax, (int) altoMax);
	glFramebufferRenderbufferF(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
	if (glCheckFramebufferStatusF(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "el framebuffer no vale\n");
		return false;
	}
	const char* (*glGetStringF)(unsigned) = (void*) eglGetProcAddress("glGetString");
	printf("OpenGL: %s, %s (%ux%u)\n", glGetStringF(0x1F01), glGetStringF(0x1F02), anchoMax, altoMax);
	return true;
}

static uint8_t* lectura;
static uint8_t* buzon;
static bool buzonNuevo;
static pthread_mutex_t mutexBuzon = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condBuzon = PTHREAD_COND_INITIALIZER;
static _Atomic long fotogramas;

static void* hiloVideo(void* arg) {
	(void) arg;
	uint8_t* copia = malloc(ANCHO * ALTO * 4);
	int fd = open(rutaVideo, O_WRONLY);
	if (fd < 0) {
		perror(rutaVideo);
		return NULL;
	}
	for (;;) {
		pthread_mutex_lock(&mutexBuzon);
		while (!buzonNuevo) pthread_cond_wait(&condBuzon, &mutexBuzon);
		memcpy(copia, buzon, ANCHO * ALTO * 4);
		buzonNuevo = false;
		pthread_mutex_unlock(&mutexBuzon);
		size_t hecho = 0;
		while (hecho < ANCHO * ALTO * 4) {
			ssize_t n = write(fd, copia + hecho, ANCHO * ALTO * 4 - hecho);
			if (n <= 0) {
				if (n < 0 && errno == EINTR) continue;
				close(fd);
				return NULL;
			}
			hecho += n;
		}
	}
}

/* Escala (vecino más próximo) una imagen RGBX a ANCHO x ALTO y la deja en el buzón */
static void publicar(const uint8_t* rgbx, unsigned w, unsigned h, size_t paso, bool alReves) {
	pthread_mutex_lock(&mutexBuzon);
	for (unsigned y = 0; y < ALTO; ++y) {
		unsigned sy = y * h / ALTO;
		if (alReves) sy = h - 1 - sy;
		const uint32_t* fila = (const uint32_t*) (rgbx + sy * paso);
		uint32_t* destino = (uint32_t*) (buzon + y * ANCHO * 4);
		for (unsigned x = 0; x < ANCHO; ++x) destino[x] = fila[x * w / ANCHO];
	}
	buzonNuevo = true;
	pthread_cond_signal(&condBuzon);
	pthread_mutex_unlock(&mutexBuzon);
}

static void videoRefresco(const void* datos, unsigned w, unsigned h, size_t paso) {
	if (!datos || !w || !h) {
		return; /* imagen repetida */
	}
	fotogramas++;
	if (datos == RETRO_HW_FRAME_BUFFER_VALID) {
		if (w > anchoMax) w = anchoMax;
		if (h > altoMax) h = altoMax;
		glBindFramebufferF(GL_READ_FRAMEBUFFER, fbo);
		glPixelStoreiF(GL_PACK_ALIGNMENT, 4);
		glReadPixelsF(0, 0, (int) w, (int) h, GL_RGBA, GL_UNSIGNED_BYTE, lectura);
		publicar(lectura, w, h, w * 4, !hw.bottom_left_origin ? false : true);
		return;
	}
	/* Por software (angrylion): XRGB8888, es decir B G R X en memoria -> R G B X */
	for (unsigned y = 0; y < h && y < altoMax; ++y) {
		const uint8_t* o = (const uint8_t*) datos + y * paso;
		uint8_t* d = lectura + y * w * 4;
		for (unsigned x = 0; x < w; ++x) {
			d[4 * x] = o[4 * x + 2];
			d[4 * x + 1] = o[4 * x + 1];
			d[4 * x + 2] = o[4 * x];
			d[4 * x + 3] = 0;
		}
	}
	publicar(lectura, w, h, w * 4, false);
}

/* --- Entorno de libretro: lo que el núcleo pide al programa --- */
static void registro(enum retro_log_level nivel, const char* formato, ...) {
	if (nivel < RETRO_LOG_WARN && !getenv("N64LINK_DEPURAR")) return;
	va_list a;
	va_start(a, formato);
	vfprintf(stderr, formato, a);
	va_end(a);
}

static struct retro_system_av_info av;

static bool entorno(unsigned orden, void* datos) {
	switch (orden) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback*) datos)->log = registro;
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
		*(const char**) datos = carpetaTemporal;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		return *(enum retro_pixel_format*) datos == RETRO_PIXEL_FORMAT_XRGB8888;
	case RETRO_ENVIRONMENT_SET_HW_RENDER: {
		struct retro_hw_render_callback* cb = datos;
		if (cb->context_type != RETRO_HW_CONTEXT_OPENGL && cb->context_type != RETRO_HW_CONTEXT_OPENGL_CORE) {
			return false; /* Vulkan, no */
		}
		if (!hayGrafica()) {
			fprintf(stderr, "sin gráfica: el núcleo dibuja en memoria\n");
			return false;
		}
		cb->get_current_framebuffer = fboActual;
		cb->get_proc_address = procGl;
		hw = *cb;
		usaGl = true;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
		*(unsigned*) datos = RETRO_HW_CONTEXT_OPENGL;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable* v = datos;
		v->value = NULL;
		for (int i = 0; i < nOpciones; ++i) {
			if (!strcmp(opClave[i], v->key)) {
				v->value = opValor[i];
				return true;
			}
		}
		return false;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool*) datos = false;
		return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned*) datos = 0;  /* opciones a la antigua: basta con GET_VARIABLE */
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		if (datos) *(int*) datos = 3;
		return true;
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		av = *(struct retro_system_av_info*) datos;
		if (av.timing.sample_rate > 0) frecuenciaNucleo = av.timing.sample_rate;
		return true;
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
	case RETRO_ENVIRONMENT_SET_VARIABLES:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
	case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
	case RETRO_ENVIRONMENT_SET_MESSAGE:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
		return true;
	}
	return false;
}

/* --- Partida: la flashram del núcleo <-> jugador1.sav --- */
#define DESPL_FLASH (0x800 + 4 * 0x8000 + 0x8000)  /* eeprom, mempacks y sram van antes (libretro_memory.h) */
#define TAM_FLASH 0x20000
static uint8_t copiaFlash[TAM_FLASH];

static uint8_t* flashDelNucleo(void) {
	uint8_t* m = r_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t t = r_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	return m && t >= DESPL_FLASH + TAM_FLASH ? m + DESPL_FLASH : NULL;
}

static void cargarPartida(void) {
	uint8_t* flash = flashDelNucleo();
	if (!flash) return;
	memset(flash, 0xFF, TAM_FLASH);  /* flash borrada */
	FILE* f = fopen(rutaPartida, "rb");
	if (f) {
		size_t n = fread(flash, 1, TAM_FLASH, f);
		fclose(f);
		printf("partida cargada (%zu bytes)\n", n);
	}
	memcpy(copiaFlash, flash, TAM_FLASH);
}

static void devolverPartida(void) {
	uint8_t* flash = flashDelNucleo();
	if (!flash || !memcmp(flash, copiaFlash, TAM_FLASH)) return;
	memcpy(copiaFlash, flash, TAM_FLASH);
	char tmp[1100];
	snprintf(tmp, sizeof(tmp), "%s.tmp", rutaPartida);
	FILE* f = fopen(tmp, "wb");
	if (!f) return;
	bool bien = fwrite(copiaFlash, 1, TAM_FLASH, f) == TAM_FLASH;
	bien = !fclose(f) && bien;
	if (bien && !rename(tmp, rutaPartida)) printf("partida guardada\n");
}

/* --- Estados (para probar) --- */
static volatile sig_atomic_t pedirEstado;
static void alSenal(int s) { (void) s; pedirEstado = 1; }

static void guardarEstado(const char* ruta) {
	size_t t = r_serialize_size();
	void* d = malloc(t);
	if (d && r_serialize(d, t)) {
		FILE* f = fopen(ruta, "wb");
		if (f) {
			fwrite(d, 1, t, f);
			fclose(f);
			printf("ss %s\n", ruta);
		}
	}
	free(d);
}

static void cargarEstado(const char* ruta) {
	FILE* f = fopen(ruta, "rb");
	if (!f) return;
	fseek(f, 0, SEEK_END);
	long t = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* d = malloc(t);
	if (fread(d, 1, t, f) == (size_t) t && r_unserialize(d, t)) printf("ls %s\n", ruta);
	else printf("ls fallo %s\n", ruta);
	fclose(f);
	free(d);
}

/* --- Órdenes, en su hilo --- */
static _Atomic bool enPausa, reiniciar, salir, pedirStat;

static void* hiloOrdenes(void* arg) {
	(void) arg;
	char linea[256];
	while (fgets(linea, sizeof(linea), stdin)) {
		int j, m, x, y;
		double v;
		if (sscanf(linea, "k %d %d %d %d", &j, &m, &x, &y) == 4) {
			if (j == 0) {
				botones = (uint32_t) m & 0xFFFF;
				ejeX = x < -80 ? -80 : x > 80 ? 80 : x;
				ejeY = y < -80 ? -80 : y > 80 ? 80 : y;
			}
		} else if (sscanf(linea, "k %d %d", &j, &m) == 2) {
			if (j == 0) botones = (uint32_t) m & 0xFFFF;
		} else if (sscanf(linea, "vel %lf", &v) == 1) {
			if (v >= 0.5 && v <= 3) {
				factorVelocidad = (int) (v * 100 + 0.5);
				printf("vel %.2f\n", v);
			}
		} else if (sscanf(linea, "pausa %d", &m) == 1) {
			enPausa = !!m;
			printf("pausa %d\n", m ? 1 : 0);
		} else if (sscanf(linea, "sav %d", &j) == 1) {
			if (j == 0) reiniciar = true;
		} else if (!strncmp(linea, "stat", 4)) {
			pedirStat = true;
		} else if (!strncmp(linea, "quit", 4)) {
			break;
		}
	}
	salir = true;
	return NULL;
}

int main(int argc, char** argv) {
	if (argc < 5) {
		fprintf(stderr, "uso: %s <rom> <carpeta de la partida> 1 <carpeta de las tuberías>\n", argv[0]);
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	signal(SIGUSR1, alSenal);
	setvbuf(stdout, NULL, _IOLBF, 0);
	snprintf(rutaPartida, sizeof(rutaPartida), "%s/jugador1.sav", argv[2]);
	snprintf(rutaVideo, sizeof(rutaVideo), "%s/p0.video", argv[4]);
	snprintf(rutaAudio, sizeof(rutaAudio), "%s/p0.audio", argv[4]);
	snprintf(carpetaTemporal, sizeof(carpetaTemporal), "%s/lrlink-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	if (!mkdtemp(carpetaTemporal)) {
		perror("mkdtemp");
		return 1;
	}

	/* El núcleo: el de N64LINK_NUCLEO, o parallel_n64_libretro.so junto a este programa */
	char rutaNucleo[1100];
	if (getenv("N64LINK_NUCLEO")) {
		snprintf(rutaNucleo, sizeof(rutaNucleo), "%s", getenv("N64LINK_NUCLEO"));
	} else {
		char yo[1024] = "";
		ssize_t n = readlink("/proc/self/exe", yo, sizeof(yo) - 1);
		if (n > 0) yo[n] = 0;
		char* barra = strrchr(yo, '/');
		if (barra) barra[1] = 0;
		snprintf(rutaNucleo, sizeof(rutaNucleo), "%sparallel_n64_libretro.so", yo);
	}
	nucleo = dlopen(rutaNucleo, RTLD_NOW | RTLD_LOCAL);
	if (!nucleo) {
		fprintf(stderr, "no se pudo cargar el núcleo %s: %s\n", rutaNucleo, dlerror());
		return 1;
	}
	r_init = simbolo("retro_init");
	r_deinit = simbolo("retro_deinit");
	r_set_environment = simbolo("retro_set_environment");
	r_set_video_refresh = simbolo("retro_set_video_refresh");
	r_set_audio_sample = simbolo("retro_set_audio_sample");
	r_set_audio_sample_batch = simbolo("retro_set_audio_sample_batch");
	r_set_input_poll = simbolo("retro_set_input_poll");
	r_set_input_state = simbolo("retro_set_input_state");
	r_get_system_av_info = simbolo("retro_get_system_av_info");
	r_load_game = simbolo("retro_load_game");
	r_unload_game = simbolo("retro_unload_game");
	r_run = simbolo("retro_run");
	r_reset = simbolo("retro_reset");
	r_serialize_size = simbolo("retro_serialize_size");
	r_serialize = simbolo("retro_serialize");
	r_unserialize = simbolo("retro_unserialize");
	r_get_memory_data = simbolo("retro_get_memory_data");
	r_get_memory_size = simbolo("retro_get_memory_size");

	leerOpciones();
	/* Sin gráfica se sigue: al núcleo se le negará el OpenGL y, con angrylion, dibuja en memoria */
	if (hayGrafica() && !abrirEgl()) return 1;
	r_set_environment(entorno);
	r_init();
	r_set_video_refresh(videoRefresco);
	r_set_audio_sample(sonidoMuestra);
	r_set_audio_sample_batch(sonidoLote);
	r_set_input_poll(entradaPoll);
	r_set_input_state(entradaEstado);

	/* La ROM, en memoria */
	FILE* f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	long tam = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* datosRom = malloc(tam);
	if (fread(datosRom, 1, tam, f) != (size_t) tam) {
		fprintf(stderr, "no se pudo leer la ROM\n");
		return 1;
	}
	fclose(f);
	struct retro_game_info juego = { .path = argv[1], .data = datosRom, .size = (size_t) tam, .meta = NULL };
	if (!r_load_game(&juego)) {
		fprintf(stderr, "el núcleo no carga la ROM\n");
		return 1;
	}
	r_get_system_av_info(&av);
	if (av.timing.sample_rate > 0) frecuenciaNucleo = av.timing.sample_rate;
	if (av.geometry.max_width > anchoMax) anchoMax = av.geometry.max_width;
	if (av.geometry.max_height > altoMax) altoMax = av.geometry.max_height;
	lectura = malloc((size_t) anchoMax * altoMax * 4);
	buzon = calloc(ANCHO * ALTO, 4);
	if (usaGl) {
		if (!crearContexto()) return 1;
		if (hw.context_reset) hw.context_reset();
	}
	cargarPartida();
	printf("listo jugadores=1 (%.2f imagenes/s, sonido a %.0f Hz)\n", av.timing.fps, frecuenciaNucleo);

	pthread_t t;
	pthread_create(&t, NULL, hiloVideo, NULL);
	pthread_detach(t);
	pthread_create(&t, NULL, hiloOrdenes, NULL);
	pthread_detach(t);

	double fps = av.timing.fps > 1 ? av.timing.fps : 60;
	long long siguiente = ahoraNs(), ultimoStat = ahoraNs(), ultimoGuardado = ahoraNs();
	long fotogramasPrevios = 0, vueltas = 0;
	while (!salir) {
		if (pedirStat) {
			pedirStat = false;
			long long ahora = ahoraNs();
			long f2 = fotogramas;
			printf("fps %.1f\n", (f2 - fotogramasPrevios) / ((ahora - ultimoStat) / 1e9));
			if (getenv("N64LINK_DEPURAR")) printf("mando lecturas=%ld botones=0x%04x x=%d y=%d\n", (long) lecturasMando, (unsigned) botones, (int) ejeX, (int) ejeY);
			ultimoStat = ahora;
			fotogramasPrevios = f2;
		}
		if (reiniciar) {
			/* El servidor ha dejado otra partida en jugador1.sav: como cambiar de cartucho */
			reiniciar = false;
			r_reset();
			cargarPartida();
			printf("sav 0 ok\n");
		}
		if (pedirEstado && getenv("N64LINK_GUARDAR_EN")) {
			pedirEstado = 0;
			guardarEstado(getenv("N64LINK_GUARDAR_EN"));
		}
		if (enPausa) {
			usleep(20000);
			siguiente = ahoraNs();
			continue;
		}
		r_run();
		if (++vueltas == 100 && getenv("N64LINK_ESTADO")) cargarEstado(getenv("N64LINK_ESTADO"));
		long long ahora = ahoraNs();
		if (ahora - ultimoGuardado > 1000000000LL) {
			ultimoGuardado = ahora;
			devolverPartida();
		}
		/* Al ritmo de la consola (o más rápido, con vel) */
		siguiente += (long long) (1e9 / fps * 100 / factorVelocidad);
		if (siguiente > ahora) {
			struct timespec espera = { (siguiente - ahora) / 1000000000LL, (siguiente - ahora) % 1000000000LL };
			nanosleep(&espera, NULL);
		} else if (ahora - siguiente > 200000000LL) {
			siguiente = ahora;  /* si va lento, sin intentar recuperar lo perdido */
		}
	}
	devolverPartida();
	r_unload_game();
	r_deinit();
	char orden[1200];
	snprintf(orden, sizeof(orden), "rm -rf '%s'", carpetaTemporal);
	if (system(orden)) {}
	return 0;
}
