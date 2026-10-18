#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include "device.h"
#include "lumine_mouse.h"
static int fd = -1;

static int open_mouse(MOUSEDEVICE *p) {
	fd = open("/dev/mouse", O_RDONLY | O_NONBLOCK);
	return fd < 0 ? DRIVER_OKNULLDEV : fd;
}

static void close_mouse(void) {
	if (fd >= 0)
		close(fd);
	fd = -1;
}

static int buttons(void) {
	return MWBUTTON_L | MWBUTTON_M | MWBUTTON_R;
}

static void acceleration(int *scale, int *threshold) {
	*scale = 1;
	*threshold = 0;
}

static int read_mouse(MWCOORD *x, MWCOORD *y, MWCOORD *z, int *b) {
	mouse_event_t e;

	if (fd < 0 || read(fd, &e, sizeof(e)) != sizeof(e))
		return MOUSE_NODATA;
	*x = e.dx;
	*y = e.dy;
	*z = 0;
	*b = ((e.buttons & 1) ? MWBUTTON_L : 0) | ((e.buttons & 2) ? MWBUTTON_R : 0) |
	     ((e.buttons & 4) ? MWBUTTON_M : 0);
	return MOUSE_RELPOS;
}

static int poll_mouse(void) {
	struct pollfd p = { fd, POLLIN, 0 };
	return fd >= 0 && poll(&p, 1, 0) > 0;
}

MOUSEDEVICE mousedev = { open_mouse, close_mouse, buttons, acceleration, read_mouse, poll_mouse };
