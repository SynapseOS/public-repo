// settings.c: Консольное редактирование системных настроек
// Автор: Елчинян Арен Артурович, 2026
// Параметры можно изменить через CLI или интерактивный терминал
// Подсистема: пользовательские программы, settings
// Создан: 12.10.2026
// Последнее изменение: 14.10.2026

#define _GNU_SOURCE
#include "../common/settings/settings.h"
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
static struct termios saved;
static bool raw_mode;

static void restore(void) {
	if (raw_mode) {
		tcsetattr(0, TCSAFLUSH, &saved);
		fputs("\033[0m\033[?25h\033[?1049l", stdout);
		fflush(stdout);
	}
}

static void interrupted(int sig) {
	restore( );
	_exit(128 + sig);
}

static int input(void) {
	unsigned char c;

	if (read(0, &c, 1) != 1) {
		return -1;
	}

	if (c == 27) {
		struct pollfd p = { 0, POLLIN, 0 };
		unsigned char seq[2];

		for (int i = 0; i < 2; ++i) {
			if (poll(&p, 1, 100) <= 0 || read(0, &seq[i], 1) != 1) {
				return 27;
			}
		}

		if (seq[0] == '[' || seq[0] == 'O') {
			return seq[1] == 'A'   ? 1001
			       : seq[1] == 'B' ? 1002
			       : seq[1] == 'C' ? 1003
			       : seq[1] == 'D' ? 1004
			                       : 27;
		}

		return 27;
	}

	return c;
}

static bool dirty(struct settings *s) {
	for (int i = 0; i < setting_count; ++i) {
		if (s->changed[i]) {
			return true;
		}
	}

	return false;
}

static void draw(struct settings *s, int selection, int top, int rows, const char *status) {
	printf("\033[H\033[2J\033[1;36m%s\033[0m   [ru/en]\n\n", settings_tr(s->ru, "System settings"));

	for (int r = 0; r < rows; ++r) {
		int i = top + r;

		if (i >= setting_count) {
			break;
		}

		if (i == selection) {
			fputs("\033[7m", stdout);
		}

		printf("%c %s / %s: %s\033[0m\033[K\n", s->changed[i] ? '*' : ' ', setting_specs[i].section,
		       settings_label(s, i), s->values[i]);
	}

	printf("\033[%d;1H%s\033[K\n%s\033[K", rows + 4,
	       settings_tr(s->ru, "Enter: edit / toggle   S: save   Q: quit"), status);
	fflush(stdout);
}

static void edit(struct settings *s, int i, int row) {
	const struct setting_spec *spec = &setting_specs[i];

	if (spec->type == SET_BOOL || spec->type == SET_CHOICE) {
		settings_cycle(s, i);
		return;
	}

	if (!strcmp(spec->key, "Animations")) {
		return;
	}

	char value[SETTINGS_VALUE];
	snprintf(value, sizeof(value), "%s", s->values[i]);
	size_t n = strlen(value);

	for (;;) {
		printf("\033[%d;1H\033[K%s: %s\033[?25h", row, settings_tr(s->ru, "Value"), value);
		fflush(stdout);
		int c = input( );

		if (c < 0 || c == 27) {
			break;
		}

		if (c == '\r' || c == '\n') {
			settings_set(s, i, value);
			break;
		}

		if (c == 1) {
			n = 0;
			value[0] = 0;
		} else if (c == 127 || c == 8) {
			if (n) {
				--n;

				while (n && (value[n] & 0xc0) == 0x80) {
					--n;
				}

				value[n] = 0;
			}
		} else if (c >= 32 && c < 256 && n < sizeof(value) - 1) {
			value[n++] = c;
			value[n] = 0;
		}
	}

	fputs("\033[?25l", stdout);
}

int main(int argc, char **argv) {
	const char *root = "", *get = NULL, *set = NULL, *value = NULL, *language = NULL;

	for (int a = 1; a < argc; ++a) {
		if (!strcmp(argv[a], "--root") && a + 1 < argc) {
			root = argv[++a];
		} else if (!strcmp(argv[a], "--lang") && a + 1 < argc) {
			language = argv[++a];
		} else if (!strcmp(argv[a], "--get") && a + 1 < argc) {
			get = argv[++a];
		} else if (!strcmp(argv[a], "--set") && a + 2 < argc) {
			set = argv[++a];
			value = argv[++a];
		} else {
			fprintf(stderr, "Usage: settings [--root DIR] [--lang ru|en] [--get "
			                "SECTION.KEY | --set SECTION.KEY VALUE]\n");
			return 2;
		}
	}

	struct settings s;

	if (settings_load(&s, root)) {
		fprintf(stderr, "%s\n", s.error);
		return 1;
	}

	if (language) {
		if (strcmp(language, "ru") && strcmp(language, "en")) {
			return 2;
		}

		s.ru = !strcmp(language, "ru");
	}

	if (get || set) {
		char name[128];
		snprintf(name, sizeof(name), "%s", get ? get : set);
		char *dot = strchr(name, '.');

		if (!dot) {
			return 2;
		}

		*dot++ = 0;
		int i = settings_find(name, dot);

		if (i < 0) {
			return 2;
		}

		if (get) {
			puts(s.values[i]);
			return 0;
		}

		if (settings_set(&s, i, value) || settings_save(&s)) {
			fprintf(stderr, "%s\n", s.error);
			return 1;
		}

		return 0;
	}

	if (!isatty(0) || tcgetattr(0, &saved)) {
		fputs("settings: interactive terminal required\n", stderr);
		return 1;
	}

	struct termios raw = saved;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_iflag &= ~(ICRNL | IXON);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;

	if (tcsetattr(0, TCSAFLUSH, &raw)) {
		return 1;
	}

	raw_mode = true;
	atexit(restore);
	signal(SIGINT, interrupted);
	signal(SIGTERM, interrupted);
	signal(SIGHUP, interrupted);
	fputs("\033[?1049h\033[?25l", stdout);
	int selected = 0, top = 0;
	char status[256] = "";

	for (;;) {
		struct winsize ws = { 0 };
		ioctl(1, TIOCGWINSZ, &ws);
		int rows = ws.ws_row >= 10 ? ws.ws_row - 6 : 18;

		if (selected < top) {
			top = selected;
		}

		if (selected >= top + rows) {
			top = selected - rows + 1;
		}

		draw(&s, selected, top, rows, status);
		int c = input( );

		if (c < 0) {
			break;
		}

		if (c == 1001) {
			if (selected) {
				selected--;
			}
		} else if (c == 1002 || c == '\t') {
			if (selected < setting_count - 1) {
				selected++;
			} else {
				selected = 0;
			}
		} else if (c == '\r' || c == '\n' || c == 1003) {
			edit(&s, selected, rows + 5);
		} else if (c == 's' || c == 'S' || c == 19) {
			if (settings_save(&s)) {
				snprintf(status, sizeof(status), "%s", s.error);
			} else {
				snprintf(status, sizeof(status), "%s. %s", settings_tr(s.ru, "Configuration saved"),
				         settings_tr(s.ru, "Reboot required for system changes"));
			}
		} else if (c == 'q' || c == 'Q' || c == 27) {
			if (!dirty(&s)) {
				break;
			}

			snprintf(status, sizeof(status), "%s",
			         settings_tr(s.ru, "Discard unsaved settings? (y/n)"));
			draw(&s, selected, top, rows, status);
			int answer = input( );

			if (answer == 'y' || answer == 'Y') {
				break;
			}

			status[0] = 0;
		}

		if (s.error[0]) {
			snprintf(status, sizeof(status), "%s", s.error);
		}
	}

	return 0;
}
