/*
    wifi.c: Консольное управление беспроводной сетью
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: Linux-compatible userspace

    Описание: Сканирование, подключение и сохранение профиля через /dev/wifi
    Подсистема: пользовательские сетевые программы
    Первое изменение: 16.11.2026
    Последнее изменение: 26.11.2026
    Доступ: общедоступный
*/

#include <lumine/wifi.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <termios.h>

#ifndef WIFI_DEVICE
#define WIFI_DEVICE "/dev/wifi"
#endif
#ifndef WIFI_PROFILE_DIR
#define WIFI_PROFILE_DIR "/etc/network"
#endif
#ifndef WIFI_PROFILE
#define WIFI_PROFILE WIFI_PROFILE_DIR "/wifi.conf"
#endif

static void wifi_wipe(void *data, size_t size) {
    volatile uint8_t *bytes = data;

    while (size-- != 0U) {
        *bytes++ = 0U;
    }
}

static void wifi_print_ssid(const uint8_t *ssid, size_t size) {
    for (size_t i = 0; i < size; i++) {
        unsigned byte = ssid[i];
        if ((byte == 0xd0U || byte == 0xd1U) && i + 1U < size && (ssid[i + 1U] & 0xc0U) == 0x80U) {
            putchar((int)byte);
            putchar((int)ssid[++i]);
            continue;
        }
        putchar(byte >= 32U && byte <= 126U && byte != '\\' ? (int)byte : '?');
    }
}

static void wifi_print_ipv4(uint32_t address) {
    printf("%u.%u.%u.%u", (unsigned)(address >> 24U), (unsigned)((address >> 16U) & 255U),
           (unsigned)((address >> 8U) & 255U), (unsigned)(address & 255U));
}

struct wifi_adapter {
    const char *name, *device, *profile;
};

static const struct wifi_adapter adapters[] = {
    {"usb", "/dev/wifi-usb", WIFI_PROFILE_DIR "/wifi-usb.conf"},
    {"onboard", "/dev/wifi-onboard", WIFI_PROFILE_DIR "/wifi-onboard.conf"},
    {"auto", WIFI_DEVICE, WIFI_PROFILE}
};
static const struct wifi_adapter *adapter;

// На плате предпочитать подключённый USB-адаптер, сохраняя выбор на всё время команды
static int wifi_open(void) {
    const int flags = O_RDWR | O_CLOEXEC | O_NOFOLLOW;
    if (!adapter) {
        for (unsigned i = 0; i < 2; ++i) {
            int fd = open(adapters[i].device, flags);
            if (fd >= 0) {
                adapter = &adapters[i];
                return fd;
            }
            if (errno != ENOENT && errno != ENODEV && errno != ENXIO) {
                perror(adapters[i].device);
                return -1;
            }
        }
        adapter = &adapters[2];
    }
    int fd = open(adapter->device, flags);
    if (fd < 0) {
        fprintf(stderr, "wifi: %s: %s\n", adapter->device, strerror(errno));
    }
    return fd;
}

static int wifi_scan(int fd) {
    struct syn_wifi_scan result = {.version = SYN_WIFI_ABI_VERSION};

    if (ioctl(fd, SYN_WIFI_IOCTL_SCAN, &result) < 0) {
        perror("wifi: scan");
        return 1;
    }

    if (result.version != SYN_WIFI_ABI_VERSION || result.count > SYN_WIFI_SCAN_LIMIT) {
        fprintf(stderr, "wifi: invalid scan response\n");
        return 1;
    }

    puts("SSID                             BSSID              Signal  Ch  Security");

    for (uint32_t i = 0; i < result.count; i++) {
        const struct syn_wifi_bss *network = &result.entries[i];

        if (network->ssid_length > SYN_WIFI_SSID_LIMIT) {
            continue;
        }

        wifi_print_ssid(network->ssid, network->ssid_length);

        for (unsigned pad = network->ssid_length; pad < SYN_WIFI_SSID_LIMIT + 1U; pad++) {
            putchar(' ');
        }

        for (unsigned b = 0; b < 6U; b++) {
            printf("%02x%s", network->bssid[b], b == 5U ? "" : ":");
        }

        int signal = ((int)network->rssi_dbm + 100) * 2;

        if (signal < 0) {
            signal = 0;
        } else if (signal > 100) {
            signal = 100;
        }

        printf("  %3d%%   %2u  %s\n", signal, network->channel,
               network->security == SYN_WIFI_SECURITY_WPA2_PSK ? "WPA2-PSK" : "Open");
    }

    return 0;
}

