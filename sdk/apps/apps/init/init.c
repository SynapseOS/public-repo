// init.c: Инициализация пользовательского пространства
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Настройка аккаунтов, терминалов, сети и запуск служб
// Подсистема: init
// Первое изменение в истории Git: 09.10.2026
// Последнее изменение: 23.11.2026
// Доступ: общедоступный

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <glob.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <time.h>
#include <crypt.h>
#include <shadow.h>
#include <pwd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include "../common/service_log.h"
#include "dns-refresh.h"

#ifndef gettid
#define gettid() syscall(SYS_gettid)
#endif

#ifndef TIOCSCTTY
#define TIOCSCTTY 0x540E
#endif

extern char **environ;

#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_DIM "\033[2m"
#define ANSI_RED "\033[31m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_BLUE "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN "\033[36m"
#define ANSI_WHITE "\033[97m"

#define MAX_DNS_SERVERS 16
#define INI_MAX_LINE 512
#define INI_MAX_SECTION 64

typedef struct {
    char hostname[256];
    char domain[256];
    char language[16];
    char timezone[64];
    int stats_enabled;
    int stats_delay;
    char stats_server[256];
    char stats_default[256];
    char default_shell[256];
    char terminal_font[256];
    int serial_consoles;
    int net_interface;
    char net_mode[16];
    char power_profile[16];
    char net_ip[64];
    char net_subnet[64];
    char net_gateway[64];
    char dns_servers[MAX_DNS_SERVERS][64];
    int dns_count;
    int dns_explicit;
    int ssh_enabled;
    int ssh_valid;
    int ssh_port;
    char ssh_login[64];
    char ssh_password[256];
    char ssh_password_hash[256];
    int ftp_enabled;
    int ftp_port;
    int ftp_passive_min;
    int ftp_passive_max;
    int desktop_enabled;
    int desktop_login_manager;
    int display_fps;
    char desktop_path[256];
    int setup_check_updates;
    int qga_enabled;
    char qga_path[256];
} syn_cfg_t;

typedef int (*ini_handler_t)(void *user, const char *section, const char *key,
                             const char *value);

static const char *timezone_posix(const char *name) {
    struct mapping {
        const char *name, *value;
    };
    static const struct mapping known[] = {
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
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        if (!strcmp(name, known[i].name)) {
            return known[i].value;
        }
    }
    return "UTC0";
}

static void configure_locale(const syn_cfg_t *cfg) {
    setenv("LANG", !strncmp(cfg->language, "ru", 2) ? "ru_RU.UTF-8" : "en_US.UTF-8", 1);
    setenv("TZ", timezone_posix(cfg->timezone), 1);
    tzset();
}

static char *strip_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '\0') {
        return s;
    }

    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        end--;
    }
    *(end + 1) = '\0';
    return s;
}

static int ini_parse_file(FILE *fp, ini_handler_t handler, void *user) {
    char line[INI_MAX_LINE];
    char section[INI_MAX_SECTION] = "";
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
        }
        if (len > 0 && line[len - 1] == '\r') {
            line[--len] = '\0';
        }

        char *s = strip_ws(line);
        if (*s == '\0' || *s == '#' || *s == ';') {
            continue;
        }
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end) {
                return lineno;
            }
            *end = '\0';
            strncpy(section, strip_ws(s + 1), INI_MAX_SECTION - 1);
            section[INI_MAX_SECTION - 1] = '\0';
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            return lineno;
        }
        *eq = '\0';
        const char *key = strip_ws(s);
        const char *value = strip_ws(eq + 1);
        if (handler(user, section, key, value) != 0) {
            return lineno;
        }
    }
    return 0;
}

static int ini_parse_path(const char *path, ini_handler_t handler, void *user) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    int err = ini_parse_file(fp, handler, user);
    fclose(fp);
    return err;
}

static int synapse_config_handler(void *user, const char *section, const char *key,
                                  const char *value) {
    syn_cfg_t *cfg = (syn_cfg_t *)user;
    if (strcmp(section, "System") == 0) {
        if (strcmp(key, "Hostname") == 0) {
            strncpy(cfg->hostname, value, sizeof(cfg->hostname) - 1);
        } else if (strcmp(key, "Domain") == 0) {
            strncpy(cfg->domain, value, sizeof(cfg->domain) - 1);
        } else if (strcmp(key, "Language") == 0) {
            strncpy(cfg->language, value, sizeof(cfg->language) - 1);
        } else if (strcmp(key, "Timezone") == 0) {
            strncpy(cfg->timezone, value, sizeof(cfg->timezone) - 1);
        }
    } else if (!strcmp(section, "Display") && !strcmp(key, "FPS")) {
        char *end = NULL;
        long fps = strtol(value, &end, 10);
        if (end != value && *end == '\0' && fps >= 1 && fps <= 240) {
            cfg->display_fps = (int)fps;
        }
    } else if (!strcmp(section, "Power") && !strcmp(key, "Profile")) {
        if (!strcmp(value, "balanced") || !strcmp(value, "firmware") ||
            !strcmp(value, "powersave") || !strcmp(value, "performance")) {
            snprintf(cfg->power_profile, sizeof(cfg->power_profile), "%s", value);
        }
    } else if (strcmp(section, "Setup") == 0) {
        if (!strcmp(key, "CheckUpdates")) {
            cfg->setup_check_updates = !strcmp(value, "1") ||
                                       !strcasecmp(value, "true") ||
                                       !strcasecmp(value, "yes");
        }
    } else if (strcmp(section, "stats") == 0) {
        if (strcmp(key, "stats") == 0) {
            cfg->stats_enabled =
                (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
                 strcasecmp(value, "yes") == 0);
        } else if (strcmp(key, "statsDelay") == 0) {
            cfg->stats_delay = atoi(value);
        } else if (strcmp(key, "statsServer") == 0) {
            strncpy(cfg->stats_server, value, sizeof(cfg->stats_server) - 1);
        } else if (strcmp(key, "statsDefault") == 0) {
            strncpy(cfg->stats_default, value, sizeof(cfg->stats_default) - 1);
        }
    } else if (strcmp(section, "Boot") == 0) {
        if (strcmp(key, "DefaultShell") == 0) {
            strncpy(cfg->default_shell, value, sizeof(cfg->default_shell) - 1);
        }
    } else if (strcmp(section, "Desktop") == 0) {
        if (strcmp(key, "Enabled") == 0) {
            cfg->desktop_enabled =
                strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0;
        } else if (strcmp(key, "LoginManager") == 0) {
            cfg->desktop_login_manager =
                strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0;
        } else if (strcmp(key, "Path") == 0) {
            strncpy(cfg->desktop_path, value, sizeof(cfg->desktop_path) - 1);
        }
    } else if (strcmp(section, "Terminal") == 0) {
        if (strcmp(key, "Font") == 0) {
            strncpy(cfg->terminal_font, value, sizeof(cfg->terminal_font) - 1);
        } else if (strcmp(key, "SerialConsoles") == 0) {
            char *end = NULL;
            long count = strtol(value, &end, 10);
            if (end != value && *end == '\0' && count >= 0 && count <= 2) {
                cfg->serial_consoles = (int)count;
            }
        }
    } else if (strcmp(section, "SSH") == 0) {
        if (strcmp(key, "Enabled") == 0) {
            cfg->ssh_enabled =
                strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0;
        } else if (strcmp(key, "Login") == 0) {
            size_t n = strlen(value);
            if (!n || n >= sizeof(cfg->ssh_login)) {
                cfg->ssh_valid = 0;
                return 0;
            }

            for (size_t j = 0; j < n; j++) {
                if (!isalnum((unsigned char)value[j]) && value[j] != '_' &&
                    value[j] != '-' && value[j] != '.') {
                    cfg->ssh_valid = 0;
                    return 0;
                }
            }
            strcpy(cfg->ssh_login, value);
        } else if (strcmp(key, "Password") == 0) {
            if (strlen(value) >= sizeof(cfg->ssh_password)) {
                cfg->ssh_valid = 0;
            } else {
                strcpy(cfg->ssh_password, value);
            }
        } else if (strcmp(key, "PasswordHash") == 0) {
            if (strlen(value) >= sizeof(cfg->ssh_password_hash) || strchr(value, ':')) {
                cfg->ssh_valid = 0;
            } else {
                strcpy(cfg->ssh_password_hash, value);
            }
        } else if (strcmp(key, "Port") == 0) {
            char *end;
            long port = strtol(value, &end, 10);
            if (!*value || *end || port < 1 || port > 65535) {
                cfg->ssh_valid = 0;
            } else {
                cfg->ssh_port = (int)port;
            }
        }
    } else if (strcmp(section, "FTP") == 0) {
        if (strcmp(key, "Enabled") == 0) {
            cfg->ftp_enabled = !strcmp(value, "1") || !strcasecmp(value, "true");
        } else {
            char *end;
            long port = strtol(value, &end, 10);
            if (!*value || *end || port < 1 || port > 65535) {
                return -1;
            }
            if (!strcmp(key, "Port")) {
                cfg->ftp_port = port;
            } else if (!strcmp(key, "PassiveMin")) {
                cfg->ftp_passive_min = port;
            } else if (!strcmp(key, "PassiveMax")) {
                cfg->ftp_passive_max = port;
            }
        }
    } else if (strcmp(section, "GuestAgent") == 0) {
        if (strcmp(key, "Enabled") == 0) {
            cfg->qga_enabled =
                strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0;
        } else if (strcmp(key, "Path") == 0) {
            snprintf(cfg->qga_path, sizeof(cfg->qga_path), "%s", value);
        }
    } else if (strcmp(section, "Network") == 0) {
        if (strcmp(key, "Interface") == 0) {
            cfg->net_interface = atoi(value);
        } else if (strcmp(key, "Mode") == 0) {
            strncpy(cfg->net_mode, value, sizeof(cfg->net_mode) - 1);
        } else if (strcmp(key, "IP") == 0) {
            strncpy(cfg->net_ip, value, sizeof(cfg->net_ip) - 1);
        } else if (strcmp(key, "Subnet") == 0) {
            strncpy(cfg->net_subnet, value, sizeof(cfg->net_subnet) - 1);
        } else if (strcmp(key, "Gateway") == 0) {
            strncpy(cfg->net_gateway, value, sizeof(cfg->net_gateway) - 1);
        }
    } else if (strcmp(section, "DNS") == 0) {
        if (cfg->dns_count < MAX_DNS_SERVERS) {
            strncpy(cfg->dns_servers[cfg->dns_count], value,
                    sizeof(cfg->dns_servers[0]) - 1);
            cfg->dns_count++;
            cfg->dns_explicit = 1;
        }
    }
    return 0;
}

