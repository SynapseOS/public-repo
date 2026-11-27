// cloud-init.c: Начальная настройка облачной системы
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Ограниченный набор cloud-config и получение метаданных OpenStack
// Подсистема: установка и начальная настройка системы
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 25.11.2026
// Доступ: общедоступный

// Поддерживается ограниченный набор cloud-config для OpenStack:
// hostname, ssh_authorized_keys, root/password, users(root),
// write_files (path/content/permissions/owner/encoding b64),
// runcmd (строки через sh -c и списки аргументов)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <time.h>
#include <crypt.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/wait.h>
#include "../qemu-ga/json.h"
#include "../common/service_log.h"
#define LIMIT 262144
#define MAX_WRITE_FILES 32
#define MAX_FILE_BYTES (64 * 1024)
#define MAX_RUNCMD 64
#define MAX_RUNCMD_TEXT 4096
#define MAX_NET_IFACES 8
#define MAX_NET_ROUTES 16
static char hostname[256], instance[256], password[256], password_hash[256];
static char keys[32768], network[32768], dns[2048];
static const char *test_mac;
static int failed;
static int nocloud;

// Отложенные действия user-data, применяемые один раз на отпечаток данных
struct write_file_entry {
    char path[512];
    char *content;
    size_t length;
    long permissions;
    int binary;
};
static struct write_file_entry wf_entries[MAX_WRITE_FILES];
static int wf_count;
static char *runcmd[MAX_RUNCMD];
static int runcmd_count;

// Сетевые интерфейсы и маршруты из network_data.json
struct net_iface_cfg {
    int index;
    char ip[64];
    char mask[64];
    char gw[64];
};
struct net_route_cfg {
    char dest[32];
    int prefix;
    char gw[64];
    int index;
    unsigned metric;
    int has_gw;
};
static struct net_iface_cfg net_ifaces[MAX_NET_IFACES];
static int net_iface_count;
static struct net_route_cfg net_routes[MAX_NET_ROUTES];
static int net_route_count;

