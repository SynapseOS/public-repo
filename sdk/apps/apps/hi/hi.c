// hi.c: Первый запуск и установка SynapseOS
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Выбор языка, первоначальные настройки и сбор сведений о системе
// Подсистема: установка и начальная настройка системы
// Первое изменение: до текущей уборки, исходная дата не установлена
// Последнее изменение: 03.11.2026
// Доступ: общедоступный

#define _GNU_SOURCE
#include "../common/settings/settings.h"
#include "installer.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <shadow.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define HI_DIR "/var/lib/hi"
#define HI_DONE HI_DIR "/complete"
#define HI_LOG_DIR "/var/log/hi"
#define KEY_UP 1001
#define KEY_DOWN 1002
#define KEY_RIGHT 1003
#define KEY_LEFT 1004

static struct termios saved_termios;
static bool raw_active, ru = true;

static const char *tr(const char *en, const char *russian) {
    return ru ? russian : en;
}

static void terminal_restore(void) {
    if (!raw_active) {
        return;
    }
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
    fputs("\033[0m\033[?25h\033[?1049l", stdout);
    fflush(stdout);
    raw_active = false;
}

static int terminal_raw(void) {
    if (raw_active) {
        return 0;
    }
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved_termios)) {
        return -1;
    }

    struct termios raw = saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_iflag &= ~(ICRNL | IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw)) {
        return -1;
    }
    raw_active = true;
    fputs("\033[?1049h\033[?25l", stdout);
    return 0;
}

static void interrupted(int sig) {
    terminal_restore();
    _exit(128 + sig);
}

static int key_read(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) {
        return -1;
    }
    if (c != 27) {
        return c;
    }

    struct pollfd p = {STDIN_FILENO, POLLIN, 0};
    unsigned char seq[2];
    for (int i = 0; i < 2; ++i) {
        if (poll(&p, 1, 100) <= 0 || read(STDIN_FILENO, &seq[i], 1) != 1) {
            return 27;
        }
    }
    if (seq[0] != '[' && seq[0] != 'O') {
        return 27;
    }
    return seq[1] == 'A'   ? KEY_UP
           : seq[1] == 'B' ? KEY_DOWN
           : seq[1] == 'C' ? KEY_RIGHT
           : seq[1] == 'D' ? KEY_LEFT
                           : 27;
}

static void title(const char *en, const char *russian) {
    printf("\033[H\033[2J\033[1;36mHI\033[0m  \033[1m%s\033[0m\n\n", tr(en, russian));
}

static int choose(const char *en, const char *russian, const char **items_en,
                  const char **items_ru, int count, int selected, bool back) {
    for (;;) {
        title(en, russian);
        for (int i = 0; i < count; ++i) {
            printf("%s  %s\033[0m\033[K\n", i == selected ? "\033[7m" : "",
                   ru ? items_ru[i] : items_en[i]);
        }
        printf("\n\033[2m%s\033[0m\033[K",
               tr(back ? "Arrows: select   Enter: continue   Esc: back"
                       : "Arrows: select   Enter: continue",
                  back ? "Стрелки: выбор   Enter: продолжить   Esc: назад"
                       : "Стрелки: выбор   Enter: продолжить"));
        fflush(stdout);
        int c = key_read();
        if (c == KEY_UP) {
            selected = selected ? selected - 1 : count - 1;
        } else if (c == KEY_DOWN || c == '\t') {
            selected = (selected + 1) % count;
        } else if (c == '\r' || c == '\n' || c == KEY_RIGHT) {
            return selected;
        } else if ((c == 27 || c == KEY_LEFT || c == 127) && back) {
            return -1;
        }
    }
}

static int prompt(const char *label_en, const char *label_ru, char *value, size_t cap) {
    size_t n = strlen(value);
    for (;;) {
        printf("\r\033[K%s: %s\033[?25h", tr(label_en, label_ru), value);
        fflush(stdout);
        int c = key_read();
        if (c < 0 || c == 27) {
            fputs("\033[?25l\n", stdout);
            return -1;
        }
        if (c == '\r' || c == '\n') {
            fputs("\033[?25l\n", stdout);
            return 0;
        }
        if ((c == 127 || c == 8) && n) {
            --n;
            while (n && ((unsigned char)value[n] & 0xc0) == 0x80) {
                --n;
            }
            value[n] = 0;
        } else if (c >= 32 && c < 256 && n + 1 < cap) {
            value[n++] = (char)c;
            value[n] = 0;
        }
    }
}

static int mkdir_one(const char *path, mode_t mode) {
    if (!mkdir(path, mode) || errno == EEXIST) {
        return chmod(path, mode);
    }
    return -1;
}