static void print_config(const syn_cfg_t *cfg) {
    printf(ANSI_BOLD ANSI_MAGENTA "[init]" ANSI_RESET " " ANSI_CYAN
                                  "Hostname:    " ANSI_RESET " %s\n",
           cfg->hostname[0] ? cfg->hostname : "(not set)");
    printf(ANSI_BOLD ANSI_MAGENTA "[init]" ANSI_RESET " " ANSI_CYAN
                                  "Domain:      " ANSI_RESET " %s\n",
           cfg->domain[0] ? cfg->domain : "(not set)");
    printf(ANSI_BOLD ANSI_MAGENTA "[init]" ANSI_RESET " " ANSI_CYAN
                                  "Language:    " ANSI_RESET " %s\n",
           cfg->language[0] ? cfg->language : "(not set)");
    printf(ANSI_BOLD ANSI_MAGENTA "[init]" ANSI_RESET " " ANSI_CYAN
                                  "Timezone:    " ANSI_RESET " %s\n",
           cfg->timezone[0] ? cfg->timezone : "(not set)");
    printf(ANSI_BOLD ANSI_MAGENTA "[init]" ANSI_RESET " " ANSI_CYAN
                                  "Def. Shell:  " ANSI_RESET " %s\n",
           cfg->default_shell[0] ? cfg->default_shell : "/bin/sh");
    printf(ANSI_BOLD ANSI_MAGENTA "[init]" ANSI_RESET " " ANSI_CYAN
                                  "Network Mode:" ANSI_RESET " %s\n",
           cfg->net_mode[0] ? cfg->net_mode : "(not set)");
    if (cfg->net_mode[0] && strcasecmp(cfg->net_mode, "Static") == 0) {
        printf(ANSI_BOLD ANSI_MAGENTA "[init]" ANSI_RESET "   " ANSI_DIM
                                      "IP:      " ANSI_RESET " %s\n",
               cfg->net_ip[0] ? cfg->net_ip : "(not set)");
        printf(ANSI_BOLD ANSI_MAGENTA "[init]" ANSI_RESET "   " ANSI_DIM
                                      "Subnet:  " ANSI_RESET " %s\n",
               cfg->net_subnet[0] ? cfg->net_subnet : "(not set)");
        printf(ANSI_BOLD ANSI_MAGENTA "[init]" ANSI_RESET "   " ANSI_DIM
                                      "Gateway: " ANSI_RESET " %s\n",
               cfg->net_gateway[0] ? cfg->net_gateway : "(not set)");
    }
    if (cfg->stats_enabled) {
        printf(ANSI_BOLD ANSI_MAGENTA "[init]" ANSI_RESET " " ANSI_CYAN
                                      "Stats:       " ANSI_RESET
                                      " " ANSI_BOLD ANSI_GREEN "enabled" ANSI_RESET
                                      " (delay=%ds, server=%s)\n",
               cfg->stats_delay, cfg->stats_server);
        printf(ANSI_BOLD ANSI_MAGENTA "[init]" ANSI_RESET "   " ANSI_DIM
                                      "Default: " ANSI_RESET " %s\n",
               cfg->stats_default);
    } else {
        printf(ANSI_BOLD ANSI_MAGENTA "[init]" ANSI_RESET " " ANSI_CYAN
                                      "Stats:       " ANSI_RESET " " ANSI_RED
                                      "disabled" ANSI_RESET "\n");
    }
    printf(ANSI_BOLD ANSI_MAGENTA "[init]" ANSI_RESET " " ANSI_CYAN
                                  "DNS servers: " ANSI_RESET " %d\n",
           cfg->dns_count);
    for (int i = 0; i < cfg->dns_count; i++) {
        printf(ANSI_BOLD ANSI_MAGENTA "[init]" ANSI_RESET "   " ANSI_BLUE
                                      "%2d." ANSI_RESET " %s\n",
               i + 1, cfg->dns_servers[i]);
    }
}

static void configure_terminal(const syn_cfg_t *cfg) {
    if (!cfg->terminal_font[0]) {
        return;
    }

    FILE *f = fopen(cfg->terminal_font, "rb");
    if (!f) {
        perror("[init] terminal font");
        return;
    }
    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return;
    }
    long size = ftell(f);
    if (size < 32 || size > 2 * 1024 * 1024) {
        fclose(f);
        fprintf(stderr, "[init] invalid font size\n");
        return;
    }
    rewind(f);
    void *data = malloc(size);
    if (!data) {
        fclose(f);
        return;
    }
    if (fread(data, 1, size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return;
    }
    fclose(f);
    int fd = open("/dev/tty0", O_RDWR);
    if (fd >= 0) {
        struct {
            uint64_t data, size;
        } req = {(uintptr_t)data, (uint64_t)size};
        if (ioctl(fd, 0x53594601UL, &req) < 0) {
            perror("[init] terminal font");
        } else {
            printf("[init] terminal font: %s\n", cfg->terminal_font);
        }
        close(fd);
    }
    free(data);
}
static void parse_setup(syn_cfg_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    struct utsname u;
    uname(&u);

    snprintf(cfg->hostname, sizeof(cfg->hostname), "%s", u.nodename);
    strncpy(cfg->domain, "local", sizeof(cfg->domain) - 1);
    strncpy(cfg->language, "en_US.UTF-8", sizeof(cfg->language) - 1);
    strncpy(cfg->timezone, "Europe/Moscow", sizeof(cfg->timezone) - 1);
    strncpy(cfg->default_shell, "/bin/sh", sizeof(cfg->default_shell) - 1);
    strcpy(cfg->terminal_font, "/assets/fonts/dejavu-mono.psf");
    cfg->serial_consoles = 2;
    strncpy(cfg->net_mode, "DHCP", sizeof(cfg->net_mode) - 1);
    strncpy(cfg->stats_default, "/bin/stats", sizeof(cfg->stats_default) - 1);
    cfg->stats_delay = 600;
    cfg->ssh_enabled = 1;
    cfg->ssh_valid = 1;
    cfg->ssh_port = 22;
    strcpy(cfg->ssh_login, "root");
    cfg->setup_check_updates = 1;
    cfg->qga_enabled = 1;
    strcpy(cfg->qga_path, "/dev/virtio-ports/org.qemu.guest_agent.0");
    int err = ini_parse_path("/etc/settings.ini", synapse_config_handler, cfg);
    if (err != 0) {
        fprintf(stderr,
                ANSI_YELLOW "[init] /etc/settings.ini not found or invalid (line %d), "
                            "using defaults" ANSI_RESET "\n",
                err);
    }

    struct stat cloud_settings;
    if (stat("/etc/cloud/settings.ini", &cloud_settings) == 0) {
        char fallback[MAX_DNS_SERVERS][64];
        int count = cfg->dns_count;
        memcpy(fallback, cfg->dns_servers, sizeof(fallback));
        cfg->dns_count = 0;
        ini_parse_path("/etc/cloud/settings.ini", synapse_config_handler, cfg);
        for (int i = 0; i < count && cfg->dns_count < MAX_DNS_SERVERS; i++) {
            int duplicate = 0;
            for (int j = 0; j < cfg->dns_count; j++) {
                if (!strcmp(fallback[i], cfg->dns_servers[j])) {
                    duplicate = 1;
                }
            }
            if (!duplicate) {
                strcpy(cfg->dns_servers[cfg->dns_count++], fallback[i]);
            }
        }
    }
    if (cfg->dns_count == 0) {
        strncpy(cfg->dns_servers[0], "8.8.8.8", sizeof(cfg->dns_servers[0]) - 1);
        strncpy(cfg->dns_servers[1], "1.1.1.1", sizeof(cfg->dns_servers[1]) - 1);
        cfg->dns_count = 2;
    }
    printf("[init] SSH: %s, login=%s, port=%d\n",
           cfg->ssh_enabled && cfg->ssh_valid ? "enabled" : "disabled", cfg->ssh_login,
           cfg->ssh_port);
    print_config(cfg);
}

static void create_file(const char *path, const char *content) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, content, strlen(content));
        close(fd);
    } else {
        fprintf(stderr, ANSI_RED "[init] Failed to create %s: %s" ANSI_RESET "\n", path,
                strerror(errno));
    }
}

static int ensure_dir(const char *path, mode_t mode) {
    if (mkdir(path, mode) != 0 && errno != EEXIST) {
        fprintf(stderr, "[init] mkdir %s: %s\n", path, strerror(errno));
        return 0;
    }
    if (chmod(path, mode) != 0) {
        fprintf(stderr, "[init] chmod %s: %s\n", path, strerror(errno));
        return 0;
    }
    return 1;
}

static int write_private_file(const char *path, const char *content, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        perror(path);
        return 0;
    }

    size_t len = strlen(content);
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, content + done, len - done);
        if (n <= 0) {
            close(fd);
            return 0;
        }
        done += n;
    }

    int ok = fchmod(fd, mode) == 0;
    close(fd);
    return ok;
}

// Обновить системную учётную запись без удаления других пользователей
static int write_managed_accounts(const char *path, const char *managed, mode_t mode,
                                  const char *login, const char *previous_login) {
    char staged[256];
    if (snprintf(staged, sizeof(staged), "%s.new", path) >= (int)sizeof(staged)) {
        return 0;
    }

    FILE *source = fopen(path, "r");
    if (source == NULL && errno != ENOENT) {
        return 0;
    }

    int fd = open(staged, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        if (source != NULL) {
            fclose(source);
        }
        return 0;
    }

    FILE *target = fdopen(fd, "w");
    if (target == NULL) {
        close(fd);
        unlink(staged);
        if (source != NULL) {
            fclose(source);
        }
        return 0;
    }

    int ok = fputs(managed, target) >= 0;
    char *line = NULL;
    size_t capacity = 0;
    if (source != NULL) {
        while (ok && getline(&line, &capacity, source) >= 0) {
            char *separator = strchr(line, ':');
            if (separator == NULL) {
                ok = 0;
                break;
            }

            size_t length = (size_t)(separator - line);
            bool managed_login = strlen(login) == length && strncmp(line, login, length) == 0;
            bool old_login = previous_login[0] != 0 && strlen(previous_login) == length &&
                             strncmp(line, previous_login, length) == 0;
            if ((length == 4 && strncmp(line, "root", 4) == 0) || managed_login || old_login) {
                continue;
            }

            ok = fputs(line, target) >= 0;
        }

        if (ferror(source)) {
            ok = 0;
        }
    }

    free(line);
    if (source != NULL) {
        fclose(source);
    }

    if (fflush(target) != 0 || fchmod(fd, mode) != 0 || fsync(fd) != 0) {
        ok = 0;
    }

    if (fclose(target) != 0) {
        ok = 0;
    }

    if (ok && rename(staged, path) == 0) {
        return 1;
    }

    unlink(staged);
    return 0;
}

static void setup_dropbear_links(void) {
    const char *names[] = {"dropbear",    "dbclient",   "ssh",
                           "dropbearkey", "ssh-keygen", "dropbearconvert"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[64];
        snprintf(path, sizeof(path), "/bin/%s", names[i]);
        if (symlink("dropbearmulti", path) != 0 && errno != EEXIST) {
            fprintf(stderr, "[init] symlink %s: %s\n", path, strerror(errno));
        }
    }
}

