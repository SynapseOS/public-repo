#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "device.h"
#include "lumine_hid.h"

static int open_keyboard(KBDDEVICE *p) {
	kbd_event_t e;

	while (syscall(102000, &e) == 0) {
	}

	return DRIVER_OKNOTFILEDESC;
}

static void close_keyboard(void) {}

static void modifiers(MWKEYMOD *all, MWKEYMOD *current) {
	if (all)
		*all = MWKMOD_SHIFT | MWKMOD_CTRL | MWKMOD_ALT;
	if (current)
		*current = 0;
}

static int poll_keyboard(void) {
	return syscall(102004) > 0;
}

static int read_keyboard(MWKEY *key, MWKEYMOD *mods, MWSCANCODE *scan) {
	kbd_event_t e;

	if (syscall(102000, &e))
		return KBD_NODATA;
	*mods = 0;
	*scan = e.keycode;

	if (e.modifiers & (KBD_MOD_LSHIFT | KBD_MOD_RSHIFT))
		*mods |= MWKMOD_SHIFT;
	if (e.modifiers & (KBD_MOD_LCTRL | KBD_MOD_RCTRL))
		*mods |= MWKMOD_CTRL;
	if (e.modifiers & (KBD_MOD_LALT | KBD_MOD_RALT))
		*mods |= MWKMOD_ALT;
	if (!getenv("SYNAPSE_NXTERM_KEYS") && e.is_pressed &&
	    (e.keycode == VK_ESCAPE || (e.keycode == 'C' && (*mods & MWKMOD_CTRL))))
		return KBD_QUIT;
	switch (e.keycode) {
		case VK_ESCAPE:
			*key = MWKEY_ESCAPE;
			break;
		case VK_INSERT:
			*key = MWKEY_INSERT;
			break;
		case VK_DELETE:
			*key = MWKEY_DELETE;
			break;
		case VK_HOME:
			*key = MWKEY_HOME;
			break;
		case VK_END:
			*key = MWKEY_END;
			break;
		case VK_PAGEUP:
			*key = MWKEY_PAGEUP;
			break;
		case VK_PAGEDOWN:
			*key = MWKEY_PAGEDOWN;
			break;
		case VK_F1:
			*key = MWKEY_F1;
			break;
		case VK_F2:
			*key = MWKEY_F2;
			break;
		case VK_F3:
			*key = MWKEY_F3;
			break;
		case VK_F4:
			*key = MWKEY_F4;
			break;
		case VK_F5:
			*key = MWKEY_F5;
			break;
		case VK_F6:
			*key = MWKEY_F6;
			break;
		case VK_F7:
			*key = MWKEY_F7;
			break;
		case VK_F8:
			*key = MWKEY_F8;
			break;
		case VK_F9:
			*key = MWKEY_F9;
			break;
		case VK_F10:
			*key = MWKEY_F10;
			break;
		case VK_F11:
			*key = MWKEY_F11;
			break;
		case VK_F12:
			*key = MWKEY_F12;
			break;
		case VK_ENTER:
			*key = MWKEY_ENTER;
			break;
		case VK_BACKSPACE:
			*key = MWKEY_BACKSPACE;
			break;
		case VK_SPACE:
			*key = ' ';
			break;
		case VK_TAB:
			*key = MWKEY_TAB;
			break;
		case VK_LEFT:
			*key = MWKEY_LEFT;
			break;
		case VK_RIGHT:
			*key = MWKEY_RIGHT;
			break;
		case VK_UP:
			*key = MWKEY_UP;
			break;
		case VK_DOWN:
			*key = MWKEY_DOWN;
			break;
		default:
			if (e.keycode < 39 || e.keycode > 126)
				return KBD_NODATA;
			*key = e.keycode;

			if (*key >= 'A' && *key <= 'Z') {
				if (!(((*mods & MWKMOD_SHIFT) != 0) ^ ((e.modifiers & KBD_MOD_CAPSLOCK) != 0)))
					*key += 32;
			} else if (*mods & MWKMOD_SHIFT) {
				const char *normal = "1234567890-=[];\\'`,./", *shifted = "!@#$%^&*()_+{}:|\"~<>?";
				const char *at = strchr(normal, (char)*key);

				if (at)
					*key = shifted[at - normal];
			}
	}

	return e.is_pressed ? KBD_KEYPRESS : KBD_KEYRELEASE;
}

KBDDEVICE kbddev = { open_keyboard, close_keyboard, modifiers, read_keyboard, poll_keyboard };
