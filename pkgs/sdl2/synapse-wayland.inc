// synapse-wayland.inc: окна SDL2 с OSMesa и общей памятью Wayland
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl, SDL2 и Wayland
// Подсистема: графические приложения
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, Zlib
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <poll.h>
#include <errno.h>
#include "xdg-shell-client-protocol.h"
#include "../../events/scancodes_linux.h"

struct SynapseWayland;

struct SynapseWaylandBuffer {
	struct wl_buffer *buffer;
	unsigned char *pixels;
	size_t length;
	SDL_bool busy;
};

struct SynapseWayland {
	SynapseVideo *video;
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct wl_output *output;
	struct xdg_wm_base *shell;
	struct wl_surface *surface;
	struct wl_surface *cursor_surface;
	struct wl_cursor_theme *cursor_theme;
	uint32_t cursor_serial;
	SDL_bool cursor_focus;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct xkb_context *xkb_context;
	struct xkb_keymap *keymap;
	struct xkb_state *key_state;
	struct SynapseWaylandBuffer buffers[2];
	int output_width;
	int output_height;
	int output_refresh;
	Uint32 repeat_key;
	Uint32 repeat_next;
	int repeat_rate;
	int repeat_delay;
	SDL_bool repeating;
	int border;
	int caption;
	SDL_bool fullscreen;
	SDL_bool maximized;
	SDL_bool decoration_press;
	SDL_bool configured;
	SDL_bool failed;
	int configure_width;
	int configure_height;
	double wheel[2];
	int discrete[2];
	SDL_bool has_discrete[2];
};

static void syn_wl_repeat_tick(struct SynapseWayland *state);

// Обработать только уже доставленные события, не блокируя игровой поток
static int syn_wl_dispatch(SynapseVideo *video) {
	struct SynapseWayland *state = video->wayland;

	if (state->failed) {
		return SDL_SetError("Wayland connection closed");
	}

	while (wl_display_prepare_read(state->display) != 0) {
		if (wl_display_dispatch_pending(state->display) < 0) {
			state->failed = SDL_TRUE;
			return SDL_SetError("Wayland event dispatch failed");
		}
	}

	int flushed = wl_display_flush(state->display);

	if (flushed < 0 && errno != EAGAIN) {
		wl_display_cancel_read(state->display);
		state->failed = SDL_TRUE;
		return SDL_SetError("Wayland flush failed");
	}

	struct pollfd ready = { .fd = wl_display_get_fd(state->display), .events = POLLIN };
	int result = poll(&ready, 1, 0);

	if (result > 0 && (ready.revents & POLLIN) != 0) {
		if (wl_display_read_events(state->display) < 0) {
			state->failed = SDL_TRUE;
			return SDL_SetError("Wayland read failed");
		}
	} else {
		wl_display_cancel_read(state->display);

		if (result > 0 && (ready.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
			state->failed = SDL_TRUE;
			return SDL_SetError("Wayland compositor disconnected");
		}
	}

	if (wl_display_dispatch_pending(state->display) < 0) {
		state->failed = SDL_TRUE;
		return SDL_SetError("Wayland pending events failed");
	}

	syn_wl_repeat_tick(state);
	return 0;
}

// Отметить освобождённый сервером буфер доступным для следующего кадра
static void syn_wl_buffer_release(void *data, struct wl_buffer *buffer) {
	(void)buffer;
	struct SynapseWaylandBuffer *slot = data;
	slot->busy = SDL_FALSE;
}

static const struct wl_buffer_listener syn_wl_buffer_listener = { syn_wl_buffer_release };

// Освободить локальные отображения, сервер удерживает собственные ссылки до завершения кадра
static void syn_wl_buffers_destroy(struct SynapseWayland *state) {
	for (unsigned i = 0; i < SDL_arraysize(state->buffers); i++) {
		struct SynapseWaylandBuffer *slot = &state->buffers[i];

		if (slot->buffer != NULL) {
			wl_buffer_destroy(slot->buffer);
		}

		if (slot->pixels != NULL) {
			munmap(slot->pixels, slot->length);
		}

		SDL_memset(slot, 0, sizeof(*slot));
	}
}

// Создать удалённый временный файл и передать его Weston через SCM_RIGHTS
static int syn_wl_buffer_create(struct SynapseWayland *state, struct SynapseWaylandBuffer *slot,
                                int width, int height) {
	char path[256];
	const char *runtime = SDL_getenv("XDG_RUNTIME_DIR");

	if (runtime == NULL || width <= 0 || height <= 0 || width > 8192 || height > 8192) {
		return SDL_SetError("Invalid Wayland buffer or runtime directory");
	}

	int count = SDL_snprintf(path, sizeof(path), "%s/sdl-shm-XXXXXX", runtime);

	if (count < 0 || (size_t)count >= sizeof(path)) {
		return SDL_SetError("Wayland runtime path too long");
	}

	int fd = mkstemp(path);

	if (fd < 0) {
		return SDL_SetError("Wayland shared file creation failed");
	}

	unlink(path);

	if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
		close(fd);
		return SDL_SetError("Wayland shared descriptor flags failed");
	}

	size_t length = (size_t)width * (size_t)height * 4U;

	if (ftruncate(fd, (off_t)length) != 0) {
		close(fd);
		return SDL_SetError("Wayland shared file sizing failed");
	}

	void *pixels = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (pixels == MAP_FAILED) {
		close(fd);
		return SDL_SetError("Wayland shared mapping failed");
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, (int)length);

	if (pool == NULL) {
		munmap(pixels, length);
		close(fd);
		return SDL_OutOfMemory( );
	}

	slot->buffer =
	    wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	if (slot->buffer == NULL) {
		munmap(pixels, length);
		return SDL_OutOfMemory( );
	}

	slot->pixels = pixels;
	slot->length = length;
	wl_buffer_add_listener(slot->buffer, &syn_wl_buffer_listener, slot);
	return 0;
}