// Подготовить SSH-аккаунт и использовать только полное сохранённое состояние
static int setup_ssh_account(syn_cfg_t *cfg) {
    if (!ensure_dir("/root", 0700) || !ensure_dir("/root/.ssh", 0700) ||
        !ensure_dir("/etc/dropbear", 0700)) {
        return 0;
    }
    setup_dropbear_links();
    if (!cfg->ssh_valid) {
        fprintf(stderr, "[init] invalid SSH configuration\n");
        return 0;
    }

    char previous[512] = "", prior_login[128] = "", prior_hash[256] = "";
    FILE *state = fopen("/etc/dropbear/account.state", "r");
    if (state) {
        fgets(previous, sizeof(previous), state);
        fclose(state);
    }

    char *separator = strchr(previous, ':');
    if (separator) {
        *separator++ = 0;
        separator[strcspn(separator, "\r\n")] = 0;
        if (strlen(previous) < sizeof(prior_login) && strlen(separator) < sizeof(prior_hash)) {
            strcpy(prior_login, previous);
            strcpy(prior_hash, separator);
        }
    }

    int input_password = cfg->ssh_password[0] != '\0' || cfg->ssh_password_hash[0] != '\0';
    char hash[256] = "*";
    char *same = cfg->ssh_password[0] && !cfg->ssh_password_hash[0] &&
                         !strcmp(prior_login, cfg->ssh_login) && prior_hash[0] == '$'
                     ? crypt(cfg->ssh_password, prior_hash)
                     : NULL;
    if (same && !strcmp(same, prior_hash)) {
        strcpy(hash, prior_hash);
    } else if (cfg->ssh_password_hash[0]) {
        if (strncmp(cfg->ssh_password_hash, "$6$", 3) != 0) {
            fprintf(stderr, "[init] SSH PasswordHash must use SHA-512 crypt ($6$)\n");
            return 0;
        }
        strcpy(hash, cfg->ssh_password_hash);
    } else if (cfg->ssh_password[0]) {
        unsigned char random[16];
        size_t done = 0;
        while (done < sizeof(random)) {
            ssize_t n = getrandom(random + done, sizeof(random) - done, 0);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                perror("[init] SSH password salt");
                return 0;
            }
            done += n;
        }
        const char alphabet[] =
            "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        char salt[48] = "$6$rounds=10000$";
        size_t prefix = strlen(salt);
        for (size_t i = 0; i < sizeof(random); i++) {
            salt[prefix + i] = alphabet[random[i] & 63];
        }
        salt[prefix + sizeof(random)] = '$';
        salt[prefix + sizeof(random) + 1] = 0;
        char *result = crypt(cfg->ssh_password, salt);
        if (!result || result[0] == '*') {
            fprintf(stderr, "[init] SSH password hashing failed\n");
            return 0;
        }
        snprintf(hash, sizeof(hash), "%s", result);
    }
    memset(cfg->ssh_password, 0, sizeof(cfg->ssh_password));
    // Сохранённый хеш относится к входным настройкам аккаунта
    // Локальную смену пароля сохраняем, пока эти настройки не изменятся
    struct spwd *current = getspnam(cfg->ssh_login);
    if (!input_password && current != NULL && current->sp_pwdp != NULL &&
        current->sp_pwdp[0] != '\0' && current->sp_pwdp[0] != '!' && current->sp_pwdp[0] != '*' &&
        getpwnam(cfg->ssh_login) != NULL) {
        // Пароль владельца не становится пустым из-за отсутствующей настройки SSH
        // Совпадающий прежний управляемый хеш можно отозвать удалением входных реквизитов
        int managed_password = strcmp(prior_login, cfg->ssh_login) == 0 &&
                               prior_hash[0] == '$' && strcmp(prior_hash, current->sp_pwdp) == 0;
        if (!managed_password) {
            snprintf(previous, sizeof(previous), "%s:*\n", cfg->ssh_login);
            return write_private_file("/etc/dropbear/account.state", previous, 0600);
        }
    }
    if (!strcmp(prior_login, cfg->ssh_login) && !strcmp(prior_hash, hash) && current &&
        current->sp_pwdp && getpwnam(cfg->ssh_login)) {
        printf("[init] preserving locally managed account password\n");
        return 1;
    }

    char passwd[512], shadow[512];
    if (strcmp(cfg->ssh_login, "root") == 0) {
        snprintf(passwd, sizeof(passwd), "root:x:0:0:root:/root:/bin/sh\n");
        snprintf(shadow, sizeof(shadow), "root:%s:19000:0:99999:7:::\n", hash);
    } else {
        snprintf(passwd, sizeof(passwd),
                 "root:x:0:0:root:/root:/bin/sh\n%s:x:0:0:SSH admin:/root:/bin/sh\n",
                 cfg->ssh_login);
        snprintf(shadow, sizeof(shadow),
                 "root:*:19000:0:99999:7:::\n%s:%s:19000:0:99999:7:::\n",
                 cfg->ssh_login, hash);
    }
    snprintf(previous, sizeof(previous), "%s:%s\n", cfg->ssh_login, hash);
    return write_managed_accounts("/etc/passwd", passwd, 0644, cfg->ssh_login, prior_login) &&
           write_managed_accounts("/etc/shadow", shadow, 0600, cfg->ssh_login, prior_login) &&
           write_private_file("/etc/dropbear/account.state", previous, 0600);
}

// Проверить, что у административной учётной записи есть способ входа по SSH
static int ssh_access_ready(const char *login) {
    struct spwd *account = getspnam(login);
    if (account && account->sp_pwdp && !strncmp(account->sp_pwdp, "$6$", 3)) {
        return 1;
    }

    FILE *keys = fopen("/root/.ssh/authorized_keys", "r");
    if (!keys) {
        return 0;
    }

    char line[4096];
    int ready = 0;
    while (fgets(line, sizeof(line), keys)) {
        char *key = line;
        while (isspace((unsigned char)*key)) {
            key++;
        }

        if (!strncmp(key, "ssh-ed25519 ", 12) || !strncmp(key, "ssh-rsa ", 8) ||
            !strncmp(key, "ecdsa-sha2-", 12)) {
            char *material = strchr(key, ' ');
            while (material && *material == ' ') {
                material++;
            }

            if (material && strspn(material, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                                           "0123456789+/=") >= 32) {
                ready = 1;
                break;
            }
        }
    }

    fclose(keys);
    return ready;
}

// Отделить ожидание первого ключа от отказа уже настроенной службы
static int cloud_ssh_health_ready(int enabled, int credentials_pending, int account_ready,
                                  time_t stable_since, time_t now) {
    if (!enabled || credentials_pending) {
        return 1;
    }

    return account_ready && stable_since != 0 && now >= stable_since && now - stable_since >= 5;
}

static void generate_etc_files(const syn_cfg_t *cfg) {
    char buf[2048];
    const char *hostname = cfg->hostname[0] ? cfg->hostname : "synapse";
    const char *domain = cfg->domain[0] ? cfg->domain : "local";
    snprintf(buf, sizeof(buf), "%s\n", hostname);
    create_file("/etc/hostname", buf);
    snprintf(buf, sizeof(buf),
             "127.0.0.1\tlocalhost\n"
             "127.0.1.1\t%s.%s\t%s\n"
             "::1\t\tlocalhost ip6-localhost ip6-loopback\n",
             hostname, domain, hostname);
    create_file("/etc/hosts", buf);
    char resolv[1024] = "";
    int off = 0;
    // Не подменяем явно заданные DNS адресами DHCP
    if (!strcasecmp(cfg->net_mode, "DHCP") && !cfg->dns_explicit) {
        off += snprintf(resolv + off, sizeof(resolv) - off, "options synapse-dhcp\n");
    }
    if (domain[0]) {
        off += snprintf(resolv + off, sizeof(resolv) - off, "domain %s\nsearch %s\n",
                        domain, domain);
    }

    for (int i = 0; i < cfg->dns_count; i++) {
        off += snprintf(resolv + off, sizeof(resolv) - off, "nameserver %s\n",
                        cfg->dns_servers[i]);
    }
    create_file("/etc/resolv.conf", resolv);
    // Не стирать группы пользователей при следующей загрузке
    if (access("/etc/group", F_OK) != 0) {
        create_file("/etc/group", "root:x:0:\n");
    }
}

static void service_quote(FILE *f, const char *arg) {
    fputc(39, f);
    for (; *arg; arg++) {
        if (*arg == 39) {
            fputs("'\\''", f);
        } else {
            fputc(*arg, f);
        }
    }
    fputc(39, f);
}

static const char *service_name(const char *path) {
    if (!strcmp(path, "/bin/dropbear")) {
        return "ssh";
    }
    if (!strcmp(path, "/bin/qemu-ga")) {
        return "qga";
    }
    if (!strcmp(path, "/bin/ftpd")) {
        return "ftp";
    }
    if (!strcmp(path, "/bin/sh")) {
        return "rc-local";
    }
    const char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

enum spawn_mode {
    SPAWN_DIRECT,
    SPAWN_MANAGED,
    SPAWN_UNTIL_SUCCESS
};

// Запустить службу под супервизором либо выполнить задачу напрямую
static pid_t spawn_process(const char *path, char *const argv[], char *const envp[],
                           enum spawn_mode mode) {
    const char *name = service_name(path);
    int managed = mode != SPAWN_DIRECT && strcmp(path, "/bin/sh") &&
                  access("/bin/service", X_OK) == 0;
    if (managed) {
        mkdir("/etc/services", 0755);
        char definition[512];
        snprintf(definition, sizeof(definition), "/etc/services/%s", name);
        FILE *f = fopen(definition, "w");
        if (!f) {
            managed = 0;
        } else {
            fputs("#!/bin/sh\nexec ", f);
            for (size_t i = 0; argv[i]; i++) {
                service_quote(f, argv[i]);
                fputc(' ', f);
            }
            fputc('\n', f);
            if (fclose(f)) {
                managed = 0;
            } else {
                chmod(definition, 0700);
            }
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, ANSI_RED "[init] fork() failed: %s" ANSI_RESET "\n",
                strerror(errno));
        return -1;
    }
    if (pid == 0) {
        if (managed) {
            const char *action = "supervise";
            if (mode == SPAWN_UNTIL_SUCCESS) {
                action = "supervise-once";
            }
            execl("/bin/service", "service", action, name, (char *)NULL);
            perror("[init] service supervisor");
            _exit(127);
        }
        if (!strcmp(path, "/bin/sh")) {
            setsid();
            int nullfd = open("/dev/null", O_RDONLY);
            if (nullfd >= 0) {
                dup2(nullfd, 0);
                if (nullfd > 2) {
                    close(nullfd);
                }
            }
            service_log_start("rc-local", 1);
        } else {
            int nullfd = open("/dev/null", O_RDONLY);
            if (nullfd >= 0) {
                dup2(nullfd, STDIN_FILENO);
                if (nullfd > 2) {
                    close(nullfd);
                }
            }
            service_log_start(name, 1);
        }
        execve(path, argv,
               (!strcmp(path, "/bin/dropbear") || !strcmp(path, "/bin/qemu-ga") ||
                !strcmp(path, "/bin/ftpd"))
                   ? environ
                   : envp);
        fprintf(stderr, ANSI_RED "[init] execve(%s) failed: %s" ANSI_RESET "\n", path,
                strerror(errno));
        _exit(127);
    }
    return pid;
}

// Запустить системную шину сообщений только при установленном пакете D-Bus
static void start_system_bus(void) {
    static const char socket_path[] = "/var/run/dbus/system_bus_socket";
    struct stat socket_status;

    if (access("/bin/dbus-daemon", X_OK) != 0 ||
        access("/usr/share/dbus-1/system.conf", R_OK) != 0) {
        return;
    }

    if (!ensure_dir("/var/run/dbus", 0755)) {
        return;
    }

    char *const argv[] = {"/bin/dbus-daemon", "--system", "--nofork", NULL};
    pid_t pid = spawn_process("/bin/dbus-daemon", argv, environ, SPAWN_MANAGED);
    if (pid <= 0) {
        return;
    }

    for (unsigned attempt = 0; attempt < 50; attempt++) {
        if (stat(socket_path, &socket_status) == 0 && S_ISSOCK(socket_status.st_mode)) {
            printf("[init] system D-Bus ready (pid=%d)\n", (int)pid);
            return;
        }

        usleep(20000);
    }

    fprintf(stderr, "[init] system D-Bus socket unavailable\n");
}

static void spawn_setup_check(const syn_cfg_t *cfg) {
    if (!cfg->setup_check_updates || access("/bin/setup", X_OK)) {
        return;
    }

    pid_t child = fork();
    if (child != 0) {
        return; // Дочерний процесс не ожидается и не перезапускается супервизором
    }
    setsid();
    int fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        if (fd > 2) {
            close(fd);
        }
    }
    service_log_start("setup-updates", 1);
    sleep(15); // Даём сетевой настройке завершиться независимо от этой задачи
    if (cfg->desktop_enabled) {
        execl("/bin/setup", "setup", "check", (char *)NULL);
    } else {
        execl("/bin/setup", "setup", "check", "--notify", (char *)NULL);
    }
    _exit(127);
}

