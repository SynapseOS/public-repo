// synapse-input.inc: Клавиатура и указатель SynapseOS для wlroots
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl, SynAPI и wlroots 0.19
// Описание: Передать события системных устройств в Labwc
// Подсистема: графическая оболочка
// Первое изменение: 28.10.2026
// Последнее изменение: 28.10.2026
// Доступ: общедоступный

#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#include <linux/input-event-codes.h>
#include <lumine/pointer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>

#include "lumine_hid.h"

static struct {
    struct wlr_keyboard keyboard;
    struct wlr_pointer pointer;
    struct wl_event_source *timer;
    int pointer_fd;
    uint32_t device_buttons[SYN_POINTER_MAX];
    uint32_t buttons;
    bool ready;
} synapse_input;

static const struct wlr_keyboard_impl synapse_keyboard_impl = {
    .name = "synapse-keyboard",
};

static const struct wlr_pointer_impl synapse_pointer_impl = {
    .name = "synapse-pointer",
};

// Сопоставить клавишу SynAPI с кодом Linux evdev для XKB
static uint32_t synapse_input_key(uint16_t key) {
    static const uint16_t special[] = {
        0, KEY_ESC, KEY_TAB, KEY_CAPSLOCK, KEY_LEFTSHIFT, KEY_RIGHTSHIFT,
        KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTALT, KEY_RIGHTALT, KEY_ENTER,
        KEY_BACKSPACE, KEY_SPACE, KEY_INSERT, KEY_DELETE, KEY_HOME, KEY_END,
        KEY_PAGEUP, KEY_PAGEDOWN, KEY_LEFT, KEY_UP, KEY_RIGHT, KEY_DOWN,
        KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8,
    };
    static const uint16_t upper[] = {
        KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_NUMLOCK, KEY_SCROLLLOCK,
        KEY_SYSRQ, KEY_LEFTMETA, KEY_RIGHTMETA,
    };
    static const char letters[] = "1234567890-=QWERTYUIOP[]ASDFGHJKL;'`\\ZXCVBNM,./";
    static const uint16_t codes[] = {
        KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
        KEY_MINUS, KEY_EQUAL, KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U,
        KEY_I, KEY_O, KEY_P, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_A, KEY_S,
        KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON,
        KEY_APOSTROPHE, KEY_GRAVE, KEY_BACKSLASH, KEY_Z, KEY_X, KEY_C, KEY_V,
        KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_DOT, KEY_SLASH,
    };

    if (key < sizeof(special) / sizeof(special[0])) {
        return special[key];
    }

    if (key >= 0x110 && key < 0x110 + sizeof(upper) / sizeof(upper[0])) {
        return upper[key - 0x110];
    }

    for (size_t i = 0; i < sizeof(letters) - 1; i++) {
        if ((uint16_t)(unsigned char)letters[i] == key) {
            return codes[i];
        }
    }

    return 0;
}

// Передать один шаг колеса прокрутки
static void synapse_input_axis(uint32_t time_msec, enum wl_pointer_axis axis, int32_t steps) {
    if (steps == 0) {
        return;
    }

    if (steps > 120) {
        steps = 120;
    }

    if (steps < -120) {
        steps = -120;
    }

    struct wlr_pointer_axis_event event = {
        .pointer = &synapse_input.pointer,
        .time_msec = time_msec,
        .source = WL_POINTER_AXIS_SOURCE_WHEEL,
        .orientation = axis,
        .relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
        .delta = (double)steps * 10.0,
        .delta_discrete = steps * WLR_POINTER_AXIS_DISCRETE_STEP,
    };

    wl_signal_emit_mutable(&synapse_input.pointer.events.axis, &event);
}