// Принять новый размер, сохранив связанный с потоком контекст OSMesa
static int syn_wl_resize(SynapseVideo *video, int width, int height) {
	if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
		return SDL_SetError("Unsupported Wayland window dimensions");
	}

	if (width == video->w && height == video->h && video->pixels != NULL) {
		return 0;
	}

	unsigned char *pixels = SDL_calloc((size_t)width * (size_t)height, 4);

	if (pixels == NULL) {
		return SDL_OutOfMemory( );
	}

	OSMesaContext context = OSMesaGetCurrentContext( );

	if (context != NULL && !OSMesaMakeCurrent(context, pixels, GL_UNSIGNED_BYTE, width, height)) {
		SDL_free(pixels);
		return SDL_SetError("OSMesa resize failed");
	}

	syn_wl_buffers_destroy(video->wayland);
	SDL_free(video->pixels);
	video->pixels = pixels;
	video->w = width;
	video->h = height;
	video->var.xres = (unsigned)width;
	video->var.yres = (unsigned)height;
	video->fix.line_length = (unsigned)width * 4U;

	if (video->window != NULL) {
		SDL_SendWindowEvent(video->window, SDL_WINDOWEVENT_RESIZED, width, height);
	}

	return 0;
}

// Подтвердить настройку поверхности перед первой передачей буфера
static void syn_wl_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
	struct SynapseWayland *state = data;
	xdg_surface_ack_configure(surface, serial);

	if (state->configure_width > 0 && state->configure_height > 0 && state->video->window != NULL &&
	    syn_wl_resize(state->video, state->configure_width, state->configure_height) != 0) {
		state->failed = SDL_TRUE;
	}

	state->configured = SDL_TRUE;
}

static const struct xdg_surface_listener syn_wl_surface_listener = { syn_wl_surface_configure };

// Сохранить предложенный сервером размер до подтверждения поверхности
static void syn_wl_top_configure(void *data, struct xdg_toplevel *top, int32_t width,
                                 int32_t height, struct wl_array *states) {
	(void)top;
	(void)states;
	struct SynapseWayland *state = data;
	uint32_t *item;
	state->fullscreen = SDL_FALSE;
	state->maximized = SDL_FALSE;
	wl_array_for_each(item, states) {
		if (*item == XDG_TOPLEVEL_STATE_FULLSCREEN) {
			state->fullscreen = SDL_TRUE;
		}

		if (*item == XDG_TOPLEVEL_STATE_MAXIMIZED) {
			state->maximized = SDL_TRUE;
		}
	}

	int old_border = state->border;
	int old_caption = state->caption;
	state->border = state->fullscreen ? 0 : 4;
	state->caption = state->fullscreen ? 0 : 32;

	if (state->border != old_border || state->caption != old_caption) {
		syn_wl_buffers_destroy(state);
	}

	state->configure_width = width > 0 ? SDL_max(1, width - 2 * state->border) : 0;
	state->configure_height = height > 0 ? SDL_max(1, height - state->caption - state->border) : 0;
}

// Доставить приложению стандартное событие закрытия окна
static void syn_wl_top_close(void *data, struct xdg_toplevel *top) {
	(void)top;
	struct SynapseWayland *state = data;

	if (state->video->window != NULL) {
		SDL_SendWindowEvent(state->video->window, SDL_WINDOWEVENT_CLOSE, 0, 0);
	}
}

static const struct xdg_toplevel_listener syn_wl_top_listener = { .configure = syn_wl_top_configure,
	                                                              .close = syn_wl_top_close };

