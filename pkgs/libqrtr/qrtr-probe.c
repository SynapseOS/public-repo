// qrtr-probe.c: Проверка BSD libqrtr через стандартные socket вызовы
// Автор: Елчинян Арен Артурович 2026
// Архитектура: AArch64 и x86_64, ABI: Linux AF42 и POSIX
// Описание: Только локальный roundtrip, без публикации служб или запросов модему
// Подсистема: телефонные пакеты
// Первое изменение: 25.11.2026
// Последнее изменение: 25.11.2026
// Доступ: общедоступный
#define _GNU_SOURCE
#include <libqrtr.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
_Static_assert(sizeof(struct sockaddr_qrtr) == 12, "Linux sockaddr_qrtr");
_Static_assert(sizeof(struct qrtr_ctrl_pkt) == 20, "Linux qrtr_ctrl_pkt");

// Открытие AF42 может забрать sticky endpoint lease, запускать только в подходящем boot профиле
int main(void) {
    int first = qrtr_open(0);
    if (first < 0) {
        perror("qrtr-probe: AF42 unavailable");
        return 1;
    }
    int second = qrtr_open(0);
    if (second < 0) {
        close(first);
        return 1;
    }
    struct sockaddr_qrtr a = {0}, b = {0}, source = {0};
    socklen_t alen = sizeof(a), blen = sizeof(b), slen = sizeof(source);
    int result = 1;
    if (getsockname(first, (void *)&a, &alen) || getsockname(second, (void *)&b, &blen) ||
        alen != sizeof(a) || blen != sizeof(b) || a.sq_family != AF_QIPCRTR || b.sq_family != AF_QIPCRTR ||
        a.sq_node != b.sq_node || !a.sq_port || !b.sq_port || a.sq_port == b.sq_port) {
        fputs("qrtr-probe: invalid local sockaddr\n", stderr);
        goto done;
    }
    static const unsigned char message[] = {0x51, 0x52, 0x54, 0x52, 0x00, 0x7f};
    if (qrtr_sendto(first, b.sq_node, b.sq_port, message, sizeof(message)) != 0 ||
        qrtr_poll(second, 1000) <= 0) {
        perror("qrtr-probe: local send/poll");
        goto done;
    }
    unsigned char reply[sizeof(message)] = {0};
    if (recvfrom(second, reply, 2, MSG_PEEK | MSG_TRUNC, (void *)&source, &slen) != sizeof(message) ||
        slen != sizeof(source) || source.sq_node != a.sq_node || source.sq_port != a.sq_port) {
        perror("qrtr-probe: PEEK/TRUNC");
        goto done;
    }
    uint32_t node = 0, port = 0;
    if (qrtr_recvfrom(second, reply, sizeof(reply), &node, &port) != sizeof(message) ||
        memcmp(reply, message, sizeof(message)) || node != a.sq_node || port != a.sq_port) {
        fputs("qrtr-probe: datagram mismatch\n", stderr);
        goto done;
    }
    if (recvfrom(second, reply, sizeof(reply), MSG_DONTWAIT, NULL, NULL) != -1 || errno != EAGAIN) {
        fputs("qrtr-probe: empty nonblocking queue mismatch\n", stderr);
        goto done;
    }
    printf("LIBQRTR_LOCAL_PASS node=%u datagram_bytes=%zu remote_queries=0\n", a.sq_node, sizeof(message));
    result = 0;
done:
    qrtr_close(second);
    qrtr_close(first);
    return result;
}