static int write_file(const char *path, const char *data, mode_t mode);
static long long milliseconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int simple(const char *s) {
    return *s && !strpbrk(s, "\r\n[]=;");
}
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) {
        s++;
    }

    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) {
        s[--n] = 0;
    }
    if (n >= 2 &&
        ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        s[n - 1] = 0;
        s++;
    }
    return s;
}
static int append(char *out, size_t cap, const char *text) {
    size_t n = strlen(out), m = strlen(text);
    if (n + m >= cap) {
        failed = 1;
        return -1;
    }
    memcpy(out + n, text, m + 1);
    return 0;
}
static size_t key_identity_length(const char *s) {
    const char *end = s;
    for (int field = 0; field < 2; field++) {
        while (*end && *end != ' ' && *end != '\t' && *end != '\n') {
            end++;
        }
        if (field == 0) {
            while (*end == ' ' || *end == '\t') {
                end++;
            }
        }
    }
    return (size_t)(end - s);
}
static int contains_key(const char *list, const char *key) {
    size_t identity = key_identity_length(key);
    for (const char *line = list; *line;) {
        const char *end = strchr(line, '\n');
        if (!end) {
            end = line + strlen(line);
        }
        if (key_identity_length(line) == identity && !strncmp(line, key, identity)) {
            return 1;
        }
        line = *end ? end + 1 : end;
    }
    return 0;
}
static void add_key(const char *key) {
    if (strpbrk(key, "\r\n")) {
        char *copy = strdup(key);
        if (copy == NULL) {
            failed = 1;
            return;
        }

        char *save;
        for (char *p = strtok_r(copy, "\r\n", &save); p;
             p = strtok_r(NULL, "\r\n", &save)) {
            add_key(p);
        }
        free(copy);
        return;
    }
    if (strncmp(key, "ssh-ed25519 ", 12) && strncmp(key, "ssh-rsa ", 8) &&
        strncmp(key, "ecdsa-sha2-", 11)) {
        return;
    }
    if (contains_key(keys, key)) {
        return;
    }
    if (append(keys, sizeof(keys), key) == 0) {
        append(keys, sizeof(keys), "\n");
    }
}
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }

    char *b = malloc(LIMIT + 1);
    if (!b) {
        fclose(f);
        return NULL;
    }

    size_t n = fread(b, 1, LIMIT, f);
    int bad = ferror(f) || fgetc(f) != EOF;
    fclose(f);
    if (bad) {
        free(b);
        return NULL;
    }
    b[n] = 0;
    return b;
}
static int wait_socket(int fd, short events, long long deadline) {
    while (milliseconds() < deadline) {
        struct pollfd p = {fd, events, 0};
        int n = poll(&p, 1, (int)(deadline - milliseconds()));
        if (n > 0) {
            return (p.revents & (events | POLLHUP)) ? 0 : -1;
        }
        if (n < 0 && errno != EINTR) {
            return -1;
        }
    }
    return -1;
}
static char *http_get(const char *base, const char *file) {
    // Адрес метаданных задаётся как IPv4 и не требует DNS
    char host[64];
    unsigned port = 80;
    int offset = 0;
    if (sscanf(base, "http://%63[0-9.]:%u%n", host, &port, &offset) < 2) {
        port = 80;
        if (sscanf(base, "http://%63[0-9.]%n", host, &offset) != 1) {
            return NULL;
        }
    }
    if (base[offset] || port == 0 || port > 65535) {
        return NULL;
    }

    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        return NULL;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return NULL;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    long long deadline = milliseconds() + 2500;
    if (connect(fd, (void *)&a, sizeof(a)) < 0 && errno != EINPROGRESS) {
        close(fd);
        return NULL;
    }
    if (wait_socket(fd, POLLOUT, deadline)) {
        close(fd);
        return NULL;
    }

    char request[512];
    int len = snprintf(
        request, sizeof(request),
        "GET /openstack/latest/%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
        file, host);
    for (int sent = 0; sent < len;) {
        ssize_t n = send(fd, request + sent, len - sent, 0);
        if (n > 0) {
            sent += n;
        } else if ((errno != EAGAIN && errno != EINTR) ||
                   wait_socket(fd, POLLOUT, deadline)) {
            close(fd);
            return NULL;
        }
    }

    char *response = malloc(LIMIT + 1);
    if (!response) {
        close(fd);
        return NULL;
    }

    size_t n = 0;
    int done = 0;
    while (n < LIMIT) {
        ssize_t got = recv(fd, response + n, LIMIT - n, 0);
        if (got > 0) {
            n += got;
        } else if (got == 0) {
            done = 1;
            break;
        } else if ((errno != EAGAIN && errno != EINTR) ||
                   wait_socket(fd, POLLIN, deadline)) {
            break;
        }
    }
    close(fd);
    response[n] = 0;
    char *body = strstr(response, "\r\n\r\n");
    int status = 0;
    if (!done || !body || sscanf(response, "HTTP/%*s %d", &status) != 1 ||
        status != 200) {
        free(response);
        return NULL;
    }
    *body = 0;
    body += 4;
    if (strcasestr(response, "Transfer-Encoding: chunked")) {
        char *in = body, *out = body;
        size_t left = n - (body - response);
        for (;;) {
            char *end = strstr(in, "\r\n"), *hexend;
            if (!end) {
                goto bad;
            }
            errno = 0;
            unsigned long chunk = strtoul(in, &hexend, 16);
            if (errno || hexend == in || (hexend != end && *hexend != ';')) {
                goto bad;
            }

            size_t header = end + 2 - in;
            if (header > left) {
                goto bad;
            }
            left -= header;
            in = end + 2;
            if (!chunk) {
                *out = 0;
                break;
            }
            if (chunk > left || left - chunk < 2 || memcmp(in + chunk, "\r\n", 2)) {
                goto bad;
            }
            memmove(out, in, chunk);
            out += chunk;
            in += chunk + 2;
            left -= chunk + 2;
        }
    } else {
        char *cl = strcasestr(response, "Content-Length:");
        if (cl && strtoul(cl + 15, NULL, 10) != n - (body - response)) {
            goto bad;
        }
    }
    memmove(response, body, strlen(body) + 1);
    return response;
bad:
    free(response);
    return NULL;
}
static int text(const struct json *j, int o, const char *key, char *out, size_t n) {
    return json_string(j, json_get(j, o, key), out, n) < 0 ? -1 : 0;
}
// В тестовом режиме MAC-адреса через запятую сопоставляются индексам по порядку
static int interface_for_mac(const char *mac) {
    if (test_mac) {
        int index = 0;
        const char *cursor = test_mac;
        for (;;) {
            const char *comma = strchr(cursor, ',');
            size_t length = comma ? (size_t)(comma - cursor) : strlen(cursor);
            char candidate[32];
            if (length < sizeof(candidate)) {
                memcpy(candidate, cursor, length);
                candidate[length] = 0;
                if (!strcasecmp(candidate, mac)) {
                    return index;
                }
            }
            if (!comma) {
                return -1;
            }
            cursor = comma + 1;
            index++;
        }
    }
    long count = syscall(106000);
    for (int i = 0; i < count; i++) {
        unsigned char b[6];
        char s[32];
        if (syscall(106009, i, b, sizeof(b)) < 0) {
            continue;
        }
        snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3],
                 b[4], b[5]);
        if (!strcasecmp(s, mac)) {
            return i;
        }
    }
    return -1;
}
static int parse_metadata(const char *data) {
    struct json *j = calloc(1, sizeof(*j));
    if (!j) {
        return -1;
    }
    if (json_parse(j, data, strlen(data)) < 0 || j->t[0].type != J_OBJECT) {
        free(j);
        return -1;
    }
    text(j, 0, "uuid", instance, sizeof(instance));
    text(j, 0, "hostname", hostname, sizeof(hostname));
    int object = json_get(j, 0, "public_keys");
    if (object >= 0 && j->t[object].type == J_OBJECT) {
        for (int t = object + 1; t < j->t[object].next;) {
            int value = t + 1;
            char key[8192];
            if (json_string(j, value, key, sizeof(key)) >= 0) {
                add_key(key);
            }
            t = j->t[value].next;
        }
    }

    int array = json_get(j, 0, "keys");
    if (array >= 0 && j->t[array].type == J_ARRAY) {
        for (int t = array + 1; t < j->t[array].next; t = j->t[t].next) {
            char key[8192];
            if (!text(j, t, "data", key, sizeof(key))) {
                add_key(key);
            }
        }
    }
    free(j);
    return simple(instance) ? 0 : -1;
}
// Разобрать адрес и маску маршрута в канонический CIDR
// Принимает network со знаком «/», dotted netmask или числовой префикс
static int route_cidr(const char *network, const char *netmask, char *out, size_t cap,
                      int *prefix_out) {
    char ip[80];
    snprintf(ip, sizeof(ip), "%s", network);
    int prefix = -1;
    char *slash = strchr(ip, '/');
    if (slash) {
        *slash = 0;
        char *end = NULL;
        long value = strtol(slash + 1, &end, 10);
        if (end == slash + 1 || (end && *end) || value < 0 || value > 32) {
            return -1;
        }
        prefix = (int)value;
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) {
        return -1;
    }

    if (prefix < 0) {
        if (!netmask || !*netmask) {
            return -1;
        }
        struct in_addr mask_addr;
        if (inet_pton(AF_INET, netmask, &mask_addr) == 1) {
            uint32_t mask = ntohl(mask_addr.s_addr);
            uint32_t inverse = ~mask;
            if ((inverse & (inverse + 1U)) != 0) {
                return -1;
            }
            prefix = 0;
            for (uint32_t bits = mask; bits & 0x80000000U; bits <<= 1) {
                prefix++;
            }
        } else {
            char *end = NULL;
            long value = strtol(netmask, &end, 10);
            if (end == netmask || (end && *end) || value < 0 || value > 32) {
                return -1;
            }
            prefix = (int)value;
        }
    }

    uint32_t host = ntohl(addr.s_addr);
    uint32_t mask = prefix == 0 ? 0U : 0xffffffffU << (32 - prefix);
    struct in_addr net;
    net.s_addr = htonl(host & mask);
    char text[16];
    if (!inet_ntop(AF_INET, &net, text, sizeof(text))) {
        return -1;
    }
    if (snprintf(out, cap, "%s/%d", text, prefix) >= (int)cap) {
        return -1;
    }
    *prefix_out = prefix;
    return 0;
}