static int wifi_status(int fd) {
    struct syn_wifi_status status = {.version = SYN_WIFI_ABI_VERSION};

    if (ioctl(fd, SYN_WIFI_IOCTL_STATUS, &status) < 0) {
        perror("wifi: status");
        return 1;
    }

    if (status.version != SYN_WIFI_ABI_VERSION || status.ssid_length > SYN_WIFI_SSID_LIMIT) {
        fprintf(stderr, "wifi: invalid status response\n");
        return 1;
    }

    printf("Adapter: %s\n", status.enabled ? "on" : "off");

    if (!status.associated) {
        puts("Connection: disconnected");
        return 0;
    }

    fputs("Connection: ", stdout);
    wifi_print_ssid(status.ssid, status.ssid_length);
    printf("\nBSSID: %02x:%02x:%02x:%02x:%02x:%02x\nChannel: %u\n",
           status.bssid[0], status.bssid[1], status.bssid[2],
           status.bssid[3], status.bssid[4], status.bssid[5], status.channel);
    printf("Link: %u Mb/s\n", status.link_mbps);

    if (status.ready && status.ipv4 != 0) {
        fputs("IPv4: ", stdout);
        wifi_print_ipv4(status.ipv4);
        fputs("\nGateway: ", stdout);
        wifi_print_ipv4(status.gateway);
        putchar('\n');
    } else {
        puts("IPv4: waiting for DHCP");
    }

    return 0;
}

static int wifi_write_all(int fd, const char *text, size_t size) {
    while (size != 0U) {
        ssize_t done = write(fd, text, size);

        if (done < 0 && errno == EINTR) {
            continue;
        }

        if (done <= 0) {
            return -1;
        }

        text += done;
        size -= (size_t)done;
    }

    return 0;
}

static bool wifi_save_profile(const struct syn_wifi_connect *request) {
    if (mkdir(WIFI_PROFILE_DIR, 0755) < 0 && errno != EEXIST) {
        return false;
    }

    char temporary[sizeof(WIFI_PROFILE_DIR "/wifi-onboard.conf.tmp.") + 24];
    int name_length = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", adapter->profile, (long)getpid());

    if (name_length < 0 || (size_t)name_length >= sizeof(temporary)) {
        return false;
    }

    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);

    if (fd < 0) {
        return false;
    }

    char text[256];
    size_t used = 0;
    static const char digits[] = "0123456789abcdef";
    const char *labels[] = {"ssid=", "password="};
    const uint8_t *values[] = {request->ssid, request->password};
    size_t lengths[] = {request->ssid_length, request->password_length};

    for (unsigned field = 0; field < 2U; field++) {
        size_t label_length = strlen(labels[field]);
        memcpy(text + used, labels[field], label_length);
        used += label_length;

        for (size_t i = 0; i < lengths[field]; i++) {
            text[used++] = digits[values[field][i] >> 4U];
            text[used++] = digits[values[field][i] & 15U];
        }

        text[used++] = '\n';
    }

    bool okay = wifi_write_all(fd, text, used) == 0 && fsync(fd) == 0;
    wifi_wipe(text, sizeof(text));

    if (close(fd) < 0) {
        okay = false;
    }

    if (okay) {
        okay = rename(temporary, adapter->profile) == 0;
    }

    if (!okay) {
        unlink(temporary);
    }

    return okay;
}

static int wifi_hex(unsigned c) {
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }

    if (c >= 'a' && c <= 'f') {
        return (int)(c - 'a' + 10U);
    }

    return -1;
}

