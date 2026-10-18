// mouse.h: Формат событий мыши
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: Формат событий мыши в пользовательском пространстве
// Создан: 11.10.2026
// Последнее изменение: 14.10.2026

#ifndef DRV_MOUSE_H
#define DRV_MOUSE_H
#include <stdint.h>

// Относительное движение, положительный Y направлен вниз
// Кнопки соответствуют левой, правой и средней
typedef struct {
	int32_t dx, dy;
	uint32_t buttons;
} mouse_event_t;

#define MOUSE_LEFT 1
#define MOUSE_RIGHT 2
#define MOUSE_MIDDLE 4
void mouse_init(void);
#endif