#define MAX_TRACKED_CONSOLES 8

struct tracked_console {
    char path[64];
    pid_t pid;
    time_t retry_at;
};

// Передать короткий отчёт в очередь ядра без ожидания записи на диск
static int report_necromant(const char *message) {
    int fd = open("/dev/necro", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }

    size_t length = strlen(message);
    int queued = write(fd, message, length) == (ssize_t)length;
    if (!queued) {
        fprintf(stderr, "[init] necromant report was not queued\n");
    }
    close(fd);
    return queued;
}

// Передать Cloud право сообщить об отсутствии метаданных отдельно от успеха
static void exec_cloud_init(int cloud_required) {
    if (cloud_required) {
        execl("/bin/cloud-init", "cloud-init", "--report-degraded", NULL);
    } else {
        execl("/bin/cloud-init", "cloud-init", NULL);
    }
    _exit(127);
}

// Остановить cloud-init при отказе часов и не оставить второй писатель настроек
static pid_t stop_cloud_init_without_clock(pid_t child, int *status) {
    report_necromant("F cloud-init wait clock unavailable");
    if (kill(child, SIGKILL) != 0 && errno != ESRCH) {
        return -2;
    }

    for (unsigned attempt = 0; attempt < 30; attempt++) {
        pid_t waited = waitpid(child, status, WNOHANG);
        if (waited == child) {
            return child;
        }
        if (waited < 0 && errno == ECHILD) {
            return -1;
        }
        if (waited < 0 && errno != EINTR) {
            return -2;
        }

        usleep(100000);
    }

    report_necromant("F cloud-init could not be stopped");
    return -2;
}

// Ждать cloud-init ограниченное время и не оставлять второй писатель настроек
static pid_t wait_cloud_init(pid_t child, int *status, int *timed_out) {
    if (child <= 0 || status == NULL || timed_out == NULL) {
        return -1;
    }

    *timed_out = 0;
    for (unsigned stage = 0; stage < 3; stage++) {
        struct timespec started;
        if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) {
            return stop_cloud_init_without_clock(child, status);
        }
        time_t deadline = started.tv_sec + (stage == 0 ? 30 : 3);
        for (;;) {
            pid_t waited = waitpid(child, status, WNOHANG);
            if (waited == child) {
                return child;
            }
            if (waited < 0 && errno != EINTR) {
                return -1;
            }

            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
                return stop_cloud_init_without_clock(child, status);
            }
            if (now.tv_sec >= deadline) {
                break;
            }
            usleep(100000);
        }

        *timed_out = 1;
        if (stage == 0) {
            report_necromant("F cloud-init timed out");
            kill(child, SIGTERM);
        } else if (stage == 1) {
            kill(child, SIGKILL);
        }
    }

    report_necromant("F cloud-init could not be stopped");
    return -2;
}

// Различить успешную настройку, отсутствие метаданных и настоящий отказ
static int cloud_init_result(pid_t waited, pid_t child, int status, int cloud_required) {
    if (waited != child || !WIFEXITED(status)) {
        return 0;
    }

    int code = WEXITSTATUS(status);
    if (code == 0) {
        return 1;
    }
    if (cloud_required && code == 3) {
        return 2;
    }

    return 0;
}

// Узнать редакцию образа по файлу, созданному сборщиком
static int is_cloud_edition(void) {
    FILE *file = fopen("/etc/synapse-edition", "r");
    if (file == NULL) {
        return 0;
    }

    char edition[16] = {0};
    int result = fgets(edition, sizeof(edition), file) != NULL &&
                 (strcmp(edition, "cloud\n") == 0 || strcmp(edition, "cloud") == 0);
    fclose(file);
    return result;
}

// Проверить обязательную программу Cloud перед её запуском
static int cloud_binary_ready(const char *path) {
    struct stat info;
    unsigned char header[20];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }

    int ready = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                info.st_size >= 64 && access(path, X_OK) == 0 &&
                read(fd, header, sizeof(header)) == (ssize_t)sizeof(header) &&
                memcmp(header, "\177ELF", 4) == 0 && header[4] == 2 &&
                header[5] == 1 && header[6] == 1 && header[18] == 0x3e &&
                header[19] == 0;
    close(fd);
    return ready;
}

// Восстановить обязательную программу из локального boot-раздела
static int restore_cloud_binary(const char *name, int force) {
    char source_path[64];
    char target_path[64];
    char temporary_path[64];
    snprintf(source_path, sizeof(source_path), "/boot/recovery/%s", name);
    snprintf(target_path, sizeof(target_path), "/bin/%s", name);
    snprintf(temporary_path, sizeof(temporary_path), "/bin/.necro-%s-XXXXXX", name);

    if (!force && cloud_binary_ready(target_path)) {
        return 1;
    }

    int source = open(source_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source < 0) {
        return 0;
    }

    struct stat info;
    if (fstat(source, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 64 || info.st_size > 16 * 1024 * 1024) {
        close(source);
        return 0;
    }

    unsigned char magic[4];
    if (read(source, magic, sizeof(magic)) != (ssize_t)sizeof(magic) ||
        memcmp(magic, "\177ELF", sizeof(magic)) != 0 || lseek(source, 0, SEEK_SET) != 0) {
        close(source);
        return 0;
    }

    int target = mkstemp(temporary_path);
    if (target < 0) {
        close(source);
        return 0;
    }

    int success = 1;
    off_t copied = 0;
    unsigned char buffer[16384];
    while (copied < info.st_size) {
        ssize_t count = read(source, buffer, sizeof(buffer));
        if (count <= 0 || count > info.st_size - copied) {
            success = 0;
            break;
        }

        ssize_t written = 0;
        while (written < count) {
            ssize_t step = write(target, buffer + written, (size_t)(count - written));
            if (step <= 0) {
                success = 0;
                break;
            }
            written += step;
        }
        if (!success) {
            break;
        }
        copied += count;
    }

    if (success && (fchmod(target, 0755) != 0 || fsync(target) != 0)) {
        success = 0;
    }
    if (close(target) != 0) {
        success = 0;
    }
    close(source);

    if (success && rename(temporary_path, target_path) != 0) {
        success = 0;
    }
    if (!success) {
        unlink(temporary_path);
        return 0;
    }

    int directory = open("/bin", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) {
        if (fsync(directory) != 0) {
            fprintf(stderr, "[init] cannot sync recovered /bin/%s directory\n", name);
        }
        close(directory);
    }
    return cloud_binary_ready(target_path);
}

// Вернуть критические программы Cloud до запуска служб
static void restore_cloud_programs(void) {
    static const char *const names[] = {"sh", "cloud-init", "dropbear", "service"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[64];
        char report[96];
        snprintf(path, sizeof(path), "/bin/%s", names[i]);
        if (cloud_binary_ready(path)) {
            continue;
        }

        snprintf(report, sizeof(report), "F cloud %s unavailable", names[i]);
        report_necromant(report);
        if (restore_cloud_binary(names[i], 0)) {
            snprintf(report, sizeof(report), "R cloud %s restored", names[i]);
            report_necromant(report);
            fprintf(stderr, "[init] restored /bin/%s from boot recovery\n", names[i]);
        } else {
            fprintf(stderr, "[init] cannot restore /bin/%s from boot recovery\n", names[i]);
        }
    }
}

// Прочитать состояние обязательной службы SSH у её супервизора
static int ssh_service_state(pid_t supervisor, time_t now) {
    FILE *status = fopen("/var/run/services/ssh/status", "r");
    if (status == NULL) {
        return 0;
    }

    long stamp = 0;
    long owner = 0;
    long child = 0;
    char state[32];
    int wanted = 0;
    int delay = 0;
    int parsed = fscanf(status, "%ld %ld %ld %31s %d %d", &stamp, &owner,
                        &child, state, &wanted, &delay);
    fclose(status);

    if (parsed != 6 || owner != (long)supervisor || stamp < 0 ||
        stamp > (long)now || (long)now - stamp > 20) {
        return 0;
    }
    if (child == 0 && strcmp(state, "quarantined") == 0) {
        return 1;
    }
    if (child > 0 && strcmp(state, "running") == 0) {
        return 2;
    }
    return 0;
}

// Подтвердить ответ SSH, чтобы открытый или ошибочно готовый сокет не считался службой
static int ssh_direct_ready(pid_t process, int port) {
    if (process <= 0 || port <= 0 || port > 65535) {
        return 0;
    }

    int connection = socket(AF_INET, SOCK_STREAM, 0);
    if (connection < 0) {
        return 0;
    }
    int flags = fcntl(connection, F_GETFL, 0);
    if (flags < 0 || fcntl(connection, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(connection);
        return 0;
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int ready = connect(connection, (struct sockaddr *)&address, sizeof(address)) == 0;
    if (!ready && errno == EINPROGRESS) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(connection, &writable);
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
        if (select(connection + 1, NULL, &writable, NULL, &timeout) > 0) {
            int error = 0;
            socklen_t error_size = sizeof(error);
            ready = getsockopt(connection, SOL_SOCKET, SO_ERROR, &error, &error_size) == 0 &&
                error_size == sizeof(error) && error == 0;
        }
    }
    if (ready) {
        char banner[8];
        size_t received = 0;
        while (received < sizeof(banner)) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(connection, &readable);
            struct timeval timeout = {.tv_sec = 0, .tv_usec = 200000};
            if (select(connection + 1, &readable, NULL, NULL, &timeout) <= 0) {
                break;
            }

            ssize_t count = recv(connection, banner + received, sizeof(banner) - received, 0);
            if (count <= 0) {
                break;
            }
            received += (size_t)count;
        }
        ready = received == sizeof(banner) && memcmp(banner, "SSH-2.0-", sizeof(banner)) == 0;
    }
    close(connection);
    return ready;
}

// Ограничить ожидание команды перезапуска, чтобы init продолжал проверять службы
static int wait_ssh_restart(pid_t command, int *status) {
    if (command <= 0 || status == NULL) {
        return 0;
    }

    for (unsigned stage = 0; stage < 3; stage++) {
        struct timespec started;
        if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) {
            kill(command, SIGKILL);
            report_necromant("F cloud ssh restart clock unavailable");
            return 0;
        }

        time_t deadline = started.tv_sec + (stage == 0 ? 5 : 2);
        for (;;) {
            pid_t waited = waitpid(command, status, WNOHANG);
            if (waited == command) {
                return stage == 0 && WIFEXITED(*status) && WEXITSTATUS(*status) == 0;
            }
            if (waited < 0 && errno != EINTR) {
                return 0;
            }

            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
                kill(command, SIGKILL);
                report_necromant("F cloud ssh restart clock unavailable");
                return 0;
            }
            if (now.tv_sec >= deadline) {
                break;
            }

            usleep(100000);
        }

        if (stage == 0) {
            report_necromant("F cloud ssh restart timed out");
            kill(command, SIGTERM);
        } else if (stage == 1) {
            kill(command, SIGKILL);
        }
    }

    report_necromant("F cloud ssh restart could not be stopped");
    return 0;
}

// Восстановить dropbear и попросить действующий супервизор запустить его заново
static int recover_ssh(void) {
    if (!restore_cloud_binary("dropbear", 1)) {
        report_necromant("F cloud ssh binary recovery failed");
        return 0;
    }

    pid_t command = fork();
    if (command < 0) {
        report_necromant("F cloud ssh restart fork failed");
        return 0;
    }
    if (command == 0) {
        execl("/bin/service", "service", "restart", "ssh", (char *)NULL);
        _exit(127);
    }

    int status = 0;
    if (!wait_ssh_restart(command, &status)) {
        report_necromant("F cloud ssh restart request failed");
        return 0;
    }

    report_necromant("R cloud ssh binary restored and restart requested");
    return 1;
}

