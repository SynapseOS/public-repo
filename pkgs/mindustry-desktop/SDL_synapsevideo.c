/* SynapseOS SDL2 video backend. Uses Mesa OSMesa and the kernel's framebuffer
 * ownership/input ABI. This replaces SDL's offscreen backend in our build.
 * SPDX-License-Identifier: Zlib */
// SDL_synapsevideo.c: экран, ввод и показатели игры SynapseOS
// Архитектура: x86_64, ABI: SDL2, musl и SynAPI
// Подсистема: графический клиент
// Первое изменение: 13.10.2026, последнее изменение: 31.10.2026
// Статус доступа: общедоступный, лицензия Zlib указана выше
#include "../../SDL_internal.h"
#ifdef SDL_VIDEO_DRIVER_OFFSCREEN
#include "../SDL_sysvideo.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_windowevents_c.h"
#include "SDL_timer.h"
#include <stdio.h>
#include <GL/osmesa.h>
#include <GL/glext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <lumine/pointer.h>
#include "lumine_fb.h"
#include "lumine_hid.h"

// Дополнительные коды SynAPI сохраняют физические клавиши и размер события
#define SYN_LAYOUT_EN 0x100
#define SYN_LAYOUT_RU 0x101
#define SYN_STATE_RUSSIAN (1u << 8)

typedef struct {
	struct SynapseWayland *wayland;
	int fb, pointer;
	unsigned char *screen;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	SDL_Window *window;
	unsigned char *pixels;
	int w, h, mx, my, ox, oy, interval;
	Uint32 buttons;
	Uint32 metric_start, metric_frames;
	Uint64 metric_render, metric_copy, metric_present;
	Uint64 metric_finish, metric_wait, metric_fill, metric_decor, metric_commit;
	Uint64 metric_gl_state, metric_gl_fence_wait, metric_gl_readback, metric_gl_restore, metric_gl_flip;
	Uint64 metric_pbo_enqueue, metric_pbo_wait, metric_pbo_map;
	unsigned metric_pbo_frames, metric_pbo_fallbacks;
	unsigned metric_gl_fence_frames, metric_gl_fence_failures;
	unsigned egl_trace_count;
	Uint32 egl_sample_time, egl_sample_hash;
	struct SynapseGLContext *gl_context;
	unsigned char *hud_font;
	Uint32 hud_width, hud_height, hud_bytes, hud_stride;
	Uint16 hud_glyph[128];
	Uint32 hud_time, hud_frames;
	Uint64 hud_busy, hud_idle, hud_rx, hud_tx;
	SDL_bool hud_cpu_valid, hud_net_valid, russian;
	char hud_line[2][128];
} SynapseVideo;

typedef struct SynapseGLContext {
	OSMesaContext osmesa;
	EGLDisplay display;
	EGLContext context;
	EGLSurface surface;
	EGLConfig config;
	struct gbm_device *gbm;
	struct gbm_surface *gbm_surface;
	unsigned char *readback;
	GLuint pbo[2];
	GLsync pbo_fence[2];
	unsigned pbo_next;
	SDL_bool pbo_active, pbo_failed;
	int render_fd;
	SDL_bool egl;
} SynapseGLContext;

static void hud_draw(SynapseVideo *s);
static int syn_egl_resize(SynapseVideo *video, int width, int height);
#include "synapse-wayland.inc"

// Загрузить ограниченный шрифт PSF2 и соответствие печатных символов ASCII
static void hud_font_load(SynapseVideo *s) {
	const char *enabled = SDL_getenv("SYNAPSE_HUD");
	FILE *file;
	Uint32 header[8];
	size_t size;

	if (enabled != NULL && SDL_strcmp(enabled, "0") == 0) {
		return;
	}

	file = fopen("/assets/fonts/dejavu-mono.psf", "rb");

	if (file == NULL) {
		file = fopen("/usr/share/sdl2/dejavu-mono.psf", "rb");
	}

	if (file == NULL) {
		SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Synapse HUD font unavailable");
		return;
	}

	if (fread(header, sizeof(header), 1, file) != 1 || header[0] != 0x864ab572U || header[2] < 32 ||
	    header[2] > 4096 || header[4] == 0 || header[4] > 4096 || header[6] == 0 ||
	    header[6] > 32 || header[7] == 0 || header[7] > 16 ||
	    header[5] < ((header[7] + 7) / 8) * header[6] || header[5] > 128) {
		fclose(file);
		return;
	}

	size = (size_t)header[4] * header[5];
	s->hud_font = SDL_malloc(size);

	if (s->hud_font == NULL || fseek(file, (long)header[2], SEEK_SET) != 0 ||
	    fread(s->hud_font, 1, size, file) != size) {
		SDL_free(s->hud_font);
		s->hud_font = NULL;
		fclose(file);
		return;
	}

	s->hud_width = header[7];
	s->hud_height = header[6];
	s->hud_bytes = header[5];
	s->hud_stride = (header[7] + 7) / 8;

	for (unsigned c = 0; c < SDL_arraysize(s->hud_glyph); c++) {
		s->hud_glyph[c] = c < header[4] ? (Uint16)c : 0;
	}

	if ((header[3] & 1U) != 0) {
		unsigned glyph = 0;
		SDL_bool sequence = SDL_FALSE;

		SDL_memset(s->hud_glyph, 0, sizeof(s->hud_glyph));

		for (size_t i = 0; i < 1048576 && glyph < header[4]; i++) {
			int byte = fgetc(file);

			if (byte == EOF) {
				break;
			}

			if (byte == 255) {
				glyph++;
				sequence = SDL_FALSE;
			} else if (byte == 254) {
				sequence = SDL_TRUE;
			} else if (byte >= 32 && byte < 127 && !sequence) {
				s->hud_glyph[byte] = (Uint16)glyph;
			}
		}
	}

	fclose(file);
}

// Прочитать накопленное рабочее и свободное время всех процессоров
static SDL_bool hud_cpu(Uint64 *busy, Uint64 *idle) {
	FILE *file = fopen("/proc/performance", "r");
	char line[192];
	unsigned count = 0;

	if (file == NULL) {
		return SDL_FALSE;
	}

	*busy = 0;
	*idle = 0;

	while (fgets(line, sizeof(line), file) != NULL) {
		unsigned long long work, rest;

		if (sscanf(line, "cpu %*u %llu %llu", &work, &rest) == 2) {
			*busy += work;
			*idle += rest;
			count++;
		}
	}

	fclose(file);
	return count > 0 ? SDL_TRUE : SDL_FALSE;
}

