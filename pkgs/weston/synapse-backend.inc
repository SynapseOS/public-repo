// synapse-backend.inc: вывод Weston в framebuffer и ввод SynapseOS
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl, SynAPI и libweston 13
// Подсистема: оконная система
// Первое изменение: 17.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/input-event-codes.h>
#include <lumine/pointer.h>
#include "lumine_fb.h"
#include "lumine_hid.h"
#include "libweston/backend.h"
#include "libweston/libweston-internal.h"

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
		                                KEY_F8,
		                                KEY_F9,
		                                KEY_F10,
		                                KEY_F11,
		                                KEY_F12,
		                                KEY_NUMLOCK,
		                                KEY_SCROLLLOCK };
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
			if ((pointer.flags & SYN_POINTER_ABSOLUTE) != 0) {
				backend->mouse_x = (double)pointer.x * (backend->width - 1) / 65535.0;
				backend->mouse_y = (double)pointer.y * (backend->height - 1) / 65535.0;
			} else {
				backend->mouse_x += pointer.x;
				backend->mouse_y += pointer.y;
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

			struct weston_coord_global position = { .c = { backend->mouse_x, backend->mouse_y } };
			notify_motion_absolute(&backend->fake_seat, &time, position);
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
	weston_log("Synapse framebuffer: %ux%u, stride %u, Pixman, keyboard and pointer\n",
	           backend->width, backend->height, backend->stride);
	return 0;

fail:
	weston_log("Synapse framebuffer/input failed: %s\n", strerror(errno));
	synapse_close(backend);
	return -1;
}

// Скопировать только повреждённые прямоугольники готового изображения
static void synapse_present(struct headless_output *output, pixman_region32_t *damage) {
	struct headless_backend *backend = output->backend;
	static bool first = true;

	if (first) {
		weston_log("Synapse first completed frame\n");
		first = false;
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