static int parse_network(const char *data) {
    struct json *j = calloc(1, sizeof(*j));
    if (!j) {
        return -1;
    }
    if (json_parse(j, data, strlen(data)) < 0) {
        free(j);
        return -1;
    }

    net_iface_count = 0;
    net_route_count = 0;
    int links = json_get(j, 0, "links"), nets = json_get(j, 0, "networks"),
        services = json_get(j, 0, "services");
    if (nets >= 0 && j->t[nets].type == J_ARRAY) {
        for (int t = nets + 1; t < j->t[nets].next; t = j->t[t].next) {
            char type[32], link[128], mac[32] = "", ip[64], mask[64];
            if (text(j, t, "type", type, sizeof(type)) || strcmp(type, "ipv4")) {
                continue;
            }
            if (text(j, t, "link", link, sizeof(link))) {
                continue;
            }
            if (links >= 0 && j->t[links].type == J_ARRAY) {
                for (int l = links + 1; l < j->t[links].next; l = j->t[l].next) {
                    char id[128];
                    if (!text(j, l, "id", id, sizeof(id)) && !strcmp(id, link)) {
                        text(j, l, "ethernet_mac_address", mac, sizeof(mac));
                    }
                }
            }

            int index = interface_for_mac(mac);
            if (index < 0) {
                fprintf(stderr, "[cloud-init] no interface matching network MAC\n");
                continue;
            }
            if (text(j, t, "ip_address", ip, sizeof(ip)) ||
                text(j, t, "netmask", mask, sizeof(mask))) {
                continue;
            }

            struct in_addr check;
            if (inet_pton(AF_INET, ip, &check) != 1 ||
                inet_pton(AF_INET, mask, &check) != 1) {
                failed = 1;
                continue;
            }
            if (net_iface_count >= MAX_NET_IFACES) {
                fprintf(stderr, "[cloud-init] too many static IPv4 interfaces\n");
                failed = 1;
                continue;
            }

            struct net_iface_cfg *iface = &net_ifaces[net_iface_count];
            memset(iface, 0, sizeof(*iface));
            iface->index = index;
            snprintf(iface->ip, sizeof(iface->ip), "%s", ip);
            snprintf(iface->mask, sizeof(iface->mask), "%s", mask);
            net_iface_count++;

            char entry_gw[64] = "";
            text(j, t, "gateway", entry_gw, sizeof(entry_gw));

            int routes = json_get(j, t, "routes");
            if (routes >= 0 && j->t[routes].type == J_ARRAY) {
                for (int r = routes + 1; r < j->t[routes].next; r = j->t[r].next) {
                    char destination[80], route_mask[64] = "", route_gw[64] = "";
                    long metric = 0;
                    if (text(j, r, "network", destination, sizeof(destination))) {
                        continue;
                    }
                    text(j, r, "netmask", route_mask, sizeof(route_mask));
                    text(j, r, "gateway", route_gw, sizeof(route_gw));
                    char metric_text[32];
                    if (!text(j, r, "metric", metric_text, sizeof(metric_text))) {
                        metric = strtol(metric_text, NULL, 10);
                        if (metric < 0 || metric > 0xffffffffL) {
                            metric = 0;
                        }
                    }

                    char cidr[32];
                    int prefix = 0;
                    if (route_cidr(destination, route_mask, cidr, sizeof(cidr), &prefix)) {
                        failed = 1;
                        continue;
                    }

                    if (prefix == 0) {
                        // Маршрут по умолчанию становится шлюзом своего интерфейса
                        if (route_gw[0] && inet_pton(AF_INET, route_gw, &check) == 1 &&
                            !iface->gw[0]) {
                            snprintf(iface->gw, sizeof(iface->gw), "%s", route_gw);
                        }
                        continue;
                    }

                    if (route_gw[0] && inet_pton(AF_INET, route_gw, &check) != 1) {
                        failed = 1;
                        continue;
                    }
                    if (net_route_count >= MAX_NET_ROUTES) {
                        fprintf(stderr, "[cloud-init] too many static routes\n");
                        failed = 1;
                        continue;
                    }
                    struct net_route_cfg *route = &net_routes[net_route_count];
                    memset(route, 0, sizeof(*route));
                    snprintf(route->dest, sizeof(route->dest), "%s", cidr);
                    snprintf(route->gw, sizeof(route->gw), "%s", route_gw);
                    route->has_gw = route_gw[0] != 0;
                    route->index = index;
                    route->metric = (unsigned)metric;
                    net_route_count++;
                }
            }
            if (!iface->gw[0] && entry_gw[0] &&
                inet_pton(AF_INET, entry_gw, &check) == 1) {
                snprintf(iface->gw, sizeof(iface->gw), "%s", entry_gw);
            }
            if (!iface->gw[0]) {
                strcpy(iface->gw, "0.0.0.0");
            }
        }
    }

    // Сформировать секции [Network] и [Network.N] для init
    for (int i = 0; i < net_iface_count; i++) {
        struct net_iface_cfg *iface = &net_ifaces[i];
        char line[320];
        if (i == 0) {
            append(network, sizeof(network), "[Network]\nMode = Static\n");
        } else {
            snprintf(line, sizeof(line), "[Network.%d]\n", i + 1);
            append(network, sizeof(network), line);
        }
        snprintf(line, sizeof(line), "Interface = %d\nIP = %s\nSubnet = %s\nGateway = %s\n",
                 iface->index, iface->ip, iface->mask, iface->gw);
        append(network, sizeof(network), line);
    }

    // Дополнительные маршруты, включая on-link записи без шлюза
    for (int i = 0; i < net_route_count; i++) {
        struct net_route_cfg *route = &net_routes[i];
        char line[64];
        snprintf(line, sizeof(line), "[Route.%d]\n", i + 1);
        append(network, sizeof(network), line);
        append(network, sizeof(network), "Destination = ");
        append(network, sizeof(network), route->dest);
        append(network, sizeof(network), "\nInterface = ");
        snprintf(line, sizeof(line), "%d\n", route->index);
        append(network, sizeof(network), line);
        if (route->has_gw) {
            append(network, sizeof(network), "Gateway = ");
            append(network, sizeof(network), route->gw);
            append(network, sizeof(network), "\n");
        }
        if (route->metric) {
            append(network, sizeof(network), "Metric = ");
            snprintf(line, sizeof(line), "%u\n", route->metric);
            append(network, sizeof(network), line);
        }
    }

    if (services >= 0 && j->t[services].type == J_ARRAY) {
        int count = 0;
        for (int t = services + 1; t < j->t[services].next; t = j->t[t].next) {
            char type[32], address[64];
            struct in_addr addr;
            if (text(j, t, "type", type, sizeof(type)) || strcmp(type, "dns") ||
                text(j, t, "address", address, sizeof(address)) ||
                inet_pton(AF_INET, address, &addr) != 1) {
                continue;
            }

            char line[96];
            snprintf(line, sizeof(line), "%d = %s\n", ++count, address);
            append(dns, sizeof(dns), line);
        }
    }
    free(j);
    return failed ? -1 : 0;
}
// Проверить, что путь записи абсолютный и не выходит через «..»
static int safe_output_path(const char *path) {
    size_t length = strlen(path);
    if (length < 2 || length >= 400 || path[0] != '/') {
        return 0;
    }

    const char *part = path;
    while (*part) {
        const char *slash = strchr(part, '/');
        size_t size = slash ? (size_t)(slash - part) : strlen(part);
        if (size == 2 && part[0] == '.' && part[1] == '.') {
            return 0;
        }
        if (!slash) {
            break;
        }
        part = slash + 1;
    }
    return 1;
}