// Снимать счётчики раз в секунду и сохранять текст для последующих кадров
static void hud_sample(SynapseVideo *s) {
	Uint32 now = SDL_GetTicks( );
	Uint32 elapsed = now - s->hud_time;
	char memory[56] = "RAM n/a";
	char cpu[32] = "CPU n/a";
	char network[72] = "RX n/a TX n/a";
	struct sysinfo info;
	Uint64 busy, idle;
	SDL_bool valid;
	long rx, tx;
	double fps;

	s->hud_frames++;

	if (s->hud_time != 0 && elapsed < 1000) {
		return;
	}

	if (sysinfo(&info) == 0 && info.totalram > 0 && info.mem_unit > 0 &&
	    info.freeram <= info.totalram) {
		Uint64 total = (Uint64)info.totalram * info.mem_unit;
		Uint64 used = ((Uint64)info.totalram - info.freeram) * info.mem_unit;
		SDL_snprintf(memory, sizeof(memory), "RAM %llu/%llu MiB (%u%%)",
		             (unsigned long long)(used / 1048576), (unsigned long long)(total / 1048576),
		             (unsigned)(100 * used / total));
	}

	valid = hud_cpu(&busy, &idle);

	if (valid && s->hud_cpu_valid && busy >= s->hud_busy && idle >= s->hud_idle) {
		Uint64 work = busy - s->hud_busy;
		Uint64 sum = work + idle - s->hud_idle;

		if (sum > 0) {
			SDL_snprintf(cpu, sizeof(cpu), "CPU %.1f%%", 100.0 * work / sum);
		}
	}

	s->hud_cpu_valid = valid;

	if (valid) {
		s->hud_busy = busy;
		s->hud_idle = idle;
	}

	rx = syscall(106008);
	tx = syscall(106007);

	if (rx >= 0 && tx >= 0 && s->hud_net_valid && elapsed > 0 && (Uint64)rx >= s->hud_rx &&
	    (Uint64)tx >= s->hud_tx) {
		double scale = 1000.0 / elapsed / 1024.0;
		SDL_snprintf(network, sizeof(network), "RX %.1f KiB/s TX %.1f KiB/s",
		             ((Uint64)rx - s->hud_rx) * scale, ((Uint64)tx - s->hud_tx) * scale);
	}

	s->hud_net_valid = rx >= 0 && tx >= 0 ? SDL_TRUE : SDL_FALSE;

	if (s->hud_net_valid) {
		s->hud_rx = (Uint64)rx;
		s->hud_tx = (Uint64)tx;
	}

	fps = s->hud_time != 0 && elapsed > 0 ? s->hud_frames * 1000.0 / elapsed : 0;
	SDL_snprintf(s->hud_line[0], sizeof(s->hud_line[0]), "%s | %s | FPS %.1f | %s", memory, cpu,
	             fps, s->russian ? "RU" : "EN");
	SDL_snprintf(s->hud_line[1], sizeof(s->hud_line[1]), "%s", network);
	s->hud_frames = 0;
	s->hud_time = now;
}

// Рисовать показатели поверх готового кадра, не меняя буфер Mesa
static void hud_draw(SynapseVideo *s) {
	if (s->hud_font == NULL) {
		return;
	}

	hud_sample(s);

	for (unsigned row = 0; row < 2; row++) {
		unsigned y0 = row * s->hud_height;

		for (unsigned gy = 0; gy < s->hud_height && y0 + gy < s->var.yres; gy++) {
			Uint32 *dest = (Uint32 *)(s->screen + (y0 + gy) * s->fix.line_length);

			for (unsigned x = 0; x < s->var.xres; x++) {
				dest[x] = 0x081018;
			}

			for (unsigned i = 0; s->hud_line[row][i] != 0 && (i + 1) * s->hud_width <= s->var.xres;
			     i++) {
				unsigned ch = (unsigned char)s->hud_line[row][i];
				unsigned glyph = ch < 128 ? s->hud_glyph[ch] : s->hud_glyph['?'];
				const unsigned char *bits = s->hud_font + glyph * s->hud_bytes + gy * s->hud_stride;

				for (unsigned gx = 0; gx < s->hud_width; gx++) {
					if ((bits[gx / 8] & (0x80U >> (gx % 8))) != 0) {
						dest[i * s->hud_width + gx] = 0x55ffff;
					}
				}
			}
		}
	}
}

// Подготовить framebuffer, шрифт и устройства ввода
static int video_init(_THIS) {
	SynapseVideo *s = _this->driverdata;

	struct {
		unsigned x, y, w, h;
	} clear;

	SDL_DisplayMode mode;
	kbd_event_t e;
	// Оставить процессорное время Java и звуку при обычном запуске JAR
	if (SDL_getenv("LP_NUM_THREADS") == NULL && SDL_setenv("LP_NUM_THREADS", "2", 0) != 0) {
		return SDL_SetError("Cannot configure Mesa worker threads");
	}

	if (SDL_getenv("WAYLAND_DISPLAY") != NULL) {
		int result = syn_wl_init(_this);

		if (result == 0) {
			hud_font_load(s);
		}

		return result;
	}

	s->russian = (syscall(102001) & SYN_STATE_RUSSIAN) != 0;
	s->fb = open("/dev/fb0", O_RDWR);

	if (s->fb < 0 || ioctl(s->fb, 0x4600, &s->var) || ioctl(s->fb, 0x4602, &s->fix))
		return SDL_SetError("Synapse framebuffer unavailable");
	if (s->var.bits_per_pixel != 32 || s->var.red.offset != 16 || s->var.green.offset != 8 ||
	    s->var.blue.offset != 0)
		return SDL_SetError("Synapse requires XRGB8888 framebuffer");
	s->screen =
	    mmap(NULL, s->fix.line_length * s->var.yres, PROT_READ | PROT_WRITE, MAP_SHARED, s->fb, 0);
	if (s->screen == MAP_FAILED) {
		s->screen = NULL;
		return SDL_SetError("Framebuffer mmap failed");
	}

	if (ioctl(s->fb, 0x4626, 0))
		return SDL_SetError("Framebuffer busy: close ClearUI before starting the game");
	SDL_memset(s->screen, 0, s->fix.line_length * s->var.yres);
	clear.x = 0;
	clear.y = 0;
	clear.w = s->var.xres;
	clear.h = s->var.yres;
	ioctl(s->fb, 0x4621, &clear);
	ioctl(s->fb, 0x4622, 0);
	hud_font_load(s);
	s->pointer = open("/dev/pointers", O_RDONLY | O_NONBLOCK);
	SDL_zero(mode);
	mode.format = SDL_PIXELFORMAT_RGB888;
	mode.w = s->var.xres;
	mode.h = s->var.yres;
	mode.refresh_rate = 60;

	if (SDL_AddBasicVideoDisplay(&mode) < 0)
		return -1;
	SDL_AddDisplayMode(&_this->displays[0], &mode);

	while (syscall(102000, &e) == 0) {
	}

	s->russian = (syscall(102001) & SYN_STATE_RUSSIAN) != 0;
	return 0;
}

static void video_quit(_THIS) {
	SynapseVideo *s = _this->driverdata;
	SDL_free(s->hud_font);
	s->hud_font = NULL;

	if (s->wayland != NULL) {
		syn_wl_quit(s);
		return;
	}

	if (s->pointer >= 0)
		close(s->pointer);
	if (s->screen)
		munmap(s->screen, s->fix.line_length * s->var.yres);
	if (s->fb >= 0) {
		ioctl(s->fb, 0x4627, 0);
		close(s->fb);
	}

	s->fb = s->pointer = -1;
	s->screen = NULL;
}

static int set_mode(_THIS, SDL_VideoDisplay *d, SDL_DisplayMode *m) {
	return 0;
}

static int create_window(_THIS, SDL_Window *w) {
	SynapseVideo *s = _this->driverdata;

	if (s->wayland != NULL) {
		return syn_wl_window_create(_this, w);
	}

	if (s->window)
		return SDL_SetError("Synapse supports one SDL window");
	w->w = SDL_min(w->w, (int)s->var.xres);
	w->h = SDL_min(w->h, (int)s->var.yres);
	s->w = w->w;
	s->h = w->h;
	s->pixels = SDL_calloc((size_t)s->w * s->h, 4);

	if (!s->pixels)
		return SDL_OutOfMemory( );
	s->window = w;
	w->driverdata = s;
	s->ox = ((int)s->var.xres - s->w) / 2;
	s->oy = ((int)s->var.yres - s->h) / 2;
	s->mx = s->w / 2;
	s->my = s->h / 2;
	SDL_SetKeyboardFocus(w);
	SDL_SetMouseFocus(w);
	return 0;
}

static void destroy_window(_THIS, SDL_Window *w) {
	SynapseVideo *s = _this->driverdata;

	if (s->wayland != NULL) {
		syn_wl_window_destroy(s);
		w->driverdata = NULL;
		return;
	}

	SDL_free(s->pixels);
	s->pixels = NULL;
	s->window = NULL;
	w->driverdata = NULL;
}

