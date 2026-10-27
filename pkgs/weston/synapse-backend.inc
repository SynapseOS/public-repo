// synapse-backend.inc: вывод Weston в framebuffer и ввод SynapseOS
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl, SynAPI и libweston 13
// Подсистема: оконная система
// Первое изменение: 17.10.2026, последнее изменение: 27.10.2026
// Статус доступа: общедоступный, MIT

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <synapse/gpu.h>
#include <linux/input-event-codes.h>
#include <lumine/pointer.h>
#include "lumine_fb.h"
#include "lumine_hid.h"
#include "libweston/backend.h"
#include "libweston/libweston-internal.h"

static bool synapse_modeset_supported;
static uint32_t synapse_initial_width, synapse_initial_height;
static uint32_t synapse_attempt_width, synapse_attempt_height;
static struct weston_mode synapse_requested_mode;
static struct wl_event_source *synapse_mode_timer;
static uint8_t *synapse_gl_pixels;
static size_t synapse_gl_pixels_size;

// Выбрать GL при наличии render-узла и сохранить явный выбор пользователя
static enum weston_renderer_type synapse_renderer(enum weston_renderer_type requested) {
	if (requested != WESTON_RENDERER_AUTO) {
		return requested;
	}

	int render = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (render >= 0) {
		close(render);
		return WESTON_RENDERER_GL;
	}

	return WESTON_RENDERER_PIXMAN;
}

// Преобразовать физическую клавишу SynAPI в код evdev для XKB
static uint32_t synapse_key(uint16_t key) {
	static const uint16_t special[] = { 0,
		                                KEY_ESC,
		                                KEY_TAB,
		                                KEY_CAPSLOCK,
		                                KEY_LEFTSHIFT,
		                                KEY_RIGHTSHIFT,
		                                KEY_LEFTCTRL,
		                                KEY_RIGHTCTRL,
		                                KEY_LEFTALT,
		                                KEY_RIGHTALT,
		                                KEY_ENTER,
		                                KEY_BACKSPACE,
		                                KEY_SPACE,
		                                KEY_INSERT,
		                                KEY_DELETE,
		                                KEY_HOME,
		                                KEY_END,
		                                KEY_PAGEUP,
		                                KEY_PAGEDOWN,
		                                KEY_LEFT,
		                                KEY_UP,
		                                KEY_RIGHT,
		                                KEY_DOWN,
		                                KEY_F1,
		                                KEY_F2,
		                                KEY_F3,
		                                KEY_F4,
		                                KEY_F5,
		                                KEY_F6,
		                                KEY_F7,
		                                KEY_F8 };
	static const char letters[] = "1234567890-=QWERTYUIOP[]ASDFGHJKL;'`\\ZXCVBNM,./";
	static const uint16_t codes[] = {
		KEY_1,         KEY_2, KEY_3,     KEY_4,         KEY_5,          KEY_6,
		KEY_7,         KEY_8, KEY_9,     KEY_0,         KEY_MINUS,      KEY_EQUAL,
		KEY_Q,         KEY_W, KEY_E,     KEY_R,         KEY_T,          KEY_Y,
		KEY_U,         KEY_I, KEY_O,     KEY_P,         KEY_LEFTBRACE,  KEY_RIGHTBRACE,
		KEY_A,         KEY_S, KEY_D,     KEY_F,         KEY_G,          KEY_H,
		KEY_J,         KEY_K, KEY_L,     KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE,
		KEY_BACKSLASH, KEY_Z, KEY_X,     KEY_C,         KEY_V,          KEY_B,
		KEY_N,         KEY_M, KEY_COMMA, KEY_DOT,       KEY_SLASH
	};

	if (key < ARRAY_LENGTH(special)) {
		return special[key];
	}

	static const uint16_t upper_special[] = { KEY_F9, KEY_F10, KEY_F11, KEY_F12,
		KEY_NUMLOCK, KEY_SCROLLLOCK, KEY_SYSRQ, KEY_LEFTMETA, KEY_RIGHTMETA };
	if (key >= 0x110 && key < 0x110 + ARRAY_LENGTH(upper_special)) {
		return upper_special[key - 0x110];
	}

	for (size_t i = 0; i < sizeof(letters) - 1; i++) {
		if ((uint16_t)(unsigned char)letters[i] == key) {
			return codes[i];
		}
	}

	return 0;
}

