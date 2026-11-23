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
#include <lumine/bthci.h>
#include <lumine/wifi.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
    "General", "Display", "Network", "Services", "Privacy", "Wi-Fi", "Bluetooth"
};

static const char *const category_names_ru[] = {
    "Общие", "Экран", "Сеть", "Службы", "Приватность", "Wi-Fi", "Bluetooth"
};

#define CATEGORY_COUNT 7
#define BT_ACTIONS 3

static struct syn_wifi_scan wifi_networks = {.version = SYN_WIFI_ABI_VERSION};

static unsigned wifi_count(void) {
    return wifi_networks.count <= SYN_WIFI_SCAN_LIMIT ? wifi_networks.count : 0U;
}

static void wifi_wipe(void *memory, size_t size) {
    volatile uint8_t *bytes = memory;

    while (size--) {
        *bytes++ = 0U;
    }
}

static void wifi_text(const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        unsigned c = data[i];
        putchar(c >= 32U && c <= 126U && c != '\\' ? (int)c : '?');
    }
}

static bool wifi_profile_store(const struct syn_wifi_connect *connection) {
    if (mkdir("/etc/network", 0755) < 0 && errno != EEXIST) {
        return false;
    }

    char temporary[80];
    snprintf(temporary, sizeof(temporary), "/etc/network/wifi.conf.tmp.%ld", (long)getpid());
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);

    if (fd < 0) {
        return false;
    }

    char text[256];
    size_t used = 0;
    static const char digits[] = "0123456789abcdef";
    const char *names[] = {"ssid=", "password="};
    const uint8_t *values[] = {connection->ssid, connection->password};
    size_t lengths[] = {connection->ssid_length, connection->password_length};

    for (unsigned field = 0; field < 2U; field++) {
        size_t name_length = strlen(names[field]);
        memcpy(text + used, names[field], name_length);
        used += name_length;

        for (size_t i = 0; i < lengths[field]; i++) {
            text[used++] = digits[values[field][i] >> 4U];
            text[used++] = digits[values[field][i] & 15U];
        }

        text[used++] = '\n';
    }

    size_t position = 0;

    while (position < used) {
        ssize_t count = write(fd, text + position, used - position);

        if (count < 0 && errno == EINTR) {
            continue;
        }

        if (count <= 0) {
            break;
        }

        position += (size_t)count;
    }

    bool okay = position == used && fsync(fd) == 0;
    wifi_wipe(text, sizeof(text));

    if (close(fd) != 0) {
        okay = false;
    }

    if (okay) {
        okay = rename(temporary, "/etc/network/wifi.conf") == 0;
    }

    if (!okay) {
        unlink(temporary);
    }

    return okay;
}