// Разобрать значение permissions вида 0644 или '0644'
static int parse_permissions(const char *value, long *out) {
    if (!*value) {
        return -1;
    }

    char *end = NULL;
    long mode = strtol(value, &end, 8);
    while (end && isspace((unsigned char)*end)) {
        end++;
    }
    if (end == value || (end && *end) || mode < 0 || mode > 07777) {
        return -1;
    }
    *out = mode;
    return 0;
}

// Проверить, что владелец файла поддерживается (только root)
static int parse_owner(const char *value) {
    return !strcmp(value, "root") || !strcmp(value, "root:root") || !strcmp(value, "0") ||
           !strcmp(value, "0:0");
}

// Декодировать содержимое write_files при encoding: b64
static unsigned char *decode_base64(const char *text, size_t *out_length) {
    static signed char table[256];
    static int ready;
    if (!ready) {
        memset(table, -1, sizeof(table));
        const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) {
            table[(unsigned char)alphabet[i]] = (signed char)i;
        }
        ready = 1;
    }

    size_t capacity = strlen(text) / 4 * 3 + 3;
    unsigned char *out = malloc(capacity);
    if (!out) {
        return NULL;
    }

    size_t used = 0;
    unsigned value = 0;
    int bits = 0;
    for (const char *p = text; *p; p++) {
        if (isspace((unsigned char)*p)) {
            continue;
        }
        if (*p == '=') {
            break;
        }
        int digit = table[(unsigned char)*p];
        if (digit < 0) {
            free(out);
            return NULL;
        }
        value = (value << 6) | (unsigned)digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (used >= capacity) {
                free(out);
                return NULL;
            }
            out[used++] = (unsigned char)(value >> bits);
        }
    }
    *out_length = used;
    return out;
}

// Отпечаток user-data для однократного применения runcmd и write_files
static void fingerprint_userdata(const char *vendor, const char *user, char out[17]) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (int pass = 0; pass < 2; pass++) {
        const char *data = pass ? user : vendor;
        if (!data) {
            continue;
        }
        for (const unsigned char *p = (const unsigned char *)data; *p; p++) {
            hash ^= *p;
            hash *= UINT64_C(1099511628211);
        }
        hash ^= 0xff;
        hash *= UINT64_C(1099511628211);
    }
    snprintf(out, 17, "%016llx", (unsigned long long)hash);
}

// Записать файл write_files с проверкой пути и прав
static int apply_write_file(const struct write_file_entry *entry) {
    if (!safe_output_path(entry->path)) {
        fprintf(stderr, "[cloud-init] unsafe write_files path rejected\n");
        return -1;
    }

    char *data = entry->content;
    size_t length = entry->length;
    unsigned char *decoded = NULL;
    if (entry->binary && entry->content) {
        decoded = decode_base64(entry->content, &length);
        if (!decoded) {
            fprintf(stderr, "[cloud-init] invalid base64 in write_files\n");
            return -1;
        }
        data = (char *)decoded;
    }

    // Создать недостающие каталоги вдоль пути
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", entry->path);
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(dir, 0755) && errno != EEXIST) {
                free(decoded);
                return -1;
            }
            *p = '/';
        }
    }

    mode_t mode = entry->permissions >= 0 ? (mode_t)entry->permissions : 0644;
    int fd = open(entry->path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) {
        free(decoded);
        return -1;
    }

    size_t done = 0;
    int bad = 0;
    while (done < length) {
        ssize_t n = write(fd, data + done, length - done);
        if (n <= 0) {
            bad = -1;
            break;
        }
        done += n;
    }
    if (fchmod(fd, mode)) {
        bad = -1;
    }
    if (close(fd)) {
        bad = -1;
    }
    free(decoded);
    return bad;
}

// Разобрать элемент runcmd вида [команда, аргумент, ...]
static int runcmd_argv(char *text, char **argv, int max) {
    size_t length = strlen(text);
    if (length < 2 || text[0] != '[' || text[length - 1] != ']') {
        return -1;
    }
    text[length - 1] = 0;
    int count = 0;
    char *save = NULL;
    for (char *part = strtok_r(text + 1, ",", &save); part && count < max - 1;
         part = strtok_r(NULL, ",", &save)) {
        while (isspace((unsigned char)*part)) {
            part++;
        }
        size_t n = strlen(part);
        while (n && isspace((unsigned char)part[n - 1])) {
            part[--n] = 0;
        }
        if (n >= 2 && ((part[0] == '"' && part[n - 1] == '"') ||
                       (part[0] == '\'' && part[n - 1] == '\''))) {
            part[n - 1] = 0;
            part++;
        }
        if (!*part) {
            continue;
        }
        argv[count++] = part;
    }
    argv[count] = NULL;
    return count;
}

// Выполнить одну команду runcmd и вернуть её код завершения
static int run_one_command(const char *command) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        chdir("/");
        char *argv[64];
        int argc = runcmd_argv((char *)command, argv, 64);
        if (argc > 0) {
            execvp(argv[0], argv);
        } else {
            execl("/bin/sh", "sh", "-c", command, (char *)NULL);
            execl("/bin/bash", "bash", "-c", command, (char *)NULL);
        }
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 128 + WTERMSIG(status);
}