// Передать прокрутку как шаги колеса и непрерывное значение Wayland
static void synapse_axis(struct headless_backend *backend, const struct timespec *time,
                         uint32_t axis, int64_t steps) {
	if (steps == 0) {
		return;
	}

	if (steps > 120) {
		steps = 120;
	}

	if (steps < -120) {
		steps = -120;
	}

	struct weston_pointer_axis_event event = { .axis = axis,
		                                       .value = (double)steps * 10.0,
		                                       .has_discrete = true,
		                                       .discrete = (int32_t)steps };
	notify_axis_source(&backend->fake_seat, WL_POINTER_AXIS_SOURCE_WHEEL);
	notify_axis(&backend->fake_seat, time, &event);
}

// Опросить ограниченное число событий, не задерживая обработку клиентов
static int synapse_input(void *data) {
	struct headless_backend *backend = data;
	static bool first = true;

	if (first) {
		weston_log("Synapse first input dispatch\n");
		first = false;
	}

	struct timespec time;
	kbd_event_t key;
	clock_gettime(CLOCK_MONOTONIC, &time);

	for (unsigned i = 0; i < 256 && syscall(102000, &key) == 0; i++) {
		uint32_t code = synapse_key(key.keycode);

		if (code == 0) {
			continue;
		}

		if (key.is_pressed && code == KEY_ESC && (key.modifiers & 12) != 0 &&
		    (key.modifiers & 48) != 0) {
			weston_compositor_exit(backend->compositor);
			return 0;
		}

		notify_key(&backend->fake_seat, &time, code,
		           key.is_pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
		           STATE_UPDATE_AUTOMATIC);
	}

	struct syn_pointer_event pointer;

	for (unsigned i = 0;
	     i < 256 && read(backend->pointer_fd, &pointer, sizeof(pointer)) == sizeof(pointer); i++) {
		if (pointer.version != SYN_POINTER_VERSION || pointer.device >= SYN_POINTER_MAX ||
		    (pointer.flags & ~SYN_POINTER_FLAGS_MASK) != 0) {
			continue;
		}

		if ((pointer.flags & SYN_POINTER_WHEEL) != 0) {
			synapse_axis(backend, &time, WL_POINTER_AXIS_HORIZONTAL_SCROLL, pointer.x);
			synapse_axis(backend, &time, WL_POINTER_AXIS_VERTICAL_SCROLL, -(int64_t)pointer.y);
		} else if ((pointer.flags & SYN_POINTER_DISCONNECTED) == 0) {
			struct weston_pointer_motion_event motion = { 0 };
			motion.time = time;

			if ((pointer.flags & SYN_POINTER_ABSOLUTE) != 0) {
				double old_x = backend->mouse_x;
				double old_y = backend->mouse_y;

				backend->mouse_x = (double)pointer.x * (backend->width - 1) / 65535.0;
				backend->mouse_y = (double)pointer.y * (backend->height - 1) / 65535.0;
				motion.mask = WESTON_POINTER_MOTION_ABS | WESTON_POINTER_MOTION_REL;
				motion.rel = weston_coord(backend->mouse_x - old_x, backend->mouse_y - old_y);
			} else {
				backend->mouse_x += pointer.x;
				backend->mouse_y += pointer.y;
				motion.mask = WESTON_POINTER_MOTION_REL;
				motion.rel = weston_coord(pointer.x, pointer.y);
			}

			if (backend->mouse_x < 0) {
				backend->mouse_x = 0;
			}

			if (backend->mouse_y < 0) {
				backend->mouse_y = 0;
			}

			if (backend->mouse_x >= backend->width) {
				backend->mouse_x = backend->width - 1;
			}

			if (backend->mouse_y >= backend->height) {
				backend->mouse_y = backend->height - 1;
			}

			motion.abs.c = weston_coord(backend->mouse_x, backend->mouse_y);
			notify_motion(&backend->fake_seat, &time, &motion);
		}

		static const uint16_t buttons[] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA };
		uint32_t state = (pointer.flags & SYN_POINTER_DISCONNECTED) != 0 ? 0 : pointer.buttons;
		backend->device_buttons[pointer.device] = state & SYN_POINTER_BUTTONS_MASK;
		state = 0;

		for (unsigned device = 0; device < SYN_POINTER_MAX; device++) {
			state |= backend->device_buttons[device];
		}

		for (unsigned button = 0; button < ARRAY_LENGTH(buttons); button++) {
			uint32_t bit = 1U << button;

			if (((backend->buttons ^ state) & bit) != 0) {
				notify_button(&backend->fake_seat, &time, buttons[button],
				              (state & bit) != 0 ? WL_POINTER_BUTTON_STATE_PRESSED
				                                 : WL_POINTER_BUTTON_STATE_RELEASED);
			}
		}

		backend->buttons = state;
		notify_pointer_frame(&backend->fake_seat);
	}

	wl_event_source_timer_update(backend->input_timer, 8);
	return 0;
}