static void wifi_draw(bool ru, int selection, int top, int rows, const char *status) {
    struct syn_wifi_status state = {.version = SYN_WIFI_ABI_VERSION};
    int fd = open("/dev/wifi", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    bool available = fd >= 0 && ioctl(fd, SYN_WIFI_IOCTL_STATUS, &state) == 0 &&
                     state.version == SYN_WIFI_ABI_VERSION;

    printf("\033[H\033[2J\033[1;36m%s\033[0m   [ru/en]\n",
           ru ? "Настройки системы" : "System settings");

    for (int c = 0; c < CATEGORY_COUNT; c++) {
        printf("%s%s\033[0m  ", c == 5 ? "\033[7m" : "", ru ? category_names_ru[c] : category_names_en[c]);
    }

    printf("\n\n%s: %s\n", ru ? "Адаптер" : "Adapter",
           !available ? (ru ? "не найден" : "not found") : state.enabled ? "on" : "off");

    if (available && state.associated && state.ssid_length <= SYN_WIFI_SSID_LIMIT) {
        printf("%s: ", ru ? "Подключено" : "Connected");
        wifi_text(state.ssid, state.ssid_length);

        if (state.ready) {
            printf(" (%u.%u.%u.%u)", state.ipv4 >> 24U, (state.ipv4 >> 16U) & 255U,
                   (state.ipv4 >> 8U) & 255U, state.ipv4 & 255U);
        } else {
            printf(" (%s)", ru ? "Получение IP-адреса..." : "Getting IP address...");
        }

        putchar('\n');
    }

    for (int row = 0; row < rows && top + row < (int)wifi_count() + 3; row++) {
        int position = top + row;
        printf("%s", position == selection ? "\033[7m" : "");

        if (position == 0) {
            printf("%s", ru ? "Сканировать" : "Scan");
        } else if (position == 1) {
            printf("%s", ru ? "Включить / выключить" : "Turn on / off");
        } else if (position == 2) {
            printf("%s", ru ? "Отключиться" : "Disconnect");
        } else {
            const struct syn_wifi_bss *network = &wifi_networks.entries[position - 3];
            unsigned signal = (unsigned)((network->rssi_dbm + 100) * 2);

            if (signal > 100U) {
                signal = network->rssi_dbm < -100 ? 0U : 100U;
            }

            wifi_text(network->ssid, network->ssid_length <= SYN_WIFI_SSID_LIMIT ? network->ssid_length : 0U);
            printf("  %u%%  ch %u  %s", signal, network->channel,
                   network->security == SYN_WIFI_SECURITY_WPA2_PSK ? "WPA2-PSK" : "Open");
        }

        fputs("\033[0m\033[K\n", stdout);
    }

    printf("\033[%d;1H%s\033[K\n%s\033[K", rows + 7,
           ru ? "←/→: раздел   ↑/↓: выбор   Enter: действие   Q: выход"
              : "Left/Right: category   Up/Down: select   Enter: action   Q: quit", status);
    fflush(stdout);

    if (fd >= 0) {
        close(fd);
    }
}

static void wifi_action(bool ru, int selection, int row, char status[256]) {
    int fd = open("/dev/wifi", O_RDWR | O_CLOEXEC | O_NOFOLLOW);

    if (fd < 0) {
        snprintf(status, 256, "%s", ru ? "Адаптер Wi-Fi не найден" : "Wi-Fi adapter not found");
        return;
    }

    int result = -1;

    if (selection == 0) {
        wifi_networks = (struct syn_wifi_scan){.version = SYN_WIFI_ABI_VERSION};
        result = ioctl(fd, SYN_WIFI_IOCTL_SCAN, &wifi_networks);

        if (result == 0 && wifi_networks.count > SYN_WIFI_SCAN_LIMIT) {
            result = -1;
        }
    } else if (selection == 1) {
        struct syn_wifi_status state = {.version = SYN_WIFI_ABI_VERSION};
        struct syn_wifi_enabled enabled = {.version = SYN_WIFI_ABI_VERSION};

        if (ioctl(fd, SYN_WIFI_IOCTL_STATUS, &state) == 0) {
            enabled.enabled = !state.enabled;
            result = ioctl(fd, SYN_WIFI_IOCTL_SET_ENABLED, &enabled);
        }
    } else if (selection == 2) {
        result = ioctl(fd, SYN_WIFI_IOCTL_DISCONNECT, 0);
    } else if ((unsigned)(selection - 3) < wifi_count()) {
        const struct syn_wifi_bss *network = &wifi_networks.entries[selection - 3];

        if (!network->ssid_length || network->ssid_length > SYN_WIFI_SSID_LIMIT ||
            network->security > SYN_WIFI_SECURITY_WPA2_PSK) {
            close(fd);
            snprintf(status, 256, "%s", ru ? "Некорректная сеть" : "Invalid network");
            return;
        }

        struct syn_wifi_connect connection = {.version = SYN_WIFI_ABI_VERSION};
        connection.ssid_length = network->ssid_length;
        memcpy(connection.ssid, network->ssid, connection.ssid_length);

        if (network->security == SYN_WIFI_SECURITY_WPA2_PSK) {
            printf("\033[%d;1H\033[K%s: \033[?25h", row,
                   ru ? "Пароль Wi-Fi" : "Wi-Fi password");
            fflush(stdout);
            int key;

            while ((key = input()) >= 0 && key != '\r' && key != '\n' && key != 27) {
                if (key == 127 || key == 8) {
                    if (connection.password_length) {
                        connection.password_length--;
                        fputs("\b \b", stdout);
                    }
                } else if (key >= 32 && key < 127 && connection.password_length < SYN_WIFI_PASSWORD_LIMIT) {
                    connection.password[connection.password_length++] = (uint8_t)key;
                    putchar('*');
                }

                fflush(stdout);
            }

            fputs("\033[?25l", stdout);

            if (key == 27 || connection.password_length < 8U) {
                wifi_wipe(&connection, sizeof(connection));
                close(fd);
                return;
            }
        }

        result = ioctl(fd, SYN_WIFI_IOCTL_CONNECT, &connection);

        if (result == 0 && !wifi_profile_store(&connection)) {
            result = -1;
        }
        wifi_wipe(&connection, sizeof(connection));
    }

    snprintf(status, 256, "%s",
             result == 0 ? (ru ? "Готово" : "Done")
                         : (ru ? "Операция Wi-Fi не выполнена" : "Wi-Fi operation failed"));
    close(fd);
}

// Запросить сведения первого доступного адаптера /dev/bthciN
static bool bt_info(struct syn_bthci_info *info) {
    for (unsigned i = 0; i < 4U; ++i) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/bthci%u", i);
        int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            continue;
        }

        info->version = SYN_BTHCI_ABI_VERSION;
        bool found = ioctl(fd, SYN_BTHCI_IOCTL_INFO, info) == 0;
        close(fd);

        if (found) {
            return true;
        }
    }

    return false;
}

// Подпись действия Bluetooth по номеру строки
static const char *bt_action_name(bool ru, int which) {
    if (which == 0) {
        return ru ? "Найти устройства рядом" : "Scan nearby devices";
    }
    if (which == 1) {
        return ru ? "Найти устройства Bluetooth LE" : "Scan BLE devices";
    }
    return ru ? "Применить настройки к адаптеру" : "Apply settings to adapter";
}