static int wait_child(pid_t child) {
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return 1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

// Ограничить ожидание сетевого клиента, не задерживая мастер настройки навсегда
static int wait_child_bounded(pid_t child, unsigned seconds) {
    struct timespec started;
    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) {
        kill(child, SIGKILL);
        int status = 0;
        (void)waitpid(child, &status, WNOHANG);
        return 1;
    }

    int status = 0;
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }

        if (result < 0 && errno != EINTR) {
            return 1;
        }

        struct timespec now;
        time_t deadline_sec = started.tv_sec + (time_t)seconds;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
            now.tv_sec > deadline_sec ||
            (now.tv_sec == deadline_sec && now.tv_nsec >= started.tv_nsec)) {
            break;
        }

        const struct timespec delay = {0, 100000000};
        nanosleep(&delay, NULL);
    }

    kill(child, SIGTERM);
    for (int attempt = 0; attempt < 10; attempt++) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child || (result < 0 && errno != EINTR)) {
            return 124;
        }

        const struct timespec delay = {0, 100000000};
        nanosleep(&delay, NULL);
    }

    kill(child, SIGKILL);
    (void)waitpid(child, &status, WNOHANG);
    return 124;
}

// Запустить команду и при необходимости ограничить время её работы
static int run_plain_timeout(const char *path, char *const argv[], int output_fd,
                             unsigned seconds) {
    pid_t child = fork();
    if (child < 0) {
        return 1;
    }
    if (!child) {
        if (output_fd >= 0) {
            dup2(output_fd, STDOUT_FILENO);
            dup2(output_fd, STDERR_FILENO);
        }
        execv(path, argv);
        dprintf(STDERR_FILENO, "%s: %s\n", path, strerror(errno));
        _exit(127);
    }
    return seconds == 0U ? wait_child(child) : wait_child_bounded(child, seconds);
}

// Запустить обычную команду без ограничения по времени
static int run_plain(const char *path, char *const argv[], int output_fd) {
    return run_plain_timeout(path, argv, output_fd, 0U);
}

static void pause_screen(void) {
    fputs(tr("\nPress Enter to return.", "\nНажмите Enter для возврата."), stdout);
    fflush(stdout);
    for (;;) {
        int c = key_read();
        if (c == '\n' || c == '\r' || c < 0) {
            break;
        }
    }
}

static int run_visible(const char *heading_en, const char *heading_ru, const char *path,
                       char *const argv[]) {
    terminal_restore();
    printf("\033[2J\033[H\033[1;36mHI — %s\033[0m\n\n", tr(heading_en, heading_ru));
    fflush(stdout);
    int status = run_plain(path, argv, -1);
    printf("\n%s: %d\n%s", tr("Exit status", "Код завершения"), status,
           tr("Press Enter to return...", "Нажмите Enter для возврата..."));
    fflush(stdout);
    char line[8];
    (void)fgets(line, sizeof(line), stdin);
    terminal_raw();
    return status;
}

static int setting(struct settings *s, const char *section, const char *key) {
    (void)s;
    int index = settings_find(section, key);
    if (index < 0) {
        terminal_restore();
        fprintf(stderr, "hi: missing setting %s.%s\n", section, key);
        exit(1);
    }
    return index;
}

struct zone {
    const char *name, *posix;
};
static const struct zone zones[] = {
    {"UTC", "UTC0"},
    {"Europe/Moscow", "MSK-3"},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Asia/Yekaterinburg", "YEKT-5"},
    {"Asia/Novosibirsk", "NOVT-7"},
    {"Asia/Vladivostok", "VLAT-10"},
    {"Asia/Tokyo", "JST-9"},
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
};

static const char *zone_posix(const char *name) {
    for (size_t i = 0; i < sizeof(zones) / sizeof(zones[0]); ++i) {
        if (!strcmp(name, zones[i].name)) {
            return zones[i].posix;
        }
    }
    return "UTC0";
}

static int network_page(struct settings *s, bool allow_back) {
    const char *en[] = {"Keep current settings", "Automatic (DHCP)", "Static IPv4"};
    const char *rr[] = {"Сохранить текущие настройки", "Автоматически (DHCP)",
                        "Статический IPv4"};
    int choice = choose("Network", "Сеть", en, rr, 3, 0, allow_back);
    if (choice < 0) {
        return -1;
    }
    if (!choice) {
        return 0;
    }
    if (choice == 1) {
        return settings_set(s, setting(s, "Network", "Mode"), "DHCP");
    }

    char ip[64] = "192.168.1.100", mask[64] = "255.255.255.0",
         gateway[64] = "192.168.1.1", dns[64] = "1.1.1.1";
    title("Static IPv4", "Статический IPv4");
    if (prompt("Address", "Адрес", ip, sizeof(ip)) ||
        prompt("Netmask", "Маска", mask, sizeof(mask)) ||
        prompt("Gateway", "Шлюз", gateway, sizeof(gateway)) ||
        prompt("DNS server", "Сервер DNS", dns, sizeof(dns))) {
        return -1;
    }

    struct in_addr parsed;
    if (inet_pton(AF_INET, ip, &parsed) != 1 ||
        inet_pton(AF_INET, mask, &parsed) != 1 ||
        inet_pton(AF_INET, gateway, &parsed) != 1 ||
        inet_pton(AF_INET, dns, &parsed) != 1) {
        return 1;
    }
    return settings_set(s, setting(s, "Network", "Mode"), "Static") ||
           settings_set(s, setting(s, "Network", "IP"), ip) ||
           settings_set(s, setting(s, "Network", "Subnet"), mask) ||
           settings_set(s, setting(s, "Network", "Gateway"), gateway) ||
           settings_set(s, setting(s, "DNS", "1"), dns);
}

