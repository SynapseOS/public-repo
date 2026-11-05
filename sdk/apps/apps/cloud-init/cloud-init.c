// cloud-init.c: Начальная настройка облачной системы
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Ограниченный набор cloud-config и получение метаданных OpenStack
// Подсистема: установка и начальная настройка системы
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 01.11.2026
// Доступ: общедоступный

// Поддерживается ограниченный набор cloud-config для OpenStack
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
#include "../qemu-ga/json.h"
#include "../common/service_log.h"
#define LIMIT 262144
static char hostname[256], instance[256], password[256], password_hash[256];
static char keys[32768], network[8192], dns[2048];
static const char *test_mac;
static int failed;
static int nocloud;
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
static int interface_for_mac(const char *mac) {
    if (test_mac) {
        return !strcasecmp(mac, test_mac) ? 0 : -1;
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
static int parse_network(const char *data) {
    struct json *j = calloc(1, sizeof(*j));
    if (!j) {
        return -1;
    }
    if (json_parse(j, data, strlen(data)) < 0) {
        free(j);
        return -1;
    }

    int links = json_get(j, 0, "links"), nets = json_get(j, 0, "networks"),
        services = json_get(j, 0, "services");
    if (nets >= 0 && j->t[nets].type == J_ARRAY) {
        for (int t = nets + 1; t < j->t[nets].next; t = j->t[t].next) {
            char type[32], link[128], mac[32] = "", ip[64], mask[64], gw[64] = "";
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
            if (network[0]) {
                fprintf(stderr,
                        "[cloud-init] only one static IPv4 interface is supported\n");
                continue;
            }
            if (text(j, t, "ip_address", ip, sizeof(ip)) ||
                text(j, t, "netmask", mask, sizeof(mask))) {
                continue;
            }

            int routes = json_get(j, t, "routes");
            if (routes >= 0 && j->t[routes].type == J_ARRAY) {
                for (int r = routes + 1; r < j->t[routes].next; r = j->t[r].next) {
                    char destination[64], route_mask[64];
                    if (!text(j, r, "network", destination, sizeof(destination)) &&
                        !text(j, r, "netmask", route_mask, sizeof(route_mask)) &&
                        !strcmp(destination, "0.0.0.0") &&
                        !strcmp(route_mask, "0.0.0.0")) {
                        text(j, r, "gateway", gw, sizeof(gw));
                    }
                }
            }
            if (!gw[0]) {
                text(j, t, "gateway", gw, sizeof(gw));
            }
            if (!gw[0]) {
                strcpy(gw, "0.0.0.0");
            }

            struct in_addr addr;
            if (inet_pton(AF_INET, ip, &addr) != 1 ||
                inet_pton(AF_INET, mask, &addr) != 1 ||
                inet_pton(AF_INET, gw, &addr) != 1) {
                failed = 1;
                continue;
            }
            snprintf(network, sizeof(network),
                     "[Network]\nMode = Static\nInterface = %d\nIP = %s\nSubnet = "
                     "%s\nGateway = %s\n",
                     index, ip, mask, gw);
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
static void parse_userdata(char *data) {
    if (!data || strncmp(data, "#cloud-config", 13)) {
        if (data && *data) {
            fprintf(stderr, "[cloud-init] user-data scripts are not supported\n");
        }
        return;
    }

    char *save;
    int keylist = 0;
    for (char *line = strtok_r(data, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        int indentation = (int)strspn(line, " \t");
        char *s = trim(line);
        if (!*s || *s == '#') {
            continue;
        }
        if (keylist && *s == '-') {
            add_key(trim(s + 1));
            continue;
        }
        if (!indentation) {
            keylist = 0;
        }

        char *colon = strchr(s, ':');
        if (!colon) {
            continue;
        }
        *colon = 0;
        char *value = trim(colon + 1);
        if ((!strcmp(s, "hostname") || (!strcmp(s, "fqdn") && !hostname[0])) && simple(value) &&
            strlen(value) < sizeof(hostname)) {
            strcpy(hostname, value);
        } else if (!strcmp(s, "ssh_authorized_keys")) {
            keylist = 1;
        } else if (!strcmp(s, "root") && *value && strlen(value) < sizeof(password)) {
            if (!strncmp(value, "$6$", 3)) {
                strcpy(password_hash, value);
            } else {
                strcpy(password, value);
            }
        } else if (!strcmp(s, "password") || !strcmp(s, "passwd") ||
                   !strcmp(s, "hashed_passwd")) {
            if (*value && strlen(value) < sizeof(password)) {
                if (!strncmp(value, "$6$", 3)) {
                    strcpy(password_hash, value);
                } else if (!strcmp(s, "password")) {
                    strcpy(password, value);
                }
            }
        } else if (*s == '-' && !strncmp(s + 1, " name", 5) && strcmp(value, "root")) {
            fprintf(stderr, "[cloud-init] only the root account is supported\n");
            failed = 1;
        } else if (!strcmp(s, "chpasswd")) {
            continue;
        } else if (!strcmp(s, "users") || !strcmp(s, "expire")) {
            continue;
        } else if (!strcmp(s, "runcmd") || !strcmp(s, "write_files") ||
                   !strcmp(s, "packages")) {
            fprintf(stderr, "[cloud-init] unsupported cloud-config field: %s\n", s);
        }
    }
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
    char status[512];
    snprintf(status, sizeof(status), "source=%s\ninstance=%s\nexperimental=true\n",
             method, instance);
    snprintf(path, sizeof(path), "%s/status", output);
    if (write_file(path, status, 0600)) {
        return 1;
    }
    printf("[cloud-init] %s configuration ready for instance %s\n", method, instance);
    return 0;
}