// Запомнить завершение обязательной службы или локальной оболочки
static void reap_children(struct tracked_console *consoles, size_t count,
                          pid_t *ssh_supervisor, time_t *ssh_retry_at, int *ssh_exited, pid_t *desktop) {
    int status = 0;
    pid_t pid;
    *ssh_exited = 0;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        time_t retry_at = time(NULL) + 5;
        if (pid == *desktop) *desktop = 0;
        if (pid == *ssh_supervisor) {
            *ssh_exited = 1;
            *ssh_supervisor = -1;
            *ssh_retry_at = retry_at;
            int code = WIFEXITED(status) ? WEXITSTATUS(status) :
                WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
            fprintf(stderr, "[init] SSH process exited (code=%d), restart scheduled\n", code);
            char report[96];
            snprintf(report, sizeof(report), "F ssh process exited code=%d", code);
            report_necromant(report);
        }

        for (size_t i = 0; i < count; i++) {
            if (pid == consoles[i].pid) {
                consoles[i].pid = -1;
                consoles[i].retry_at = retry_at;
                fprintf(stderr, "[init] shell on %s exited; restart scheduled\n", consoles[i].path);
                char report[96];
                snprintf(report, sizeof(report), "F shell %.72s exited", consoles[i].path);
                report_necromant(report);
            }
        }
    }
}

static inline char *get_mode(long long free_memory) {
    if (free_memory > 1024 * 1024 * 1024) {
        return ANSI_BOLD ANSI_CYAN "server" ANSI_RESET;
    } else if (free_memory > 256 * 1024 * 1024) {
        return ANSI_BOLD ANSI_GREEN "standard" ANSI_RESET;
    } else {
        return ANSI_BOLD ANSI_RED "economy" ANSI_RESET;
    }
}

static void configure_network(const syn_cfg_t *config) {
    if (strcasecmp(config->net_mode, "Static")) {
        return;
    }

    struct in_addr address[3];
    if (inet_pton(AF_INET, config->net_ip, &address[0]) != 1 ||
        inet_pton(AF_INET, config->net_subnet, &address[1]) != 1 ||
        inet_pton(AF_INET, config->net_gateway, &address[2]) != 1) {
        fprintf(stderr, "[init] invalid static IPv4 configuration\n");
        return;
    }
    if (syscall(106024, config->net_interface, address) < 0) {
        fprintf(stderr, "[init] cannot configure static IPv4\n");
    } else {
        printf("[init] static IPv4: %s mask %s gateway %s\n", config->net_ip,
               config->net_subnet, config->net_gateway);
    }
}

static pid_t spawn_tty_shell(const char *tty_path, const char *shell,
                             char *const shell_argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, ANSI_RED "[init] fork for %s failed: %s" ANSI_RESET "\n",
                tty_path, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        setsid();
        int fd = open(tty_path, O_RDWR);
        if (fd < 0) _exit(126);
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2) close(fd);
        ioctl(0, TIOCSCTTY, 1);
        unsetenv("LUMINE_LOG_SERVICE");
        unsetenv("LUMINE_LOG_CONSOLE_FD");
        setenv("SHELL_TTY", tty_path, 1);
        setenv("TERM", "linux", 1);
        execve(shell, shell_argv, environ);
        fprintf(stderr, ANSI_RED "[init] execve(%s) on %s failed: %s" ANSI_RESET "\n",
                shell, tty_path, strerror(errno));
        _exit(127);
    }
    printf(ANSI_BOLD ANSI_WHITE "[init]" ANSI_RESET " shell on " ANSI_CYAN
                                "%s" ANSI_RESET " spawned " ANSI_BOLD ANSI_GREEN
                                "OK" ANSI_RESET " (pid: " ANSI_YELLOW "%d" ANSI_RESET
                                ")\n",
           tty_path, (int)pid);
    return pid;
}

// Выбрать работающую оболочку для локальной консоли
static const char *select_shell(const char *requested) {
    if (requested != NULL && access(requested, X_OK) == 0) {
        return requested;
    }

    if (access("/bin/sh", X_OK) == 0) {
        fprintf(stderr, "[init] configured shell unavailable; using /bin/sh\n");
        return "/bin/sh";
    }

    if (access("/bin/bash", X_OK) == 0) {
        fprintf(stderr, "[init] /bin/sh unavailable; using /bin/bash\n");
        return "/bin/bash";
    }

    fprintf(stderr, "[init] no executable recovery shell is available\n");
    return requested;
}

// Запустить оболочку и сохранить её PID для восстановления
static void track_console(struct tracked_console *consoles, size_t *count,
                          const char *path, const char *shell, char *const argv[]) {
    if (*count >= MAX_TRACKED_CONSOLES || strlen(path) >= sizeof(consoles[0].path)) {
        fprintf(stderr, "[init] cannot supervise console %s\n", path);
        return;
    }

    struct tracked_console *console = &consoles[*count];
    strcpy(console->path, path);
    console->pid = spawn_tty_shell(path, shell, argv);
    console->retry_at = time(NULL) + 5;
    if (console->pid < 0) {
        char report[96];
        snprintf(report, sizeof(report), "F shell %s failed to start", path);
        report_necromant(report);
    }
    (*count)++;
}

// Загружаем правила после настройки cloud и до запуска сетевых служб
// Файл примера не включает фильтрацию
static void load_firewall_config(void) {
    struct stat config;
    if (stat("/etc/firewall.conf", &config) < 0) {
        if (errno != ENOENT) {
            perror("[init] firewall config");
        }
        return;
    }
    if (!S_ISREG(config.st_mode)) {
        fprintf(stderr, "[init] firewall: /etc/firewall.conf must be a regular file\n");
        return;
    }
    if (access("/bin/firewall", X_OK) < 0) {
        fprintf(stderr,
                "[init] firewall: configured but /bin/firewall is unavailable\n");
        return;
    }
    // Сетевой поток создаёт управляющее устройство после инициализации lwIP
    for (unsigned tries = 0; tries < 50 && access("/dev/firewall", F_OK) < 0; tries++) {
        usleep(100000);
    }
    if (access("/dev/firewall", F_OK) < 0) {
        fprintf(
            stderr,
            "[init] firewall: control device unavailable after 5s; rules not loaded\n");
        return;
    }

    pid_t child = fork();
    if (child < 0) {
        perror("[init] firewall fork");
        return;
    }
    if (child == 0) {
        execl("/bin/firewall", "firewall", "load", "/etc/firewall.conf", (char *)NULL);
        perror("[init] firewall exec");
        _exit(127);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("[init] firewall: loaded /etc/firewall.conf\n");
    } else {
        fprintf(stderr, "[init] firewall: loading failed; previous kernel rules remain "
                        "unchanged\n");
    }
}

// При завершении графической сессии запускаем консоль на том же терминале
static pid_t start_desktop_session(const syn_cfg_t *cfg) {
    const char *program = cfg->desktop_login_manager ? "/bin/synapse-session-manager" :
        (cfg->desktop_path[0] ? cfg->desktop_path : "/bin/desktop");
    if (!cfg->desktop_enabled || access(program, X_OK) != 0) {
        return 0;
    }

    pid_t supervisor = fork();
    if (supervisor < 0) {
        perror("[init] desktop supervisor");
        return 0;
    }
    if (supervisor == 0) {
        for (int attempt = 0; attempt < 3; attempt++) {
            pid_t child = fork();
            if (child == 0) {
                execl(program, program, (char *)NULL);
                perror("[init] desktop exec");
                _exit(127);
            }
            if (child < 0) {
                break;
            }

            int status = 0;
            pid_t waited;
            do {
                waited = waitpid(child, &status, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited < 0 || (WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
                break;
            }
            fprintf(stderr, "[init] desktop exited; restart attempt %d/3\n",
                    attempt + 1);
            sleep(3);
        }
        fprintf(stderr, "[init] desktop stopped; returning to local console\n");
        if (cfg->desktop_login_manager) {
            syscall(102005L, (unsigned long)UINT32_MAX);
        }

        _exit(0);
    }
    printf("[init] desktop session supervisor pid=%d\n", (int)supervisor);
    return supervisor;
}

// При первом запуске ждём завершения HI
// Выбор языка должен завершиться до запуска пользовательской сессии
static int run_first_boot_setup(void) {
    if (access("/var/lib/hi/complete", F_OK) == 0) {
        return 0;
    }
    if (access("/bin/hi", X_OK) != 0 || access("/dev/tty0", R_OK | W_OK) != 0) {
        fprintf(stderr, "[init] HI or its terminal is unavailable for first-boot setup\n");
        return -1;
    }

    pid_t child = fork();
    if (child < 0) {
        perror("[init] HI fork");
        return -1;
    }
    if (child == 0) {
        setsid();
        int fd = open("/dev/tty0", O_RDWR);
        if (fd < 0) {
            _exit(126);
        }
        ioctl(fd, TIOCSCTTY, 1);
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2) {
            close(fd);
        }
        unsetenv("LUMINE_LOG_SERVICE");
        unsetenv("LUMINE_LOG_CONSOLE_FD");
        setenv("SHELL_TTY", "/dev/tty0", 1);
        setenv("TERM", "linux", 1);
        execl("/bin/hi", "hi", "--first-boot", (char *)NULL);
        _exit(127);
    }

    int status = 0;
    pid_t waited = 0;
    unsigned heartbeat_tick = 0;
    while (waited == 0) {
        waited = waitpid(child, &status, WNOHANG);
        if (waited < 0 && errno == EINTR) {
            waited = 0;
            continue;
        }

        if (waited == 0) {
            if (heartbeat_tick++ % 5U == 0U) {
                report_necromant("P init waiting for first-boot setup");
            }

            sleep(1);
        }
    }
    if (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        access("/var/lib/hi/complete", F_OK) == 0) {
        printf("[init] HI first-boot setup completed\n");
        return 1;
    }
    fprintf(stderr, "[init] HI first-boot setup exited without completion\n");
    return -1;
}

static void apply_boot_session(syn_cfg_t *config) {
    // Выбор сессии в Limine действует на эту загрузку и не меняет settings.ini
    if (config->display_fps > 0 && syscall(104100L, (unsigned long)config->display_fps) != 0) {
        perror("[init] display FPS");
    }
    if (config->power_profile[0]) {
        int fd = open("/proc/power-profile", O_WRONLY | O_CLOEXEC);
        size_t length = strlen(config->power_profile);
        if (fd >= 0) {
            if (write(fd, config->power_profile, length) != (ssize_t)length) {
                perror("[init] CPU power profile");
            }
            close(fd);
        }
    }
    const char *session = getenv("SYNAPSE_SESSION");
    if (session && !strcmp(session, "console")) {
        config->desktop_enabled = 0;
    }
    if (session && !strcmp(session, "desktop")) {
        const char *program = config->desktop_path;

        if (program[0] == '\0' || !strcmp(program, "/bin/sh") || access(program, X_OK) != 0) {
            if (access("/bin/weston-extended", X_OK) == 0) {
                program = "/bin/weston-extended";
            } else if (access("/bin/desktop", X_OK) == 0) {
                program = "/bin/desktop";
            } else {
                config->desktop_enabled = 0;
                return;
            }

            snprintf(config->desktop_path, sizeof(config->desktop_path), "%s", program);
        }

        config->desktop_enabled = 1;
    }
}

enum usb_module {
    USB_ASIX, USB_LAN78, USB_RTL8152, USB_WIFI, USB_TURING, USB_SERIAL, USB_RNDIS,
    USB_NCM, USB_IPHETH, USB_UVC, USB_UAS, USB_MODULE_COUNT
};

static const char *const usb_module_names[USB_MODULE_COUNT] = {
    "usb-asix", "usb-lan78xx", "usb-rtl8152", "wifi-usb", "turing-screen", "usb-serial", "usb-rndis",
    "usb-ncm", "usb-ipheth", "usb-uvc", "usb-uas"
};

static unsigned usb_present, usb_loaded;
static bool usb_monitor;

// Сопоставить интерфейс с правилами probe пакетных драйверов
static unsigned usb_match(unsigned vendor, unsigned product, unsigned cls, unsigned sub, unsigned proto) {
    unsigned wanted = 0;
    switch ((vendor << 16) | product) {
        case 0x0b957720: wanted = 1u << USB_ASIX; break;
        case 0x04247800: wanted = 1u << USB_LAN78; break;
        case 0x0bda8152: case 0x0bda8153: wanted = 1u << USB_RTL8152; break;
        case 0x0bdac811: case 0x2357012d: case 0x0bdab812: wanted = 1u << USB_WIFI; break;
        case 0x1a865722: return 1u << USB_TURING;
        case 0x1a867523: case 0x10c4ea60: case 0x10c4ea61: case 0x04036001: case 0x067b2303:
            wanted = 1u << USB_SERIAL;
            break;
    }
    if ((cls == 0xe0 && sub == 1 && proto == 3) || (cls == 2 && sub == 2 && proto == 0xff)) {
        wanted |= 1u << USB_RNDIS;
    } else if (cls == 2 && sub == 2) {
        wanted |= 1u << USB_SERIAL;
    }
    if ((cls == 2 && sub == 0x0d) || (cls == 0xef && sub == 2 && proto == 1)) wanted |= 1u << USB_NCM;
    if (vendor == 0x05ac && cls == 0xff && sub == 0xfd && proto == 1) wanted |= 1u << USB_IPHETH;
    if (cls == 14 && sub == 2) wanted |= 1u << USB_UVC;
    if (cls == 8 && sub == 6 && proto == 0x62) wanted |= 1u << USB_UAS;
    return wanted;
}

static bool usb_snapshot(unsigned *present) {
    FILE *file = fopen("/proc/usb-interfaces", "r");
    if (!file) return false;

    unsigned mask = 0, vendor, product, cls, sub, proto;
    char line[80];
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "%x %x %x %x %x", &vendor, &product, &cls, &sub, &proto) == 5) {
            mask |= usb_match(vendor, product, cls, sub, proto);
        }
    }
    bool ok = !ferror(file);
    fclose(file);
    if (ok) *present = mask;
    return ok;
}