static void window_size(_THIS, SDL_Window *w) {
	/* Fixed framebuffer mode; report the actual drawable dimensions. */
	SynapseVideo *s = _this->driverdata;

	if (s->wayland != NULL) {
		if (syn_wl_resize(s, w->w, w->h) != 0) {
			SDL_SendWindowEvent(w, SDL_WINDOWEVENT_CLOSE, 0, 0);
		}

		return;
	}

	w->w = s->w;
	w->h = s->h;
}

static void drawable(_THIS, SDL_Window *w, int *x, int *y) {
	*x = w->w;
	*y = w->h;
}

static int load_gl(_THIS, const char *path) {
	return 0;
}

static void unload_gl(_THIS) {}

// Освободить пробные буферы чтения, пока EGL-контекст ещё активен
static void syn_pbo_reset(SynapseGLContext *gl) {
	PFNGLDELETESYNCPROC delete_sync =
	    (PFNGLDELETESYNCPROC)eglGetProcAddress("glDeleteSync");
	PFNGLDELETEBUFFERSPROC delete_buffers =
	    (PFNGLDELETEBUFFERSPROC)eglGetProcAddress("glDeleteBuffers");

	for (unsigned i = 0; i < SDL_arraysize(gl->pbo_fence); i++) {
		if (gl->pbo_fence[i] != NULL && delete_sync != NULL) {
			delete_sync(gl->pbo_fence[i]);
		}

		gl->pbo_fence[i] = NULL;
	}

	if (gl->pbo_active && delete_buffers != NULL) {
		delete_buffers((GLsizei)SDL_arraysize(gl->pbo), gl->pbo);
	}

	SDL_memset(gl->pbo, 0, sizeof(gl->pbo));
	gl->pbo_active = SDL_FALSE;
	gl->pbo_next = 0;
}

// Проверить доступность двух буферов чтения без изменения обычного режима
static SDL_bool syn_pbo_init(SynapseGLContext *gl, size_t bytes) {
	const char *enabled = SDL_getenv("SYNAPSE_GL_PBO");
	PFNGLGENBUFFERSPROC gen_buffers =
	    (PFNGLGENBUFFERSPROC)eglGetProcAddress("glGenBuffers");
	PFNGLBINDBUFFERPROC bind_buffer =
	    (PFNGLBINDBUFFERPROC)eglGetProcAddress("glBindBuffer");
	PFNGLBUFFERDATAPROC buffer_data =
	    (PFNGLBUFFERDATAPROC)eglGetProcAddress("glBufferData");
	GLint saved_buffer = 0;

	if (gl->pbo_active) {
		return SDL_TRUE;
	}

	if (gl->pbo_failed || enabled == NULL || SDL_strcmp(enabled, "1") != 0 ||
	    gen_buffers == NULL || bind_buffer == NULL || buffer_data == NULL ||
	    eglGetProcAddress("glMapBufferRange") == NULL ||
	    eglGetProcAddress("glUnmapBuffer") == NULL ||
	    eglGetProcAddress("glFenceSync") == NULL ||
	    eglGetProcAddress("glClientWaitSync") == NULL ||
	    eglGetProcAddress("glDeleteSync") == NULL) {
		return SDL_FALSE;
	}

	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &saved_buffer);
	gen_buffers((GLsizei)SDL_arraysize(gl->pbo), gl->pbo);

	for (unsigned i = 0; i < SDL_arraysize(gl->pbo); i++) {
		bind_buffer(GL_PIXEL_PACK_BUFFER, gl->pbo[i]);
		buffer_data(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)bytes, NULL, GL_STREAM_READ);
	}

	bind_buffer(GL_PIXEL_PACK_BUFFER, (GLuint)saved_buffer);

	if (glGetError( ) != GL_NO_ERROR || gl->pbo[0] == 0 || gl->pbo[1] == 0) {
		gl->pbo_active = SDL_TRUE;
		syn_pbo_reset(gl);
		gl->pbo_failed = SDL_TRUE;
		return SDL_FALSE;
	}

	gl->pbo_active = SDL_TRUE;
	fprintf(stderr, "SYNAPSE_EGL_PBO_ACTIVE buffers=2 bytes=%zu\n", bytes);
	return SDL_TRUE;
}

// Читать предыдущий готовый кадр, сохраняя прежний вывод при любой ошибке
static SDL_bool syn_pbo_read_frame(SynapseVideo *video, size_t bytes) {
	SynapseGLContext *gl = video->gl_context;
	PFNGLBINDBUFFERPROC bind_buffer =
	    (PFNGLBINDBUFFERPROC)eglGetProcAddress("glBindBuffer");
	PFNGLFENCESYNCPROC fence_sync =
	    (PFNGLFENCESYNCPROC)eglGetProcAddress("glFenceSync");
	PFNGLCLIENTWAITSYNCPROC wait_sync =
	    (PFNGLCLIENTWAITSYNCPROC)eglGetProcAddress("glClientWaitSync");
	PFNGLMAPBUFFERRANGEPROC map_buffer =
	    (PFNGLMAPBUFFERRANGEPROC)eglGetProcAddress("glMapBufferRange");
	PFNGLUNMAPBUFFERPROC unmap_buffer =
	    (PFNGLUNMAPBUFFERPROC)eglGetProcAddress("glUnmapBuffer");
	PFNGLDELETESYNCPROC delete_sync =
	    (PFNGLDELETESYNCPROC)eglGetProcAddress("glDeleteSync");
	unsigned current = gl->pbo_next;
	unsigned previous = current ^ 1U;
	Uint64 started = SDL_GetPerformanceCounter( );
	Uint64 enqueued;
	Uint64 waited;
	SDL_bool ready = SDL_FALSE;

	bind_buffer(GL_PIXEL_PACK_BUFFER, gl->pbo[current]);
	glReadPixels(0, 0, video->w, video->h, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	gl->pbo_fence[current] = fence_sync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	enqueued = SDL_GetPerformanceCounter( );

	if (glGetError( ) != GL_NO_ERROR || gl->pbo_fence[current] == NULL) {
		goto fallback;
	}

	if (gl->pbo_fence[previous] == NULL) {
		bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
		glReadPixels(0, 0, video->w, video->h, GL_BGRA, GL_UNSIGNED_BYTE,
		             gl->readback);

		if (glGetError( ) != GL_NO_ERROR) {
			goto fallback;
		}

		gl->pbo_next = previous;
		video->metric_pbo_enqueue += enqueued - started;
		video->metric_pbo_fallbacks++;
		return SDL_TRUE;
	}

	GLenum result = wait_sync(gl->pbo_fence[previous], GL_SYNC_FLUSH_COMMANDS_BIT,
	                          1000000000ULL);
	waited = SDL_GetPerformanceCounter( );

	if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) {
		goto fallback;
	}

	bind_buffer(GL_PIXEL_PACK_BUFFER, gl->pbo[previous]);
	void *mapped = map_buffer(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT);

	if (mapped != NULL) {
		SDL_memcpy(gl->readback, mapped, bytes);
		ready = unmap_buffer(GL_PIXEL_PACK_BUFFER) == GL_TRUE;
	}

	if (!ready || glGetError( ) != GL_NO_ERROR) {
		goto fallback;
	}

	delete_sync(gl->pbo_fence[previous]);
	gl->pbo_fence[previous] = NULL;
	bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
	gl->pbo_next = previous;
	video->metric_pbo_enqueue += enqueued - started;
	video->metric_pbo_wait += waited - enqueued;
	video->metric_pbo_map += SDL_GetPerformanceCounter( ) - waited;
	video->metric_pbo_frames++;
	return SDL_TRUE;

fallback:
	bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
	syn_pbo_reset(gl);
	gl->pbo_failed = SDL_TRUE;
	video->metric_pbo_fallbacks++;
	return SDL_FALSE;
}

