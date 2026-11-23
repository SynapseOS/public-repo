/*
    btctl.c: Управление Bluetooth-адаптерами через /dev/bthciN
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI: Linux-compatible userspace и HCI H4
    Описание: Сброс, сведения, сканирование LE и BR/EDR, включение приёма
    Подсистема: пользовательские программы, Bluetooth
    Первое изменение: 23.11.2026
    Последнее изменение: 23.11.2026
    Доступ: общедоступный
*/

#define _GNU_SOURCE
#include <lumine/bthci.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifndef BT_DEVICE
#define BT_DEVICE "/dev/bthci0"
#endif

#define HCI_RESET 0x0c03u
#define HCI_INQUIRY 0x0401u
#define HCI_INQUIRY_CANCEL 0x0402u
#define HCI_WRITE_NAME 0x0c13u
#define HCI_WRITE_SCAN_ENABLE 0x0c1au
#define HCI_WRITE_CLASS 0x0c24u
#define HCI_WRITE_INQUIRY_MODE 0x0c45u
#define HCI_WRITE_PAIRING 0x0c56u
#define HCI_WRITE_LE_HOST 0x0c6du
#define HCI_SET_EVENT_MASK 0x0c01u
#define HCI_READ_LOCAL_VERSION 0x1001u
#define HCI_READ_BUFFER_SIZE 0x1005u
#define HCI_READ_BD_ADDR 0x1009u
#define HCI_LE_SET_EVENT_MASK 0x2001u
#define HCI_LE_SET_SCAN_PARAMS 0x200bu
#define HCI_LE_SET_SCAN_ENABLE 0x200cu

struct event { uint8_t code, len, data[255]; };

struct bt_device {
    uint8_t address[6];
    int8_t rssi;
    uint32_t cod;
    char name[64];
    unsigned seen;
};

static struct bt_device found[64];
static unsigned found_count;

static int64_t milliseconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
        perror("clock_gettime");
        exit(1);
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Кадр H4 может занимать несколько чтений, устройство открыто без блокировок
static int transfer(int fd, uint8_t *p, size_t n, int tx, int64_t until) {
    while (n) {
        int64_t left = until - milliseconds();
        if (left <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct pollfd port = {fd, tx ? POLLOUT : POLLIN, 0};
        int ready = poll(&port, 1, (int)left);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            if (!ready) {
                errno = ETIMEDOUT;
            }
            return -1;
        }
        ssize_t out = tx ? write(fd, p, n) : read(fd, p, n);
        if (out < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        }
        if (out <= 0) {
            if (!out) {
                errno = EIO;
            }
            return -1;
        }
        p += out;
        n -= (size_t)out;
    }
    return 0;
}

// Пропустить остаток кадра неизвестного типа по его объявленной длине
static int frame_skip(int fd, uint8_t type, uint8_t a, uint8_t b, int64_t until) {
    unsigned rest;
    if (type == SYN_BTHCI_H4_ACL) {
        uint8_t dlen[2];
        if (transfer(fd, dlen, 2, 0, until)) {
            return -1;
        }
        (void)a;
        (void)b;
        rest = (unsigned)dlen[0] | ((unsigned)dlen[1] << 8);
    } else if (type == SYN_BTHCI_H4_SCO) {
        // Длина данных SCO идёт третьим байтом заголовка, читаем её отдельно
        (void)a;
        (void)b;
        uint8_t dlen;
        if (transfer(fd, &dlen, 1, 0, until)) {
            return -1;
        }
        rest = dlen;
    } else {
        errno = EPROTO;
        return -1;
    }
    uint8_t skip[64];
    while (rest) {
        unsigned chunk = rest > sizeof(skip) ? sizeof(skip) : rest;
        if (transfer(fd, skip, chunk, 0, until)) {
            return -1;
        }
        rest -= chunk;
    }
    return 0;
}

// Следующее событие H4 типа 04 с ограничением по времени
static int event_read(int fd, struct event *ev, int64_t until) {
    uint8_t header[3];
    for (;;) {
        if (transfer(fd, header, sizeof(header), 0, until)) {
            return -1;
        }
        if (header[0] == SYN_BTHCI_H4_EVENT) {
            break;
        }
        if (frame_skip(fd, header[0], header[1], header[2], until)) {
            return -1;
        }
    }
    ev->code = header[1];
    ev->len = header[2];
    return transfer(fd, ev->data, ev->len, 0, until);
}