// Подтвердить проверку доступности клиента
static void syn_wl_ping(void *data, struct xdg_wm_base *shell, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener syn_wl_shell_listener = { syn_wl_ping };

static SynapseVideo *syn_wl_cursor_video;

// Выбрать изображение из общей темы Weston
static SDL_Cursor *syn_wl_system_cursor(SDL_SystemCursor id) {
	SDL_Cursor *cursor = SDL_calloc(1, sizeof(*cursor));

	if (cursor != NULL) {
		cursor->driverdata = (void *)(uintptr_t)(id + 1);
	}

	return cursor;
}

// Пользовательские растры заменяются общей стрелкой рабочего стола
static SDL_Cursor *syn_wl_custom_cursor(SDL_Surface *surface, int x, int y) {
	(void)surface;
	(void)x;
	(void)y;
	return syn_wl_system_cursor(SDL_SYSTEM_CURSOR_ARROW);
}

// Передать изображение указателя Weston отдельно от кадра приложения
static int syn_wl_show_cursor(SDL_Cursor *cursor) {
	SynapseVideo *video = syn_wl_cursor_video;

	if (video == NULL || video->wayland == NULL) {
		return 0;
	}

	struct SynapseWayland *state = video->wayland;

	if (!state->cursor_focus || state->pointer == NULL) {
		return 0;
	}

	if (cursor == NULL) {
		wl_pointer_set_cursor(state->pointer, state->cursor_serial, NULL, 0, 0);
		return 0;
	}

	static const char *names[] = { "left_ptr",       "xterm",      "watch",       "crosshair",
		                           "left_ptr_watch", "size_fdiag", "size_bdiag",  "size_hor",
		                           "size_ver",       "fleur",      "not-allowed", "hand2" };
	unsigned id = (unsigned)(uintptr_t)cursor->driverdata - 1U;
	struct wl_cursor *image = wl_cursor_theme_get_cursor(
	    state->cursor_theme, id < SDL_arraysize(names) ? names[id] : "left_ptr");
	if (image == NULL) {
		image = wl_cursor_theme_get_cursor(state->cursor_theme, "left_ptr");
	}

	if (image == NULL || image->image_count == 0) {
		return SDL_SetError("Desktop cursor image unavailable");
	}

	struct wl_cursor_image *frame = image->images[0];
	struct wl_buffer *buffer = wl_cursor_image_get_buffer(frame);

	if (buffer == NULL) {
		return SDL_SetError("Desktop cursor buffer unavailable");
	}

	wl_pointer_set_cursor(state->pointer, state->cursor_serial, state->cursor_surface,
	                      (int)frame->hotspot_x, (int)frame->hotspot_y);
	wl_surface_attach(state->cursor_surface, buffer, 0, 0);
	wl_surface_damage(state->cursor_surface, 0, 0, (int)frame->width, (int)frame->height);
	wl_surface_commit(state->cursor_surface);
	wl_display_flush(state->display);
	return 0;
}

// Освободить описание указателя, общая тема принадлежит видеоустройству
static void syn_wl_free_cursor(SDL_Cursor *cursor) {
	SDL_free(cursor);
}

// Сфокусировать указатель, изображение курсора рисуется внутри кадра SDL
static void syn_wl_pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                                 struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
	(void)surface;
	struct SynapseWayland *state = data;
	(void)pointer;
	state->cursor_serial = serial;
	state->cursor_focus = SDL_TRUE;
	state->video->mx = (int)wl_fixed_to_double(x) - state->border;
	state->video->my = (int)wl_fixed_to_double(y) - state->caption;

	if (state->video->window != NULL) {
		SDL_SetMouseFocus(state->video->window);
		syn_wl_show_cursor(SDL_GetMouse( )->cursor_shown ? SDL_GetMouse( )->cur_cursor : NULL);
	}
}

// Снять фокус указателя при уходе на другое окно
static void syn_wl_pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                                 struct wl_surface *surface) {
	struct SynapseWayland *state = data;
	state->cursor_focus = SDL_FALSE;
	(void)pointer;
	(void)serial;
	(void)surface;
	SDL_SetMouseFocus(NULL);
}

// Передать координаты указателя в пределах клиентского окна
static void syn_wl_pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                                  wl_fixed_t x, wl_fixed_t y) {
	(void)pointer;
	(void)time;
	struct SynapseWayland *state = data;
	SynapseVideo *video = state->video;
	video->mx = (int)wl_fixed_to_double(x) - state->border;
	video->my = (int)wl_fixed_to_double(y) - state->caption;

	if (video->window != NULL) {
		SDL_SendMouseMotion(video->window, 0, 0, video->mx, video->my);
	}
}