// Создать буфер вывода EGL после изменения размера окна
static int syn_egl_resize(SynapseVideo *video, int width, int height) {
	SynapseGLContext *gl = video->gl_context;
	unsigned char *readback = SDL_malloc((size_t)width * (size_t)height * 4);

	if (readback == NULL) {
		return SDL_OutOfMemory( );
	}

	struct gbm_surface *gbm_surface = gbm_surface_create(gl->gbm, (uint32_t)width,
	                                                   (uint32_t)height, GBM_FORMAT_ARGB8888,
	                                                   GBM_BO_USE_RENDERING);

	if (gbm_surface == NULL) {
		SDL_free(readback);
		return SDL_SetError("GBM resize failed");
	}

	EGLSurface surface = eglCreateWindowSurface(gl->display, gl->config,
	                                           (EGLNativeWindowType)gbm_surface, NULL);

	if (surface == EGL_NO_SURFACE ||
	    eglMakeCurrent(gl->display, surface, surface, gl->context) != EGL_TRUE) {
		if (surface != EGL_NO_SURFACE) {
			eglDestroySurface(gl->display, surface);
		}

		gbm_surface_destroy(gbm_surface);
		SDL_free(readback);
		return SDL_SetError("EGL resize failed: 0x%x", eglGetError( ));
	}

	syn_pbo_reset(gl);
	gl->pbo_failed = SDL_FALSE;
	eglDestroySurface(gl->display, gl->surface);
	gbm_surface_destroy(gl->gbm_surface);
	SDL_free(gl->readback);
	gl->surface = surface;
	gl->gbm_surface = gbm_surface;
	gl->readback = readback;
	return 0;
}

// Попробовать VirtIO GPU через EGL, не меняя программный путь по умолчанию
static SynapseGLContext *syn_egl_create(_THIS) {
	SynapseVideo *video = _this->driverdata;
	SynapseGLContext *gl = SDL_calloc(1, sizeof(*gl));
	EGLint count = 0;
	const char *stage = "render node";
	EGLint config_attrs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
		                   EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
		                   EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
		                   EGL_STENCIL_SIZE, 8, EGL_NONE };
	EGLint context_attrs[] = {
		EGL_CONTEXT_MAJOR_VERSION, _this->gl_config.major_version,
		EGL_CONTEXT_MINOR_VERSION, _this->gl_config.minor_version,
		EGL_CONTEXT_OPENGL_PROFILE_MASK,
		_this->gl_config.profile_mask == SDL_GL_CONTEXT_PROFILE_CORE
		    ? EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT
		    : EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
		EGL_NONE
	};

	if (gl == NULL) {
		return NULL;
	}

	gl->render_fd = -1;
	gl->display = EGL_NO_DISPLAY;
	gl->context = EGL_NO_CONTEXT;
	gl->surface = EGL_NO_SURFACE;
	gl->render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);

	if (gl->render_fd < 0) {
		goto fail;
	}

	stage = "GBM device";
	gl->gbm = gbm_create_device(gl->render_fd);

	if (gl->gbm == NULL) {
		goto fail;
	}

	stage = "EGL display/config";
	gl->display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gl->gbm, NULL);

	if (gl->display == EGL_NO_DISPLAY || eglInitialize(gl->display, NULL, NULL) != EGL_TRUE ||
	    eglBindAPI(EGL_OPENGL_API) != EGL_TRUE ||
	    eglChooseConfig(gl->display, config_attrs, &gl->config, 1, &count) != EGL_TRUE ||
	    count != 1) {
		if (gl->display != EGL_NO_DISPLAY && count == 0) {
			EGLConfig configs[16];
			EGLint available = 0;

			if (eglGetConfigs(gl->display, configs, 16, &available) == EGL_TRUE) {
				fprintf(stderr, "SYNAPSE_EGL_CONFIGS count=%d\n", available);

				for (EGLint i = 0; i < available && i < 16; i++) {
					EGLint surface_type = 0;
					EGLint render_type = 0;
					EGLint depth = 0;
					EGLint stencil = 0;
					eglGetConfigAttrib(gl->display, configs[i], EGL_SURFACE_TYPE,
					                   &surface_type);
					eglGetConfigAttrib(gl->display, configs[i], EGL_RENDERABLE_TYPE,
					                   &render_type);
					eglGetConfigAttrib(gl->display, configs[i], EGL_DEPTH_SIZE, &depth);
					eglGetConfigAttrib(gl->display, configs[i], EGL_STENCIL_SIZE, &stencil);
					fprintf(stderr,
					        "SYNAPSE_EGL_CONFIG index=%d surface=0x%x render=0x%x "
					        "depth=%d stencil=%d\n",
					        i, surface_type, render_type, depth, stencil);
				}
			}
		}

		goto fail;
	}

	stage = "EGL context/surface";
	gl->context = eglCreateContext(gl->display, gl->config, EGL_NO_CONTEXT, context_attrs);
	EGLint context_error = eglGetError( );
	const char *allow_downgrade = SDL_getenv("SYNAPSE_GL_EGL_DOWNGRADE");
	const char *automatic = SDL_getenv("SYNAPSE_GL_EGL_AUTO");

	if (gl->context == EGL_NO_CONTEXT &&
	    ((allow_downgrade != NULL && SDL_strcmp(allow_downgrade, "1") == 0) ||
	     (automatic != NULL && SDL_strcmp(automatic, "1") == 0))) {
		const EGLint versions[][2] = { { 4, 3 }, { 3, 3 } };

		for (size_t i = 0; i < SDL_arraysize(versions); i++) {
			if (versions[i][0] > _this->gl_config.major_version ||
			    (versions[i][0] == _this->gl_config.major_version &&
			     versions[i][1] >= _this->gl_config.minor_version)) {
				continue;
			}

			context_attrs[1] = versions[i][0];
			context_attrs[3] = versions[i][1];
			gl->context = eglCreateContext(gl->display, gl->config, EGL_NO_CONTEXT,
			                               context_attrs);
			context_error = eglGetError( );
			fprintf(stderr, "SYNAPSE_EGL_CONTEXT_TRY version=%d.%d context=%p egl=0x%x\n",
			        versions[i][0], versions[i][1], gl->context, context_error);

			if (gl->context != EGL_NO_CONTEXT) {
				break;
			}
		}
	}

	EGLint visual = 0;
	eglGetConfigAttrib(gl->display, gl->config, EGL_NATIVE_VISUAL_ID, &visual);
	gl->gbm_surface = gbm_surface_create(gl->gbm, (uint32_t)video->w,
	                                      (uint32_t)video->h, GBM_FORMAT_ARGB8888,
	                                      GBM_BO_USE_RENDERING);

	if (gl->gbm_surface != NULL) {
		gl->surface = eglCreateWindowSurface(gl->display, gl->config,
		                                     (EGLNativeWindowType)gl->gbm_surface, NULL);
	}

	fprintf(stderr,
	        "SYNAPSE_EGL_CREATE context=%p gbm_surface=%p egl_surface=%p "
	        "visual=0x%x requested=0x%x version=%d.%d profile=0x%x "
	        "context_error=0x%x errno=%d egl=0x%x\n",
	        gl->context, (void *)gl->gbm_surface, gl->surface, visual,
	        GBM_FORMAT_ARGB8888, _this->gl_config.major_version,
	        _this->gl_config.minor_version, _this->gl_config.profile_mask,
	        context_error, errno, eglGetError( ));

	if (gl->context == EGL_NO_CONTEXT || gl->surface == EGL_NO_SURFACE ||
	    eglMakeCurrent(gl->display, gl->surface, gl->surface, gl->context) != EGL_TRUE) {
		goto fail;
	}

	const GLubyte *version = glGetString(GL_VERSION);
	int gl_major = 0;
	int gl_minor = 0;
	stage = "OpenGL capabilities";

	if (version == NULL || sscanf((const char *)version, "%d.%d", &gl_major, &gl_minor) != 2 ||
	    gl_major < 3 || (gl_major == 3 && gl_minor < 3) ||
	    eglGetProcAddress("glBindBuffer") == NULL ||
	    eglGetProcAddress("glCreateShader") == NULL ||
	    eglGetProcAddress("glGenVertexArrays") == NULL ||
	    eglGetProcAddress("glDrawElementsInstanced") == NULL ||
	    eglGetProcAddress("glBlitFramebuffer") == NULL) {
		goto fail;
	}

	fprintf(stderr, "SYNAPSE_EGL_VERSION %s\n", version);

	stage = "readback allocation";
	gl->readback = SDL_malloc((size_t)video->w * (size_t)video->h * 4);

	if (gl->readback == NULL) {
		goto fail;
	}

	gl->egl = SDL_TRUE;
	fprintf(stderr, "SYNAPSE_EGL_ACTIVE renderD128 OpenGL GBM readback\n");
	return gl;

