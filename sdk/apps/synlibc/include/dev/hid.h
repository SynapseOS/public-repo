/*
    hid.h: Коды клавиш и события клавиатуры
    Автор: Елчинян Арен Артурович 2026
    Описание: Физические клавиши, модификаторы и уведомления раскладки
    Подсистема: пользовательская библиотека
    Первое изменение: исходная дата не установлена
    Последнее изменение: 17.10.2026
*/

#ifndef DRV_HID_H
#define DRV_HID_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Виртуальные клавиши
typedef enum {
	VK_NONE = 0,
	VK_ESCAPE,
	VK_TAB,
	VK_CAPSLOCK,
	VK_LSHIFT,
	VK_RSHIFT,
	VK_LCTRL,
	VK_RCTRL,
	VK_LALT,
	VK_RALT,
	VK_ENTER,
	VK_BACKSPACE,
	VK_SPACE,
	VK_INSERT,
	VK_DELETE,
	VK_HOME,
	VK_END,
	VK_PAGEUP,
	VK_PAGEDOWN,
	VK_LEFT,
	VK_UP,
	VK_RIGHT,
	VK_DOWN,
	VK_F1,
	VK_F2,
	VK_F3,
	VK_F4,
	VK_F5,
	VK_F6,
	VK_F7,
	VK_F8,
	VK_F9,
	VK_F10,
	VK_F11,
	VK_F12,
	VK_NUMLOCK,
	VK_SCROLLLOCK,
} vk_code_t;

#define KBD_MOD_LSHIFT (1 << 0)
#define KBD_MOD_RSHIFT (1 << 1)
#define KBD_MOD_LCTRL (1 << 2)
#define KBD_MOD_RCTRL (1 << 3)
#define KBD_MOD_LALT (1 << 4)
#define KBD_MOD_RALT (1 << 5)
#define KBD_MOD_CAPSLOCK (1 << 6)

// Уведомления смены раскладки, структура события сохраняет размер четыре байта
#define VK_LAYOUT_EN 0x100
#define VK_LAYOUT_RU 0x101
#define KBD_STATE_RUSSIAN (1u << 8)

typedef struct __attribute__((packed)) {
	uint16_t keycode;
	bool is_pressed;
	uint8_t modifiers;
} kbd_event_t;

#define KBD_LED_SCROLL (1 << 0)
#define KBD_LED_NUM (1 << 1)
#define KBD_LED_CAPS (1 << 2)

#define KBD_MOD_LSHIFT (1 << 0)
#define KBD_MOD_RSHIFT (1 << 1)
#define KBD_MOD_LCTRL (1 << 2)
#define KBD_MOD_RCTRL (1 << 3)
#define KBD_MOD_LALT (1 << 4)
#define KBD_MOD_RALT (1 << 5)
#define KBD_MOD_CAPSLOCK (1 << 6)
#define KBD_MOD_NUMLOCK (1 << 7)

#define KBD_OK 0
#define KBD_ERR_EMPTY -2
#define KBD_ERR_FAULT -14
#define KBD_ERR_INVAL -22

bool kbd_pop_event(kbd_event_t *event);
bool kbd_has_event(void);
uint8_t kbd_get_modifiers(void);
uint8_t kbd_get_leds(void);
void kbd_set_leds(uint8_t leds);

#endif