// Преобразовать кнопки evdev в кнопки SDL
static void syn_wl_pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                                  uint32_t time, uint32_t button, uint32_t pressed) {
	(void)pointer;
	(void)time;
	struct SynapseWayland *state = data;
	SynapseVideo *video = state->video;
	int x = video->mx;
	int y = video->my;

	if (!pressed && state->decoration_press) {
		state->decoration_press = SDL_FALSE;
		return;
	}

	if (pressed && button == 0x110 && !state->fullscreen &&
	    (x < 0 || y < 0 || x >= video->w || y >= video->h)) {
		uint32_t edge = XDG_TOPLEVEL_RESIZE_EDGE_NONE;
		state->decoration_press = SDL_TRUE;

		if (x < 0) {
			edge |= XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
		} else if (x >= video->w) {
			edge |= XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
		}

		if (y >= video->h) {
			edge |= XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
		} else if (y < -state->caption + 4) {
			edge |= XDG_TOPLEVEL_RESIZE_EDGE_TOP;
		}

		if (edge != XDG_TOPLEVEL_RESIZE_EDGE_NONE) {
			xdg_toplevel_resize(state->toplevel, state->seat, serial, edge);
		} else if (x >= video->w - 28) {
			SDL_SendWindowEvent(video->window, SDL_WINDOWEVENT_CLOSE, 0, 0);
		} else if (x >= video->w - 60) {
			if (state->maximized) {
				xdg_toplevel_unset_maximized(state->toplevel);
			} else {
				xdg_toplevel_set_maximized(state->toplevel);
			}
		} else {
			xdg_toplevel_move(state->toplevel, state->seat, serial);
		}

		return;
	}

	Uint8 mapped = 0;

	if (button == 0x110) {
		mapped = SDL_BUTTON_LEFT;
	} else if (button == 0x111) {
		mapped = SDL_BUTTON_RIGHT;
	} else if (button == 0x112) {
		mapped = SDL_BUTTON_MIDDLE;
	} else if (button == 0x113) {
		mapped = SDL_BUTTON_X1;
	} else if (button == 0x114) {
		mapped = SDL_BUTTON_X2;
	}

	if (mapped != 0 && state->video->window != NULL) {
		SDL_SendMouseButton(state->video->window, 0, pressed ? SDL_PRESSED : SDL_RELEASED, mapped);
	}
}

// Накапливать непрерывную прокрутку до границы группы событий
static void syn_wl_pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                                uint32_t axis, wl_fixed_t value) {
	(void)pointer;
	(void)time;
	struct SynapseWayland *state = data;

	if (axis < 2) {
		state->wheel[axis] += wl_fixed_to_double(value) / 10.0;
	}
}

// Доставить одно событие колеса, предпочитая точное число шагов
static void syn_wl_pointer_frame(void *data, struct wl_pointer *pointer) {
	(void)pointer;
	struct SynapseWayland *state = data;
	double vertical = state->has_discrete[0] ? state->discrete[0] : state->wheel[0];
	double horizontal = state->has_discrete[1] ? state->discrete[1] : state->wheel[1];

	if (state->video->window != NULL && (vertical != 0 || horizontal != 0)) {
		SDL_SendMouseWheel(state->video->window, 0, (float)horizontal, (float)-vertical,
		                   SDL_MOUSEWHEEL_NORMAL);
	}

	SDL_memset(state->wheel, 0, sizeof(state->wheel));
	SDL_memset(state->discrete, 0, sizeof(state->discrete));
	SDL_memset(state->has_discrete, 0, sizeof(state->has_discrete));
}

// Источник прокрутки не меняет направление осей
static void syn_wl_pointer_source(void *data, struct wl_pointer *pointer, uint32_t source) {
	(void)data;
	(void)pointer;
	(void)source;
}

// Завершение плавной прокрутки не создаёт дополнительного шага
static void syn_wl_pointer_stop(void *data, struct wl_pointer *pointer, uint32_t time,
                                uint32_t axis) {
	(void)data;
	(void)pointer;
	(void)time;
	(void)axis;
}

// Сохранить дискретные шаги колеса отдельно от непрерывного значения
static void syn_wl_pointer_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
                                    int32_t steps) {
	(void)pointer;
	struct SynapseWayland *state = data;

	if (axis < 2) {
		state->discrete[axis] += steps;
		state->has_discrete[axis] = SDL_TRUE;
	}
}

static const struct wl_pointer_listener syn_wl_pointer_listener = {
	.enter = syn_wl_pointer_enter,
	.leave = syn_wl_pointer_leave,
	.motion = syn_wl_pointer_motion,
	.button = syn_wl_pointer_button,
	.axis = syn_wl_pointer_axis,
	.frame = syn_wl_pointer_frame,
	.axis_source = syn_wl_pointer_source,
	.axis_stop = syn_wl_pointer_stop,
	.axis_discrete = syn_wl_pointer_discrete
};

// Принять таблицу XKB, переданную сервером в отдельном файле
static void syn_wl_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd,
                          uint32_t length) {
	(void)keyboard;
	struct SynapseWayland *state = data;

	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || length == 0 || length > 4U * 1024U * 1024U) {
		close(fd);
		state->failed = SDL_TRUE;
		return;
	}

	char *map = mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);

	if (map == MAP_FAILED) {
		state->failed = SDL_TRUE;
		return;
	}

	struct xkb_keymap *keymap = NULL;

	if (map[length - 1] == 0) {
		keymap = xkb_keymap_new_from_string(state->xkb_context, map, XKB_KEYMAP_FORMAT_TEXT_V1,
		                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
	}

	munmap(map, length);
	struct xkb_state *key_state = keymap != NULL ? xkb_state_new(keymap) : NULL;

	if (key_state == NULL) {
		xkb_keymap_unref(keymap);
		state->failed = SDL_TRUE;
		return;
	}

	xkb_state_unref(state->key_state);
	xkb_keymap_unref(state->keymap);
	state->keymap = keymap;
	state->key_state = key_state;
}