static int timezone_page(struct settings *s, bool allow_back) {
    const char *names[sizeof(zones) / sizeof(zones[0])];
    int selected = 0;
    int index = setting(s, "System", "Timezone");
    for (size_t i = 0; i < sizeof(zones) / sizeof(zones[0]); ++i) {
        names[i] = zones[i].name;
        if (!strcmp(s->values[index], zones[i].name)) {
            selected = (int)i;
        }
    }

    int choice = choose("Time zone", "Часовой пояс", names, names,
                        (int)(sizeof(zones) / sizeof(zones[0])), selected, allow_back);
    if (choice < 0) {
        return -1;
    }
    return settings_set(s, index, zones[choice].name);
}

static int clock_page(struct settings *s, bool allow_back) {
    const char *en[] = {"Keep current time", "Synchronize with NTP",
                        "Set date and time manually"};
    const char *rr[] = {"Сохранить текущее время", "Синхронизировать по NTP",
                        "Установить дату и время вручную"};
    int choice = choose("Date and time", "Дата и время", en, rr, 3, 0, allow_back);
    if (choice < 0 || !choice) {
        return choice < 0 ? -1 : 0;
    }
    if (choice == 1) {
        char *ntp[] = {"ntpdate", "-s", "pool.ntp.org", NULL};
        int status = run_visible("NTP synchronization", "Синхронизация NTP",
                                 "/bin/ntpdate", ntp);
        if (!status) {
            char *hw[] = {"hwclock", "-w", NULL};
            run_plain("/bin/hwclock", hw, -1);
        }
        return status;
    }

    char value[64] = "2026-10-13 12:00:00";
    title("Manual date and time", "Ручная установка даты и времени");
    puts(tr("Format: YYYY-MM-DD HH:MM:SS", "Формат: ГГГГ-ММ-ДД ЧЧ:ММ:СС"));
    if (prompt("Local time", "Местное время", value, sizeof(value))) {
        return -1;
    }

    struct tm tm = {0};
    if (!strptime(value, "%Y-%m-%d %H:%M:%S", &tm)) {
        return 1;
    }
    setenv("TZ", zone_posix(s->values[setting(s, "System", "Timezone")]), 1);
    tzset();
    time_t seconds = mktime(&tm);
    struct timespec ts = {seconds, 0};
    if (seconds < 0 || clock_settime(CLOCK_REALTIME, &ts)) {
        return 1;
    }

    char *hw[] = {"hwclock", "-w", NULL};
    return run_plain("/bin/hwclock", hw, -1);
}

struct font_choice {
    const char *name, *path;
};
static const struct font_choice fonts[] = {
    {"DejaVu Sans Mono", "/assets/fonts/dejavu-mono.psf"},
    {"Terminus", "/assets/fonts/terminus.psf"},
    {"Hack", "/assets/fonts/hack.psf"},
    {"Noto Mono", "/assets/fonts/noto-mono.psf"},
    {"Liberation Mono", "/assets/fonts/liberation-mono.psf"},
    {"Ubuntu Mono", "/assets/fonts/ubuntu-mono.psf"},
};

static int font_page(struct settings *s, bool allow_back) {
    const char *names[sizeof(fonts) / sizeof(fonts[0])];
    int selected = 0, index = setting(s, "Terminal", "Font");
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); ++i) {
        names[i] = fonts[i].name;
        if (!strcmp(s->values[index], fonts[i].path)) {
            selected = (int)i;
        }
    }

    int choice = choose("Monospace font", "Моноширинный шрифт", names, names,
                        (int)(sizeof(fonts) / sizeof(fonts[0])), selected, allow_back);
    if (choice < 0) {
        return -1;
    }
    return settings_set(s, index, fonts[choice].path);
}

static int write_complete(void) {
    if (mkdir_one(HI_DIR, 0700)) {
        return -1;
    }

    char temp[128];
    snprintf(temp, sizeof(temp), HI_DIR "/complete.tmp.%ld", (long)getpid());
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return -1;
    }
    dprintf(fd, "completed=%ld\n", (long)time(NULL));
    int result = fsync(fd) || close(fd) || rename(temp, HI_DONE);
    if (result) {
        unlink(temp);
    }
    return result ? -1 : 0;
}