// Закрыть ввод и вернуть владение экраном консоли
static void synapse_close(struct headless_backend *backend) {
	free(synapse_gl_pixels);
	synapse_gl_pixels = NULL;
	synapse_gl_pixels_size = 0;

	if (synapse_mode_timer != NULL) {
		wl_event_source_remove(synapse_mode_timer);
		synapse_mode_timer = NULL;
	}

	if (backend->input_timer != NULL) {
		wl_event_source_remove(backend->input_timer);
		backend->input_timer = NULL;
	}

	if (backend->seat_ready) {
		weston_seat_release(&backend->fake_seat);
		backend->seat_ready = false;
	}

	if (backend->pointer_fd >= 0) {
		close(backend->pointer_fd);
		backend->pointer_fd = -1;
	}

	if (backend->screen != NULL) {
		munmap(backend->screen, backend->screen_bytes);
		backend->screen = NULL;
	}

	if (backend->fb_fd >= 0) {
		if (backend->claimed) {
			ioctl(backend->fb_fd, 0x4627, 0);
			backend->claimed = false;
		}

		close(backend->fb_fd);
		backend->fb_fd = -1;
	}
}

// Проверить формат, получить экран и подготовить место ввода
static int synapse_open(struct headless_backend *backend) {
	struct fb_var_screeninfo variable;
	struct fb_fix_screeninfo fixed;
	backend->fb_fd = -1;
	backend->pointer_fd = -1;
	backend->fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);

	if (backend->fb_fd < 0 || ioctl(backend->fb_fd, 0x4600, &variable) != 0 ||
	    ioctl(backend->fb_fd, 0x4602, &fixed) != 0) {
		goto fail;
	}

	if (variable.bits_per_pixel != 32 || variable.red.offset != 16 || variable.green.offset != 8 ||
	    variable.blue.offset != 0 || variable.xres == 0 || variable.yres == 0 ||
	    variable.xres > 16384 || variable.yres > 16384 || fixed.line_length < variable.xres * 4U ||
	    fixed.line_length > 262144U) {
		errno = ENOTSUP;
		goto fail;
	}

	backend->width = variable.xres;
	backend->height = variable.yres;
	backend->stride = fixed.line_length;
	synapse_initial_width = backend->width;
	synapse_initial_height = backend->height;
	synapse_attempt_width = 0;
	synapse_attempt_height = 0;
	struct syn_fb_display_v1 display_info;
	synapse_modeset_supported = ioctl(backend->fb_fd, SYN_FB_DISPLAY_QUERY_V1, &display_info) == 0 &&
	                           (display_info.capabilities & SYN_FB_MODESET) != 0;
	backend->screen_bytes = (size_t)backend->stride * backend->height;
	void *screen =
	    mmap(NULL, backend->screen_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, backend->fb_fd, 0);
	if (screen == MAP_FAILED) {
		goto fail;
	}

	backend->screen = screen;

	if (ioctl(backend->fb_fd, 0x4626, 0) != 0) {
		goto fail;
	}

	backend->claimed = true;
	backend->pointer_fd = open("/dev/pointers", O_RDONLY | O_NONBLOCK | O_CLOEXEC);

	if (backend->pointer_fd < 0) {
		goto fail;
	}

	weston_seat_init(&backend->fake_seat, backend->compositor, "synapse-seat");
	backend->seat_ready = true;

	if (weston_seat_init_pointer(&backend->fake_seat) < 0 ||
	    weston_seat_init_keyboard(&backend->fake_seat, NULL) < 0) {
		goto fail;
	}

	struct wl_event_loop *loop = wl_display_get_event_loop(backend->compositor->wl_display);
	backend->input_timer = wl_event_loop_add_timer(loop, synapse_input, backend);

	if (backend->input_timer == NULL) {
		goto fail;
	}

	wl_event_source_timer_update(backend->input_timer, 8);
	weston_log("Synapse framebuffer: %ux%u, stride %u, keyboard and pointer\n",
	           backend->width, backend->height, backend->stride);
	return 0;