// Передать фокус клавиатуры приложению
static void syn_wl_key_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                             struct wl_surface *surface, struct wl_array *keys) {
	(void)keyboard;
	(void)serial;
	(void)surface;
	(void)keys;
	struct SynapseWayland *state = data;

	if (state->video->window != NULL) {
		SDL_SetKeyboardFocus(state->video->window);
	}
}

// Отпустить удерживаемые клавиши при уходе фокуса
static void syn_wl_key_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                             struct wl_surface *surface) {
	struct SynapseWayland *state = data;
	state->repeating = SDL_FALSE;
	(void)keyboard;
	(void)serial;
	(void)surface;
	SDL_ResetKeyboard( );
	SDL_SetKeyboardFocus(NULL);
}

// Передать физическую клавишу и текст без изменения времени повтора
static void syn_wl_deliver_key(struct SynapseWayland *state, uint32_t key, uint32_t pressed) {
	SDL_SendKeyboardKey(pressed ? SDL_PRESSED : SDL_RELEASED, linux_scancode_table[key]);

	if (pressed && state->key_state != NULL &&
	    (SDL_GetModState( ) & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) == 0) {
		char text[32];
		int length = xkb_state_key_get_utf8(state->key_state, key + 8U, text, sizeof(text));

		if (length > 0 && (size_t)length < sizeof(text) && (unsigned char)text[0] >= 32) {
			SDL_SendKeyboardText(text);
		}
	}
}

// Повторять удерживаемую клавишу без отдельного потока
static void syn_wl_repeat_tick(struct SynapseWayland *state) {
	Uint32 now = SDL_GetTicks( );

	if (state->repeating && state->repeat_rate > 0 && SDL_TICKS_PASSED(now, state->repeat_next)) {
		syn_wl_deliver_key(state, state->repeat_key, 1);
		state->repeat_next = now + (Uint32)SDL_max(1, 1000 / state->repeat_rate);
	}
}

// Начать или остановить повтор по данным клавиатуры сервера
static void syn_wl_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
                       uint32_t key, uint32_t pressed) {
	(void)keyboard;
	(void)serial;
	(void)time;
	struct SynapseWayland *state = data;

	if (state->video->window == NULL || key >= SDL_arraysize(linux_scancode_table)) {
		return;
	}

	syn_wl_deliver_key(state, key, pressed);

	if (!pressed && state->repeat_key == key) {
		state->repeating = SDL_FALSE;
	} else if (pressed && state->keymap != NULL &&
	           xkb_keymap_key_repeats(state->keymap, key + 8U)) {
		state->repeat_key = key;
		state->repeat_next = SDL_GetTicks( ) + (Uint32)state->repeat_delay;
		state->repeating = state->repeat_rate > 0 ? SDL_TRUE : SDL_FALSE;
	}
}

// Применить модификаторы и активную раскладку из состояния сервера
static void syn_wl_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                             uint32_t depressed, uint32_t latched, uint32_t locked,
                             uint32_t group) {
	(void)keyboard;
	(void)serial;
	struct SynapseWayland *state = data;

	if (state->key_state != NULL) {
		xkb_state_update_mask(state->key_state, depressed, latched, locked, 0, 0, group);
		state->video->russian = group == 1;
		state->video->hud_time = 0;
	}
}

// Принять частоту и задержку повтора, отключение применяется сразу
static void syn_wl_repeat(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {
	(void)keyboard;
	struct SynapseWayland *state = data;
	state->repeat_rate = SDL_clamp(rate, 0, 100);
	state->repeat_delay = SDL_clamp(delay, 0, 2000);

	if (state->repeat_rate == 0) {
		state->repeating = SDL_FALSE;
	}
}

static const struct wl_keyboard_listener syn_wl_keyboard_listener = { .keymap = syn_wl_keymap,
	                                                                  .enter = syn_wl_key_enter,
	                                                                  .leave = syn_wl_key_leave,
	                                                                  .key = syn_wl_key,
	                                                                  .modifiers = syn_wl_modifiers,
	                                                                  .repeat_info =
	                                                                      syn_wl_repeat };

// Создать устройства ввода, объявленные местом пользователя
static void syn_wl_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
	struct SynapseWayland *state = data;

	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && state->pointer == NULL) {
		state->pointer = wl_seat_get_pointer(seat);

		if (state->pointer != NULL) {
			wl_pointer_add_listener(state->pointer, &syn_wl_pointer_listener, state);
		}
	}

	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && state->keyboard == NULL) {
		state->keyboard = wl_seat_get_keyboard(seat);

		if (state->keyboard != NULL) {
			wl_keyboard_add_listener(state->keyboard, &syn_wl_keyboard_listener, state);
		}
	}
}

