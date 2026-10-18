// demo.c: Демонстрация Nano-X
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: Демонстрация Nano-X в пользовательском пространстве
// Создан: 11.10.2026
// Последнее изменение: 14.10.2026

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include "nano-X.h"

int main(void) {
	if (GrOpen( ) < 0) {
		fputs("nano-x: cannot open graphics\n", stderr);
		return 1;
	}

	GR_WINDOW_ID window = GrNewWindow(GR_ROOT_WINDOW_ID, 60, 80, 560, 300, 2, GR_RGB(28, 34, 48),
	                                  GR_RGB(80, 160, 220));
	GR_WM_PROPERTIES props = { 0 };
	props.flags = GR_WM_FLAGS_TITLE;
	props.title = "Nano-X on SynapseOS";
	GrSetWMProperties(window, &props);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_CLOSE_REQ |
	                           GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN);
	GR_GC_ID gc = GrNewGC( );
	GrSetGCUseBackground(gc, GR_FALSE);
	GrSetGCForeground(gc, GR_RGB(230, 235, 245));
	GrMapWindow(window);
	struct utsname u;
	uname(&u);
	char text[160];
	snprintf(text, sizeof(text), "%s %s - static musl build", u.sysname, u.release);
	puts("nano-x: native framebuffer, PS/2 mouse and keyboard. Esc exits.");
	fflush(stdout);

	for (;;) {
		GR_EVENT event;
		GrCheckNextEvent(&event);

		if (event.type == GR_EVENT_TYPE_CLOSE_REQ) {
			break;
		}

		if (event.type == GR_EVENT_TYPE_EXPOSURE) {
			GrText(window, gc, 24, 40, "Nano-X is running", -1, GR_TFASCII | GR_TFTOP);
			GrText(window, gc, 24, 80, text, -1, GR_TFASCII | GR_TFTOP);
			GrText(window, gc, 24, 120, "Mouse: click to draw. Keyboard: Esc to exit.", -1,
			       GR_TFASCII | GR_TFTOP);
		}

		if (event.type == GR_EVENT_TYPE_BUTTON_DOWN) {
			GrFillEllipse(window, gc, event.button.x, event.button.y, 10, 10);
		}

		if (event.type == GR_EVENT_TYPE_KEY_DOWN && event.keystroke.ch == 27) {
			break;
		}

		usleep(16000);
	}

	GrDestroyWindow(window);
	GrDestroyGC(gc);
	GrClose( );
	return 0;
}