// Показать отдельное согласие и передать явный выбор клиенту отправки
static int data_sharing_page(struct settings *s) {
    bool enabled = false;
    title("Current consent", "Текущее согласие");
    char *status_args[] = {"stats", "--consent-status", NULL};
    run_plain("/bin/stats", status_args, STDOUT_FILENO);
    pause_screen();
    // Полный текст разбит на страницы и доступен без сети
    const char *line = data_consent_text;
    while (*line) {
        title("Data sharing consent", "Согласие на отправку данных");
        for (unsigned row = 0; row < 10 && *line; ++row) {
            const char *end = strchr(line, '\n');
            if (end == NULL) {
                puts(line);
                line += strlen(line);
                break;
            }
            printf("%.*s\n", (int)(end - line), line);
            line = end + 1;
        }
        pause_screen();
    }
    for (;;) {
        title("Your choice", "Ваш выбор");
        printf("%s\n", tr("Refusing does not limit the OS, packages or support.",
                           "Отказ не ограничивает ОС, пакеты и поддержку."));
        printf("%s %s\n%s %s\n\n%s", !enabled ? ">" : " ",
               tr("Do not send / withdraw consent", "Не отправлять / отозвать согласие"),
               enabled ? ">" : " ",
               tr("I consent and enable sending", "Даю согласие и включаю отправку"),
               tr("Arrows: select   Enter: confirm   Esc: back",
                  "Стрелки: выбор   Enter: подтвердить   Esc: назад"));
        fflush(stdout);
        int key = key_read();
        if (key < 0 || key == 27 || key == KEY_LEFT) {
            return -1;
        }
        if (key == KEY_UP || key == KEY_DOWN || key == '\t') {
            enabled = !enabled;
        } else if (key == '\r' || key == '\n') {
            // Клиент мог подтвердить или отозвать согласие, пока открыт мастер
            struct settings latest;
            if (settings_load(&latest, s->root)) {
                return 1;
            }
            // Сохраняем только несохранённый выбор обычных полей мастера
            for (int i = 0; i < setting_public_count; ++i) {
                if (s->changed[i] && settings_set(&latest, i, s->values[i])) {
                    return 1;
                }
            }
            *s = latest;
            if (settings_set(s, setting(s, "stats", "stats"), "0")) {
                return 1;
            }
            if (!enabled && *settings_get(s, "DataConsent", "Token") &&
                settings_set(s, setting(s, "DataConsent", "State"), "revoking")) {
                return 1;
            }
            if (enabled && !strcmp(settings_get(s, "DataConsent", "State"), "active") &&
                settings_set(s, setting(s, "DataConsent", "State"), "pending")) {
                return 1;
            }
            if (settings_save(s)) {
                return 1;
            }
            title("Applying your choice", "Применение выбора");
            fflush(stdout);
            char *args[] = {"stats", enabled ? "--consent-accept" : "--consent-revoke", NULL};
            int status = run_plain_timeout("/bin/stats", args, STDOUT_FILENO, 90U);
            if (settings_load(s, "")) {
                return 1;
            }
            if (status) {
                title("No server confirmation", "Нет подтверждения сервера");
                puts(tr("Without confirmed consent, reports are not sent.",
                        "Без подтверждённого согласия отчёты не отправляются."));
                const char *state = settings_get(s, "DataConsent", "State");
                if (!strcmp(state, "pending")) {
                    puts(tr("Consent confirmation will be retried at boot.",
                            "Подтверждение согласия повторится при загрузке."));
                } else if (!strcmp(state, "revoking")) {
                    puts(tr("Reports are stopped. Withdrawal will be retried at boot.",
                            "Отправка остановлена. Отзыв повторится при загрузке."));
                } else {
                    puts(tr("Check the consent status and try again in HI.",
                            "Проверьте состояние согласия и повторите действие в HI."));
                }
                pause_screen();
            }
            return 0;
        }
    }
}

// Отличить заданный владельцем пароль от пустой или закрытой записи root
static int administrator_password_ready(void) {
    errno = 0;
    struct spwd *account = getspnam("root");
    if (account == NULL || account->sp_pwdp == NULL) {
        if (errno == 0) {
            errno = EINVAL;
        }
        return -1;
    }

    const char *hash = account->sp_pwdp;
    return hash[0] != '\0' && hash[0] != '!' && hash[0] != '*';
}

// Получить пароль через штатный passwd без передачи секрета в аргументах
static int enroll_administrator(void) {
    int ready = administrator_password_ready();
    if (ready != 0) {
        return ready > 0 ? 0 : -1;
    }

    terminal_restore();
    title("Administrator password", "Пароль администратора");
    puts(tr("Set your own root password and enter it twice.",
            "Задайте свой пароль root и введите его два раза."));
    fflush(stdout);
    char *const arguments[] = {"passwd", "root", NULL};
    int status = run_plain("/bin/passwd", arguments, -1);
    int restored = tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
    int raw = terminal_raw();
    if (status != 0 || restored != 0 || raw != 0 || administrator_password_ready() != 1) {
        errno = EACCES;
        return -1;
    }

    return 0;
}