// Дождаться завершения команды, пропуская асинхронные события контроллера
static int command(int fd, unsigned op, const uint8_t *arg, unsigned len, struct event *ev) {
    uint8_t packet[259] = {SYN_BTHCI_H4_COMMAND, (uint8_t)op, (uint8_t)(op >> 8), (uint8_t)len};
    int64_t until = milliseconds() + 3000;
    if (len) {
        memcpy(packet + 4, arg, len);
    }
    if (transfer(fd, packet, len + 4, 1, until)) {
        return -1;
    }
    while (!event_read(fd, ev, until)) {
        if (ev->code == 0x0e && ev->len >= 4 && ev->data[1] == packet[1] && ev->data[2] == packet[2]) {
            if (!ev->data[3]) {
                return 0;
            }
            fprintf(stderr, "HCI %04x status %02x\n", op, ev->data[3]);
            errno = EIO;
            return -1;
        }
        if (ev->code == 0x0f && ev->len == 4 && ev->data[2] == packet[1] &&
            ev->data[3] == packet[2] && ev->data[0]) {
            fprintf(stderr, "HCI %04x status %02x\n", op, ev->data[0]);
            errno = EIO;
            return -1;
        }
    }
    return -1;
}

// Команда с немедленным ответом Command Status, завершение приходит событием позже
static int command_status(int fd, unsigned op, const uint8_t *arg, unsigned len,
                          struct event *ev) {
    uint8_t packet[259] = {SYN_BTHCI_H4_COMMAND, (uint8_t)op, (uint8_t)(op >> 8), (uint8_t)len};
    int64_t until = milliseconds() + 3000;
    if (len) {
        memcpy(packet + 4, arg, len);
    }
    if (transfer(fd, packet, len + 4, 1, until)) {
        return -1;
    }
    while (!event_read(fd, ev, until)) {
        if (ev->code != 0x0f || ev->len != 4 || ev->data[2] != packet[1] ||
            ev->data[3] != packet[2]) {
            continue;
        }
        if (!ev->data[0]) {
            return 0;
        }
        fprintf(stderr, "HCI %04x status %02x\n", op, ev->data[0]);
        errno = EIO;
        return -1;
    }
    return -1;
}

// Найти завершённое имя в рекламных данных
static void adv_name(const uint8_t *data, unsigned len, char *out, size_t cap) {
    out[0] = 0;
    unsigned pos = 0;
    while (pos + 2 <= len) {
        unsigned field = data[pos];
        uint8_t type = data[pos + 1];
        if (!field || pos + 1 + field > len) {
            break;
        }
        if ((type == 0x08 || type == 0x09) && field > 1) {
            unsigned name = field - 1;
            if (name >= cap) {
                name = (unsigned)cap - 1;
            }
            memcpy(out, data + pos + 2, name);
            out[name] = 0;
            return;
        }
        pos += 1 + field;
    }
}

// Запомнить устройство или обновить имя у уже виденного
static void found_add(const uint8_t *address, int rssi, uint32_t cod, const char *name) {
    for (unsigned i = 0; i < found_count; ++i) {
        if (!memcmp(found[i].address, address, 6)) {
            if (name && *name && !*found[i].name) {
                snprintf(found[i].name, sizeof(found[i].name), "%s", name);
            }
            found[i].rssi = (int8_t)rssi;
            found[i].seen++;
            return;
        }
    }
    if (found_count >= sizeof(found) / sizeof(found[0])) {
        return;
    }
    struct bt_device *d = &found[found_count++];
    memcpy(d->address, address, 6);
    d->rssi = (int8_t)rssi;
    d->cod = cod;
    d->seen = 1;
    snprintf(d->name, sizeof(d->name), "%s", name ? name : "");
}