// Применить write_files и runcmd один раз на отпечаток user-data
// Возвращает 0 при применении, 1 при пропуске и -1 при ошибке
static int apply_provisioning_actions(const char *output, const char *fingerprint) {
    char state_path[512];
    snprintf(state_path, sizeof(state_path), "%s/instance.state", output);
    char *state = read_file(state_path);
    int skip = 0;
    if (state) {
        char saved_instance[256] = "", saved_fp[32] = "", saved_status[32] = "";
        char *save = NULL;
        for (char *line = strtok_r(state, "\r\n", &save); line;
             line = strtok_r(NULL, "\r\n", &save)) {
            char *eq = strchr(line, '=');
            if (!eq) {
                continue;
            }
            *eq = 0;
            if (!strcmp(line, "instance")) {
                snprintf(saved_instance, sizeof(saved_instance), "%s", eq + 1);
            } else if (!strcmp(line, "fingerprint")) {
                snprintf(saved_fp, sizeof(saved_fp), "%s", eq + 1);
            } else if (!strcmp(line, "actions")) {
                snprintf(saved_status, sizeof(saved_status), "%s", eq + 1);
            }
        }
        skip = !strcmp(saved_instance, instance) && !strcmp(saved_fp, fingerprint) &&
               !strcmp(saved_status, "done");
    }
    free(state);

    if (skip) {
        printf("[cloud-init] provisioning actions already applied for this user-data\n");
        return 1;
    }

    for (int i = 0; i < wf_count; i++) {
        if (apply_write_file(&wf_entries[i]) < 0) {
            fprintf(stderr, "[cloud-init] write_files entry %d failed\n", i);
            return -1;
        }
    }

    for (int i = 0; i < runcmd_count; i++) {
        int code = run_one_command(runcmd[i]);
        if (code != 0) {
            fprintf(stderr, "[cloud-init] runcmd entry %d exited with %d\n", i, code);
            return -2;
        }
    }

    // Зафиксировать успешное применение, чтобы повторная загрузка не повторяла действия
    char body[640];
    snprintf(body, sizeof(body), "instance=%s\nfingerprint=%s\nactions=done\n",
             instance, fingerprint);
    char path[512];
    snprintf(path, sizeof(path), "%s/instance.state", output);
    if (write_file(path, body, 0600)) {
        return -1;
    }
    return 0;
}

// Сохранить разобранный элемент write_files в список
static int wf_store(const char *path) {
    if (wf_count >= MAX_WRITE_FILES) {
        fprintf(stderr, "[cloud-init] too many write_files entries\n");
        failed = 1;
        return -1;
    }
    struct write_file_entry *entry = &wf_entries[wf_count];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->path, sizeof(entry->path), "%s", path);
    entry->permissions = -1;
    wf_count++;
    return 0;
}

// Разобрать скалярный ключ cloud-config вне списков write_files и runcmd
// Ключ может встречаться на любом уровне вложенности, как в исходном разборе
static void userdata_scalar(char *key, char *value) {
    if ((!strcmp(key, "hostname") || (!strcmp(key, "fqdn") && !hostname[0])) &&
        simple(value) && strlen(value) < sizeof(hostname)) {
        strcpy(hostname, value);
    } else if (!strcmp(key, "root") && *value && strlen(value) < sizeof(password)) {
        if (!strncmp(value, "$6$", 3)) {
            strcpy(password_hash, value);
        } else {
            strcpy(password, value);
        }
    } else if (!strcmp(key, "password") || !strcmp(key, "passwd") ||
               !strcmp(key, "hashed_passwd")) {
        if (*value && strlen(value) < sizeof(password)) {
            if (!strncmp(value, "$6$", 3)) {
                strcpy(password_hash, value);
            } else if (!strcmp(key, "password")) {
                strcpy(password, value);
            }
        }
    } else if (!strcmp(key, "packages")) {
        fprintf(stderr, "[cloud-init] unsupported cloud-config field: %s\n", key);
    } else if (!strcmp(key, "- name") && strcmp(value, "root")) {
        fprintf(stderr, "[cloud-init] only the root account is supported\n");
        failed = 1;
    }
}

// Сохранить собранный блочный скаляр в элемент write_files
static void block_scalar_finish(char **buf, size_t *used, int fold, int chomp,
                                struct write_file_entry *entry) {
    char *data = *buf;
    size_t n = *used;
    *buf = NULL;
    *used = 0;

    if (data == NULL) {
        free(entry->content);
        entry->content = strdup("");
        entry->length = 0;
        return;
    }

    if (fold) {
        // В folded-скаляре обычный перевод строки становится пробелом,
        // пустая строка сохраняет границу абзаца
        char *out = malloc(n + 1);
        if (!out) {
            free(data);
            failed = 1;
            return;
        }
        size_t w = 0;
        int fresh = 1;
        for (size_t r = 0; r < n;) {
            size_t e = r;
            while (e < n && data[e] != '\n') {
                e++;
            }
            size_t length = e - r;
            if (length == 0) {
                out[w++] = '\n';
                fresh = 1;
            } else {
                if (!fresh && w > 0 && out[w - 1] != '\n') {
                    out[w++] = ' ';
                }
                memcpy(out + w, data + r, length);
                w += length;
                fresh = 0;
            }
            r = e + 1;
        }
        free(data);
        data = out;
        n = w;
    }

    size_t tail = n;
    while (tail > 0 && data[tail - 1] == '\n') {
        tail--;
    }
    if (chomp == 1) {
        n = tail;
    } else if (chomp == 0) {
        n = tail > 0 || n == 0 ? tail + (n > tail ? 1 : 0) : 0;
        if (n > 0 && tail == 0) {
            n = 0;
        }
    }

    free(entry->content);
    entry->content = malloc(n + 1);
    if (!entry->content) {
        free(data);
        failed = 1;
        return;
    }
    memcpy(entry->content, data, n);
    entry->content[n] = 0;
    entry->length = n;
    free(data);
}