fail:
	fprintf(stderr, "SYNAPSE_EGL_FALLBACK stage=%s egl=0x%x errno=%d configs=%d\n",
	        stage, eglGetError( ), errno, count);
	SDL_free(gl->readback);

	if (gl->display != EGL_NO_DISPLAY) {
		eglMakeCurrent(gl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

		if (gl->surface != EGL_NO_SURFACE) {
			eglDestroySurface(gl->display, gl->surface);
		}

		if (gl->gbm_surface != NULL) {
			gbm_surface_destroy(gl->gbm_surface);
		}

		if (gl->context != EGL_NO_CONTEXT) {
			eglDestroyContext(gl->display, gl->context);
		}

		eglTerminate(gl->display);
	}

	if (gl->gbm != NULL) {
		gbm_device_destroy(gl->gbm);
	}

	if (gl->render_fd >= 0) {
		close(gl->render_fd);
	}

	SDL_free(gl);
	return NULL;
}

static void *get_proc(_THIS, const char *name) {
	SynapseVideo *video = _this->driverdata;

	if (video->gl_context != NULL && video->gl_context->egl) {
		return (void *)eglGetProcAddress(name);
	}

	return (void *)OSMesaGetProcAddress(name);
}

static int make_current(_THIS, SDL_Window *w, SDL_GLContext c) {
	SynapseVideo *s = _this->driverdata;
	SynapseGLContext *gl = c;
	(void)w;

	if (gl == NULL) {
		if (s->gl_context != NULL && s->gl_context->egl) {
			eglMakeCurrent(s->gl_context->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
			               EGL_NO_CONTEXT);
		}

		OSMesaMakeCurrent(NULL, NULL, GL_UNSIGNED_BYTE, 0, 0);
		s->gl_context = NULL;
		return 0;
	}

	if (gl->egl) {
		if (eglMakeCurrent(gl->display, gl->surface, gl->surface, gl->context) != EGL_TRUE) {
			return SDL_SetError("eglMakeCurrent failed: 0x%x", eglGetError( ));
		}
	} else {
		if (!OSMesaMakeCurrent(gl->osmesa, s->pixels, GL_UNSIGNED_BYTE, s->w, s->h)) {
			return SDL_SetError("OSMesaMakeCurrent failed");
		}

		OSMesaPixelStore(OSMESA_Y_UP, 0);
	}

	s->gl_context = gl;
	return 0;
}

static SDL_GLContext create_context(_THIS, SDL_Window *w) {
	int profile = _this->gl_config.profile_mask == SDL_GL_CONTEXT_PROFILE_CORE
	                  ? OSMESA_CORE_PROFILE
	                  : OSMESA_COMPAT_PROFILE;
	int attrs[] = { OSMESA_FORMAT,
		            OSMESA_BGRA,
		            OSMESA_DEPTH_BITS,
		            24,
		            OSMESA_STENCIL_BITS,
		            8,
		            OSMESA_PROFILE,
		            profile,
		            OSMESA_CONTEXT_MAJOR_VERSION,
		            _this->gl_config.major_version,
		            OSMESA_CONTEXT_MINOR_VERSION,
		            _this->gl_config.minor_version,
		            0 };
	SynapseVideo *video = _this->driverdata;
	SynapseGLContext *gl = NULL;
	const char *egl_enabled = SDL_getenv("SYNAPSE_GL_EGL");
	const char *egl_auto = SDL_getenv("SYNAPSE_GL_EGL_AUTO");
	SDL_bool egl_disabled = egl_enabled != NULL && SDL_strcmp(egl_enabled, "0") == 0;
	SDL_bool egl_requested = egl_enabled != NULL && SDL_strcmp(egl_enabled, "1") == 0;
	SDL_bool auto_requested = egl_auto != NULL && SDL_strcmp(egl_auto, "1") == 0;

	if (video->wayland != NULL && !egl_disabled && (egl_requested || auto_requested)) {
		gl = syn_egl_create(_this);
	}

	if (gl != NULL) {
		video->gl_context = gl;
		return gl;
	}

	gl = SDL_calloc(1, sizeof(*gl));

	if (gl == NULL) {
		SDL_OutOfMemory( );
		return NULL;
	}

	gl->osmesa = OSMesaCreateContextAttribs(attrs, NULL);

	if (gl->osmesa == NULL) {
		SDL_free(gl);
		SDL_SetError("Requested OpenGL context unsupported by Mesa OSMesa");
		return NULL;
	}

	if (make_current(_this, w, gl) != 0) {
		OSMesaDestroyContext(gl->osmesa);
		SDL_free(gl);
		return NULL;
	}

	return gl;
}

static void delete_context(_THIS, SDL_GLContext c) {
	SynapseVideo *video = _this->driverdata;
	SynapseGLContext *gl = c;

	if (gl == NULL) {
		return;
	}

	if (video->gl_context == gl) {
		if (gl->egl) {
			syn_pbo_reset(gl);
		}

		make_current(_this, NULL, NULL);
	}

	if (gl->egl) {
		eglDestroySurface(gl->display, gl->surface);
		gbm_surface_destroy(gl->gbm_surface);
		eglDestroyContext(gl->display, gl->context);
		eglTerminate(gl->display);
		gbm_device_destroy(gl->gbm);
		close(gl->render_fd);
		SDL_free(gl->readback);
	} else {
		OSMesaDestroyContext(gl->osmesa);
	}

	SDL_free(gl);
}

// Записать краткую выборку пикселей тестового кадра без сохранения изображения
static void syn_egl_sample_frame(SynapseVideo *s) {
	if (SDL_getenv("SYNAPSE_GL_EGL_PIXELS") == NULL) {
		return;
	}

	Uint32 now = SDL_GetTicks( );

	if (now - s->egl_sample_time < 1000) {
		return;
	}

	Uint32 hash = 2166136261U;
	unsigned lit = 0;
	size_t stride = (size_t)s->w * 4U;

	for (unsigned row = 0; row < 16U; row++) {
		size_t y = (size_t)row * (size_t)s->h / 16U;

		for (unsigned column = 0; column < 16U; column++) {
			size_t x = (size_t)column * (size_t)s->w / 16U;
			const unsigned char *pixel = s->gl_context->readback + y * stride + x * 4U;

			if ((pixel[0] | pixel[1] | pixel[2]) != 0U) {
				lit++;
			}

			for (unsigned channel = 0; channel < 4U; channel++) {
				hash = (hash ^ pixel[channel]) * 16777619U;
			}
		}
	}

	fprintf(stderr, "SYNAPSE_EGL_FRAME_SAMPLE ticks=%u lit=%u/256 hash=%08x changed=%u\n",
	        now, lit, hash, hash != s->egl_sample_hash ? 1U : 0U);
	s->egl_sample_time = now;
	s->egl_sample_hash = hash;
}