fail:
	weston_log("Synapse framebuffer/input failed: %s\n", strerror(errno));
	synapse_close(backend);
	return -1;
}

// Считать кадр GL и передать повреждённые прямоугольники в framebuffer
static void synapse_present_gl(struct headless_output *output, pixman_region32_t *damage) {
	struct headless_backend *backend = output->backend;
	struct weston_compositor *compositor = output->base.compositor;
	const struct pixel_format_info *format = compositor->read_format;
	int count = 0;
	pixman_box32_t *rectangles = pixman_region32_rectangles(damage, &count);

	if (count == 0 || format == NULL || backend->height == 0 ||
	    backend->width > INT_MAX || backend->height > INT_MAX) {
		return;
	}

	if (format->format != DRM_FORMAT_ARGB8888 && format->format != DRM_FORMAT_ABGR8888) {
		weston_log("Synapse cannot present GL format %s\n", format->drm_format_name);
		return;
	}

	const pixman_box32_t *extent = pixman_region32_extents(damage);
	int left = extent->x1 > 0 ? extent->x1 : 0;
	int top = extent->y1 > 0 ? extent->y1 : 0;
	int read_right = extent->x2 < (int)backend->width ? extent->x2 : (int)backend->width;
	int read_bottom = extent->y2 < (int)backend->height ? extent->y2 : (int)backend->height;
	if (read_right <= left || read_bottom <= top) {
		return;
	}

	size_t read_width = (size_t)(read_right - left);
	size_t read_height = (size_t)(read_bottom - top);
	if (read_width > SIZE_MAX / 4U / read_height) {
		return;
	}

	size_t bytes = read_width * read_height * 4U;
	if (bytes > synapse_gl_pixels_size) {
		uint8_t *pixels = realloc(synapse_gl_pixels, bytes);
		if (pixels == NULL) {
			weston_log("Synapse cannot allocate GL readback buffer\n");
			return;
		}

		synapse_gl_pixels = pixels;
		synapse_gl_pixels_size = bytes;
	}

	if (compositor->renderer->read_pixels(&output->base, format, synapse_gl_pixels,
	                                      (uint32_t)left, backend->height - (uint32_t)read_bottom,
	                                      (uint32_t)read_width, (uint32_t)read_height) != 0) {
		weston_log("Synapse GL frame readback failed\n");
		return;
	}

	for (int i = 0; i < count; i++) {
		int x = rectangles[i].x1 > 0 ? rectangles[i].x1 : 0;
		int y = rectangles[i].y1 > 0 ? rectangles[i].y1 : 0;
		int right = rectangles[i].x2 < (int)backend->width ? rectangles[i].x2 : (int)backend->width;
		int bottom = rectangles[i].y2 < (int)backend->height ? rectangles[i].y2 : (int)backend->height;
		if (right <= x || bottom <= y) {
			continue;
		}

		for (int row = y; row < bottom; row++) {
			uint8_t *target = backend->screen + (size_t)row * backend->stride + (size_t)x * 4U;
			const uint8_t *source = synapse_gl_pixels +
			                        ((size_t)read_bottom - 1U - (size_t)row) * read_width * 4U +
			                        ((size_t)x - (size_t)left) * 4U;

			if (format->format == DRM_FORMAT_ARGB8888) {
				memcpy(target, source, (size_t)(right - x) * 4U);
			} else {
				for (int column = x; column < right; column++) {
					target[0] = source[2];
					target[1] = source[1];
					target[2] = source[0];
					target[3] = 0xffU;
					target += 4;
					source += 4;
				}
			}
		}

		struct {
			uint32_t x, y, width, height;
		} rectangle = { (uint32_t)x, (uint32_t)y, (uint32_t)(right - x), (uint32_t)(bottom - y) };
		ioctl(backend->fb_fd, 0x4621, &rectangle);
	}

	ioctl(backend->fb_fd, 0x4622, 0);
}

