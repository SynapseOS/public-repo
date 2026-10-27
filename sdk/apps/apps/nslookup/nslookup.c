// nslookup.c: Запросы к DNS-серверу
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Чтение resolv.conf, проверка DNS-сообщений и вывод ответов
// Подсистема: пользовательские системные утилиты
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DNS_SIZE 4096
#define SERVERS 8
#define ANSWERS 64
struct answer {
    char name[256];
    char ip[INET6_ADDRSTRLEN];
};
static unsigned get16(const unsigned char *p) {
    return ((unsigned)p[0] << 8) | p[1];
}
static void put16(unsigned char *p, unsigned n) {
    p[0] = n >> 8;
    p[1] = n;
}
static long long now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        perror("nslookup: monotonic clock");
        exit(1);
    }
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
// Возвращаем число байтов в исходной записи, включая указатель сжатия
static int name_read(const unsigned char *p, size_t len, size_t at, char out[256],
                     size_t *consumed) {
    size_t start = at, used = 0;
    unsigned jumps = 0;
    int jumped = 0;
    for (;;) {
        if (at >= len) {
            return -1;
        }

        unsigned c = p[at++];
        if ((c & 0xc0) == 0xc0) {
            if (at >= len || ++jumps > 128) {
                return -1;
            }

            unsigned target = ((c & 63) << 8) | p[at++];
            if (target >= at - 2) {
                return -1;
            }
            if (!jumped) {
                *consumed = at - start;
            }
            jumped = 1;
            at = target;
            continue;
        }
        if (c & 0xc0) {
            return -1;
        }
        if (!c) {
            break;
        }
        if (at + c > len || used + c + (used != 0) > 253) {
            return -1;
        }
        if (used) {
            out[used++] = '.';
        }

        for (unsigned i = 0; i < c; i++) {
            unsigned ch = p[at++];
            if (ch < 33 || ch > 126 || ch == '.') {
                return -1;
            }
            out[used++] = (char)ch;
        }
    }
    out[used] = 0;
    if (!jumped) {
        *consumed = at - start;
    }
    return 0;
}
static int name_write(unsigned char *p, const char *name) {
    size_t n = strlen(name), used = 0;
    if (!n || n > 253) {
        return -1;
    }
    const char *label = name;
    while (*label) {
        const char *dot = strchr(label, '.');
        size_t count = dot ? (size_t)(dot - label) : strlen(label);
        if (!count || count > 63) {
            return -1;
        }
        p[used++] = (unsigned char)count;
        for (size_t i = 0; i < count; i++) {
            unsigned ch = (unsigned char)label[i];
            if (ch < 33 || ch > 126) {
                return -1;
            }
            p[used++] = (unsigned char)tolower(ch);
        }
        if (!dot) {
            break;
        }
        label = dot + 1;
    }
    p[used++] = 0;
    return (int)used;
}
// Проверяем всё сообщение до вывода ответов
static int response(const unsigned char *p, size_t len, unsigned id, const char *name,
                    unsigned type, struct answer *out, unsigned *count) {
    char owner[256];
    size_t used, at = 12;
    if (len < 12 || get16(p) != id || !(p[2] & 0x80) || (p[2] & 0x78) ||
        get16(p + 4) != 1) {
        return -1;
    }
    if (name_read(p, len, at, owner, &used)) {
        return -1;
    }
    at += used;
    if (at + 4 > len || strcasecmp(owner, name) || get16(p + at) != type ||
        get16(p + at + 2) != 1) {
        return -1;
    }
    at += 4;
    if (p[2] & 2) {
        return -2;
    }

    unsigned answers = get16(p + 6);
    unsigned records = answers + get16(p + 8) + get16(p + 10);
    *count = 0;
    for (unsigned i = 0; i < records; i++) {
        if (name_read(p, len, at, owner, &used)) {
            return -1;
        }
        at += used;
        if (at + 10 > len) {
            return -1;
        }

        unsigned kind = get16(p + at), cls = get16(p + at + 2),
                 bytes = get16(p + at + 8);
        at += 10;
        if (at + bytes > len) {
            return -1;
        }
        if ((kind == 1 && bytes != 4) || (kind == 28 && bytes != 16)) {
            return -1;
        }
        if (kind == 5 || kind == 2 || kind == 12) {
            char ignored[256];
            if (name_read(p, len, at, ignored, &used) || used != bytes) {
                return -1;
            }
        }
        if (i < answers && kind == type && cls == 1 && *count < ANSWERS) {
            struct answer *a = &out[(*count)++];
            strcpy(a->name, owner);
            if (!inet_ntop(kind == 1 ? AF_INET : AF_INET6, p + at, a->ip,
                           sizeof(a->ip))) {
                return -1;
            }
        }
        at += bytes;
    }
    if (at != len) {
        return -1;
    }
    return (int)(p[3] & 15);
}
static int query(const char *server, unsigned port, const char *name, unsigned type,
                 unsigned seconds, struct answer *out, unsigned *count) {
    struct sockaddr_storage storage = {0};
    struct sockaddr_in *v4 = (struct sockaddr_in *)&storage;
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&storage;
    socklen_t addrlen;
    int family;
    if (inet_pton(AF_INET, server, &v4->sin_addr) == 1) {
        family = AF_INET;
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        addrlen = sizeof(*v4);
    } else if (inet_pton(AF_INET6, server, &v6->sin6_addr) == 1) {
        family = AF_INET6;
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        addrlen = sizeof(*v6);
    } else {
        fprintf(stderr, "nslookup: DNS server must be a numeric IP address\n");
        return -3;
    }

    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&storage, addrlen)) {
        perror("nslookup: socket/connect");
        if (fd >= 0) {
            close(fd);
        }
        return -3;
    }

    unsigned char request[512] = {0}, reply[DNS_SIZE];
    static unsigned serial;
    uint16_t id = (uint16_t)(now_ms() ^ getpid() ^ ++serial);
    int random = open("/dev/urandom", O_RDONLY);
    if (random >= 0) {
        if (read(random, &id, sizeof(id)) != sizeof(id)) {
            id ^= serial;
        }
        close(random);
    }
    put16(request, id);
    request[2] = 1;
    request[5] = 1;
    int bytes = name_write(request + 12, name);
    if (bytes < 0) {
        close(fd);
        return -3;
    }
    put16(request + 12 + bytes, type);
    put16(request + 14 + bytes, 1);
    if (send(fd, request, 16 + bytes, 0) != 16 + bytes) {
        perror("nslookup: send");
        close(fd);
        return -3;
    }
    long long deadline = now_ms() + seconds * 1000;
    int malformed = 0;
    for (;;) {
        long long left = deadline - now_ms();
        if (left <= 0) {
            break;
        }
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        struct timeval tv = {left / 1000, (left % 1000) * 1000};
        int ready = select(fd + 1, &set, NULL, NULL, &tv);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            perror("nslookup: select");
            close(fd);
            return -3;
        }
        if (!ready) {
            break;
        }
        ssize_t n = recv(fd, reply, sizeof(reply), 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            perror("nslookup: recv");
            close(fd);
            return -3;
        }
        if (n >= 2 && get16(reply) != id) {
            continue;
        }

        int result = response(reply, (size_t)n, id, name, type, out, count);
        if (result != -1) {
            close(fd);
            return result;
        }
        malformed = 1;
    }
    close(fd);
    fprintf(stderr, "nslookup: %s: %s\n", server,
            malformed ? "malformed DNS response (deadline reached)" : "DNS timeout");
    return -3;
}
static unsigned number(const char *s, unsigned maximum) {
    char *end;
    unsigned long n = strtoul(s, &end, 10);
    if (!*s || *end || !n || n > maximum) {
        return 0;
    }
    return (unsigned)n;
}
int main(int argc, char **argv) {
    const char *name = NULL, *explicit_server = NULL;
    unsigned timeout = 2, port = 53, type = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            puts("usage: nslookup [-type=A|AAAA] [-timeout=SECONDS] [-port=PORT] NAME "
                 "[DNS_SERVER]");
            return 0;
        }
        if (!strncmp(argv[i], "-type=", 6)) {
            if (!strcmp(argv[i] + 6, "A")) {
                type = 1;
            } else if (!strcmp(argv[i] + 6, "AAAA")) {
                type = 28;
            } else {
                goto usage;
            }
        } else if (!strncmp(argv[i], "-timeout=", 9)) {
            if (!(timeout = number(argv[i] + 9, 30))) {
                goto usage;
            }
        } else if (!strncmp(argv[i], "-port=", 6)) {
            if (!(port = number(argv[i] + 6, 65535))) {
                goto usage;
            }
        } else if (argv[i][0] == '-') {
            goto usage;
        } else if (!name) {
            name = argv[i];
        } else if (!explicit_server) {
            explicit_server = argv[i];
        } else {
            goto usage;
        }
    }
    if (!name) {
        goto usage;
    }

    char normalized[256];
    if (strlen(name) >= sizeof(normalized)) {
        goto usage;
    }
    strcpy(normalized, name);
    size_t length = strlen(normalized);
    if (length && normalized[length - 1] == '.') {
        normalized[--length] = 0;
    }

    unsigned char scratch[256];
    if (name_write(scratch, normalized) < 0) {
        goto usage;
    }

    char servers[SERVERS][INET6_ADDRSTRLEN];
    unsigned nservers = 0;
    if (explicit_server) {
        if (strlen(explicit_server) >= sizeof(servers[0])) {
            goto usage;
        }
        strcpy(servers[nservers++], explicit_server);
    } else {
        FILE *f = fopen("/etc/resolv.conf", "r");
        if (!f) {
            perror("nslookup: /etc/resolv.conf");
            return 1;
        }

        char line[512], key[64], value[INET6_ADDRSTRLEN];
        while (nservers < SERVERS && fgets(line, sizeof(line), f)) {
            if (sscanf(line, "%63s %45s", key, value) == 2 &&
                !strcmp(key, "nameserver")) {
                strcpy(servers[nservers++], value);
            }
        }
        fclose(f);
    }
    if (!nservers) {
        fputs("nslookup: no nameserver in /etc/resolv.conf\n", stderr);
        return 1;
    }

    unsigned types[2] = {1, 28}, found = 0;
    int failed = 0;
    if (type) {
        types[0] = type;
    }

    for (unsigned t = 0; t < (type ? 1 : 2); t++) {
        int result = -3;
        struct answer answers[ANSWERS];
        unsigned count = 0, s;
        for (s = 0; s < nservers; s++) {
            result =
                query(servers[s], port, normalized, types[t], timeout, answers, &count);
            if (result >= 0 || result == -2) {
                break;
            }
        }
        if (result == -2) {
            fputs("nslookup: truncated UDP response; TCP DNS is not supported\n",
                  stderr);
            failed = 1;
            continue;
        }
        if (result < 0) {
            failed = 1;
            continue;
        }
        if (result) {
            fprintf(stderr, "nslookup: %s: DNS %s (RCODE %d)\n", normalized,
                    result == 3   ? "NXDOMAIN"
                    : result == 2 ? "SERVFAIL"
                    : result == 5 ? "REFUSED"
                                  : "error",
                    result);
            failed = 1;
            if (result == 3) {
                break;
            }
            continue;
        }
        printf("Server: %s\n", servers[s]);
        for (unsigned i = 0; i < count; i++) {
            printf("%s\t%s\n", answers[i].name, answers[i].ip);
        }
        found += count;
    }
    if (!found && !failed) {
        fprintf(stderr, "nslookup: %s: no A/AAAA records\n", normalized);
    }
    return failed || !found;
usage:
    fputs("usage: nslookup [-type=A|AAAA] [-timeout=SECONDS] [-port=PORT] NAME "
          "[DNS_SERVER]\n",
          stderr);
    return 2;
}