// Разобрать user-data cloud-config, включая write_files и runcmd
static void parse_userdata(char *data) {
    if (!data || strncmp(data, "#cloud-config", 13)) {
        if (data && *data) {
            fprintf(stderr, "[cloud-init] user-data scripts are not supported\n");
        }
        return;
    }

    int section = 0; // 0 - верхний уровень, 1 - ключи, 2 - users, 3 - write_files, 4 - runcmd
    int item_indent = -1;
    int block_key_indent = -1;
    int block_strip = -1;
    int block_fold = 0;
    int block_chomp = 0;
    char *block_buf = NULL;
    size_t block_used = 0, block_cap = 0;
    struct write_file_entry *current = NULL;
    struct write_file_entry *block_entry = NULL;

    char *cursor = data;
    while (cursor) {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        if (newline) {
            *newline = 0;
            cursor = newline + 1;
        } else {
            cursor = NULL;
        }

        int indent = (int)strspn(line, " \t");
        char *s = trim(line);

        // Блочный скаляр content: | или content: >
        if (block_key_indent >= 0) {
            int ends = 0;
            if (!*s) {
                if (block_buf && block_used + 1 < block_cap) {
                    block_buf[block_used++] = '\n';
                }
                continue;
            }
            if (indent <= block_key_indent || (block_strip >= 0 && indent < block_strip)) {
                ends = 1;
            } else {
                if (block_strip < 0) {
                    block_strip = indent;
                }
                const char *content = line + block_strip;
                size_t add = strlen(content);
                if (block_used + add + 2 > MAX_FILE_BYTES + 2) {
                    fprintf(stderr, "[cloud-init] write_files content is too large\n");
                    failed = 1;
                    free(block_buf);
                    block_buf = NULL;
                    block_cap = 0;
                    block_key_indent = -1;
                    block_entry = NULL;
                    continue;
                }
                if (block_used + add + 2 > block_cap) {
                    size_t next = block_cap ? block_cap * 2 : 4096;
                    while (next < block_used + add + 2) {
                        next *= 2;
                    }
                    char *grown = realloc(block_buf, next);
                    if (!grown) {
                        free(block_buf);
                        block_buf = NULL;
                        block_cap = 0;
                        failed = 1;
                        block_key_indent = -1;
                        block_entry = NULL;
                        continue;
                    }
                    block_buf = grown;
                    block_cap = next;
                }
                memcpy(block_buf + block_used, content, add);
                block_used += add;
                block_buf[block_used++] = '\n';
                continue;
            }
            if (ends) {
                block_scalar_finish(&block_buf, &block_used, block_fold, block_chomp,
                                    block_entry);
                block_key_indent = -1;
                block_entry = NULL;
                free(block_buf);
                block_buf = NULL;
                block_cap = 0;
            }
        }

        if (!*s || *s == '#') {
            continue;
        }

        if (indent == 0) {
            item_indent = -1;
            char *colon = strchr(s, ':');
            if (section == 1 && *s == '-' && !colon) {
                add_key(trim(s + 1));
                continue;
            }
            if (colon) {
                *colon = 0;
                char *key = trim(s);
                char *value = trim(colon + 1);
                if (!strcmp(key, "ssh_authorized_keys")) {
                    section = 1;
                } else if (!strcmp(key, "users")) {
                    section = 2;
                } else if (!strcmp(key, "write_files")) {
                    section = 3;
                    current = NULL;
                } else if (!strcmp(key, "runcmd")) {
                    section = 4;
                } else {
                    section = 0;
                }

                userdata_scalar(key, value);
                continue;
            }
            section = 0;
        }

        if (section == 3) {
            // Элементы write_files: «- key: value» и продолжение «key: value»
            if (*s == '-' && (s[1] == ' ' || s[1] == '\t' || !s[1])) {
                char *rest = trim(s + 1);
                item_indent = indent;
                if (*rest && wf_store(rest) == 0) {
                    current = &wf_entries[wf_count - 1];
                } else {
                    current = NULL;
                }
                s = rest;
                if (!*s) {
                    continue;
                }
            } else {
                if (current == NULL) {
                    continue;
                }
                if (indent <= item_indent) {
                    current = NULL;
                    continue;
                }
            }

            char *colon = strchr(s, ':');
            if (!colon) {
                if (*s) {
                    fprintf(stderr, "[cloud-init] unsupported write_files field\n");
                }
                continue;
            }
            *colon = 0;
            char *key = trim(s);
            char *value = trim(colon + 1);
            if (!strcmp(key, "path")) {
                snprintf(current->path, sizeof(current->path), "%s", value);
            } else if (!strcmp(key, "content")) {
                if (*value == '|' || *value == '>') {
                    block_fold = *value == '>';
                    block_chomp = value[1] == '-' ? 1 : value[1] == '+' ? 2 : 0;
                    block_key_indent = indent;
                    block_strip = -1;
                    block_used = 0;
                    free(block_buf);
                    block_buf = NULL;
                    block_cap = 0;
                    block_entry = current;
                } else if (*value) {
                    free(current->content);
                    current->content = strdup(value);
                    current->length = strlen(value);
                }
            } else if (!strcmp(key, "permissions") || !strcmp(key, "mode")) {
                if (parse_permissions(value, &current->permissions)) {
                    fprintf(stderr, "[cloud-init] invalid write_files permissions\n");
                    failed = 1;
                }
            } else if (!strcmp(key, "owner")) {
                if (!parse_owner(value)) {
                    fprintf(stderr,
                            "[cloud-init] only root ownership is supported in write_files\n");
                    failed = 1;
                }
            } else if (!strcmp(key, "encoding")) {
                if (!strcmp(value, "b64") || !strcmp(value, "base64")) {
                    current->binary = 1;
                } else if (*value && strcmp(value, "text")) {
                    fprintf(stderr, "[cloud-init] unsupported write_files encoding\n");
                    failed = 1;
                }
            } else if (strcmp(key, "defer")) {
                fprintf(stderr, "[cloud-init] unsupported write_files field: %s\n", key);
            }
            continue;
        }

        if (section == 4) {
            // Команды runcmd: «- команда» или «- [аргументы]»
            if (*s == '-' && (s[1] == ' ' || s[1] == '\t')) {
                char *command = trim(s + 1);
                if (*command && runcmd_count < MAX_RUNCMD &&
                    strlen(command) < MAX_RUNCMD_TEXT) {
                    runcmd[runcmd_count] = strdup(command);
                    if (!runcmd[runcmd_count]) {
                        failed = 1;
                    } else {
                        runcmd_count++;
                    }
                } else if (*command) {
                    fprintf(stderr, "[cloud-init] too many or too long runcmd entries\n");
                    failed = 1;
                }
            }
            continue;
        }

        if (section == 1 && *s == '-') {
            add_key(trim(s + 1));
            continue;
        }

        char *colon = strchr(s, ':');
        if (!colon) {
            continue;
        }
        *colon = 0;
        char *key = trim(s);
        char *value = trim(colon + 1);
        if (!strcmp(key, "chpasswd") || !strcmp(key, "expire") || !strcmp(key, "users") ||
            !strcmp(key, "ssh_authorized_keys") || !strcmp(key, "write_files") ||
            !strcmp(key, "runcmd")) {
            continue;
        }
        userdata_scalar(key, value);
    }

    // Файл оборвался внутри блочного скаляра, сохраняем собранное
    if (block_key_indent >= 0 && block_entry != NULL) {
        block_scalar_finish(&block_buf, &block_used, block_fold, block_chomp, block_entry);
    }
    free(block_buf);
}