// Настроить систему и сохранить выбор до первого запуска пользовательской сессии
static int first_run(struct settings *s) {
    int page = 0;
    while (page < 7) {
        int result = 0;
        if (page == 0) {
            const char *en[] = {"Русский", "English"};
            int choice =
                choose("Choose language / Выберите язык",
                       "Выберите язык / Choose language", en, en, 2, ru ? 0 : 1, false);
            ru = choice == 0;
            s->ru = ru;
            result =
                settings_set(s, setting(s, "System", "Language"), ru ? "ru" : "en");
        } else if (page == 1) {
            result = network_page(s, true);
        } else if (page == 2) {
            result = timezone_page(s, true);
        } else if (page == 3) {
            result = clock_page(s, true);
        } else if (page == 4) {
            result = font_page(s, true);
        } else if (page == 5) {
            result = data_sharing_page(s);
        } else {
            const char *en[] = {"Finish setup and open shell", "Back"};
            const char *rr[] = {"Завершить настройку и открыть оболочку", "Назад"};
            title("Ready to apply", "Готово к применению");
            printf("%s: %s\n%s: %s\n%s: %s\n%s: %s\n\n", tr("Language", "Язык"),
                   ru ? "Русский" : "English", tr("Network", "Сеть"),
                   s->values[setting(s, "Network", "Mode")],
                   tr("Time zone", "Часовой пояс"),
                   s->values[setting(s, "System", "Timezone")], tr("Font", "Шрифт"),
                   s->values[setting(s, "Terminal", "Font")]);
            int choice =
                choose("Ready to apply", "Готово к применению", en, rr, 2, 0, true);
            if (choice != 0) {
                page--;
                continue;
            }
            if (enroll_administrator() != 0 || settings_save(s) || write_complete()) {
                title("Cannot finish setup", "Не удалось завершить настройку");
                printf("%s\n", s->error[0] ? s->error : strerror(errno));
                pause_screen();
                continue;
            }
            return 0;
        }
        if (result < 0) {
            if (page) {
                page--;
            }
            continue;
        }
        if (result > 0) {
            title("Invalid value or operation failed",
                  "Некорректное значение или ошибка операции");
            printf("%s\n", strerror(errno));
            pause_screen();
            continue;
        }
        page++;
    }
    return 0;
}

static void append_file(FILE *out, const char *path) {
    fprintf(out, "\n===== FILE %s =====\n", path);
    FILE *in = fopen(path, "rb");
    if (!in) {
        fprintf(out, "Unavailable: %s\n", strerror(errno));
        return;
    }

    char buffer[8192];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), in))) {
        fwrite(buffer, 1, n, out);
    }
    if (ferror(in)) {
        fprintf(out, "\nRead error: %s\n", strerror(errno));
    }
    fclose(in);
}

static void append_command(FILE *out, const char *name, const char *path,
                           char *const argv[]) {
    fprintf(out, "\n===== COMMAND %s =====\n", name);
    fflush(out);
    int status = run_plain(path, argv, fileno(out));
    fprintf(out, "\n[exit=%d]\n", status);
    fflush(out);
}

static int report_create(const char *requested, char *created, size_t capacity) {
    if (mkdir_one("/var/log", 0755) && errno != EEXIST) {
        return -1;
    }
    if (mkdir_one(HI_LOG_DIR, 0700)) {
        return -1;
    }

    char path[512];
    if (requested) {
        snprintf(path, sizeof(path), "%s", requested);
    } else {
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        char stamp[64];
        strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
        snprintf(path, sizeof(path), HI_LOG_DIR "/%s.log", stamp);
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return -1;
    }

    FILE *out = fdopen(fd, "w");
    if (!out) {
        close(fd);
        unlink(path);
        return -1;
    }
    time_t now = time(NULL);
    fprintf(out, "SynapseOS HI full system report\nGenerated: %s", ctime(&now));
    const char *files_to_read[] = {
        "/etc/hostname", "/etc/hosts",
        "/etc/resolv.conf",  "/etc/passwd",   "/etc/shadow",
        "/proc/version",     "/proc/cmdline", "/proc/cpuinfo",
        "/proc/meminfo",     "/proc/pci",     "/proc/usb",
        "/proc/modules",     "/proc/thermal", "/proc/acpi",
        "/proc/hw_errors",   "/proc/mounts",  "/proc/filesystems",
        "/var/log/boot.log"};
    for (size_t i = 0; i < sizeof(files_to_read) / sizeof(files_to_read[0]); ++i) {
        append_file(out, files_to_read[i]);
    }

    char *hw[] = {"hwinfo", NULL}, *ip[] = {"ip", "addr", NULL},
         *df[] = {"df", "-h", NULL};
    char *ps[] = {"ps", "-ef", NULL}, *svc[] = {"service", "list", NULL};
    char *packages[] = {"setup", "ls", "-i", NULL}, *disk[] = {"diskhealth", NULL};
    append_command(out, "hwinfo", "/bin/hwinfo", hw);
    append_command(out, "ip addr", "/bin/ip", ip);
    append_command(out, "df -h", "/bin/df", df);
    append_command(out, "ps -ef", "/bin/ps", ps);
    append_command(out, "service list", "/bin/service", svc);
    append_command(out, "setup ls -i", "/bin/setup", packages);
    append_command(out, "diskhealth", "/bin/diskhealth", disk);
    int failed = fflush(out);
    if (fsync(fd)) {
        failed = 1;
    }
    if (fclose(out)) {
        failed = 1;
    }
    if (failed) {
        unlink(path);
        return -1;
    }
    snprintf(created, capacity, "%s", path);
    return 0;
}