static void print_address(const uint8_t *a) {
    printf("%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
}

// Общий сброс и настройки приёма для штатной работы стека
static int host_init(int fd, struct event *ev, const char *name, uint8_t cod[3]) {
    if (command(fd, HCI_RESET, NULL, 0, ev) ||
        command(fd, HCI_READ_LOCAL_VERSION, NULL, 0, ev) || ev->len < 12) {
        return -1;
    }

    printf("hci %u rev %04x lmp %u manufacturer %u subversion %04x\n",
           ev->data[4], ev->data[5] | (unsigned)ev->data[6] << 8, ev->data[7],
           ev->data[8] | (unsigned)ev->data[9] << 8,
           ev->data[10] | (unsigned)ev->data[11] << 8);

    if (command(fd, HCI_READ_BD_ADDR, NULL, 0, ev) || ev->len < 10) {
        return -1;
    }
    printf("bd_addr ");
    print_address(ev->data + 4);
    putchar('\n');

    static const uint8_t mask[8] = {0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d};
    static const uint8_t le_mask[8] = {0x1f, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t inquiry_mode[] = {2};
    const uint8_t pairing[] = {1};
    const uint8_t le_host[] = {1, 1};
    uint8_t local_name[248] = {0};

    if (name) {
        size_t n = strlen(name);
        if (n > sizeof(local_name) - 1) {
            n = sizeof(local_name) - 1;
        }
        memcpy(local_name, name, n);
    }

    if (command(fd, HCI_SET_EVENT_MASK, mask, 8, ev) ||
        command(fd, HCI_WRITE_INQUIRY_MODE, inquiry_mode, 1, ev) ||
        command(fd, HCI_WRITE_PAIRING, pairing, 1, ev) ||
        command(fd, HCI_WRITE_LE_HOST, le_host, 2, ev) ||
        command(fd, HCI_LE_SET_EVENT_MASK, le_mask, 8, ev) ||
        (name && command(fd, HCI_WRITE_NAME, local_name, sizeof(local_name), ev)) ||
        command(fd, HCI_WRITE_CLASS, cod, 3, ev)) {
        return -1;
    }

    return 0;
}

// Сведения об адаптере и контроллере
static int cmd_info(int fd) {
    struct syn_bthci_info info = {.version = SYN_BTHCI_ABI_VERSION};
    if (!ioctl(fd, SYN_BTHCI_IOCTL_INFO, &info)) {
        printf("usb %04x:%04x speed %u interface %u eps evt=%02x acl=%02x/%02x\n",
               info.vendor, info.product, info.speed, info.interface,
               info.event_endpoint, info.acl_in_endpoint, info.acl_out_endpoint);
    }
    struct event ev;
    uint8_t cod[3] = {0x0c, 0x01, 0x00};
    return host_init(fd, &ev, NULL, cod);
}

// Включить прослушивание входящих подключений и видимость для поиска
static int cmd_up(int fd, const char *name, int speaker, bool discoverable) {
    struct event ev;
    uint8_t cod[3] = {0x0c, 0x01, 0x00};
    if (speaker == 1) {
        // Класс устройства Audio/Video Loudspeaker со службами Rendering и Audio
        cod[0] = 0x18;
        cod[1] = 0x04;
        cod[2] = 0x06;
    } else if (speaker == 2) {
        // Audio/Video Headset: Rendering, Capturing, Audio и Telephony
        cod[0] = 0x04;
        cod[1] = 0x04;
        cod[2] = 0x4e;
    }
    if (host_init(fd, &ev, name, cod)) {
        return -1;
    }
    const uint8_t enable[] = {discoverable ? 0x03 : 0x02};
    if (command(fd, HCI_WRITE_SCAN_ENABLE, enable, 1, &ev)) {
        return -1;
    }
    puts(discoverable ? "adapter is up: inquiry and page scan enabled" :
                        "adapter is up: page scan enabled, not discoverable");
    return 0;
}

// Выключить прослушивание
static int cmd_down(int fd) {
    struct event ev;
    const uint8_t disable[] = {0};
    if (command(fd, HCI_WRITE_SCAN_ENABLE, disable, 1, &ev)) {
        return -1;
    }
    puts("adapter is down");
    return 0;
}

// Только сброс контроллера
static int cmd_reset(int fd) {
    struct event ev;
    return command(fd, HCI_RESET, NULL, 0, &ev);
}

// Пассивное прослушивание объявлений Bluetooth LE
static int cmd_scan(int fd, unsigned seconds) {
    struct event ev;
    const uint8_t config[] = {0, 0x20, 0, 0x20, 0, 0, 0};
    const uint8_t enable[] = {1, 1}, disable[] = {0, 0};
    if (command(fd, HCI_LE_SET_SCAN_PARAMS, config, sizeof(config), &ev) ||
        command(fd, HCI_LE_SET_SCAN_ENABLE, enable, 2, &ev)) {
        return -1;
    }

    int64_t until = milliseconds() + (int64_t)seconds * 1000;
    int failure = 0;

    while (milliseconds() < until) {
        if (event_read(fd, &ev, until)) {
            failure = errno == ETIMEDOUT ? 0 : errno;
            break;
        }
        if (ev.code != 0x3e || ev.len < 2 || ev.data[0] != 2) {
            continue;
        }
        unsigned pos = 2;
        for (unsigned i = 0; i < ev.data[1] && pos + 10 <= ev.len; ++i) {
            uint8_t *report = ev.data + pos;
            unsigned adv_len = report[8];
            if (pos + 10u + adv_len > ev.len) {
                failure = EPROTO;
                break;
            }
            char name[64];
            adv_name(report + 9, adv_len, name, sizeof(name));
            found_add(report + 2, (int)(int8_t)report[9 + adv_len], 0, name);
            pos += 10u + adv_len;
        }
        if (failure) {
            break;
        }
    }

    command(fd, HCI_LE_SET_SCAN_ENABLE, disable, 2, &ev);

    if (failure) {
        errno = failure;
        return -1;
    }

    for (unsigned i = 0; i < found_count; ++i) {
        print_address(found[i].address);
        printf("  %d dBm  %s\n", found[i].rssi, found[i].name[0] ? found[i].name : "?");
    }
    printf("LE devices: %u\n", found_count);
    return 0;
}

// Активный опрос классических устройств BR/EDR
static int cmd_inquiry(int fd, unsigned seconds) {
    struct event ev;
    unsigned units = seconds * 100 / 128;
    if (!units) {
        units = 1;
    }
    if (units > 0x30) {
        units = 0x30;
    }
    const uint8_t inquiry[] = {0x33, 0x8b, 0x9e, (uint8_t)units, 0};
    if (command_status(fd, HCI_INQUIRY, inquiry, sizeof(inquiry), &ev)) {
        return -1;
    }

    int64_t until = milliseconds() + ((int64_t)seconds + 5) * 1000;
    int failure = 0;
    bool complete = false;

    while (!complete && milliseconds() < until) {
        if (event_read(fd, &ev, until)) {
            failure = errno == ETIMEDOUT ? 0 : errno;
            break;
        }
        if (ev.code == 0x01 && ev.len >= 1) {
            complete = true;
        } else if (ev.code == 0x02 && ev.len >= 1) {
            unsigned count = ev.data[0];
            const uint8_t *p = ev.data + 1;
            for (unsigned i = 0; i < count && p + 14 <= ev.data + ev.len; ++i) {
                found_add(p, -60,
                          p[9] | ((uint32_t)p[10] << 8) | ((uint32_t)p[11] << 16), NULL);
                p += 14;
            }
        } else if (ev.code == 0x2f && ev.len >= 15) {
            const uint8_t *p = ev.data + 1;
            char name[64];
            unsigned eir = ev.len - 15;
            if (eir > 240) {
                eir = 240;
            }
            adv_name(p + 14, eir, name, sizeof(name));
            found_add(p, (int)(int8_t)p[13],
                      p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16), name);
        }
    }

    if (failure) {
        errno = failure;
        return -1;
    }
    if (!complete) {
        command(fd, HCI_INQUIRY_CANCEL, NULL, 0, &ev);
    }

    for (unsigned i = 0; i < found_count; ++i) {
        print_address(found[i].address);
        printf("  cod %06x  %d dBm  %s\n", found[i].cod, found[i].rssi,
               found[i].name[0] ? found[i].name : "?");
    }
    printf("BR/EDR devices: %u\n", found_count);
    return 0;
}

static int device_open(const char *path) {
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
    }
    return fd;
}