static int write_file(const char *path, const char *data, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        return -1;
    }

    size_t len = strlen(data), done = 0;
    while (done < len) {
        ssize_t n = write(fd, data + done, len - done);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        done += n;
    }

    int bad = fchmod(fd, mode);
    if (close(fd)) {
        bad = -1;
    }
    return bad;
}

static char *config_file(const char *root, const char *file) {
    const char *versions[] = {"latest",     "2018-08-27", "2017-02-22",
                              "2016-10-06", "2015-10-15", "2012-08-10"};
    for (size_t i = 0; i < sizeof(versions) / sizeof(*versions); i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/openstack/%s/%s", root, versions[i], file);
        char *data = read_file(path);
        if (data) {
            return data;
        }
    }
    return NULL;
}

// Прочитать файл из корня диска NoCloud
static char *nocloud_file(const char *root, const char *file) {
    char path[512];
    char alias[64];
    size_t length = strlen(file);

    if (length >= sizeof(alias) ||
        snprintf(path, sizeof(path), "%s/%s", root, file) >= (int)sizeof(path)) {
        return NULL;
    }

    char *data = read_file(path);
    if (data != NULL) {
        return data;
    }

    // ISO9660 без Rock Ridge заменяет дефис подчёркиванием
    for (size_t index = 0; index < length; index++) {
        alias[index] = file[index] == '-' ? '_' : file[index];
    }
    alias[length] = 0;

    if (snprintf(path, sizeof(path), "%s/%s", root, alias) < (int)sizeof(path)) {
        data = read_file(path);
        if (data != NULL) {
            return data;
        }
    }

    // ISO level 1 оставляет только первые восемь символов имени
    if (length > 8U) {
        alias[8] = 0;
        if (snprintf(path, sizeof(path), "%s/%s", root, alias) < (int)sizeof(path)) {
            return read_file(path);
        }
    }

    return NULL;
}

// Определить формат метаданных на диске
static char *config_metadata(const char *root) {
    char *data = config_file(root, "meta_data.json");

    if (data != NULL) {
        nocloud = 0;
        return data;
    }

    data = nocloud_file(root, "meta-data");
    if (data != NULL) {
        nocloud = 1;
    }

    return data;
}