// Имя места не влияет на выбор окна
static void syn_wl_seat_name(void *data, struct wl_seat *seat, const char *name) {
	(void)data;
	(void)seat;
	(void)name;
}

static const struct wl_seat_listener syn_wl_seat_listener = { syn_wl_capabilities,
	                                                          syn_wl_seat_name };

// Геометрия в миллиметрах не меняет размер программного экрана
static void syn_wl_output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                                   int32_t physical_width, int32_t physical_height,
                                   int32_t subpixel, const char *make, const char *model,
                                   int32_t transform) {
	(void)data;
	(void)output;
	(void)x;
	(void)y;
	(void)physical_width;
	(void)physical_height;
	(void)subpixel;
	(void)make;
	(void)model;
	(void)transform;
}

// Сохранить фактический режим экрана из Weston
static void syn_wl_output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width,
                               int32_t height, int32_t refresh) {
	(void)output;
	struct SynapseWayland *state = data;

	if ((flags & WL_OUTPUT_MODE_CURRENT) != 0 && width > 0 && height > 0 && width <= 16384 &&
	    height <= 16384) {
		state->output_width = width;
		state->output_height = height;
		state->output_refresh = refresh > 0 ? refresh / 1000 : 60;
	}
}

static const struct wl_output_listener syn_wl_output_listener = { .geometry =
	                                                                  syn_wl_output_geometry,
	                                                              .mode = syn_wl_output_mode };

// Подключить необходимые глобальные объекты протокола
static void syn_wl_global(void *data, struct wl_registry *registry, uint32_t name,
                          const char *interface, uint32_t version) {
	struct SynapseWayland *state = data;

	if (SDL_strcmp(interface, "wl_compositor") == 0 && state->compositor == NULL) {
		state->compositor =
		    wl_registry_bind(registry, name, &wl_compositor_interface, SDL_min(version, 4));
	} else if (SDL_strcmp(interface, "wl_output") == 0 && state->output == NULL) {
		state->output = wl_registry_bind(registry, name, &wl_output_interface, 1);

		if (state->output != NULL) {
			wl_output_add_listener(state->output, &syn_wl_output_listener, state);
		}
	} else if (SDL_strcmp(interface, "wl_shm") == 0 && state->shm == NULL) {
		state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (SDL_strcmp(interface, "xdg_wm_base") == 0 && state->shell == NULL) {
		state->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);

		if (state->shell != NULL) {
			xdg_wm_base_add_listener(state->shell, &syn_wl_shell_listener, state);
		}
	} else if (SDL_strcmp(interface, "wl_seat") == 0 && state->seat == NULL && version >= 5) {
		state->seat = wl_registry_bind(registry, name, &wl_seat_interface, SDL_min(version, 7));

		if (state->seat != NULL) {
			wl_seat_add_listener(state->seat, &syn_wl_seat_listener, state);
		}
	}
}

// Удаление имён не меняет уже привязанные объекты
static void syn_wl_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener syn_wl_registry_listener = { syn_wl_global,
	                                                                  syn_wl_global_remove };

// Освободить объекты окна до закрытия соединения с сервером
static void syn_wl_window_destroy(SynapseVideo *video) {
	struct SynapseWayland *state = video->wayland;
	syn_wl_buffers_destroy(state);

	if (state->toplevel != NULL) {
		xdg_toplevel_destroy(state->toplevel);
		state->toplevel = NULL;
	}

	if (state->xdg_surface != NULL) {
		xdg_surface_destroy(state->xdg_surface);
		state->xdg_surface = NULL;
	}

	if (state->surface != NULL) {
		wl_surface_destroy(state->surface);
		state->surface = NULL;
	}

	SDL_free(video->pixels);
	video->pixels = NULL;
	video->window = NULL;
	state->configured = SDL_FALSE;
}

// Завершить сеанс, не обращаясь к framebuffer и прямому вводу ядра
static void syn_wl_quit(SynapseVideo *video) {
	struct SynapseWayland *state = video->wayland;

	if (state == NULL) {
		return;
	}

	syn_wl_cursor_video = NULL;

	if (state->cursor_surface != NULL) {
		wl_surface_destroy(state->cursor_surface);
	}

	if (state->cursor_theme != NULL) {
		wl_cursor_theme_destroy(state->cursor_theme);
	}

	syn_wl_window_destroy(video);

	if (state->keyboard != NULL) {
		wl_keyboard_destroy(state->keyboard);
	}

	if (state->pointer != NULL) {
		wl_pointer_destroy(state->pointer);
	}

	if (state->seat != NULL) {
		wl_seat_destroy(state->seat);
	}

	if (state->shell != NULL) {
		xdg_wm_base_destroy(state->shell);
	}

	if (state->shm != NULL) {
		wl_shm_destroy(state->shm);
	}

	if (state->compositor != NULL) {
		wl_compositor_destroy(state->compositor);
	}

	if (state->output != NULL) {
		wl_output_destroy(state->output);
	}

	if (state->registry != NULL) {
		wl_registry_destroy(state->registry);
	}

	if (state->display != NULL) {
		wl_display_disconnect(state->display);
	}

	xkb_state_unref(state->key_state);
	xkb_keymap_unref(state->keymap);
	xkb_context_unref(state->xkb_context);
	SDL_free(state);
	video->wayland = NULL;
}