static long hottest_millicelsius(void) {
    FILE *f = fopen("/proc/thermal", "r");
    if (!f) {
        return -1;
    }

    char name[64];
    long value, flags, hottest = -1;
    while (fscanf(f, "%63s %ld %ld", name, &value, &flags) == 3) {
        if (!flags && value > hottest) {
            hottest = value;
        }
    }
    fclose(f);
    return hottest;
}

static int cpu_stress(unsigned seconds) {
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    int workers = online > 1 ? (online > 8 ? 8 : (int)online) : 1;
    pid_t children[8];
    for (int i = 0; i < workers; ++i) {
        children[i] = fork();
        if (!children[i]) {
            struct timespec start, now;
            clock_gettime(CLOCK_MONOTONIC, &start);
            volatile uint64_t x = 0x9e3779b97f4a7c15ULL ^ (unsigned)i;
            do {
                for (int k = 0; k < 1000000; ++k) {
                    x = (x ^ (x >> 12)) * 0x2545f4914f6cdd1dULL + k;
                }
                clock_gettime(CLOCK_MONOTONIC, &now);
            } while ((unsigned)(now.tv_sec - start.tv_sec) < seconds);
            _exit(x == 0);
        }
    }

    int status = 0;
    for (unsigned elapsed = 0; elapsed < seconds; ++elapsed) {
        long temp = hottest_millicelsius();
        printf("\r%s %u/%u s", tr("CPU stress", "Нагрузка CPU"), elapsed + 1, seconds);
        if (temp >= 0) {
            printf("   %.1f C", temp / 1000.0);
        }
        fflush(stdout);
        if (temp >= 90000) {
            status = 2;
            for (int i = 0; i < workers; ++i) {
                kill(children[i], SIGTERM);
            }
            break;
        }
        sleep(1);
    }

    for (int i = 0; i < workers; ++i) {
        if (wait_child(children[i])) {
            status = status ? status : 1;
        }
    }
    puts("");
    return status;
}

static int memory_test(void) {
    struct sysinfo si;
    size_t bytes = 64 * 1024 * 1024;
    if (!sysinfo(&si)) {
        unsigned long long free_bytes = (unsigned long long)si.freeram * si.mem_unit;
        if (free_bytes / 4 < bytes) {
            bytes = (size_t)(free_bytes / 4);
        }
    }
    if (bytes < 1024 * 1024) {
        return 1;
    }
    uint64_t *memory = malloc(bytes);
    if (!memory) {
        return 1;
    }

    size_t words = bytes / sizeof(*memory);
    const uint64_t pattern = 0xa5a55a5af0f00f0fULL;
    for (size_t i = 0; i < words; ++i) {
        memory[i] = pattern ^ i;
    }

    for (size_t i = 0; i < words; ++i) {
        if (memory[i] != (pattern ^ i)) {
            free(memory);
            return 2;
        }
    }
    free(memory);
    printf("%s: %zu MiB OK\n", tr("Memory", "Память"), bytes / 1024 / 1024);
    return 0;
}

static int disk_test(bool write_test) {
    const char *path = "/var/tmp/hi-disk-test.bin";
    mkdir("/var/tmp", 0755);
    int flags = write_test ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY;
    int fd = open(write_test ? path : "/boot/boot/kernel", flags, 0600);
    if (fd < 0) {
        return 1;
    }

    unsigned char *buffer = malloc(1024 * 1024);
    if (!buffer) {
        close(fd);
        return 1;
    }

    for (size_t i = 0; i < 1024 * 1024; ++i) {
        buffer[i] = (unsigned char)(i * 131u + 17u);
    }

    int result = 0;
    size_t total = 0, target = write_test ? 64 * 1024 * 1024 : 256 * 1024 * 1024;
    while (total < target) {
        ssize_t n =
            write_test ? write(fd, buffer, 1024 * 1024) : read(fd, buffer, 1024 * 1024);
        if (!write_test && n == 0) {
            lseek(fd, 0, SEEK_SET);
            continue;
        }
        if (n <= 0) {
            result = 1;
            break;
        }
        total += (size_t)n;
    }
    if (write_test && !result && fsync(fd)) {
        result = 1;
    }
    close(fd);
    free(buffer);
    if (write_test) {
        unlink(path);
    }
    printf("%s: %zu MiB %s\n", tr("Disk", "Диск"), total / 1024 / 1024,
           result ? "FAILED" : "OK");
    return result;
}