// Скопировать только повреждённые прямоугольники готового изображения
static void synapse_present(struct headless_output *output, pixman_region32_t *damage) {
	struct headless_backend *backend = output->backend;
	static bool first = true;
	if (backend->screen == NULL || output->renderbuffer == NULL) {
		return;
	}

	if (first) {
		weston_log("Synapse first completed frame\n");
		first = false;
	}

	if (output->base.compositor->renderer->type == WESTON_RENDERER_GL) {
		synapse_present_gl(output, damage);
		return;
	}

	pixman_image_t *image =
	    output->base.compositor->renderer->pixman->renderbuffer_get_image(output->renderbuffer);
	if (image == NULL) {
		return;
	}

	const uint8_t *source = (const uint8_t *)pixman_image_get_data(image);
	int source_stride = pixman_image_get_stride(image);
	int count = 0;
	pixman_box32_t *rectangles = pixman_region32_rectangles(damage, &count);

	for (int i = 0; i < count; i++) {
		int x = rectangles[i].x1 > 0 ? rectangles[i].x1 : 0;
		int y = rectangles[i].y1 > 0 ? rectangles[i].y1 : 0;
		int right = rectangles[i].x2 < (int)backend->width ? rectangles[i].x2 : (int)backend->width;
		int bottom =
		    rectangles[i].y2 < (int)backend->height ? rectangles[i].y2 : (int)backend->height;
		if (right <= x || bottom <= y) {
			continue;
		}

		for (int row = y; row < bottom; row++) {
			memcpy(backend->screen + (size_t)row * backend->stride + (size_t)x * 4U,
			       source + (size_t)row * (size_t)source_stride + (size_t)x * 4U,
			       (size_t)(right - x) * 4U);
		}

		struct {
			uint32_t x, y, width, height;
		} rectangle = { (uint32_t)x, (uint32_t)y, (uint32_t)(right - x), (uint32_t)(bottom - y) };

		ioctl(backend->fb_fd, 0x4621, &rectangle);
	}

	if (count != 0) {
		ioctl(backend->fb_fd, 0x4622, 0);
	}
}

// Вернуть старый режим и отображение при отказе до перестройки Weston
static void synapse_restore_mode(struct headless_backend *backend, uint32_t width,
                                 uint32_t height, size_t bytes) {
	struct syn_fb_mode_v1 mode = {
		.version = 1,
		.width = width,
		.height = height,
		.format = SYN_FB_FORMAT_XRGB8888,
	};

	if (ioctl(backend->fb_fd, SYN_FB_MODE_SET_V1, &mode) < 0) {
		weston_log("Synapse could not restore display mode: %s\n", strerror(errno));
		weston_compositor_exit(backend->compositor);
		return;
	}

	void *screen = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, backend->fb_fd, 0);
	if (screen == MAP_FAILED) {
		weston_log("Synapse could not restore framebuffer mapping: %s\n", strerror(errno));
		weston_compositor_exit(backend->compositor);
		return;
	}

	backend->screen = screen;
}

// Поменять режим устройства и размер буфера Pixman в том же сеансе
static int synapse_output_switch_mode(struct weston_output *base, struct weston_mode *target) {
	struct headless_output *output = container_of(base, struct headless_output, base);
	struct headless_backend *backend = output->backend;
	const struct weston_renderer *renderer = base->compositor->renderer;
	const struct pixman_renderer_interface *pixman = renderer->pixman;
	uint32_t old_width = backend->width;
	uint32_t old_height = backend->height;
	size_t old_bytes = backend->screen_bytes;

	if (!synapse_modeset_supported || (renderer->type != WESTON_RENDERER_GL && pixman == NULL) ||
	    backend->screen == NULL ||
	    target->width < 320 || target->height < 200 || target->width > 3840 || target->height > 2160) {
		errno = ENOTSUP;
		return -1;
	}

	if (target->width == (int)old_width && target->height == (int)old_height) {
		return 0;
	}

	if (munmap(backend->screen, old_bytes) < 0) {
		return -1;
	}

	backend->screen = NULL;
	struct syn_fb_mode_v1 mode = {
		.version = 1,
		.width = target->width,
		.height = target->height,
		.format = SYN_FB_FORMAT_XRGB8888,
	};

	if (ioctl(backend->fb_fd, SYN_FB_MODE_SET_V1, &mode) < 0) {
		int saved = errno;
		synapse_restore_mode(backend, old_width, old_height, old_bytes);
		errno = saved;
		return -1;
	}

	struct syn_fb_display_v1 info;
	if (ioctl(backend->fb_fd, SYN_FB_DISPLAY_QUERY_V1, &info) < 0 ||
	    info.width != mode.width || info.height != mode.height ||
	    info.pitch != mode.width * 4U || info.buffer_bytes != (uint64_t)info.pitch * info.height) {
		synapse_restore_mode(backend, old_width, old_height, old_bytes);
		errno = EIO;
		return -1;
	}

	void *screen = mmap(NULL, info.buffer_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, backend->fb_fd, 0);
	if (screen == MAP_FAILED) {
		int saved = errno;
		synapse_restore_mode(backend, old_width, old_height, old_bytes);
		errno = saved;
		return -1;
	}

	backend->screen = screen;
	backend->screen_bytes = info.buffer_bytes;
	backend->width = info.width;
	backend->height = info.height;
	backend->stride = info.pitch;
	output->mode.width = info.width;
	output->mode.height = info.height;
	struct weston_size size = {.width = info.width, .height = info.height};
	weston_renderbuffer_unref(output->renderbuffer);
	weston_renderer_resize_output(base, &size, NULL);
	if (renderer->type == WESTON_RENDERER_GL) {
		output->renderbuffer = renderer->gl->create_fbo(base, pixel_format_get_info(DRM_FORMAT_ARGB8888),
		                                                info.width, info.height, NULL);
	} else {
		output->renderbuffer = pixman->create_image(base, pixel_format_get_info(DRM_FORMAT_XRGB8888),
		                                             info.width, info.height);
	}
	if (output->renderbuffer == NULL) {
		weston_log("Synapse could not allocate resized compositor buffer\n");
		weston_compositor_exit(backend->compositor);
		return -1;
	}

	weston_log("Synapse display switched to %ux%u without reboot\n", info.width, info.height);
	return 0;
}