static int swap(_THIS, SDL_Window *w) {
	SynapseVideo *s = _this->driverdata;
	Uint64 t0 = SDL_GetPerformanceCounter( );
	Uint64 t1, t2;
	SDL_bool profile = SDL_getenv("SYNAPSE_GL_METRICS") != NULL;

	struct {
		unsigned x, y, w, h;
	} r = { s->ox, s->oy, s->w, s->h };

	if ((w->flags & SDL_WINDOW_OPENGL) != 0) {
		if (s->gl_context != NULL && s->gl_context->egl) {
			Uint64 state_done = 0;
			Uint64 fence_done = 0;
			Uint64 read_done = 0;
			Uint64 restore_done = 0;
			size_t stride = (size_t)s->w * 4;
			SDL_bool trace = SDL_getenv("SYNAPSE_GL_EGL_TRACE") != NULL &&
			                 s->egl_trace_count < 8;
			PFNGLBINDBUFFERPROC bind_buffer =
			    (PFNGLBINDBUFFERPROC)eglGetProcAddress("glBindBuffer");
			GLint pack_alignment = 4;
			GLint pack_row_length = 0;
			GLint pack_skip_rows = 0;
			GLint pack_skip_pixels = 0;
			GLint pack_buffer = 0;

			if (bind_buffer == NULL) {
				return SDL_SetError("EGL glBindBuffer unavailable");
			}

			GLenum before_error = GL_NO_ERROR;

			if (trace) {
				before_error = glGetError( );
			}

			glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
			glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_length);
			glGetIntegerv(GL_PACK_SKIP_ROWS, &pack_skip_rows);
			glGetIntegerv(GL_PACK_SKIP_PIXELS, &pack_skip_pixels);
			glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
			bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
			glPixelStorei(GL_PACK_ALIGNMENT, 4);
			glPixelStorei(GL_PACK_ROW_LENGTH, 0);
			glPixelStorei(GL_PACK_SKIP_ROWS, 0);
			glPixelStorei(GL_PACK_SKIP_PIXELS, 0);

			if (profile) {
				state_done = SDL_GetPerformanceCounter( );
				fence_done = state_done;

				// Отдельно измерить ожидание команд GPU без изменения обычного вывода
				const char *split_wait = SDL_getenv("SYNAPSE_GL_SPLIT_WAIT");

				if (split_wait != NULL && SDL_strcmp(split_wait, "1") == 0) {
					PFNGLFENCESYNCPROC fence_sync =
					    (PFNGLFENCESYNCPROC)eglGetProcAddress("glFenceSync");
					PFNGLCLIENTWAITSYNCPROC client_wait_sync =
					    (PFNGLCLIENTWAITSYNCPROC)eglGetProcAddress("glClientWaitSync");
					PFNGLDELETESYNCPROC delete_sync =
					    (PFNGLDELETESYNCPROC)eglGetProcAddress("glDeleteSync");

					if (fence_sync != NULL && client_wait_sync != NULL && delete_sync != NULL) {
						GLsync sync = fence_sync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

						if (sync != NULL) {
							GLenum result = client_wait_sync(sync, GL_SYNC_FLUSH_COMMANDS_BIT,
							                                 1000000000ULL);
							delete_sync(sync);

							if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) {
								fence_done = SDL_GetPerformanceCounter( );
								s->metric_gl_fence_frames++;
							} else {
								s->metric_gl_fence_failures++;
							}
						} else {
							s->metric_gl_fence_failures++;
						}
					} else {
						s->metric_gl_fence_failures++;
					}
				}
			}

			SDL_bool pbo_read = SDL_FALSE;

			if (syn_pbo_init(s->gl_context, stride * (size_t)s->h)) {
				pbo_read = syn_pbo_read_frame(s, stride * (size_t)s->h);
			}

			if (!pbo_read) {
				glReadPixels(0, 0, s->w, s->h, GL_BGRA, GL_UNSIGNED_BYTE,
				             s->gl_context->readback);
			}

			if (profile) {
				read_done = SDL_GetPerformanceCounter( );
			}

			syn_egl_sample_frame(s);

			if (trace) {
				GLenum read_error = glGetError( );
				GLint read_fb = 0;
				GLint viewport[4] = { 0, 0, 0, 0 };
				glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
				glGetIntegerv(GL_VIEWPORT, viewport);
				fprintf(stderr,
				        "SYNAPSE_EGL_READBACK before=0x%x after=0x%x read_fb=%d "
				        "viewport=%d,%d,%d,%d size=%dx%d\n",
				        before_error, read_error, read_fb, viewport[0], viewport[1],
				        viewport[2], viewport[3], s->w, s->h);
				s->egl_trace_count++;
			}
			glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
			glPixelStorei(GL_PACK_ROW_LENGTH, pack_row_length);
			glPixelStorei(GL_PACK_SKIP_ROWS, pack_skip_rows);
			glPixelStorei(GL_PACK_SKIP_PIXELS, pack_skip_pixels);
			bind_buffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buffer);

			if (profile) {
				restore_done = SDL_GetPerformanceCounter( );
			}

			for (int y = 0; y < s->h; y++) {
				SDL_memcpy(s->pixels + (size_t)y * stride,
				           s->gl_context->readback + (size_t)(s->h - y - 1) * stride,
				           stride);
			}

			if (profile) {
				Uint64 flip_done = SDL_GetPerformanceCounter( );
				s->metric_gl_state += state_done - t0;
				s->metric_gl_fence_wait += fence_done - state_done;
				s->metric_gl_readback += read_done - fence_done;
				s->metric_gl_restore += restore_done - read_done;
				s->metric_gl_flip += flip_done - restore_done;
			}
		} else {
			glFinish( );
		}
	}

	if (s->wayland != NULL) {
		Uint64 finished = 0;

		if (profile) {
			finished = SDL_GetPerformanceCounter( );
		}

		int result = syn_wl_present(s);
		if (result != 0) {
			return result;
		}

		if (profile) {
			Uint64 completed = SDL_GetPerformanceCounter( );
			Uint32 now = SDL_GetTicks( );
			s->metric_render += completed - t0;
			s->metric_finish += finished - t0;
			s->metric_frames++;

			if (s->metric_start == 0) {
				s->metric_start = now;
			}

			if (now - s->metric_start >= 1000) {
				double scale = 1000.0 / SDL_GetPerformanceFrequency( ) / s->metric_frames;

				fprintf(stderr,
				        "SYNAPSE_WL_FRAME_METRIC backend=%s frames=%u elapsed_ms=%u fps=%.2f "
				        "swap_ms=%.2f finish_ms=%.2f gl_state_ms=%.2f "
				        "gl_fence_ms=%.2f gl_readback_ms=%.2f gl_restore_ms=%.2f gl_flip_ms=%.2f "
				        "gl_fence_frames=%u gl_fence_failures=%u "
				        "pbo_frames=%u pbo_fallbacks=%u pbo_enqueue_ms=%.2f "
				        "pbo_wait_ms=%.2f pbo_map_ms=%.2f "
				        "wait_ms=%.2f fill_ms=%.2f "
				        "copy_ms=%.2f decor_ms=%.2f commit_ms=%.2f\n",
				        s->gl_context != NULL && s->gl_context->egl ? "egl" : "osmesa",
				        s->metric_frames, now - s->metric_start,
				        s->metric_frames * 1000.0 / (now - s->metric_start),
				        s->metric_render * scale, s->metric_finish * scale,
				        s->metric_gl_state * scale, s->metric_gl_fence_wait * scale,
				        s->metric_gl_readback * scale,
				        s->metric_gl_restore * scale, s->metric_gl_flip * scale,
				        s->metric_gl_fence_frames, s->metric_gl_fence_failures,
				        s->metric_pbo_frames, s->metric_pbo_fallbacks,
				        s->metric_pbo_enqueue * scale, s->metric_pbo_wait * scale,
				        s->metric_pbo_map * scale,
				        s->metric_wait * scale, s->metric_fill * scale,
				        s->metric_copy * scale, s->metric_decor * scale,
				        s->metric_commit * scale);
				fflush(stderr);
				s->metric_render = 0;
				s->metric_finish = 0;
				s->metric_wait = 0;
				s->metric_fill = 0;
				s->metric_copy = 0;
				s->metric_decor = 0;
				s->metric_commit = 0;
			s->metric_gl_state = 0;
			s->metric_gl_fence_wait = 0;
			s->metric_gl_readback = 0;
			s->metric_gl_restore = 0;
			s->metric_gl_flip = 0;
			s->metric_gl_fence_frames = 0;
			s->metric_gl_fence_failures = 0;
			s->metric_pbo_enqueue = 0;
			s->metric_pbo_wait = 0;
			s->metric_pbo_map = 0;
			s->metric_pbo_frames = 0;
			s->metric_pbo_fallbacks = 0;
				s->metric_frames = 0;
				s->metric_start = now;
			}
		}

		return 0;
	}

	t1 = SDL_GetPerformanceCounter( );

	for (int y = 0; y < s->h; y++)
		SDL_memcpy(s->screen + (y + s->oy) * s->fix.line_length + s->ox * 4,
		           s->pixels + y * s->w * 4, s->w * 4);
	/* Software pointer stays visible even when SDL cannot create a system cursor. */
	for (int cy = 0; cy < 14 && s->my + cy < s->h; cy++) {
		for (int cx = 0; cx <= cy / 2 && s->mx + cx < s->w; cx++) {
			Uint32 *pixel = (Uint32 *)(s->screen + (s->my + cy + s->oy) * s->fix.line_length +
			                           (s->mx + cx + s->ox) * 4);
			*pixel = cx == 0 || cx == cy / 2 || cy == 13 ? 0 : 0xffffff;
		}
	}

	hud_draw(s);

	if (s->hud_font != NULL) {
		struct {
			unsigned x, y, w, h;
		} bar = { 0, 0, s->var.xres, s->hud_height * 2 };

		if (ioctl(s->fb, 0x4621, &bar) != 0) {
			return SDL_SetError("Statistics present failed");
		}
	}

	t2 = SDL_GetPerformanceCounter( );

	if (ioctl(s->fb, 0x4621, &r) || ioctl(s->fb, 0x4622, 0))
		return SDL_SetError("Framebuffer present failed");
	if (SDL_getenv("SYNAPSE_GL_METRICS")) {
		Uint64 t3 = SDL_GetPerformanceCounter( );
		Uint32 now = SDL_GetTicks( );
		s->metric_render += t1 - t0;
		s->metric_copy += t2 - t1;
		s->metric_present += t3 - t2;

		if (!s->metric_start)
			s->metric_start = now;
		s->metric_frames++;

		if (now - s->metric_start >= 1000) {
			fprintf(
			    stderr,
			    "SYNAPSE_GL_FRAME_METRIC ticks=%u frames=%u elapsed_ms=%u fps=%.2f finish_ms=%.2f copy_ms=%.2f present_ms=%.2f\n",
			    now, s->metric_frames, now - s->metric_start,
			    s->metric_frames * 1000.0 / (now - s->metric_start),
			    s->metric_render * 1000.0 / SDL_GetPerformanceFrequency( ) / s->metric_frames,
			    s->metric_copy * 1000.0 / SDL_GetPerformanceFrequency( ) / s->metric_frames,
			    s->metric_present * 1000.0 / SDL_GetPerformanceFrequency( ) / s->metric_frames);
			s->metric_render = s->metric_copy = s->metric_present = 0;
			fflush(stderr);
			s->metric_frames = 0;
			s->metric_start = now;
		}
	}

	return 0;
}