// Подготовить стандартное соединение Wayland и описание экрана SDL
static int syn_wl_init(_THIS) {
	SynapseVideo *video = _this->driverdata;
	struct SynapseWayland *state = SDL_calloc(1, sizeof(*state));

	if (state == NULL) {
		return SDL_OutOfMemory( );
	}

	video->wayland = state;
	state->video = video;
	state->border = 4;
	state->caption = 32;
	state->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	state->display = wl_display_connect(NULL);

	if (state->display == NULL || state->xkb_context == NULL) {
		syn_wl_quit(video);
		return SDL_SetError("Wayland compositor unavailable");
	}

	state->registry = wl_display_get_registry(state->display);

	if (state->registry == NULL) {
		syn_wl_quit(video);
		return SDL_OutOfMemory( );
	}

	wl_registry_add_listener(state->registry, &syn_wl_registry_listener, state);

	if (wl_display_roundtrip(state->display) < 0 || wl_display_roundtrip(state->display) < 0 ||
	    state->compositor == NULL || state->shm == NULL || state->shell == NULL ||
	    state->seat == NULL) {
		syn_wl_quit(video);
		return SDL_SetError("Required Wayland interfaces unavailable");
	}

	SDL_DisplayMode mode;
	SDL_zero(mode);
	mode.format = SDL_PIXELFORMAT_RGB888;
	mode.w = state->output_width > 0 ? state->output_width : 1280;
	mode.h = state->output_height > 0 ? state->output_height : 800;
	mode.refresh_rate = state->output_refresh > 0 ? state->output_refresh : 60;
	video->var.xres = mode.w;
	video->var.yres = mode.h;

	if (SDL_AddBasicVideoDisplay(&mode) < 0) {
		syn_wl_quit(video);
		return -1;
	}

	const char *theme = SDL_getenv("XCURSOR_THEME");
	const char *size = SDL_getenv("XCURSOR_SIZE");
	int cursor_size = size != NULL ? SDL_atoi(size) : 32;
	cursor_size = SDL_clamp(cursor_size, 8, 96);
	state->cursor_theme =
	    wl_cursor_theme_load(theme != NULL ? theme : "breeze_cursors", cursor_size, state->shm);
	state->cursor_surface = wl_compositor_create_surface(state->compositor);

	if (state->cursor_theme == NULL || state->cursor_surface == NULL) {
		syn_wl_quit(video);
		return SDL_SetError("Desktop cursor theme unavailable");
	}

	syn_wl_cursor_video = video;
	SDL_Mouse *mouse = SDL_GetMouse( );
	mouse->CreateCursor = syn_wl_custom_cursor;
	mouse->CreateSystemCursor = syn_wl_system_cursor;
	mouse->ShowCursor = syn_wl_show_cursor;
	mouse->FreeCursor = syn_wl_free_cursor;
	SDL_SetDefaultCursor(syn_wl_system_cursor(SDL_SYSTEM_CURSOR_ARROW));
	SDL_AddDisplayMode(&_this->displays[0], &mode);
	SDL_Log("Synapse SDL: Wayland xdg-shell, OSMesa and shared buffers");
	return 0;
}

// Создать обычное окно XDG и дождаться первой настройки сервера
static int syn_wl_window_create(_THIS, SDL_Window *window) {
	SynapseVideo *video = _this->driverdata;
	struct SynapseWayland *state = video->wayland;

	if (video->window != NULL) {
		return SDL_SetError("One SDL window per process is supported");
	}

	video->window = window;
	window->driverdata = video;

	if (syn_wl_resize(video, SDL_max(window->w, 1), SDL_max(window->h, 1)) != 0) {
		syn_wl_window_destroy(video);
		return -1;
	}

	state->surface = wl_compositor_create_surface(state->compositor);

	if (state->surface != NULL) {
		state->xdg_surface = xdg_wm_base_get_xdg_surface(state->shell, state->surface);
	}

	if (state->xdg_surface != NULL) {
		state->toplevel = xdg_surface_get_toplevel(state->xdg_surface);
	}

	if (state->toplevel == NULL) {
		syn_wl_window_destroy(video);
		return SDL_OutOfMemory( );
	}

	xdg_surface_add_listener(state->xdg_surface, &syn_wl_surface_listener, state);
	xdg_toplevel_add_listener(state->toplevel, &syn_wl_top_listener, state);
	xdg_toplevel_set_title(state->toplevel,
	                       window->title != NULL ? window->title : "SynapseOS SDL");
	xdg_toplevel_set_app_id(state->toplevel, "synapse.sdl");
	wl_surface_commit(state->surface);

	while (!state->configured && !state->failed) {
		if (wl_display_dispatch(state->display) < 0) {
			state->failed = SDL_TRUE;
		}
	}

	if (state->failed) {
		syn_wl_window_destroy(video);
		return SDL_SetError("Wayland initial configuration failed");
	}

	return 0;
}

