#ifndef DRV_MOUSE_H
#define DRV_MOUSE_H
#include <stdint.h>

/* /dev/mouse records: relative movement, Y positive downward; buttons L/R/M. */
typedef struct {
	int32_t dx, dy;
	uint32_t buttons;
} mouse_event_t;

#define MOUSE_LEFT 1
#define MOUSE_RIGHT 2
#define MOUSE_MIDDLE 4
void mouse_init(void);
#endif