static bool wifi_decode(const char *text, size_t size, uint8_t *out, size_t limit, uint8_t *length) {
    if ((size & 1U) || size / 2U > limit) {
        return false;
    }

    for (size_t i = 0; i < size / 2U; i++) {
        int high = wifi_hex((unsigned char)text[i * 2U]);
        int low = wifi_hex((unsigned char)text[i * 2U + 1U]);

        if (high < 0 || low < 0) {
            return false;
        }

        out[i] = (uint8_t)((high << 4U) | low);
    }

    *length = (uint8_t)(size / 2U);
    return true;
}

static bool wifi_load_profile(struct syn_wifi_connect *request) {
    const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
    int fd = open(adapter->profile, flags);
    if (fd < 0 && errno == ENOENT && adapter != &adapters[2]) {
        fd = open(WIFI_PROFILE, flags);
    }

    if (fd < 0) {
        return false;
    }

    struct stat info;
    char text[256];
    ssize_t count = read(fd, text, sizeof(text));
    bool okay = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                (info.st_mode & 077U) == 0U && count > 0 && count < (ssize_t)sizeof(text);
    close(fd);

    if (okay) {
        char *second = memchr(text, '\n', (size_t)count);

        if (!second || (size_t)(second - text) < 5U ||
            memcmp(text, "ssid=", 5U) != 0 || (size_t)(second + 1 - text) + 9U > (size_t)count ||
            memcmp(second + 1, "password=", 9U) != 0 || text[count - 1] != '\n') {
            okay = false;
        } else {
            okay = wifi_decode(text + 5U, (size_t)(second - text) - 5U, request->ssid,
                               SYN_WIFI_SSID_LIMIT, &request->ssid_length) &&
                   wifi_decode(second + 10U, (size_t)(text + count - second - 11U), request->password,
                               SYN_WIFI_PASSWORD_LIMIT, &request->password_length) &&
                   request->ssid_length > 0U;
        }
    }

    wifi_wipe(text, sizeof(text));
    return okay;
}

// Считать пароль с терминала без эха и без записи в историю оболочки
static bool wifi_ask_password(struct syn_wifi_connect *request) {
    struct termios saved;
    if (tcgetattr(STDIN_FILENO, &saved) < 0) return false;
    struct termios hidden = saved;
    hidden.c_lflag &= ~(tcflag_t)(ECHO | ICANON);
    hidden.c_cc[VMIN] = 1;
    hidden.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) < 0) return false;
    fputs("Wi-Fi password: ", stderr);
    fflush(stderr);
    size_t n = 0;
    bool complete = false, overflow = false;
    unsigned char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == '\n' || c == '\r') { complete = true; break; }
        if (c == 3 || c == 4) break;
        if (c == 127 || c == 8) { if (n) --n; continue; }
        if (n < sizeof(request->password)) request->password[n++] = c;
        else overflow = true;
    }
    if (tcsetattr(STDIN_FILENO, TCSANOW, &saved) < 0) complete = false;
    fputc('\n', stderr);
    request->password_length = (uint8_t)n;
    return complete && !overflow && n >= 8;
}