// Нарисовать заголовок и кнопки средствами уже загруженного шрифта
static void syn_wl_caption(SynapseVideo *video, Uint32 *pixels, int width) {
	struct SynapseWayland *state = video->wayland;

	if (state->caption == 0) {
		return;
	}

	const char *title = video->window->title != NULL ? video->window->title : "SynapseOS SDL";

	for (int y = 0; y < state->caption; y++) {
		for (int x = 0; x < width; x++) {
			pixels[y * width + x] = y < 4 ? 0x526579 : 0x26384b;
		}
	}

	if (width < 72) {
		return;
	}

	if (video->hud_font != NULL) {
		int y0 = SDL_max(4, (state->caption - (int)video->hud_height) / 2);

		for (unsigned i = 0; title[i] != 0 && (i + 2) * video->hud_width < (unsigned)(width - 72);
		     i++) {
			unsigned ch = (unsigned char)title[i];
			unsigned glyph = video->hud_glyph[ch < 128 ? ch : '?'];

			for (unsigned y = 0; y < video->hud_height && y0 + y < (unsigned)state->caption; y++) {
				const unsigned char *bits =
				    video->hud_font + glyph * video->hud_bytes + y * video->hud_stride;
				for (unsigned x = 0; x < video->hud_width; x++) {
					if ((bits[x / 8] & (0x80U >> (x % 8))) != 0) {
						pixels[(y0 + y) * width + 8 + i * video->hud_width + x] = 0xf0f4f8;
					}
				}
			}
		}
	}

	for (int i = 0; i < 12; i++) {
		pixels[(10 + i) * width + width - 24 + i] = 0xffffff;
		pixels[(10 + i) * width + width - 13 - i] = 0xffffff;
		pixels[10 * width + width - 56 + i] = 0xffffff;
		pixels[21 * width + width - 56 + i] = 0xffffff;
		pixels[(10 + i) * width + width - 56] = 0xffffff;
		pixels[(10 + i) * width + width - 45] = 0xffffff;
	}
}

// Передать готовый кадр, не перезаписывая ещё занятый сервером буфер
static int syn_wl_present(SynapseVideo *video) {
	struct SynapseWayland *state = video->wayland;

	if (syn_wl_dispatch(video) != 0) {
		return -1;
	}

	struct SynapseWaylandBuffer *slot = NULL;
	Uint32 started = SDL_GetTicks( );

	while (slot == NULL && !state->failed) {
		for (unsigned i = 0; i < SDL_arraysize(state->buffers); i++) {
			if (!state->buffers[i].busy) {
				slot = &state->buffers[i];
				break;
			}
		}

		if (slot != NULL) {
			break;
		}

		if (SDL_GetTicks( ) - started > 2000) {
			return SDL_SetError("Wayland buffer release timed out");
		}

		SDL_Delay(1);

		if (syn_wl_dispatch(video) != 0) {
			return -1;
		}
	}

	if (slot == NULL) {
		return SDL_SetError("Wayland frame unavailable");
	}

	int width = video->w + 2 * state->border;
	int height = video->h + state->caption + state->border;

	if (slot->buffer == NULL && syn_wl_buffer_create(state, slot, width, height) != 0) {
		return -1;
	}

	Uint32 *frame = (Uint32 *)slot->pixels;

	for (size_t i = 0; i < slot->length / 4; i++) {
		frame[i] = 0x526579;
	}

	for (int y = 0; y < video->h; y++) {
		SDL_memcpy(slot->pixels + ((size_t)(y + state->caption) * width + state->border) * 4,
		           video->pixels + (size_t)y * video->w * 4, (size_t)video->w * 4);
	}

	syn_wl_caption(video, frame, width);
	unsigned char *saved_screen = video->screen;
	unsigned saved_stride = video->fix.line_length;
	video->screen = slot->pixels + ((size_t)state->caption * width + state->border) * 4;
	video->fix.line_length = (unsigned)width * 4;

	if ((video->window->flags & SDL_WINDOW_OPENGL) != 0) {
		hud_draw(video);
	}

	video->fix.line_length = saved_stride;
	video->screen = saved_screen;

	slot->busy = SDL_TRUE;
	wl_surface_attach(state->surface, slot->buffer, 0, 0);
	xdg_surface_set_window_geometry(state->xdg_surface, 0, 0, width, height);
	wl_surface_damage(state->surface, 0, 0, width, height);
	wl_surface_commit(state->surface);

	if (wl_display_flush(state->display) < 0 && errno != EAGAIN) {
		state->failed = SDL_TRUE;
		return SDL_SetError("Wayland frame flush failed");
	}

	return 0;
}