static void diagnostics_menu(void) {
    const char *en[] = {"CPU stress: 10 seconds",
                        "CPU stress: 60 seconds",
                        "Memory pattern test",
                        "Disk read test",
                        "Disk write test (64 MiB)",
                        "Network test",
                        "Back"};
    const char *rr[] = {"Нагрузка CPU: 10 секунд",
                        "Нагрузка CPU: 60 секунд",
                        "Проверка памяти",
                        "Проверка чтения диска",
                        "Проверка записи диска (64 МиБ)",
                        "Проверка сети",
                        "Назад"};
    for (;;) {
        int choice = choose("Diagnostics and stress tests",
                            "Диагностика и нагрузочные тесты", en, rr, 7, 0, true);
        if (choice < 0 || choice == 6) {
            return;
        }
        terminal_restore();
        printf("\033[2J\033[H");
        int result = 0;
        if (choice == 0 || choice == 1) {
            result = cpu_stress(choice ? 60 : 10);
        } else if (choice == 2) {
            result = memory_test();
        } else if (choice == 3) {
            result = disk_test(false);
        } else if (choice == 4) {
            printf("%s [y/N] ",
                   tr("Write test creates and deletes a 64 MiB file. Continue?",
                      "Тест создаст и удалит файл 64 МиБ. Продолжить?"));
            fflush(stdout);
            char line[8];
            fgets(line, sizeof(line), stdin);
            if (line[0] == 'y' || line[0] == 'Y' || (unsigned char)line[0] == 0xd0) {
                result = disk_test(true);
            }
        } else {
            char *ping[] = {"ping", "-c", "4", "1.1.1.1", NULL};
            result = run_plain("/bin/ping", ping, -1);
            char *dns[] = {"nslookup", "synapseos.ru", NULL};
            if (!result) {
                result = run_plain("/bin/nslookup", dns, -1);
            }
        }
        printf("\n%s: %s\n%s", tr("Result", "Результат"), result ? "FAILED" : "OK",
               tr("Press Enter to return...", "Нажмите Enter для возврата..."));
        fflush(stdout);
        char line[8];
        fgets(line, sizeof(line), stdin);
        terminal_raw();
    }
}

static void update_system(void) {
    terminal_restore();
    printf("\033[2J\033[H\033[1;36mHI — %s\033[0m\n\n",
           tr("Full system update", "Комплексное обновление ОС"));
    mkdir_one(HI_DIR, 0700);
    mkdir(HI_DIR "/backups", 0700);
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char stamp[64], archive[256];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
    snprintf(archive, sizeof(archive), HI_DIR "/backups/pre-update-%s.tar", stamp);
    char *backup[] = {"backup", "create", archive, NULL};
    int status = run_plain("/bin/backup", backup, -1);
    if (!status) {
        char *setup[] = {"setup", "auto", NULL};
        status = run_plain("/bin/setup", setup, -1);
    }
    printf("\n%s: %s\n", tr("Update", "Обновление"), status ? "FAILED" : "OK");
    if (!status) {
        printf("%s [y/N] ", tr("Reboot now?", "Перезагрузить сейчас?"));
    } else {
        printf("%s", tr("Press Enter to return...", "Нажмите Enter для возврата..."));
    }
    fflush(stdout);
    char line[8];
    fgets(line, sizeof(line), stdin);
    if (!status && (line[0] == 'y' || line[0] == 'Y')) {
        char *reboot[] = {"reboot", NULL};
        run_plain("/bin/reboot", reboot, -1);
    }
    terminal_raw();
}