static int interval_set(_THIS, int n) {
	((SynapseVideo *)_this->driverdata)->interval = n;
	return 0;
}

static int interval_get(_THIS) {
	return ((SynapseVideo *)_this->driverdata)->interval;
}

static SDL_Scancode scancode(unsigned key) {
	static const SDL_Scancode special[] = {
		SDL_SCANCODE_UNKNOWN,   SDL_SCANCODE_ESCAPE, SDL_SCANCODE_TAB,
		SDL_SCANCODE_CAPSLOCK,  SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT,
		SDL_SCANCODE_LCTRL,     SDL_SCANCODE_RCTRL,  SDL_SCANCODE_LALT,
		SDL_SCANCODE_RALT,      SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE,
		SDL_SCANCODE_SPACE,     SDL_SCANCODE_INSERT, SDL_SCANCODE_DELETE,
		SDL_SCANCODE_HOME,      SDL_SCANCODE_END,    SDL_SCANCODE_PAGEUP,
		SDL_SCANCODE_PAGEDOWN,  SDL_SCANCODE_LEFT,   SDL_SCANCODE_UP,
		SDL_SCANCODE_RIGHT,     SDL_SCANCODE_DOWN,   SDL_SCANCODE_F1,
		SDL_SCANCODE_F2,        SDL_SCANCODE_F3,     SDL_SCANCODE_F4,
		SDL_SCANCODE_F5,        SDL_SCANCODE_F6,     SDL_SCANCODE_F7,
		SDL_SCANCODE_F8,        SDL_SCANCODE_F9,     SDL_SCANCODE_F10,
		SDL_SCANCODE_F11,       SDL_SCANCODE_F12,    SDL_SCANCODE_NUMLOCKCLEAR,
		SDL_SCANCODE_SCROLLLOCK
	};

	if (key < SDL_arraysize(special))
		return special[key];
	if (key >= 'A' && key <= 'Z')
		return SDL_SCANCODE_A + key - 'A';
	if (key >= '1' && key <= '9')
		return SDL_SCANCODE_1 + key - '1';
	switch (key) {
		case '0':
			return SDL_SCANCODE_0;
		case '-':
			return SDL_SCANCODE_MINUS;
		case '=':
			return SDL_SCANCODE_EQUALS;
		case '[':
			return SDL_SCANCODE_LEFTBRACKET;
		case ']':
			return SDL_SCANCODE_RIGHTBRACKET;
		case '\\':
			return SDL_SCANCODE_BACKSLASH;
		case ';':
			return SDL_SCANCODE_SEMICOLON;
		case '\'':
			return SDL_SCANCODE_APOSTROPHE;
		case '`':
			return SDL_SCANCODE_GRAVE;
		case ',':
			return SDL_SCANCODE_COMMA;
		case '.':
			return SDL_SCANCODE_PERIOD;
		case '/':
			return SDL_SCANCODE_SLASH;
	}

	return SDL_SCANCODE_UNKNOWN;
}

