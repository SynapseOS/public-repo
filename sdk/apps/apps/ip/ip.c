// ip.c: Адреса интерфейсов и подключённые маршруты
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Адреса получаем через SynAPI, сведения о маршрутах : из procfs
// Подсистема: пользовательские сетевые утилиты
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <arpa/inet.h>
// Номера SynAPI для запросов о сетевых интерфейсах
#define NET_COUNT 106000
#define NET_NAME 106001
#define NET_IPV4 106002
#define NET_IPV6 106003
#define NET_LINK 106004
#define NET_MAC 106009
static int print_routes(void) {
    FILE *f = fopen("/proc/net-interfaces", "r");
    if (!f) {
        perror("ip: route metadata");
        return 1;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int index, up;
        char ip[16], mask[16], gateway[16], extra, name[64] = {0};
        struct in_addr a, m, g;
        if (sscanf(line, "%d %15s %15s %15s %d %c", &index, ip, mask, gateway, &up,
                   &extra) != 5 ||
            index < 0 || up != 1 || inet_pton(AF_INET, ip, &a) != 1 || !a.s_addr ||
            inet_pton(AF_INET, mask, &m) != 1 || inet_pton(AF_INET, gateway, &g) != 1) {
            continue;
        }
        uint32_t bits = ntohl(m.s_addr), inverse = ~bits;
        if (!bits || (inverse & (inverse + 1))) {
            continue;
        }
        unsigned prefix = 0;
        for (uint32_t v = bits; v; v <<= 1) {
            prefix++;
        }
        if (syscall(NET_NAME, index, name, sizeof(name)) < 0) {
            continue;
        }
        struct in_addr network = {a.s_addr & m.s_addr};
        char text[16];
        if (!inet_ntop(AF_INET, &network, text, sizeof(text))) {
            continue;
        }
        printf("%s/%u dev %s src %s\n", text, prefix, name, ip);
        if (g.s_addr) {
            printf("gateway %s dev %s\n", gateway, name);
        }
    }
    fclose(f);
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "route")) {
        return print_routes();
    }
    if (argc == 6 && !strcmp(argv[1], "set")) {
        int index = -1;
        long count = syscall(NET_COUNT);
        for (int i = 0; i < count; i++) {
            char name[64] = {0};
            syscall(NET_NAME, i, name, sizeof(name));
            if (!strcmp(name, argv[2])) {
                index = i;
            }
        }
        struct in_addr address[3];
        if (index < 0 || inet_pton(AF_INET, argv[3], &address[0]) != 1 ||
            inet_pton(AF_INET, argv[4], &address[1]) != 1 ||
            inet_pton(AF_INET, argv[5], &address[2]) != 1) {
            fprintf(stderr, "ip: invalid interface, IPv4, netmask or gateway\n");
            return 1;
        }
        if (syscall(106024, index, address) < 0) {
            perror("ip: configuration");
            return 1;
        }
        puts("IPv4 configuration queued.");
        return 0;
    }
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "addr") && strcmp(argv[1], "a"))) {
        fprintf(
            stderr,
            "usage: ip [addr|a|route]\n       ip set INTERFACE IP NETMASK GATEWAY\n");
        return 1;
    }
    long count = syscall(NET_COUNT);
    if (count < 0) {
        perror("ip: network discovery");
        return 1;
    }
    if (!count) {
        puts("No network interfaces.");
    }
    for (int i = 0; i < count; i++) {
        char name[64] = {0}, ipv4[16] = {0}, ipv6[64] = {0};
        uint8_t mac[6] = {0};
        if (syscall(NET_NAME, i, name, sizeof(name)) < 0 ||
            syscall(NET_IPV4, i, ipv4, sizeof(ipv4)) < 0) {
            perror("ip: interface");
            return 1;
        }
        syscall(NET_IPV6, i, ipv6, sizeof(ipv6));
        syscall(NET_MAC, i, mac, sizeof(mac));
        printf("%s: %s\n  inet %s%s\n  link/ether %02x:%02x:%02x:%02x:%02x:%02x\n",
               name, syscall(NET_LINK, i) == 2 ? "UP" : "DOWN", ipv4,
               !strcmp(ipv4, "0.0.0.0") ? " (no IPv4 address yet)" : "", mac[0], mac[1],
               mac[2], mac[3], mac[4], mac[5]);
        if (ipv6[0] && strcmp(ipv6, "::")) {
            printf("  inet6 %s\n", ipv6);
        }
    }
    return 0;
}