static unsigned usb_module_bit(const char *file) {
    for (unsigned i = 0; i < USB_MODULE_COUNT; i++) {
        size_t length = strlen(usb_module_names[i]);
        if (!strncmp(file, usb_module_names[i], length) && !strcmp(file + length, ".so")) return 1u << i;
    }
    return 0;
}

// Загружаем модули из списков пакетов до настройки сетевых интерфейсов
static void load_kernel_modules(bool boot, unsigned added) {
    const char *disabled = getenv("SYNAPSE_NO_MODULES");
    if (disabled && !strcmp(disabled, "1")) {
        puts("[init] module autoload disabled");
        return;
    }
    if (access("/bin/insmod", X_OK)) {
        return;
    }
    if (boot) {
        usb_monitor = usb_snapshot(&usb_present);
        added = usb_present;
    }
    glob_t files = {0};
    if (!glob("/etc/modules-load.d/*.conf", 0, NULL, &files)) {
        for (size_t i = 0; i < files.gl_pathc; i++) {
            FILE *file = fopen(files.gl_pathv[i], "r");
            if (!file) {
                continue;
            }

            char line[512];
            for (;;) {
                off_t line_offset = ftello(file);
                if (fgets(line, sizeof(line), file) == NULL) {
                    break;
                }
                if (strchr(line, '\n') == NULL && strlen(line) == sizeof(line) - 1) {
                    int next;
                    while ((next = fgetc(file)) != '\n' && next != EOF) {
                    }
                    report_necromant("F module config line too long");
                    continue;
                }

                char *path = line;
                while (*path && isspace((unsigned char)*path)) {
                    path++;
                }

                // Частичная запись отметки тоже должна оставаться видимой
                bool quarantined = strncmp(path, "#!ib/modules/", 13) == 0 ||
                    strncmp(path, "#lib/modules/", 13) == 0 ||
                    strncmp(path, "/!ib/modules/", 13) == 0;
                if (quarantined && !boot) continue;
                if (quarantined) {
                    char *name = path + 13;
                    char *end = name;
                    while (*end && !isspace((unsigned char)*end)) {
                        end++;
                    }
                    *end = 0;

                    char report[96];
                    snprintf(report, sizeof(report), "F module %.68s quarantined", name);
                    report_necromant(report);
                    printf("[init] module %s quarantined after incomplete boot\n", name);
                    continue;
                }

                if (!*path || *path == '#') {
                    continue;
                }

                char *end = path;
                while (*end && !isspace((unsigned char)*end) && *end != '#') {
                    end++;
                }
                *end = 0;
                if (strncmp(path, "/lib/modules/", 13) || strstr(path, "..")) {
                    continue;
                }

                unsigned bit = usb_module_bit(path + 13);
                if ((!bit && !boot) || (bit && usb_monitor && (!(added & bit) || (usb_loaded & bit)))) {
                    continue;
                }

                if (line_offset < 0) {
                    report_necromant("F module boot guard offset unavailable");
                    continue;
                }

                int guard_fd = open(files.gl_pathv[i], O_RDWR | O_CLOEXEC | O_NOFOLLOW);
                if (guard_fd < 0) {
                    report_necromant("F module boot guard unavailable");
                    continue;
                }

                off_t guard_offset = line_offset + (off_t)(path - line);
                if (pwrite(guard_fd, "#!", 2, guard_offset) != 2 || fsync(guard_fd) != 0) {
                    report_necromant("F module boot guard write failed");
                    close(guard_fd);
                    continue;
                }

                // Модуль загружается один раз, супервизор службы ему не нужен
                pid_t pid = fork();
                if (!pid) {
                    execl("/bin/insmod", "insmod", path, (char *)NULL);
                    _exit(127);
                }

                int status = -1;
                if (pid > 0) {
                    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                    }
                }
                printf("[init] module %s %s\n", path,
                       status == 0 ? "loaded" : "failed");
                if (status != 0) {
                    char report[96];
                    snprintf(report, sizeof(report), "F module %.72s failed", path + 13);
                    report_necromant(report);
                } else {
                    usb_loaded |= bit;
                }
                // В карантине остаётся только прерванная загрузка
                if ((pid < 0 || (status >= 0 && WIFEXITED(status))) &&
                    (pwrite(guard_fd, "/l", 2, guard_offset) != 2 || fsync(guard_fd) != 0)) {
                    report_necromant("F module boot guard restore failed");
                }

                close(guard_fd);
            }
            fclose(file);
        }
    }
    globfree(&files);
}