// Показать вывод btctl построчно на отдельном экране и дождаться клавиши
static int bt_run_screen(bool ru, char *const argv[]) {
    restore();
    printf("\033[H\033[2J\033[1;36m%s\033[0m\n\n", ru ? "Bluetooth" : "Bluetooth");

    pid_t child = fork();
    if (child == 0) {
        execv(argv[0], argv);
        _exit(127);
    }

    int status = -1;

    if (child > 0) {
        int code;
        while (waitpid(child, &code, 0) < 0 && errno == EINTR) {
        }
        status = child < 0 || !WIFEXITED(code) ? -1 : WEXITSTATUS(code);
    }

    printf("\n%s", ru ? "Клавиша: назад" : "Any key: back");
    fflush(stdout);

    struct termios raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_iflag &= ~(ICRNL | IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &raw);
    input();
    fputs("\033[?1049h\033[?25l", stdout);

    return status;
}

// Выполнить действие Bluetooth выбранной строки
static void bt_action(struct settings *s, bool ru, int which, char status[256]) {
    if (which == 0) {
        char *args[] = {"btctl", "inquiry", "--time", "10", NULL};
        bt_run_screen(ru, args);
        status[0] = 0;
        return;
    }

    if (which == 1) {
        char *args[] = {"btctl", "scan", "--time", "10", NULL};
        bt_run_screen(ru, args);
        status[0] = 0;
        return;
    }

    const char *enabled = settings_get(s, "Bluetooth", "Enabled");

    if (!strcmp(enabled, "1")) {
        const char *role = settings_get(s, "Bluetooth", "Role");
        const char *name = settings_get(s, "Bluetooth", "Name");
        const char *visible = settings_get(s, "Bluetooth", "Discoverable");
        char *args[] = {"btctl", "up", "--name", (char *)(name[0] ? name : "SynapseOS"),
                        NULL, NULL, NULL};
        int slot = 4;

        if (!strcmp(role, "speaker")) {
            args[slot++] = "--speaker";
        } else if (!strcmp(role, "headset")) {
            args[slot++] = "--headset";
        }

        if (strcmp(visible, "1")) {
            args[slot++] = "--connect-only";
        }

        int rc = bt_run_screen(ru, args);
        snprintf(status, 256, "%s", rc ? (ru ? "btctl up не выполнен" : "btctl up failed")
                                       : (ru ? "Адаптер включён" : "Adapter is on"));
    } else {
        char *args[] = {"btctl", "down", NULL};
        int rc = bt_run_screen(ru, args);
        snprintf(status, 256, "%s", rc ? (ru ? "btctl down не выполнен" : "btctl down failed")
                                       : (ru ? "Адаптер выключен" : "Adapter is off"));
    }
}

// Найти категорию настройки по её разделу
static int setting_category(int index) {
    const char *section = setting_specs[index].section;

    if (!strcmp(section, "Display") || !strcmp(section, "Desktop")) {
        return 1;
    }

    if (!strcmp(section, "Network") || !strcmp(section, "DNS") ||
        !strcmp(section, "LanDiscovery")) {
        return 2;
    }

    if (!strcmp(section, "SSH") || !strcmp(section, "GuestAgent") || !strcmp(section, "FTP")) {
        return 3;
    }

    if (!strcmp(section, "stats")) {
        return 4;
    }

    if (!strcmp(section, "Bluetooth")) {
        return 6;
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

    if (category == 6) {
        struct syn_bthci_info info;
        if (bt_info(&info)) {
            printf("%s: usb %04x:%04x /dev/bthci%u\n\n",
                   settings_tr(s->ru, "Adapter"), info.vendor, info.product, info.index);
        } else {
            printf("%s\n\n", settings_tr(s->ru, "Adapter: not found"));
        }
    }

    const int settings_in_category = category_size(category);

    for (int r = 0; r < rows; ++r) {
        int position = top + r;
        int i = category_setting(category, position);

        if (i < 0) {
            int action = position - settings_in_category;
            if (category != 6 || (unsigned)action >= BT_ACTIONS) {
                break;
            }
            if (position == selection) {
                fputs("\033[7m", stdout);
            }
            printf("  %s\033[0m\033[K\n", bt_action_name(s->ru, action));
            continue;
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
        if (category == 5) {
            wifi_draw(s.ru, selected, top, rows, status);
        } else {
            draw(&s, category, selected, top, rows, status);
        }
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
            int count = category == 5 ? (int)wifi_count() + 3 :
                        category == 6 ? category_size(6) + BT_ACTIONS :
                        category_size(category);
            if (selected < count - 1) {
                selected++;
            } else {
                selected = 0;
            }
        } else if (c == '\r' || c == '\n') {
            if (category == 5) {
                wifi_action(s.ru, selected, rows + 6, status);

                if (selected >= (int)wifi_count() + 3) {
                    selected = 0;
                    top = 0;
                }
            } else if (category == 6 && selected >= category_size(6)) {
                bt_action(&s, s.ru, selected - category_size(6), status);
            } else {
                int index = category_setting(category, selected);
                if (index >= 0) {
                    edit(&s, index, rows + 6);
                }
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
