// ping.c: Запросы ICMP Echo
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, AArch64, ABI: Linux (ELF64)
//
// Подсистема: Запросы ICMP Echo в пользовательском пространстве
// Первое изменение в истории Git: 09.10.2026
// Последнее изменение: 21.11.2026
// Доступ: общедоступный

// ICMP Echo для SynapseOS на musl
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>

static volatile int running = 1;

void sigint_handler(int sig) {
    (void)sig;
    running = 0;
}

struct icmp_echo {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
    struct timeval timestamp;
    uint8_t data[];
};

static uint16_t icmp_checksum(const void *buf, int len) {
    const uint16_t *ptr = (const uint16_t *)buf;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len == 1) {
        sum += *(const uint8_t *)ptr;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);

    return (uint16_t)(~sum);
}

static double time_diff_ms(struct timeval *t1, struct timeval *t2) {
    double sec = (double)(t2->tv_sec - t1->tv_sec);
    double usec = (double)(t2->tv_usec - t1->tv_usec);
    return (sec * 1000.0) + (usec / 1000.0);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: ping <host> [count]\n");
        return 1;
    }

    signal(SIGINT, sigint_handler);

    const char *target = argv[1];
    int count = (argc > 2) ? atoi(argv[2]) : 10;
    if (count <= 0) {
        count = 10;
    }

    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *addresses;
    int error = getaddrinfo(target, NULL, &hints, &addresses);
    if (error) {
        fprintf(stderr, "ping: %s: %s\n", target, error == EAI_SYSTEM ? strerror(errno) : gai_strerror(error));
        return 1;
    }
    struct sockaddr_in dst = *(struct sockaddr_in *)addresses->ai_addr;
    freeaddrinfo(addresses);

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    int datagram = fd >= 0;
    if (!datagram) {
        fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    }
    if (fd < 0) {
        perror("ping: socket");
        return 1;
    }

    int seq = 0;
    int sent = 0, received = 0;
    double total_rtt = 0.0;
    double min_rtt = 1e9, max_rtt = 0.0;

    printf("PING %s: 56 data bytes\n", target);

    for (int i = 0; i < count && running; i++) {
        seq++;
        sent++;

        _Alignas(struct icmp_echo) uint8_t pkt[64];
        memset(pkt, 0, sizeof(pkt));

        struct icmp_echo *icmp = (struct icmp_echo *)pkt;
        icmp->type = 8;
        icmp->code = 0;
        icmp->checksum = 0;
        icmp->id = (uint16_t)(getpid() & 0xFFFF);
        icmp->seq = htons((uint16_t)seq);

        gettimeofday(&icmp->timestamp, NULL);

        for (int j = 0; j < 40; j++) {
            icmp->data[j] = (uint8_t)(j + 'A');
        }

        icmp->checksum = icmp_checksum(pkt, sizeof(pkt));

        ssize_t sent_bytes =
            sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst));
        if (sent_bytes < 0) {
            perror("ping: sendto");
            close(fd);
            return 1;
        }

        uint8_t rbuf[1500];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int got_reply = 0;


        for (int attempt = 0; attempt < 200 && running; attempt++) {
            ssize_t n = recvfrom(fd, rbuf, sizeof(rbuf), MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &fromlen);

            if (n > 0) {
                uint8_t *icmp_start = rbuf;

                if (!datagram && (rbuf[0] >> 4) == 4) {
                    int ihl = (rbuf[0] & 0x0F) * 4;
                    if (ihl < 20 || ihl > n) {
                        continue;
                    }
                    icmp_start = rbuf + ihl;
                    n -= ihl;
                }

                if (n >= (ssize_t)sizeof(struct icmp_echo)) {
                    struct icmp_echo reply;
                    memcpy(&reply, icmp_start, sizeof(reply));
                    if (from.sin_addr.s_addr == dst.sin_addr.s_addr && reply.type == 0 &&
                        reply.code == 0 && reply.seq == htons((uint16_t)seq) &&
                        (datagram || reply.id == (uint16_t)(getpid() & 0xFFFF))) {
                        struct timeval now;
                        gettimeofday(&now, NULL);

                        double rtt = time_diff_ms(&reply.timestamp, &now);

                        printf("%d bytes from %s: icmp_seq=%d time=%.3f ms\n",
                               (int)n, inet_ntoa(from.sin_addr), seq, rtt);

                        received++;
                        total_rtt += rtt;
                        if (rtt < min_rtt) {
                            min_rtt = rtt;
                        }
                        if (rtt > max_rtt) {
                            max_rtt = rtt;
                        }

                        got_reply = 1;
                        break;
                    }
                }
            }

            usleep(10000);
        }

        if (!got_reply && running) {
            printf("Request timeout for icmp_seq %d\n", seq);
        }

        if (i < count - 1 && running) {
            sleep(1);
        }
    }

    close(fd);

    printf("\n--- %s ping statistics ---\n", target);
    printf("%d packets transmitted, %d received, %.1f%% packet loss\n", sent, received,
           sent > 0 ? (100.0 * (sent - received) / sent) : 0.0);

    if (received > 0) {
        printf("rtt min/avg/max = %.3f/%.3f/%.3f ms\n", min_rtt, total_rtt / received,
               max_rtt);
    }

    return received > 0 ? 0 : 1;
}