// Прочитать обязательный идентификатор экземпляра NoCloud
static int parse_nocloud_metadata(char *data) {
    char *save;

    for (char *line = strtok_r(data, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        char *key = trim(line);
        char *colon = strchr(key, ':');

        if (colon == NULL || *key == '#') {
            continue;
        }

        *colon = 0;
        char *value = trim(colon + 1);

        if (!strcmp(key, "instance-id") && simple(value) && strlen(value) < sizeof(instance)) {
            strcpy(instance, value);
        } else if ((!strcmp(key, "local-hostname") || !strcmp(key, "hostname")) &&
                   simple(value) && strlen(value) < sizeof(hostname)) {
            strcpy(hostname, value);
        }
    }

    return simple(instance) ? 0 : -1;
}

static char *find_configdrive(char root[256]) {
    glob_t g;
    memset(&g, 0, sizeof(g));
    if (!glob("/mnt/*", 0, NULL, &g)) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            char *data = config_metadata(g.gl_pathv[i]);
            if (data) {
                snprintf(root, 256, "%s", g.gl_pathv[i]);
                globfree(&g);
                return data;
            }
        }
        globfree(&g);
    }
    const char *patterns[] = {"/dev/cd?", "/dev/sr*", "/dev/sd*", "/dev/vd*"};
    if (mkdir("/tmp/cloud", 0700) && errno != EEXIST) {
        return NULL;
    }

    int number = 0;
    for (size_t p = 0; p < sizeof(patterns) / sizeof(*patterns); p++) {
        memset(&g, 0, sizeof(g));
        if (glob(patterns[p], 0, NULL, &g)) {
            continue;
        }

        for (size_t i = 0; i < g.gl_pathc; i++) {
            struct stat st;
            if (stat(g.gl_pathv[i], &st) || !S_ISBLK(st.st_mode)) {
                continue;
            }
            snprintf(root, 256, "/tmp/cloud/drive%d", number++);
            mkdir(root, 0700);
            if (mount(g.gl_pathv[i], root, "isofs", 0, NULL) &&
                mount(g.gl_pathv[i], root, "fat32", 0, NULL) &&
                mount(g.gl_pathv[i], root, "fat12", 0, NULL)) {
                continue;
            }

            char *data = config_metadata(root);
            if (data) {
                globfree(&g);
                return data;
            }
        }
        globfree(&g);
    }
    root[0] = 0;
    return NULL;
}
int main(int argc, char **argv) {
    if (argc == 1) {
        service_log_start("cloud-init", 1);
    }
    const char *source = NULL, *url = "http://169.254.169.254", *output = "/etc/cloud",
               *key_path = "/root/.ssh/authorized_keys";
    int config_only = 0;
    int report_degraded = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config-only")) {
            config_only = 1;
            continue;
        }
        if (!strcmp(argv[i], "--report-degraded")) {
            report_degraded = 1;
            continue;
        }
        if (i + 1 >= argc) {
            return 2;
        }
        if (!strcmp(argv[i], "--source")) {
            source = argv[++i];
        } else if (!strcmp(argv[i], "--metadata-url")) {
            url = argv[++i];
        } else if (!strcmp(argv[i], "--output")) {
            output = argv[++i];
        } else if (!strcmp(argv[i], "--keys-file")) {
            key_path = argv[++i];
        } else if (!strcmp(argv[i], "--test-mac")) {
            test_mac = argv[++i];
        } else {
            return 2;
        }
    }

    char root[256] = "";
    char *meta = source ? config_metadata(source) : find_configdrive(root);
    if (source) {
        snprintf(root, sizeof(root), "%s", source);
    }
    const char *method = nocloud ? "nocloud" : "config-drive";
    if (!meta && !config_only) {
        method = "http";
        long long deadline = milliseconds() + 8000;
        do {
            meta = http_get(url, "meta_data.json");
            if (meta) {
                break;
            }
            usleep(200000);
        } while (milliseconds() < deadline);
    }
    if (!meta) {
        puts("[cloud-init] metadata unavailable; keeping local configuration");
        return report_degraded ? 3 : 0;
    }
    if (nocloud ? parse_nocloud_metadata(meta) : parse_metadata(meta)) {
        fprintf(stderr, "[cloud-init] invalid metadata; keeping local configuration\n");
        free(meta);
        return 1;
    }
    free(meta);
    char *net = !strcmp(method, "http") ? http_get(url, "network_data.json")
                : nocloud ? nocloud_file(root, "network-config")
                          : config_file(root, "network_data.json");
    char *user = !strcmp(method, "http") ? http_get(url, "user_data")
                 : nocloud ? nocloud_file(root, "user-data")
                           : config_file(root, "user_data");
    char *vendor = nocloud ? nocloud_file(root, "vendor-data") : NULL;
    // Отпечаток нужен до разбора, parse_userdata изменяет буферы user-data
    char fingerprint[17];
    fingerprint_userdata(vendor, user, fingerprint);
    if (net && parse_network(net)) {
        if (!nocloud || *net != 0) {
            failed = 1;
        }
    }
    parse_userdata(vendor);
    parse_userdata(user);
    free(net);
    free(user);
    free(vendor);
    if (failed) {
        fprintf(stderr, "[cloud-init] configuration rejected\n");
        return 1;
    }
    mkdir(output, 0700);
    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), "%s/password.hash", output);
    char *cached = read_file(cache_path);
    if (cached) {
        cached[strcspn(cached, "\r\n")] = 0;
    }
    if (password[0] && !password_hash[0] && cached && !strncmp(cached, "$6$", 3) &&
        strlen(cached) < sizeof(password_hash)) {
        char *candidate = crypt(password, cached);
        if (candidate && !strcmp(candidate, cached)) {
            strcpy(password_hash, cached);
        }
    }
    free(cached);
    if (password[0] && !password_hash[0]) {
        unsigned char random[16];
        char salt[40] = "$6$";
        if (getrandom(random, sizeof(random), 0) != (ssize_t)sizeof(random)) {
            return 1;
        }

        for (int i = 0; i < 16; i++) {
            sprintf(salt + 3 + i * 2, "%02x", random[i]);
        }
        strcat(salt, "$");
        char *hash = crypt(password, salt);
        if (!hash || strlen(hash) >= sizeof(password_hash)) {
            return 1;
        }
        strcpy(password_hash, hash);
    }

    char settings[16384] = "";
    if (simple(hostname)) {
        append(settings, sizeof(settings), "[System]\nHostname = ");
        append(settings, sizeof(settings), hostname);
        append(settings, sizeof(settings), "\n");
    }
    append(settings, sizeof(settings), network);
    if (dns[0]) {
        append(settings, sizeof(settings), "[DNS]\n");
        append(settings, sizeof(settings), dns);
    }
    if (password_hash[0] || keys[0]) {
        append(settings, sizeof(settings),
               "[SSH]\nEnabled = 1\nLogin = root\nPassword = \nPasswordHash = ");
        append(settings, sizeof(settings), password_hash);
        append(settings, sizeof(settings), "\n");
    }
    if (failed) {
        return 1;
    }
    mkdir(output, 0700);
    char path[512];
    snprintf(path, sizeof(path), "%s/settings.ini", output);
    if (write_file(path, settings, 0600)) {
        perror("[cloud-init] settings");
        return 1;
    }
    snprintf(path, sizeof(path), "%s/authorized_keys.managed", output);
    char *managed = read_file(path), *existing = read_file(key_path);
    char desired[sizeof(keys)];
    strcpy(desired, keys);
    if (existing) {
        char *save;
        for (char *line = strtok_r(existing, "\r\n", &save); line;
             line = strtok_r(NULL, "\r\n", &save)) {
            if (managed && contains_key(managed, line)) {
                continue;
            }
            if (contains_key(keys, line)) {
                continue;
            }
            if (append(keys, sizeof(keys), line) == 0) {
                append(keys, sizeof(keys), "\n");
            }
        }
    }
    if (failed) {
        free(managed);
        free(existing);
        return 1;
    }
    if (keys[0] || managed || existing) {
        mkdir("/root", 0700);
        mkdir("/root/.ssh", 0700);
        if (write_file(key_path, keys, 0600) || write_file(path, desired, 0600)) {
            perror("[cloud-init] keys");
            free(managed);
            free(existing);
            return 1;
        }
    }
    free(managed);
    free(existing);
    if (password_hash[0] && write_file(cache_path, password_hash, 0600)) {
        return 1;
    }
    if (!password_hash[0] && unlink(cache_path) != 0 && errno != ENOENT) {
        return 1;
    }
    memset(password, 0, sizeof(password));

    // Разовое применение write_files и runcmd, в config-only режиме пропускается
    const char *wf_state = "none";
    const char *rc_state = "none";
    if (!config_only && (wf_count > 0 || runcmd_count > 0)) {
        int applied = apply_provisioning_actions(output, fingerprint);
        if (applied == 1) {
            wf_state = wf_count > 0 ? "skipped" : "none";
            rc_state = runcmd_count > 0 ? "skipped" : "none";
        } else if (applied == 0) {
            wf_state = wf_count > 0 ? "done" : "none";
            rc_state = runcmd_count > 0 ? "done" : "none";
        } else {
            if (applied == -1) {
                wf_state = wf_count > 0 ? "error" : "none";
            }
            if (applied == -2) {
                rc_state = runcmd_count > 0 ? "error" : "none";
            }
            fprintf(stderr, "[cloud-init] provisioning actions failed\n");
        }
    }

    char status[768];
    snprintf(status, sizeof(status),
             "source=%s\ninstance=%s\nwrite_files=%s\nruncmd=%s\nexperimental=true\n",
             method, instance, wf_state, rc_state);
    snprintf(path, sizeof(path), "%s/status", output);
    if (write_file(path, status, 0600)) {
        return 1;
    }
    printf("[cloud-init] %s configuration ready for instance %s\n", method, instance);
    return 0;
}