int main(int argc, char **argv) {
    const char *path = BT_DEVICE;
    const char *name = "SynapseOS";
    const char *cmd = NULL;
    unsigned seconds = 8;
    int speaker = 0;
    bool discoverable = true;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--dev") && i + 1 < argc) {
            path = argv[++i];
        } else if (!strcmp(argv[i], "--name") && i + 1 < argc) {
            name = argv[++i];
        } else if (!strcmp(argv[i], "--speaker")) {
            speaker = 1;
        } else if (!strcmp(argv[i], "--headset")) {
            speaker = 2;
        } else if (!strcmp(argv[i], "--connect-only")) {
            discoverable = false;
        } else if (!strcmp(argv[i], "--time") && i + 1 < argc) {
            seconds = (unsigned)strtoul(argv[++i], NULL, 10);
            if (!seconds || seconds > 60) {
                seconds = 8;
            }
        } else if (!cmd) {
            cmd = argv[i];
        }
    }

    if (!cmd) {
        fputs("Usage: btctl [--dev PATH] info | "
              "up [--name NAME] [--speaker|--headset] [--connect-only] | "
              "down | reset | scan [--time SEC] | inquiry [--time SEC]\n", stderr);
        return 2;
    }

    int fd = device_open(path);
    if (fd < 0) {
        return 1;
    }

    int result;
    if (!strcmp(cmd, "info")) {
        result = cmd_info(fd);
    } else if (!strcmp(cmd, "up")) {
        result = cmd_up(fd, name, speaker, discoverable);
    } else if (!strcmp(cmd, "down")) {
        result = cmd_down(fd);
    } else if (!strcmp(cmd, "reset")) {
        result = cmd_reset(fd);
    } else if (!strcmp(cmd, "scan")) {
        result = cmd_scan(fd, seconds);
    } else if (!strcmp(cmd, "inquiry")) {
        result = cmd_inquiry(fd, seconds);
    } else {
        fprintf(stderr, "btctl: unknown command %s\n", cmd);
        result = -1;
        errno = EINVAL;
    }

    if (result) {
        perror("btctl");
    }
    close(fd);
    return result ? 1 : 0;
}