static void main_menu(struct settings *s) {
    const char *en[] = {"Install SynapseOS to disk",
                        "Hardware and sensors",
                        "Network settings and test",
                        "Date and time",
                        "Choose font",
                        "Diagnostics and stress tests",
                        "Full system update",
                        "Generate full report",
                        "Open system settings",
                        "Data sharing consent",
                        "Exit"};
    const char *rr[] = {"Установить SynapseOS на диск",
                        "Оборудование и датчики",
                        "Настройка и проверка сети",
                        "Дата и время",
                        "Выбор шрифта",
                        "Диагностика и нагрузочные тесты",
                        "Комплексное обновление ОС",
                        "Создать полный отчёт",
                        "Открыть системные настройки",
                        "Согласие на отправку данных",
                        "Выход"};
    bool live = getenv("SYNAPSE_LIVE") && !strcmp(getenv("SYNAPSE_LIVE"), "1");
    for (;;) {
        int choice =
            choose("System setup and diagnostics", "Настройка и диагностика системы",
                   en + (live ? 0 : 1), rr + (live ? 0 : 1), live ? 11 : 10, 0, false);
        if (live && choice == 0) {
            terminal_restore();
            hi_installer(ru);
            fputs(tr("\nPress Enter to return...", "\nНажмите Enter для возврата..."),
                  stdout);
            fflush(stdout);
            char line[8];
            fgets(line, sizeof(line), stdin);
            terminal_raw();
            continue;
        }
        if (live) {
            choice--;
        }
        if (choice == 9) {
            return;
        }
        if (choice == 0) {
            char *args[] = {"hwinfo", NULL};
            run_visible("Hardware and sensors", "Оборудование и датчики", "/bin/hwinfo",
                        args);
        } else if (choice == 1) {
            if (network_page(s, true) >= 0 && !settings_save(s)) {
                char *args[] = {"ip", "addr", NULL};
                run_visible("Network status", "Состояние сети", "/bin/ip", args);
            }
        } else if (choice == 2) {
            if (timezone_page(s, true) >= 0) {
                clock_page(s, true);
                settings_save(s);
            }
        } else if (choice == 3) {
            if (font_page(s, true) >= 0) {
                settings_save(s);
            }
        } else if (choice == 4) {
            diagnostics_menu();
        } else if (choice == 5) {
            update_system();
        } else if (choice == 6) {
            char path[512] = "";
            int status = report_create(NULL, path, sizeof(path));
            title("Full report", "Полный отчёт");
            printf("%s\n", status ? strerror(errno) : path);
            pause_screen();
        } else if (choice == 8) {
            int result = data_sharing_page(s);
            if (result > 0) {
                title("Cannot save consent choice", "Не удалось сохранить выбор согласия");
                puts(tr("The choice was not applied. Check the settings and try again.",
                        "Выбор не применён. Проверьте настройки и повторите действие."));
                printf("%s\n", s->error[0] ? s->error : strerror(errno));
                pause_screen();
            } else if (result == 0) {
                puts(tr("Sending starts at the next boot. Withdrawal blocks new reports immediately.",
                        "Отправка запускается при следующей загрузке. Отзыв сразу блокирует новые отчёты."));
                pause_screen();
            }
        } else {
            char *args[] = {"settings", NULL};
            run_visible("System settings", "Системные настройки", "/bin/settings",
                        args);
            settings_load(s, "");
            ru = s->ru;
        }
    }
}

static void usage(void) {
    puts("Usage: hi [--first-boot | --first-run | --install | --report[=PATH] | "
         "--help]\n"
         "  --first-boot  run before the graphical session and return to init\n"
         "  --first-run   run the first-boot wizard, then open /bin/sh\n"
         "  --install     install SynapseOS from a Live session\n"
         "  --report      write /var/log/hi/YYYY-MM-DD_HH-MM-SS.log\n"
         "  --report=PATH write the same report to an explicit new file");
}

int main(int argc, char **argv) {
    bool first = false, first_boot = false, install = false;
    const char *report = NULL;
    bool report_requested = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--first-boot")) {
            first = first_boot = true;
        } else if (!strcmp(argv[i], "--first-run")) {
            first = true;
        } else if (!strcmp(argv[i], "--install")) {
            install = true;
        } else if (!strcmp(argv[i], "--report")) {
            report_requested = true;
        } else if (!strncmp(argv[i], "--report=", 9) && argv[i][9]) {
            report_requested = true;
            report = argv[i] + 9;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        } else {
            usage();
            return 2;
        }
    }
    if (report_requested) {
        char path[512] = "";
        if (report_create(report, path, sizeof(path))) {
            fprintf(stderr, "hi: report: %s\n", strerror(errno));
            return 1;
        }
        puts(path);
        return 0;
    }

    struct settings s;
    if (settings_load(&s, "")) {
        fprintf(stderr, "hi: %s\n", s.error);
        return 1;
    }
    ru = s.ru;
    if (install) {
        return hi_installer(ru);
    }
    setenv("TZ", zone_posix(s.values[setting(&s, "System", "Timezone")]), 1);
    tzset();
    if (terminal_raw()) {
        fputs("hi: interactive terminal required\n", stderr);
        return 1;
    }
    atexit(terminal_restore);
    signal(SIGINT, interrupted);
    signal(SIGTERM, interrupted);
    signal(SIGHUP, interrupted);
    if (first) {
        if (access(HI_DONE, F_OK)) {
            first_run(&s);
        }
        if (getenv("SYNAPSE_LIVE") && !strcmp(getenv("SYNAPSE_LIVE"), "1")) {
            main_menu(&s);
        }
    } else {
        main_menu(&s);
    }
    terminal_restore();
    if (first && !first_boot) {
        execl("/bin/sh", "sh", (char *)NULL);
        perror("hi: /bin/sh");
        return 127;
    }
    return 0;
}