static void pump(_THIS) {
	SynapseVideo *s = _this->driverdata;
	kbd_event_t e;
	struct syn_pointer_event p;

	if (!s->window)
		return;
	if (s->wayland != NULL) {
		if (syn_wl_dispatch(s) != 0) {
			SDL_SendWindowEvent(s->window, SDL_WINDOWEVENT_CLOSE, 0, 0);
		}

		return;
	}

	for (int i = 0; i < 128 && syscall(102000, &e) == 0; i++) {
		if (e.keycode == SYN_LAYOUT_EN || e.keycode == SYN_LAYOUT_RU) {
			s->russian = e.keycode == SYN_LAYOUT_RU;
			s->hud_time = 0;
			continue;
		}

		SDL_SendKeyboardKey(e.is_pressed ? SDL_PRESSED : SDL_RELEASED, scancode(e.keycode));

		if (e.is_pressed &&
		    !(e.modifiers & (KBD_MOD_LCTRL | KBD_MOD_RCTRL | KBD_MOD_LALT | KBD_MOD_RALT))) {
			char text[4] = { 0, 0, 0, 0 };
			Uint32 letter = 0;
			SDL_bool shift = (e.modifiers & 3) != 0;
			SDL_bool upper = shift != ((e.modifiers & KBD_MOD_CAPSLOCK) != 0);

			if (s->russian) {
				static const char keys[] = "QWERTYUIOP[]ASDFGHJKL;'`ZXCVBNM,.";
				static const Uint16 letters[] = { 0x439, 0x446, 0x443, 0x43a, 0x435, 0x43d, 0x433,
					                              0x448, 0x449, 0x437, 0x445, 0x44a, 0x444, 0x44b,
					                              0x432, 0x430, 0x43f, 0x440, 0x43e, 0x43b, 0x434,
					                              0x436, 0x44d, 0x451, 0x44f, 0x447, 0x441, 0x43c,
					                              0x438, 0x442, 0x44c, 0x431, 0x44e };
				const char *key = e.keycode < 128 ? SDL_strchr(keys, e.keycode) : NULL;

				if (key != NULL && *key != 0) {
					letter = letters[key - keys];

					if (upper) {
						letter = letter == 0x451 ? 0x401 : letter - 0x20;
					}
				}
			}

			if (letter != 0) {
				text[0] = (char)(0xc0 | (letter >> 6));
				text[1] = (char)(0x80 | (letter & 0x3f));
			} else if (e.keycode >= 39 && e.keycode <= 126) {
				text[0] = (char)e.keycode;

				if (text[0] >= 'A' && text[0] <= 'Z' && !upper) {
					text[0] += 32;
				}

				if (shift) {
					const char *a = "1234567890-=[];\\'`,./";
					const char *b = "!@#$%^&*()_+{}:|\"~<>?";
					const char *key = SDL_strchr(a, text[0]);

					if (key != NULL) {
						text[0] = b[key - a];
					}
				}

				if (s->russian) {
					if (e.keycode == '/') {
						text[0] = shift ? ',' : '.';
					} else if (shift && e.keycode == '3') {
						text[0] = (char)0xe2;
						text[1] = (char)0x84;
						text[2] = (char)0x96;
					} else if (shift && e.keycode == '2') {
						text[0] = '"';
					} else if (shift && e.keycode == '4') {
						text[0] = ';';
					} else if (shift && e.keycode == '6') {
						text[0] = ':';
					} else if (shift && e.keycode == '7') {
						text[0] = '?';
					} else if (shift && e.keycode == '\\') {
						text[0] = '/';
					}
				}
			} else if (e.keycode == VK_SPACE) {
				text[0] = ' ';
			}

			if (text[0])
				SDL_SendKeyboardText(text);
		}
	}
	// Восстановить состояние, если уведомление было потеряно при переполнении очереди
	s->russian = (syscall(102001) & SYN_STATE_RUSSIAN) != 0;

	for (int i = 0; i < 128 && s->pointer >= 0 && read(s->pointer, &p, sizeof(p)) == sizeof(p);
	     i++) {
		static const Uint8 btn[] = { SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE };

		if (p.flags & SYN_POINTER_WHEEL) {
			SDL_SendMouseWheel(s->window, 0, p.x, p.y, SDL_MOUSEWHEEL_NORMAL);
			continue;
		}

		if (p.flags & SYN_POINTER_DISCONNECTED)
			p.buttons = 0;
		if (p.flags & SYN_POINTER_ABSOLUTE) {
			s->mx = (int)((int64_t)p.x * s->var.xres / 65535) - s->ox;
			s->my = (int)((int64_t)p.y * s->var.yres / 65535) - s->oy;
		} else {
			s->mx += p.x;
			s->my += p.y;
		}

		s->mx = SDL_clamp(s->mx, 0, s->w - 1);
		s->my = SDL_clamp(s->my, 0, s->h - 1);
		SDL_SendMouseMotion(s->window, 0, 0, s->mx, s->my);

		for (unsigned n = 0; n < 3; n++)
			if ((s->buttons ^ p.buttons) & (1u << n))
				SDL_SendMouseButton(s->window, 0,
				                    (p.buttons & (1u << n)) ? SDL_PRESSED : SDL_RELEASED, btn[n]);
		s->buttons = p.buttons;
	}
}

// Указать память программного окна для библиотек без OpenGL
static int create_framebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels,
                              int *pitch) {
	(void)window;
	SynapseVideo *s = _this->driverdata;

	if (s->pixels == NULL) {
		return SDL_SetError("Window framebuffer is unavailable");
	}

	*format = SDL_PIXELFORMAT_RGB888;
	*pixels = s->pixels;
	*pitch = s->w * 4;
	return 0;
}

// Передать обновлённое программное окно тем же способом, что и кадр OpenGL
static int update_framebuffer(_THIS, SDL_Window *window, const SDL_Rect *rectangles, int count) {
	(void)rectangles;
	(void)count;
	return swap(_this, window);
}

// Память принадлежит окну и освобождается в destroy_window
static void destroy_framebuffer(_THIS, SDL_Window *window) {
	(void)_this;
	(void)window;
}

// Изменить название окна Wayland
static void window_title(_THIS, SDL_Window *window) {
	SynapseVideo *s = _this->driverdata;

	if (s->wayland != NULL && s->wayland->toplevel != NULL) {
		xdg_toplevel_set_title(s->wayland->toplevel,
		                       window->title != NULL ? window->title : "SynapseOS SDL");
	}
}

// Запросить полноэкранный режим через оконный сервер
static void window_fullscreen(_THIS, SDL_Window *window, SDL_VideoDisplay *display,
                              SDL_bool fullscreen) {
	(void)window;
	(void)display;
	SynapseVideo *s = _this->driverdata;

	if (s->wayland != NULL && s->wayland->toplevel != NULL) {
		if (fullscreen) {
			xdg_toplevel_set_fullscreen(s->wayland->toplevel, NULL);
		} else {
			xdg_toplevel_unset_fullscreen(s->wayland->toplevel);
		}
	}
}

static void free_device(SDL_VideoDevice *d) {
	SDL_free(d->driverdata);
	SDL_free(d);
}

static SDL_VideoDevice *create_device(void) {
	SDL_VideoDevice *d = SDL_calloc(1, sizeof(*d));
	SynapseVideo *s = SDL_calloc(1, sizeof(*s));

	if (!d || !s) {
		SDL_free(d);
		SDL_free(s);
		SDL_OutOfMemory( );
		return NULL;
	}

	s->fb = s->pointer = -1;
	d->driverdata = s;
	d->VideoInit = video_init;
	d->VideoQuit = video_quit;
	d->SetDisplayMode = set_mode;
	d->PumpEvents = pump;
	d->CreateSDLWindow = create_window;
	d->DestroyWindow = destroy_window;
	d->SetWindowSize = window_size;
	d->GL_LoadLibrary = load_gl;
	d->GL_UnloadLibrary = unload_gl;
	d->GL_GetProcAddress = get_proc;
	d->GL_CreateContext = create_context;
	d->GL_DeleteContext = delete_context;
	d->GL_MakeCurrent = make_current;
	d->GL_SwapWindow = swap;
	d->GL_SetSwapInterval = interval_set;
	d->GL_GetSwapInterval = interval_get;
	d->GL_GetDrawableSize = drawable;
	d->CreateWindowFramebuffer = create_framebuffer;
	d->UpdateWindowFramebuffer = update_framebuffer;
	d->DestroyWindowFramebuffer = destroy_framebuffer;
	d->SetWindowTitle = window_title;
	d->SetWindowFullscreen = window_fullscreen;
	d->free = free_device;
	return d;
}

VideoBootStrap OFFSCREEN_bootstrap = { "synapse", "Synapse framebuffer / Mesa OSMesa",
	                                   create_device, NULL };
#endif
