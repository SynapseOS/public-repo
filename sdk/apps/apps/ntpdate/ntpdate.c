// ntpdate.c: Установка времени по NTP
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Запрос NTP и выбор эпохи счётчика секунд
// Подсистема: пользовательские системные утилиты
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NTP_EPOCH INT64_C(2208988800)
#define ERA INT64_C(4294967296)
#define US INT64_C(1000000)

static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put32(unsigned char *p, uint32_t n) {
    p[0] = n >> 24;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
}
static int64_t clock_us(clockid_t id) {
    struct timespec ts;
    if (clock_gettime(id, &ts)) {
        return -1;
    }
    return (int64_t)ts.tv_sec * US + ts.tv_nsec / 1000;
}
// Счётчик секунд NTP переполняется в 2036 году
// Выбираем эпоху рядом с локальной датой, а при пустом RTC используем 2026 год
static int64_t decode_time(const unsigned char *p, int64_t reference) {
    int64_t pivot = reference / US;
    if (pivot < INT64_C(1577836800)) {
        pivot = INT64_C(1767225600);
    }
    pivot += NTP_EPOCH;
    int64_t sec = (pivot / ERA) * ERA + get32(p);
    if (sec - pivot > ERA / 2) {
        sec -= ERA;
    }
    if (pivot - sec > ERA / 2) {
        sec += ERA;
    }
    return (sec - NTP_EPOCH) * US + ((uint64_t)get32(p + 4) * US >> 32);
}
static int same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_family == b->sin_family && a->sin_port == b->sin_port &&
           a->sin_addr.s_addr == b->sin_addr.s_addr;
}
static int query(const struct sockaddr_in *peer, int seconds, int64_t *offset,
                 int64_t *delay, int64_t *server_time, int *stratum) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (fd >= FD_SETSIZE) {
        close(fd);
        errno = EMFILE;
        return -1;
    }
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }

    unsigned char request[48] = {0}, reply[513];
    int64_t t1 = clock_us(CLOCK_REALTIME), start = clock_us(CLOCK_MONOTONIC);
    if (t1 < 0 || start < 0) {
        close(fd);
        return -1;
    }
    request[0] = (4 << 3) | 3;
    put32(request + 40, (uint32_t)(t1 / US + NTP_EPOCH));
    uint32_t fraction = (uint32_t)(((uint64_t)(t1 % US) << 32) / US), nonce = 0;
    int randomfd = open("/dev/urandom", O_RDONLY);
    if (randomfd >= 0) {
        if (read(randomfd, &nonce, sizeof nonce) == sizeof nonce) {
            fraction = (fraction & 0xffff0000U) | (nonce & 0xffffU);
        }
        close(randomfd);
    }
    put32(request + 44, fraction);
    if (sendto(fd, request, sizeof request, 0, (const struct sockaddr *)peer,
               sizeof *peer) != 48) {
        close(fd);
        return -1;
    }
    int64_t deadline = start + (int64_t)seconds * US;
    for (;;) {
        int64_t now = clock_us(CLOCK_MONOTONIC);
        if (now < 0) {
            break;
        }
        int64_t left = deadline - now;
        if (left <= 0) {
            errno = ETIMEDOUT;
            break;
        }

        struct timeval tv = {left / US, left % US};
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        int n = select(fd + 1, &rd, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (!n) {
            errno = ETIMEDOUT;
            break;
        }

        struct sockaddr_in sender = {0};
        socklen_t length = sizeof sender;
        ssize_t size =
            recvfrom(fd, reply, sizeof reply, 0, (struct sockaddr *)&sender, &length);
        int64_t t4 = clock_us(CLOCK_REALTIME), finish = clock_us(CLOCK_MONOTONIC);
        if (size < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            break;
        }
        if (length != sizeof sender || !same_peer(peer, &sender) || size < 48 ||
            size > 512 || size % 4) {
            continue;
        }

        int version = (reply[0] >> 3) & 7;
        if ((reply[0] & 7) != 4 || (version != 3 && version != 4) ||
            (reply[0] >> 6) == 3 || reply[1] > 15 ||
            memcmp(reply + 24, request + 40, 8)) {
            continue;
        }
        if (!reply[1]) {
            fprintf(stderr, "ntpdate: server refused request (%.4s)\n", reply + 12);
            errno = ECONNREFUSED;
            break;
        }
        if ((!get32(reply + 32) && !get32(reply + 36)) ||
            (!get32(reply + 40) && !get32(reply + 44))) {
            continue;
        }
        if (t4 < 0 || finish < 0 || llabs((t4 - t1) - (finish - start)) > 500000) {
            errno = EAGAIN;
            break;
        }
        int64_t t2 = decode_time(reply + 32, t1), t3 = decode_time(reply + 40, t1);
        if (t3 < t2 || t3 - t2 > 10 * US) {
            continue;
        }
        *delay = (t4 - t1) - (t3 - t2);
        if (*delay < -1000 || *delay > (int64_t)seconds * US) {
            continue;
        }
        *offset = ((t2 - t1) + (t3 - t4)) / 2;
        *server_time = t3;
        *stratum = reply[1];
        close(fd);
        return 0;
    }

    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}