// Прочитать только разрешение, сохранив совместимость с комментариями settings.ini
static bool synapse_read_resolution(uint32_t *width, uint32_t *height) {
	FILE *file = fopen("/etc/settings.ini", "r");
	if (file == NULL) {
		return false;
	}

	char line[256];
	bool display = false;
	bool found = false;
	while (fgets(line, sizeof(line), file) != NULL) {
		char *key = line;
		while (*key == ' ' || *key == '\t') {
			key++;
		}

		if (*key == '#' || *key == ';' || *key == '\n' || *key == 0) {
			continue;
		}

		if (*key == '[') {
			display = strncmp(key, "[Display]", 9) == 0;
			continue;
		}

		if (!display) {
			continue;
		}

		char *equal = strchr(key, '=');
		if (equal == NULL) {
			continue;
		}

		char *end = equal;
		while (end > key && (end[-1] == ' ' || end[-1] == '\t')) {
			end--;
		}

		if ((size_t)(end - key) != 10 || strncmp(key, "Resolution", 10) != 0) {
			continue;
		}

		char *value = equal + 1;
		while (*value == ' ' || *value == '\t') {
			value++;
		}

		value[strcspn(value, "\r\n")] = 0;
		if (strcmp(value, "auto") == 0) {
			*width = synapse_initial_width;
			*height = synapse_initial_height;
			found = true;
			break;
		}

		unsigned parsed_width = 0;
		unsigned parsed_height = 0;
		char extra;
		if (sscanf(value, "%ux%u%c", &parsed_width, &parsed_height, &extra) == 2) {
			*width = parsed_width;
			*height = parsed_height;
			found = true;
		}

		break;
	}

	fclose(file);
	return found;
}

// Изменить вывод после сохранения настройки без перезапуска compositor
static int synapse_mode_poll(void *data) {
	struct headless_output *output = data;
	uint32_t width = 0;
	uint32_t height = 0;
	if (output->base.enabled && synapse_read_resolution(&width, &height) &&
	    width >= 320 && height >= 200 && width <= 3840 && height <= 2160 &&
	    (width != synapse_attempt_width || height != synapse_attempt_height)) {
		synapse_attempt_width = width;
		synapse_attempt_height = height;
		synapse_requested_mode = (struct weston_mode){
			.width = width, .height = height, .refresh = 60000,
		};
		if (weston_output_mode_set_native(&output->base, &synapse_requested_mode, 1) < 0) {
			weston_log("Synapse display mode %ux%u failed: %s\n", width, height, strerror(errno));
		}
	}

	wl_event_source_timer_update(synapse_mode_timer, 500);
	return 0;
}

// Следить за изменениями настройки во время работы Weston
static void synapse_mode_watch(struct headless_output *output) {
	if (!synapse_modeset_supported || synapse_mode_timer != NULL) {
		return;
	}

	struct wl_event_loop *loop = wl_display_get_event_loop(output->base.compositor->wl_display);
	synapse_mode_timer = wl_event_loop_add_timer(loop, synapse_mode_poll, output);
	if (synapse_mode_timer != NULL) {
		wl_event_source_timer_update(synapse_mode_timer, 500);
	}
}