static int wifi_connect(int fd, int argc, char **argv, bool automatic) {
    struct syn_wifi_connect request = {.version = SYN_WIFI_ABI_VERSION};

    if (automatic) {
        if (!wifi_load_profile(&request)) {
            fprintf(stderr, "wifi: no usable private profile for %s\n", adapter->name);
            return 1;
        }
    } else {
        size_t ssid_length = strlen(argv[2]);
        bool ask = argc == 4 && !strcmp(argv[3], "--ask");
        size_t password_length = argc == 4 && !ask ? strlen(argv[3]) : 0U;

        if (ssid_length == 0U || ssid_length > SYN_WIFI_SSID_LIMIT ||
            password_length > SYN_WIFI_PASSWORD_LIMIT ||
            (password_length != 0U && password_length < 8U)) {
            fprintf(stderr, "wifi: SSID must be 1..32 bytes; WPA2 password must be 8..63 bytes\n");
            return 1;
        }

        memcpy(request.ssid, argv[2], ssid_length);
        request.ssid_length = (uint8_t)ssid_length;

        if (ask && !wifi_ask_password(&request)) {
            wifi_wipe(&request, sizeof(request));
            fputs("wifi: password entry cancelled or invalid (8..63 bytes)\n", stderr);
            return 1;
        }
        if (password_length != 0U) {
            memcpy(request.password, argv[3], password_length);
            request.password_length = (uint8_t)password_length;
        }
    }

    int result = ioctl(fd, SYN_WIFI_IOCTL_CONNECT, &request);

    if (result < 0) {
        perror("wifi: connect");
    } else if (!automatic && !wifi_save_profile(&request)) {
        perror("wifi: save profile");
        result = -1;
    }

    wifi_wipe(&request, sizeof(request));
    return result < 0 ? 1 : 0;
}

// Проверяет только фиксированный ABI, никакие unknown output flags не принимаются
static bool wifi_phy_response_valid(const struct syn_wifi_phy_stats *value) {
    if (value->version != SYN_WIFI_PHY_VERSION || value->size != sizeof(*value) || value->flags ||
        value->reserved || value->reserved2 || (value->state_flags & ~7u) ||
        (value->valid_mask & ~SYN_WIFI_PHY_VALID_ALL)) {
        return false;
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (value->reserved_tail[i]) {
            return false;
        }
    }
    return true;
}

// Только эта явная diagnostic команда ставит QUERY, обычные scan/status не затрагиваются
static int wifi_phy_stats(int fd) {
    _Static_assert(sizeof(struct syn_wifi_phy_stats) == 136, "PHY public ABI v1");
    struct syn_wifi_phy_stats value = {.version = SYN_WIFI_PHY_VERSION,
        .size = sizeof(value), .flags = SYN_WIFI_PHY_QUERY};
    if (ioctl(fd, SYN_WIFI_IOCTL_PHY_STATS, &value) < 0) {
        if (errno == ENOTTY || errno == ENOTSUP) {
            fputs("wifi: PHY diagnostics are not supported by this driver\n", stderr);
        } else {
            perror("wifi: phy-stats query");
        }
        return 1;
    }
    if (!wifi_phy_response_valid(&value) ||
        !value.generation || !value.request_serial) {
        fputs("wifi: invalid PHY query response\n", stderr);
        return 1;
    }
    uint32_t generation = value.generation, serial = value.request_serial;
    struct timespec started;
    if (clock_gettime(CLOCK_MONOTONIC, &started)) {
        perror("wifi: phy-stats clock");
        return 1;
    }
    // Bounded4s клиентский срок не означает firmware ACK, один query не повторяется
    for (unsigned attempt = 0; attempt < 81; ++attempt) {
        value = (struct syn_wifi_phy_stats){.version = SYN_WIFI_PHY_VERSION,
            .size = sizeof(value), .generation = generation};
        int result = ioctl(fd, SYN_WIFI_IOCTL_PHY_STATS, &value);
        if (!result) {
            if (!wifi_phy_response_valid(&value) ||
                value.generation != generation || value.request_serial != serial ||
                value.sample_serial != serial || value.valid_mask != SYN_WIFI_PHY_VALID_ALL ||
                value.state_flags != SYN_WIFI_PHY_CANDIDATE || value.last_error) {
                fputs("wifi: invalid or stale PHY sample\n", stderr);
                return 1;
            }
            printf("PHY candidate after query (firmware has no request ID), generation=%" PRIu32
                   " request=%" PRIu32 " layout=%" PRIu32 " bytes=%" PRIu32 "/%" PRIu32 "\n",
                   generation, serial, value.layout, value.fixed_bytes, value.data_bytes);
            printf("noise_floor=%" PRId32 " tx_power_raw=%" PRIu32 " (raw gauge, units unspecified)\n",
                   value.noise_floor, value.tx_power_raw);
            printf("tx_frame=%" PRIu64 " rx_frame=%" PRIu64 " rx_clear=%" PRIu64
                   " cycles=%" PRIu64 " phy_error=%" PRIu64 " (raw FW32, no deltas)\n",
                   value.tx_frame, value.rx_frame, value.rx_clear, value.cycles, value.phy_error);
            return 0;
        }
        if (errno != EAGAIN && errno != EBUSY) {
            if (errno == ENOTTY || errno == ENOTSUP) {
                fprintf(stderr, "wifi: PHY diagnostics are not supported by this driver/firmware layout"
                    " (layout=%" PRIu32 " bytes=%" PRIu32 "/%" PRIu32 ")\n",
                    value.layout, value.fixed_bytes, value.data_bytes);
            } else {
                perror("wifi: phy-stats sample");
            }
            return 1;
        }
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) {
            return 1;
        }
        int64_t elapsed = (int64_t)(now.tv_sec - started.tv_sec) * 1000000000LL +
            (int64_t)now.tv_nsec - started.tv_nsec;
        if (elapsed < 0 || elapsed >= 4000000000LL || attempt == 80) {
            break;
        }
        struct timespec delay = {.tv_nsec = 50000000};
        if (nanosleep(&delay, NULL) && errno != EINTR) {
            perror("wifi: phy-stats delay");
            return 1;
        }
    }
    fputs("wifi: PHY sample timed out, no automatic query retry\n", stderr);
    return 1;
}