// Запрашивать выгрузку только после исчезновения последнего подходящего интерфейса
static void update_usb_modules(void) {
    unsigned present;
    if (!usb_monitor || !usb_snapshot(&present)) return;
    unsigned added = present & ~usb_present & ~usb_loaded;
    unsigned removed = ~present & usb_loaded;
    usb_present = present;
    for (unsigned i = 0; i < USB_MODULE_COUNT; i++) {
        if (!(removed & (1u << i))) continue;
        if (syscall(SYS_delete_module, usb_module_names[i], 0) == 0 || errno == ENOENT) {
            usb_loaded &= ~(1u << i);
        }
    }
    if (added) load_kernel_modules(false, added);
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    int cfd = open("/dev/tty0", O_RDWR);
    if (cfd < 0) {
        cfd = open("/dev/console", O_RDWR);
    }
    if (cfd >= 0) {
        ioctl(cfd, TIOCSCTTY, 1);
        dup2(cfd, 0);
        dup2(cfd, 1);
        dup2(cfd, 2);
        if (cfd > 2) {
            close(cfd);
        }
    }

    // Сохраняем журнал ядра после подготовки корня для записи
    if (access("/bin/bootlog", X_OK) == 0) {
        pid_t logger = fork();
        if (logger == 0) {
            execl("/bin/bootlog", "bootlog", "start", NULL);
            _exit(127);
        }
    }
    // Вывод init не должен смешиваться с выводом интерактивных оболочек
    (void)service_log_start("init", 1);
    report_necromant("P init started");
    ensure_dir("/var", 0755);
    ensure_dir("/var/www", 0755);

    int cloud_required = is_cloud_edition();
    if (cloud_required) {
        restore_cloud_programs();
    }

    printf(ANSI_BOLD ANSI_WHITE "[init]" ANSI_RESET " tid: " ANSI_YELLOW "%d" ANSI_RESET
                                " pid: " ANSI_YELLOW "%d" ANSI_RESET "\n",
           (int)gettid(), (int)getpid());
    printf(ANSI_BOLD ANSI_WHITE "[init]" ANSI_RESET " parsing setup...\n");

    pid_t cloud = -1;
    struct stat cloud_binary;
    int live = getenv("SYNAPSE_LIVE") && !strcmp(getenv("SYNAPSE_LIVE"), "1");
    if (!live && cloud_required && stat("/bin/cloud-init", &cloud_binary) == 0 &&
        access("/bin/cloud-init", X_OK) == 0) {
        cloud = fork();
        if (cloud == 0) {
            service_log_start("cloud-init", 1);
            exec_cloud_init(cloud_required);
        }
        if (cloud < 0) {
            report_necromant("F cloud-init fork failed");
        }
    }

    syn_cfg_t config;
    parse_setup(&config);
    configure_locale(&config);
    apply_boot_session(&config);
    const char *session = getenv("SYNAPSE_SESSION");
    printf("[init] boot session: %s; live=%s\n", session ? session : "auto",
           getenv("SYNAPSE_LIVE") ? getenv("SYNAPSE_LIVE") : "0");
    configure_terminal(&config);
    generate_etc_files(&config);
    if (!getenv("SYNAPSE_NO_NETWORK") || strcmp(getenv("SYNAPSE_NO_NETWORK"), "1")) {
        configure_network(&config);
    }
    // Локальные консоли используют уже прочитанную конфигурацию
    setup_ssh_account(&config);
    if (config.hostname[0]) {
        sethostname(config.hostname, strlen(config.hostname));
    }

    printf(ANSI_BOLD ANSI_WHITE
           "[init]" ANSI_RESET " setting hostname: " ANSI_BOLD ANSI_CYAN "%s" ANSI_RESET
           "\n",
           config.hostname[0] ? config.hostname : "(not set)");

    struct utsname u;
    if (uname(&u) == 0) {
        printf(ANSI_BOLD ANSI_WHITE "[init]" ANSI_RESET " sysname: " ANSI_GREEN
                                    "%s" ANSI_RESET ", node: " ANSI_GREEN
                                    "%s" ANSI_RESET ", domain: " ANSI_GREEN
                                    "%s" ANSI_RESET "\n",
               u.sysname, u.nodename, u.domainname);
        printf(ANSI_BOLD ANSI_WHITE "[init]" ANSI_RESET " release: " ANSI_YELLOW
                                    "%s" ANSI_RESET ", version: " ANSI_YELLOW
                                    "%s" ANSI_RESET "\n",
               u.release, u.version);
        printf(ANSI_BOLD ANSI_WHITE "[init]" ANSI_RESET " machine: " ANSI_MAGENTA
                                    "%s" ANSI_RESET "\n",
               u.machine);
    } else {
        perror(ANSI_RED "[init] uname" ANSI_RESET);
    }

    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        printf(ANSI_BOLD ANSI_WHITE
               "[init]" ANSI_RESET " memory total: " ANSI_BOLD ANSI_GREEN
               "%lu.%.2lu mb" ANSI_RESET " | free: " ANSI_BOLD ANSI_GREEN
               "%lu.%.2lu mb" ANSI_RESET "\n",
               si.totalram / 1024 / 1024, (si.totalram / 1024) % 100,
               si.freeram / 1024 / 1024, (si.freeram / 1024) % 100);
    } else {
        perror(ANSI_RED "[init] sysinfo" ANSI_RESET);
    }

    printf(ANSI_BOLD ANSI_WHITE "[init]" ANSI_RESET " system mode: %s" ANSI_RESET "\n",
           get_mode(si.freeram));

    const char *shell = select_shell(config.default_shell[0] ? config.default_shell : "/bin/sh");
    char *shell_argv[] = {(char *)shell, NULL};
    struct tracked_console consoles[MAX_TRACKED_CONSOLES] = {0};
    size_t console_count = 0;

    int desktop_ready =
        config.desktop_enabled &&
        access(config.desktop_path[0] ? config.desktop_path : "/bin/desktop", X_OK) ==
            0;

    int first_boot_result = 0;
    if (desktop_ready || live) {
        first_boot_result = run_first_boot_setup();
    }
    if (first_boot_result > 0) {
        // Применяем настройки HI до запуска оболочек и служб
        parse_setup(&config);
        configure_locale(&config);
        apply_boot_session(&config);
        configure_terminal(&config);
        generate_etc_files(&config);
        if (!getenv("SYNAPSE_NO_NETWORK") ||
            strcmp(getenv("SYNAPSE_NO_NETWORK"), "1")) {
            configure_network(&config);
        }
        if (config.hostname[0]) {
            sethostname(config.hostname, strlen(config.hostname));
        }
        shell = select_shell(config.default_shell[0] ? config.default_shell : "/bin/sh");
        shell_argv[0] = (char *)shell;
        desktop_ready =
            config.desktop_enabled &&
            access(config.desktop_path[0] ? config.desktop_path : "/bin/desktop",
                   X_OK) == 0;
    } else if (first_boot_result < 0) {
        // Незавершённый мастер не разрешает запуск графической сессии в этой загрузке
        config.desktop_enabled = 0;
        desktop_ready = 0;
        fprintf(stderr, "[init] first-boot setup is incomplete, continuing in the local console\n");
    }
    if (!desktop_ready) {
        track_console(consoles, &console_count, "/dev/tty0", shell, shell_argv);
    }

    if (config.serial_consoles > 0) {
        glob_t tty_glob = {0};
        if (glob("/dev/ttyS*", 0, NULL, &tty_glob) == 0) {
            size_t count = tty_glob.gl_pathc;
            if (count > (size_t)config.serial_consoles) {
                count = (size_t)config.serial_consoles;
            }

            printf(ANSI_BOLD ANSI_WHITE "[init]" ANSI_RESET " starting %zu serial console(s)\n", count);
            for (size_t i = 0; i < count; i++) {
                track_console(consoles, &console_count, tty_glob.gl_pathv[i], shell, shell_argv);
            }

            globfree(&tty_glob);
        }
    }

    // Удаляем состояние служб с предыдущей загрузки, поскольку var/run сохраняется на
    // диске
    glob_t runtime_files = {0};
    if (glob("/var/run/services/*/*", 0, NULL, &runtime_files) == 0) {
        for (size_t i = 0; i < runtime_files.gl_pathc; i++) {
            unlink(runtime_files.gl_pathv[i]);
        }
    }
    globfree(&runtime_files);

    if (config.qga_enabled) {
        char *const qga_argv[] = {"/bin/qemu-ga", "-p", config.qga_path, NULL};
        pid_t pid = spawn_process("/bin/qemu-ga", qga_argv, environ, SPAWN_MANAGED);
        if (pid > 0) {
            printf("[init] QEMU Guest Agent spawned (pid=%d, path=%s)\n", (int)pid,
                   config.qga_path);
        }
    }

    start_system_bus();

    int cloud_ready = !cloud_required;
    int cloud_metadata_degraded = 0;

    // Сетевые службы ждут настройки cloud, оболочки и QGA уже запущены
    if (cloud > 0) {
        printf("[init] local consoles ready; cloud-init continues in background\n");
        int cloud_status = 0;
        int cloud_timed_out = 0;
        int cloud_restored = 0;
        pid_t waited = wait_cloud_init(cloud, &cloud_status, &cloud_timed_out);
        int cloud_result = cloud_init_result(waited, cloud, cloud_status, cloud_required);
        cloud_metadata_degraded = cloud_result == 2;
        cloud_ready = cloud_ready || cloud_result != 0;
        if (cloud_metadata_degraded) {
            report_necromant("F cloud metadata unavailable, using local configuration");
        }

        if (cloud_result == 0) {
            report_necromant("F cloud-init failed on first attempt");
            if (cloud_required && waited != -2 &&
                (cloud_timed_out || (waited == cloud && WIFEXITED(cloud_status) &&
                                     WEXITSTATUS(cloud_status) == 127))) {
                if (restore_cloud_binary("cloud-init", 1)) {
                    cloud_restored = 1;
                    report_necromant("R cloud cloud-init restored after launch failure");
                } else {
                    report_necromant("F cloud cloud-init restore after launch failure failed");
                }
            }
            if (waited == -2) {
                fprintf(stderr, "[init] cloud-init is still running; skipping retry\n");
            } else {
                fprintf(stderr, "[init] cloud-init failed; retrying once\n");
                cloud = fork();
                if (cloud == 0) {
                    service_log_start("cloud-init", 1);
                    exec_cloud_init(cloud_required);
                }
            }

            if (waited != -2 && cloud > 0) {
                cloud_timed_out = 0;
                waited = wait_cloud_init(cloud, &cloud_status, &cloud_timed_out);
                cloud_result = cloud_init_result(waited, cloud, cloud_status, cloud_required);
                if (cloud_result != 0) {
                    if (cloud_result == 2) {
                        cloud_metadata_degraded = 1;
                        report_necromant("F cloud metadata unavailable, using local configuration");
                    }
                    report_necromant("R cloud-init recovered on retry");
                    cloud_ready = 1;
                } else {
                    report_necromant("F cloud-init retry failed");
                    fprintf(stderr, "[init] cloud-init retry failed; checking boot recovery\n");
                    if (cloud_required && waited != -2 && !cloud_restored &&
                        restore_cloud_binary("cloud-init", 1)) {
                        cloud_restored = 1;
                        report_necromant("R cloud cloud-init restored after retry failure");
                        cloud = fork();
                        if (cloud == 0) {
                            service_log_start("cloud-init", 1);
                            exec_cloud_init(cloud_required);
                        }
                        if (cloud > 0) {
                            cloud_timed_out = 0;
                            waited = wait_cloud_init(cloud, &cloud_status, &cloud_timed_out);
                            cloud_result = cloud_init_result(waited, cloud, cloud_status, cloud_required);
                            if (cloud_result != 0) {
                                if (cloud_result == 2) {
                                    cloud_metadata_degraded = 1;
                                    report_necromant("F cloud metadata unavailable, using local configuration");
                                }
                                report_necromant("R cloud-init recovered from backup");
                                cloud_ready = 1;
                            } else {
                                report_necromant("F cloud-init backup failed");
                            }
                        } else {
                            report_necromant("F cloud-init backup fork failed");
                        }
                    }
                    if (!cloud_ready) {
                        fprintf(stderr, "[init] cloud-init unavailable; using local configuration\n");
                    }
                }
            } else if (waited != -2) {
                report_necromant("F cloud-init retry fork failed");
            }
        }
    }
    load_kernel_modules(true, 0);
    parse_setup(&config);
    configure_locale(&config);
    apply_boot_session(&config);
    generate_etc_files(&config);

    desktop_ready = first_boot_result >= 0 && config.desktop_enabled &&
                    access(config.desktop_path[0] ? config.desktop_path : "/bin/desktop", X_OK) == 0;
    pid_t desktop_started = 0;
    if (desktop_ready) {
        int display = open("/dev/tty0", O_WRONLY | O_CLOEXEC);
        if (display >= 0) {
            static const char message[] = "\033[2J\033[H\n\n  SynapseOS\n\n  Загрузка рабочего стола...\n";
            (void)write(display, message, sizeof(message) - 1);
            close(display);
        }

        desktop_started = start_desktop_session(&config);
    }

    if (!getenv("SYNAPSE_NO_NETWORK") || strcmp(getenv("SYNAPSE_NO_NETWORK"), "1")) {
        configure_network(&config);
    }
    if (config.hostname[0]) {
        sethostname(config.hostname, strlen(config.hostname));
    }

    int ssh_ready = setup_ssh_account(&config);
    int ssh_credentials_pending = 0;
    if (ssh_ready && !ssh_access_ready(config.ssh_login)) {
        fprintf(stderr, "[init] SSH disabled: provide a unique key through cloud-init or set a local password\n");
        report_necromant("P cloud waiting for administrator credentials");
        ssh_credentials_pending = 1;
        ssh_ready = 0;
    }
    load_firewall_config();

    // Клиент сам проверяет согласие и повторяет ожидающий отзыв даже при stats=0
    if ((config.stats_enabled || !config.stats_default[0] || !strcmp(config.stats_default, "/bin/stats")) &&
        access(config.stats_default[0] ? config.stats_default : "/bin/stats", X_OK) == 0) {
        const char *stats_bin =
            config.stats_default[0] ? config.stats_default : "/bin/stats";
        char stats_interval[16];
        snprintf(stats_interval, sizeof(stats_interval), "%d",
                 config.stats_delay > 0 ? config.stats_delay : 600);
        char *const stats_argv[] = {(char *)stats_bin,
                                    config.stats_server[0] ? config.stats_server
                                                           : "synapseos.ru:55902",
                                    stats_interval, NULL};
        pid_t tel_pid = spawn_process(stats_bin, stats_argv, environ, SPAWN_UNTIL_SUCCESS);
        if (tel_pid > 0) {
            printf(ANSI_BOLD ANSI_WHITE
                   "[init]" ANSI_RESET " stats client spawned " ANSI_BOLD ANSI_GREEN
                   "OK" ANSI_RESET " (pid: " ANSI_YELLOW "%d" ANSI_RESET ", bin: %s)\n",
                   (int)tel_pid, stats_bin);
        }
    }

    char ssh_port[8];
    snprintf(ssh_port, sizeof(ssh_port), "%d", config.ssh_port);
    char *const ssh_argv[] = {"/bin/dropbear", "-F", "-R", "-p", ssh_port, NULL};
    pid_t ssh_supervisor = -1;
    int ssh_direct = access("/bin/service", X_OK) != 0;
    int ssh_force_direct = 0;
    unsigned ssh_manager_failures = 0;
    int ssh_direct_accepting = 0;
    time_t ssh_direct_probe_at = 0;
    time_t ssh_unhealthy_since = 0;
    time_t ssh_retry_at = 0;
    time_t ssh_recovery_at = 0;
    time_t ssh_stable_since = 0;
    time_t ssh_credentials_probe_at = 0;
    unsigned ssh_recovery_attempts = 0;
    time_t init_loop_started = 0;
    time_t cloud_unavailable_since = 0;
    int init_health_reported = 0;
    unsigned cloud_health_ticks = 0;
    int unrecoverable_reported = 0;
    if (config.ssh_enabled && ssh_ready && access("/bin/dropbear", X_OK) == 0) {
        ssh_supervisor = spawn_process("/bin/dropbear", ssh_argv, environ, SPAWN_MANAGED);
        if (ssh_supervisor > 0) {
            printf("[init] SSH spawned (pid=%d, login=%s, port=%d)\n", (int)ssh_supervisor,
                   config.ssh_login, config.ssh_port);
        }
    }

    if (config.ftp_enabled && access("/bin/ftpd", X_OK) == 0 && ssh_ready &&
        config.ftp_passive_min <= config.ftp_passive_max) {
        char port[8], low[8], high[8];
        snprintf(port, sizeof(port), "%d", config.ftp_port ? config.ftp_port : 21);
        snprintf(low, sizeof(low), "%d",
                 config.ftp_passive_min ? config.ftp_passive_min : 40000);
        snprintf(high, sizeof(high), "%d",
                 config.ftp_passive_max ? config.ftp_passive_max : 40009);
        char *const ftp_argv[] = {"/bin/ftpd",     "-p", port, "--passive-min", low,
                                  "--passive-max", high, NULL};
        pid_t pid = spawn_process("/bin/ftpd", ftp_argv, environ, SPAWN_MANAGED);
        if (pid > 0) {
            printf("[init] FTP spawned (pid=%d, port=%s, passive=%s-%s)\n", (int)pid,
                   port, low, high);
        }
    }

    // Дополнительные локальные службы запускаются после настройки cloud
    if (access("/etc/rc.local", R_OK) == 0) {
        char *const local_argv[] = {"/bin/sh", "/etc/rc.local", NULL};
        pid_t local = spawn_process("/bin/sh", local_argv, environ, SPAWN_DIRECT);
        if (local > 0) {
            printf("[init] local services spawned (pid=%d)\n", (int)local);
        }
    }

    if (access("/bin/cron", X_OK) == 0 && access("/etc/crontab", R_OK) == 0) {
        char *const cron_argv[] = {"/bin/cron", "-n", NULL};
        pid_t pid = spawn_process("/bin/cron", cron_argv, environ, SPAWN_MANAGED);
        if (pid > 0) {
            printf("[init] cron spawned (pid=%d)\n", (int)pid);
        }
    }

    struct dns_refresh_state dns_state = {0};
    dns_refresh_file(&dns_state, "/etc/resolv.conf", "/proc/net-dns");
    spawn_setup_check(&config);

    // Службы из установленных пакетов запускаем под супервизором после настройки cloud
    if (access("/bin/service", X_OK) == 0) {
        glob_t definitions = {0};
        if (glob("/etc/services/*", 0, NULL, &definitions) == 0) {
            for (size_t i = 0; i < definitions.gl_pathc; i++) {
                const char *name = strrchr(definitions.gl_pathv[i], '/') + 1;
                if (strchr(name, '.') || !strcmp(name, "ssh") || !strcmp(name, "qga") ||
                    !strcmp(name, "ftp") || !strcmp(name, "cron") ||
                    !strcmp(name, "stats") || !strcmp(name, "rc-local")) {
                    continue;
                }

                char disabled[512];
                snprintf(disabled, sizeof(disabled), "/etc/services/%s.disabled", name);
                if (access(disabled, F_OK) == 0) {
                    continue;
                }

                pid_t supervisor = fork();
                if (supervisor == 0) {
                    execl("/bin/service", "service", "supervise", name, (char *)NULL);
                    _exit(127);
                }
            }
        }
        globfree(&definitions);
    }



    unsigned dns_tick = 0;
    for (;;) {
        sleep(1);
        if (dns_tick % 2U == 0U) update_usb_modules();
        if (dns_tick % 5U == 0U) {
            report_necromant("P init loop alive");
        }
        if (!(++dns_tick % 2)) {
            dns_refresh_file(&dns_state, "/etc/resolv.conf", "/proc/net-dns");
        }
        int ssh_exited = 0;
        reap_children(consoles, console_count, &ssh_supervisor, &ssh_retry_at, &ssh_exited, &desktop_started);
        if (desktop_ready && !desktop_started) {
            bool tracked = false;
            for (size_t i = 0; i < console_count; i++) {
                if (!strcmp(consoles[i].path, "/dev/tty0")) tracked = true;
            }
            if (!tracked) track_console(consoles, &console_count, "/dev/tty0", shell, shell_argv);
            desktop_ready = 0;
        }
        if (cloud_required && ssh_exited && !ssh_direct) {
            if (ssh_manager_failures < 3) {
                ssh_manager_failures++;
            }
            if (ssh_manager_failures < 3) {
                report_necromant("F cloud SSH manager exited");
                if (restore_cloud_binary("service", 1)) {
                    report_necromant("R cloud service manager restored");
                }
            } else {
                ssh_force_direct = 1;
                report_necromant("F cloud SSH manager failed, using direct SSH");
            }
        }

        time_t now = time(NULL);
        struct timespec clock_now;
        time_t recovery_now = clock_gettime(CLOCK_MONOTONIC, &clock_now) == 0 ?
            clock_now.tv_sec : now;
        if (init_loop_started == 0) {
            init_loop_started = recovery_now;
        }

        // Пароль или ключ можно добавить через локальную консоль и QGA без перезагрузки
        if (config.ssh_enabled && ssh_credentials_pending && recovery_now >= ssh_credentials_probe_at) {
            ssh_credentials_probe_at = recovery_now + 5;
            if (ssh_access_ready(config.ssh_login)) {
                ssh_credentials_pending = 0;
                ssh_ready = 1;
                report_necromant("R cloud administrator credentials configured");
            }
        }

        int ssh_state = 0;
        if (cloud_required && config.ssh_enabled && ssh_ready && ssh_supervisor > 0) {
            if (recovery_now >= ssh_direct_probe_at) {
                ssh_direct_accepting = ssh_direct_ready(ssh_supervisor, config.ssh_port);
                ssh_direct_probe_at = recovery_now + 5;
            }
            ssh_state = ssh_direct ?
                (ssh_direct_accepting ? 2 : 0) :
                ssh_service_state(ssh_supervisor, recovery_now);
            if (!ssh_direct_accepting && ssh_state == 2) {
                ssh_state = 0;
            }
        }
        if (ssh_state == 2) {
            ssh_unhealthy_since = 0;
            if (ssh_stable_since == 0) {
                ssh_stable_since = recovery_now;
            } else if (recovery_now - ssh_stable_since >= 60) {
                ssh_recovery_attempts = 0;
                ssh_manager_failures = 0;
            }
        } else {
            ssh_stable_since = 0;
            if (ssh_unhealthy_since == 0) {
                ssh_unhealthy_since = recovery_now;
            }
        }
        int cloud_service_ready = cloud_required && cloud_ready &&
            cloud_ssh_health_ready(config.ssh_enabled, ssh_credentials_pending, ssh_ready,
                                   ssh_stable_since, recovery_now) &&
            cloud_binary_ready("/bin/sh") && cloud_binary_ready("/bin/cloud-init") &&
            cloud_binary_ready("/bin/dropbear");
        if (cloud_required && !cloud_service_ready && !init_health_reported &&
            ++cloud_health_ticks == 10) {
            char report[128];
            snprintf(report, sizeof(report),
                     "F cloud health pending ssh=%d ready=%d account=%d stable=%lld binaries=%d%d%d",
                     ssh_state, cloud_ready, ssh_ready,
                     ssh_stable_since == 0 ? -1LL : (long long)(recovery_now - ssh_stable_since),
                     cloud_binary_ready("/bin/sh"),
                     cloud_binary_ready("/bin/cloud-init"), cloud_binary_ready("/bin/dropbear"));
            report_necromant(report);
        }
        if (!init_health_reported &&
            ((!cloud_required && access(shell, X_OK) == 0 &&
              recovery_now - init_loop_started >= 5) || cloud_service_ready)) {
            const char *health = "H init services ready";
            if (cloud_required && config.ssh_enabled && ssh_credentials_pending) {
                health = "H init local services ready, administrator credentials pending";
            } else if (cloud_metadata_degraded) {
                health = "H init local services ready, metadata unavailable";
            }

            init_health_reported = report_necromant(health);
        }
        if (cloud_required) {
            if (cloud_service_ready) {
                cloud_unavailable_since = 0;
            } else if (cloud_unavailable_since == 0) {
                cloud_unavailable_since = recovery_now;
            } else if (!unrecoverable_reported &&
                       recovery_now - cloud_unavailable_since >= 120) {
                report_necromant("F cloud required services unavailable");
                unrecoverable_reported = report_necromant("U cloud recovery exhausted");
            }
        }
        if (ssh_state == 1 && recovery_now >= ssh_recovery_at) {
            report_necromant("F cloud ssh quarantined");
            if (ssh_recovery_attempts < 3) {
                ssh_recovery_attempts++;
            }
            ssh_recovery_at = recovery_now + (ssh_recovery_attempts < 3 ? 30 : 300);
            if (recover_ssh()) {
                fprintf(stderr, "[init] SSH recovery requested after quarantine\n");
            }
        } else if (cloud_required && ssh_state == 0 && ssh_supervisor > 0 &&
                   recovery_now - ssh_unhealthy_since >= 15 && recovery_now >= ssh_recovery_at) {
            report_necromant("F cloud ssh not accepting connections");
            if (ssh_recovery_attempts < 3) {
                ssh_recovery_attempts++;
            }
            ssh_recovery_at = recovery_now + 30;
            if (ssh_direct) {
                kill(ssh_supervisor, SIGTERM);
            } else if (ssh_recovery_attempts < 3) {
                recover_ssh();
            } else {
                ssh_force_direct = 1;
                report_necromant("F cloud SSH manager stopping before direct SSH");
                kill(ssh_supervisor, SIGTERM);
            }
        }

        // Новый listener запускается только после waitpid супервизора и остановки его ребёнка
        if (config.ssh_enabled && ssh_ready && ssh_supervisor <= 0 && now >= ssh_retry_at &&
            access("/bin/dropbear", X_OK) == 0) {
            if (cloud_required && !ssh_force_direct && !cloud_binary_ready("/bin/service")) {
                report_necromant("F cloud service unavailable during SSH restart");
                if (restore_cloud_binary("service", 0)) {
                    report_necromant("R cloud service restored during SSH restart");
                }
            }
            ssh_direct = ssh_force_direct || access("/bin/service", X_OK) != 0;
            ssh_direct_accepting = 0;
            ssh_direct_probe_at = 0;
            ssh_supervisor = spawn_process("/bin/dropbear", ssh_argv, environ,
                                           ssh_direct ? SPAWN_DIRECT : SPAWN_MANAGED);
            ssh_retry_at = now + 5;
            if (ssh_supervisor > 0) {
                ssh_unhealthy_since = recovery_now;
                report_necromant("R ssh supervisor restarted");
            }
        }

        for (size_t i = 0; i < console_count; i++) {
            if (consoles[i].pid <= 0 && now >= consoles[i].retry_at) {
                consoles[i].pid = spawn_tty_shell(consoles[i].path, shell, shell_argv);
                consoles[i].retry_at = now + 5;
                if (consoles[i].pid > 0) {
                    char report[96];
                    snprintf(report, sizeof(report), "R shell %.72s restarted", consoles[i].path);
                    report_necromant(report);
                }
            }
        }
    }

    return 0;
}
