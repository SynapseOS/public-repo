/*
    pointer.h: События указательных устройств
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: Linux (ELF64)

    Описание: События указательных устройств, используемые ядром и приложениями
    Подсистема: общий интерфейс ядра и приложений
    Первое изменение: 12.10.2026
    Последнее изменение: 18.10.2026
*/

#ifndef LUMINE_POINTER_H
#define LUMINE_POINTER_H

#include <stdint.h>

#define SYN_POINTER_VERSION 1
#define SYN_POINTER_ABSOLUTE 1u
#define SYN_POINTER_DISCONNECTED 2u

// Прокрутка передаётся отдельно, x и y задают горизонтальные и вертикальные шаги
// Положительный y означает прокрутку вверх
#define SYN_POINTER_WHEEL 4u
#define SYN_POINTER_FLAGS_MASK (SYN_POINTER_ABSOLUTE | SYN_POINTER_DISCONNECTED | SYN_POINTER_WHEEL)
#define SYN_POINTER_BUTTON_LEFT 1u
#define SYN_POINTER_BUTTON_RIGHT 2u
#define SYN_POINTER_BUTTON_MIDDLE 4u
#define SYN_POINTER_BUTTON_SIDE 8u
#define SYN_POINTER_BUTTON_EXTRA 16u
#define SYN_POINTER_BUTTONS_MASK 31u
#define SYN_POINTER_MAX 16

// /dev/pointers различает устройства по идентификатору
// Координаты задают смещение либо абсолютное значение от 0 до 65535
struct syn_pointer_event {
	uint32_t version;
	uint32_t device;
	uint32_t flags;
	uint32_t buttons;
	int32_t x;
	int32_t y;
};

#endif