static void wifi_usage(void) {
    fputs("Usage: wifi [usb|onboard] scan | connect SSID [password|--ask] | status | disconnect | on | off | auto | phy-stats\n", stderr);
}

int main(int argc, char **argv) {
    bool explicit_option = argc >= 2 && !strcmp(argv[1], "--interface");
    if (explicit_option) {
        --argc;
        ++argv;
    }
    if (argc >= 2) {
        for (unsigned i = 0; i < 2; ++i) {
            if (!strcmp(argv[1], adapters[i].name)) {
                adapter = &adapters[i];
                --argc;
                ++argv;
                break;
            }
        }
    }
    if (explicit_option && !adapter) {
        wifi_usage();
        return 2;
    }
    if (argc < 2) {
        wifi_usage();
        return 2;
    }

    if (argc == 2 && !strcmp(argv[1], "--help")) {
        wifi_usage();
        return 0;
    }

    if (!strcmp(argv[1], "connect") && (argc < 3 || argc > 4)) {
        wifi_usage();
        return 2;
    }

    int fd = wifi_open();

    if (fd < 0) {
        return 1;
    }

    int result = 0;

    if (!strcmp(argv[1], "scan") && argc == 2) {
        result = wifi_scan(fd);
    } else if (!strcmp(argv[1], "connect")) {
        result = wifi_connect(fd, argc, argv, false);
    } else if (!strcmp(argv[1], "auto") && argc == 2) {
        result = wifi_connect(fd, argc, argv, true);
    } else if (!strcmp(argv[1], "phy-stats") && argc == 2) {
        result = wifi_phy_stats(fd);
    } else if (!strcmp(argv[1], "status") && argc == 2) {
        result = wifi_status(fd);
    } else if (!strcmp(argv[1], "disconnect") && argc == 2) {
        if (ioctl(fd, SYN_WIFI_IOCTL_DISCONNECT, 0) < 0) {
            perror("wifi: disconnect");
            result = 1;
        }
    } else if ((!strcmp(argv[1], "on") || !strcmp(argv[1], "off")) && argc == 2) {
        struct syn_wifi_enabled enabled = {.version = SYN_WIFI_ABI_VERSION,
                                           .enabled = (uint32_t)!strcmp(argv[1], "on")};

        if (ioctl(fd, SYN_WIFI_IOCTL_SET_ENABLED, &enabled) < 0) {
            perror("wifi: power");
            result = 1;
        }
    } else {
        wifi_usage();
        result = 2;
    }

    close(fd);
    return result;
}
