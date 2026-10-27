// settings.c: Консольное редактирование системных настроек
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Параметры можно изменить через CLI или интерактивный терминал
// Подсистема: пользовательские программы, settings
// Первое изменение в истории Git: 12.10.2026
// Последнее изменение: 25.10.2026
// Доступ: общедоступный

#define _GNU_SOURCE
#include "../common/settings/settings.h"
#include <synapse/gpu.h>
#include <errno.h>
#include <fcntl.h>
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

// Проверить возможность смены режима у видеодрайвера
static bool display_mode_available(void) {
    int fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    struct syn_fb_display_v1 info;
    bool available = ioctl(fd, SYN_FB_DISPLAY_QUERY_V1, &info) == 0 &&
                     (info.capabilities & SYN_FB_MODESET) != 0;
    close(fd);
    return available;
}

// Отклонить настройку, которую видеодрайвер не умеет применять
static bool display_change_unsupported(struct settings *s, int index) {
    int display = settings_find("Display", "Resolution");
    if (index != display || s->root[0] != 0 || !strcmp(s->values[index], "auto")) {
        return false;
    }

    if (display_mode_available()) {
        return false;
    }

    snprintf(s->error, sizeof(s->error), "%s",
             settings_tr(s->ru, "Display mode changes are unsupported by this GPU"));
    errno = ENOTSUP;
    return true;
}
static void restore(void) {
    if (raw_mode) {
        tcsetattr(0, TCSAFLUSH, &saved);
        fputs("\033[0m\033[?25h\033[?1049l", stdout);
        fflush(stdout);
    }
}
static void interrupted(int sig) {
    restore();
    _exit(128 + sig);
}
static int input(void) {
    unsigned char c;
    if (read(0, &c, 1) != 1) {
        return -1;
    }
    if (c == 27) {
        struct pollfd p = {0, POLLIN, 0};
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
    for (int i = 0; i < setting_public_count; ++i) {
        if (s->changed[i]) {
            return true;
        }
    }
    return false;
}
static const char *const category_names_en[] = {
    "General", "Display", "Network", "Services", "Privacy"
};

static const char *const category_names_ru[] = {
    "Общие", "Экран", "Сеть", "Службы", "Приватность"
};

#define CATEGORY_COUNT 5

// Найти категорию настройки по её разделу
static int setting_category(int index) {
    const char *section = setting_specs[index].section;

    if (!strcmp(section, "Display") || !strcmp(section, "Desktop")) {
        return 1;
    }

    if (!strcmp(section, "Network") || !strcmp(section, "DNS")) {
        return 2;
    }

    if (!strcmp(section, "SSH") || !strcmp(section, "GuestAgent") || !strcmp(section, "FTP")) {
        return 3;
    }

    if (!strcmp(section, "stats")) {
        return 4;
    }

    return 0;
}

// Получить номер настройки внутри выбранной категории
static int category_setting(int category, int position) {
    for (int i = 0; i < setting_public_count; ++i) {
        if (setting_category(i) != category) {
            continue;
        }

        if (position == 0) {
            return i;
        }

        --position;
    }

    return -1;
}

// Подсчитать настройки выбранной категории
static int category_size(int category) {
    int count = 0;

    for (int i = 0; i < setting_public_count; ++i) {
        if (setting_category(i) == category) {
            ++count;
        }
    }

    return count;
}

// Показать категории и настройки текущего раздела
static void draw(struct settings *s, int category, int selection, int top, int rows,
                 const char *status) {
    printf("\033[H\033[2J\033[1;36m%s\033[0m   [ru/en]\n",
           settings_tr(s->ru, "System settings"));

    for (int c = 0; c < CATEGORY_COUNT; ++c) {
        const char *name = s->ru ? category_names_ru[c] : category_names_en[c];
        printf("%s%s\033[0m  ", c == category ? "\033[7m" : "", name);
    }
    fputs("\n\n", stdout);

    for (int r = 0; r < rows; ++r) {
        int position = top + r;
        int i = category_setting(category, position);
        if (i < 0) {
            break;
        }
        if (position == selection) {
            fputs("\033[7m", stdout);
        }
        printf("%c %s / %s: %s\033[0m\033[K\n", s->changed[i] ? '*' : ' ',
               setting_specs[i].section, settings_label(s, i), s->values[i]);
    }
    printf("\033[%d;1H%s\033[K\n%s\033[K", rows + 5,
           s->ru ? "←/→: раздел   ↑/↓: выбор   Enter: изменить   "
                   "S: сохранить   Q: выход"
                 : "Left/Right: category   Up/Down: select   Enter: edit   S: save   Q: quit",
           status);
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
        printf("\033[%d;1H\033[K%s: %s\033[?25h", row, settings_tr(s->ru, "Value"),
               value);
        fflush(stdout);
        int c = input();
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
        if (i < 0 || i >= setting_public_count) {
            return 2;
        }
        if (get) {
            puts(s.values[i]);
            return 0;
        }
        if (settings_set(&s, i, value) || display_change_unsupported(&s, i) || settings_save(&s)) {
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
    int category = 0, selected = 0, top = 0;
    char status[256] = "";
    for (;;) {
        struct winsize ws = {0};
        ioctl(1, TIOCGWINSZ, &ws);
        int rows = ws.ws_row >= 10 ? ws.ws_row - 7 : 18;
        if (selected < top) {
            top = selected;
        }
        if (selected >= top + rows) {
            top = selected - rows + 1;
        }
        draw(&s, category, selected, top, rows, status);
        int c = input();
        if (c < 0) {
            break;
        }
        if (c == 1004 || c == 1003) {
            category = (category + CATEGORY_COUNT + (c == 1003 ? 1 : -1)) % CATEGORY_COUNT;
            selected = 0;
            top = 0;
        } else if (c == 1001) {
            if (selected) {
                selected--;
            }
        } else if (c == 1002 || c == '\t') {
            if (selected < category_size(category) - 1) {
                selected++;
            } else {
                selected = 0;
            }
        } else if (c == '\r' || c == '\n') {
            int index = category_setting(category, selected);
            if (index >= 0) {
                edit(&s, index, rows + 6);
            }
        } else if (c == 's' || c == 'S' || c == 19) {
            int display = settings_find("Display", "Resolution");
            int shell = settings_find("Boot", "DefaultShell");
            bool display_changed = display >= 0 && s.changed[display];
            bool shell_changed = shell >= 0 && s.changed[shell];
            if ((display_changed && display_change_unsupported(&s, display)) || settings_save(&s)) {
                snprintf(status, sizeof(status), "%s", s.error);
            } else {
                const char *effect = "Reboot required for system changes";

                if (display_changed) {
                    effect = "Display updates during the desktop session";
                } else if (shell_changed) {
                    effect = "New terminals use the selected shell";
                }
                snprintf(status, sizeof(status), "%s. %s",
                         settings_tr(s.ru, "Configuration saved"),
                         settings_tr(s.ru, effect));
            }
        } else if (c == 'q' || c == 'Q' || c == 27) {
            if (!dirty(&s)) {
                break;
            }
            snprintf(status, sizeof(status), "%s",
                     settings_tr(s.ru, "Discard unsaved settings? (y/n)"));
            draw(&s, category, selected, top, rows, status);
            int answer = input();
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