// Прочитать доступные события устройств без блокировки графического цикла
static int synapse_input_poll(void *data) {
    (void)data;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint32_t time_msec = (uint32_t)((uint64_t)now.tv_sec * 1000U + now.tv_nsec / 1000000U);

    kbd_event_t key;
    for (unsigned i = 0; i < 256 && syscall(102000, &key) == 0; i++) {
        uint32_t code = synapse_input_key(key.keycode);
        if (code == 0) {
            continue;
        }

        struct wlr_keyboard_key_event event = {
            .time_msec = time_msec,
            .keycode = code,
            .update_state = true,
            .state = key.is_pressed ? WL_KEYBOARD_KEY_STATE_PRESSED :
                WL_KEYBOARD_KEY_STATE_RELEASED,
        };
        wlr_keyboard_notify_key(&synapse_input.keyboard, &event);
    }

    struct syn_pointer_event pointer;
    for (unsigned i = 0; i < 256 &&
        read(synapse_input.pointer_fd, &pointer, sizeof(pointer)) == sizeof(pointer); i++) {
        if (pointer.version != SYN_POINTER_VERSION || pointer.device >= SYN_POINTER_MAX ||
            (pointer.flags & ~SYN_POINTER_FLAGS_MASK) != 0) {
            continue;
        }

        if ((pointer.flags & SYN_POINTER_WHEEL) != 0) {
            synapse_input_axis(time_msec, WL_POINTER_AXIS_HORIZONTAL_SCROLL, pointer.x);
            synapse_input_axis(time_msec, WL_POINTER_AXIS_VERTICAL_SCROLL, -pointer.y);
        } else if ((pointer.flags & SYN_POINTER_DISCONNECTED) == 0) {
            if ((pointer.flags & SYN_POINTER_ABSOLUTE) != 0) {
                struct wlr_pointer_motion_absolute_event motion = {
                    .pointer = &synapse_input.pointer,
                    .time_msec = time_msec,
                    .x = (double)pointer.x / 65535.0,
                    .y = (double)pointer.y / 65535.0,
                };
                wl_signal_emit_mutable(&synapse_input.pointer.events.motion_absolute, &motion);
            } else {
                struct wlr_pointer_motion_event motion = {
                    .pointer = &synapse_input.pointer,
                    .time_msec = time_msec,
                    .delta_x = pointer.x,
                    .delta_y = pointer.y,
                    .unaccel_dx = pointer.x,
                    .unaccel_dy = pointer.y,
                };
                wl_signal_emit_mutable(&synapse_input.pointer.events.motion, &motion);
            }
        }

        synapse_input.device_buttons[pointer.device] =
            (pointer.flags & SYN_POINTER_DISCONNECTED) != 0 ? 0 :
            pointer.buttons & SYN_POINTER_BUTTONS_MASK;
        uint32_t buttons = 0;
        for (unsigned device = 0; device < SYN_POINTER_MAX; device++) {
            buttons |= synapse_input.device_buttons[device];
        }

        static const uint32_t button_codes[] = {
            BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA,
        };
        for (unsigned button = 0; button < sizeof(button_codes) / sizeof(button_codes[0]); button++) {
            uint32_t bit = 1U << button;
            if (((buttons ^ synapse_input.buttons) & bit) == 0) {
                continue;
            }

            struct wlr_pointer_button_event event = {
                .pointer = &synapse_input.pointer,
                .time_msec = time_msec,
                .button = button_codes[button],
                .state = (buttons & bit) != 0 ? WL_POINTER_BUTTON_STATE_PRESSED :
                    WL_POINTER_BUTTON_STATE_RELEASED,
            };
            wlr_pointer_notify_button(&synapse_input.pointer, &event);
        }

        synapse_input.buttons = buttons;
        wl_signal_emit_mutable(&synapse_input.pointer.events.frame, &synapse_input.pointer);
    }

    wl_event_source_timer_update(synapse_input.timer, 8);
    return 0;
}

// Освободить источники событий перед разрушением backend
static void synapse_input_finish(void) {
    if (!synapse_input.ready) {
        return;
    }

    wl_event_source_remove(synapse_input.timer);
    close(synapse_input.pointer_fd);
    wlr_keyboard_finish(&synapse_input.keyboard);
    wlr_pointer_finish(&synapse_input.pointer);
    synapse_input.ready = false;
}

// Зарегистрировать клавиатуру и указатель для нового сеанса Labwc
static bool synapse_input_start(struct wlr_headless_backend *backend) {
    synapse_input.pointer_fd = open("/dev/pointers", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (synapse_input.pointer_fd < 0) {
        return false;
    }

    wlr_keyboard_init(&synapse_input.keyboard, &synapse_keyboard_impl, "synapse-keyboard");
    wlr_pointer_init(&synapse_input.pointer, &synapse_pointer_impl, "synapse-pointer");
    synapse_input.timer = wl_event_loop_add_timer(backend->event_loop, synapse_input_poll, NULL);
    if (synapse_input.timer == NULL) {
        close(synapse_input.pointer_fd);
        wlr_keyboard_finish(&synapse_input.keyboard);
        wlr_pointer_finish(&synapse_input.pointer);
        return false;
    }

    synapse_input.ready = true;
    wl_signal_emit_mutable(&backend->backend.events.new_input, &synapse_input.keyboard.base);
    wl_signal_emit_mutable(&backend->backend.events.new_input, &synapse_input.pointer.base);
    wl_event_source_timer_update(synapse_input.timer, 8);
    return true;
}