static void usage(void) {
    puts("Usage: ntpdate [-q|-s] [-t SECONDS] [-p PORT] [SERVER]\n"
         "  -q query only (default); -s set system clock\n"
         "  Default: pool.ntp.org, UDP 123, timeout 5 seconds");
}
int main(int argc, char **argv) {
    int set = 0, timeout = 5, opt;
    const char *port = "123";
    while ((opt = getopt(argc, argv, "qst:p:h")) != -1) {
        switch (opt) {
        case 'q':
            set = 0;
            break;
        case 's':
            set = 1;
            break;
        case 't': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 1 || v > 60) {
                usage();
                return 2;
            }
            timeout = v;
            break;
        }
        case 'p': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 1 || v > 65535) {
                usage();
                return 2;
            }
            port = optarg;
            break;
        }
        case 'h':
            usage();
            return 0;
        default:
            usage();
            return 2;
        }
    }
    if (argc - optind > 1) {
        usage();
        return 2;
    }
    const char *host = optind < argc ? argv[optind] : "pool.ntp.org";
    struct addrinfo hints = {0}, *addresses;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int error = getaddrinfo(host, port, &hints, &addresses);
    if (error) {
        fprintf(stderr, "ntpdate: %s: %s\n", host, gai_strerror(error));
        return 1;
    }
    int64_t offset = 0, delay = 0, server = 0;
    int stratum = 0, success = 0, count = 0, saved = ETIMEDOUT;
    for (struct addrinfo *a = addresses; a && count < 4; a = a->ai_next, count++) {
        if (!query((const struct sockaddr_in *)a->ai_addr, timeout, &offset, &delay,
                   &server, &stratum)) {
            success = 1;
            break;
        }
        saved = errno;
    }
    freeaddrinfo(addresses);
    if (!success) {
        fprintf(stderr, "ntpdate: %s: %s\n", host, strerror(saved));
        return 1;
    }
    if (set) {
        int64_t now = clock_us(CLOCK_REALTIME);
        if (now < 0) {
            perror("ntpdate: read clock");
            return 1;
        }
        int64_t target = now + offset;
        struct timespec ts = {target / US, (target % US) * 1000};
        if (ts.tv_nsec < 0) {
            ts.tv_sec--;
            ts.tv_nsec += 1000000000;
        }
        if (clock_settime(CLOCK_REALTIME, &ts)) {
            perror("ntpdate: set clock");
            return 1;
        }
    }
    printf("%s: stratum %d, offset %+.6f s, delay %.6f s%s\n", host, stratum,
           (double)offset / US, (double)delay / US, set ? ", clock updated" : "");
    return 0;
}
