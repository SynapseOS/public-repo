// main.c: Счётчики и статистика системы
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: Счётчики и статистика системы в пользовательском пространстве
// Архитектура: x86_64, ABI: Linux (ELF64)
// Первое изменение: 09.10.2026
// Последнее изменение: 19.11.2026
// Доступ: общедоступный

#define _GNU_SOURCE
#include "../common/settings/settings.h"
#include "../common/tls/tls.h"
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#ifdef STATS_LINUX
#define STATS_SETTINGS_ROOT "/var/lib/synapse-stats"
#define LOG_FILE_PATH "/var/log/synapse-stats/monitoring.log"
#else
#define STATS_SETTINGS_ROOT ""
#define LOG_FILE_PATH "/var/log/monitoring.log"
#endif
#define LOG_MAX_SIZE (1024 * 1024)

static FILE *log_fp = NULL;
static bool print_packets;

static void log_init(void) {
    if (log_fp) {
        return;
    }
    mkdir("/var/log", 0755);
    log_fp = fopen(LOG_FILE_PATH, "a");
    if (!log_fp) {
        fprintf(stderr, "[tel] FATAL: Cannot open/create %s\n", LOG_FILE_PATH);
    }
}

static void log_rotate(void) {
    if (!log_fp) {
        return;
    }
    long pos = ftell(log_fp);
    if (pos > LOG_MAX_SIZE) {
        fclose(log_fp);
        rename(LOG_FILE_PATH, LOG_FILE_PATH ".old");
        log_fp = fopen(LOG_FILE_PATH, "w");
    }
}

#define LOG(fmt, ...)                                                                  \
    do {                                                                               \
        log_init();                                                                    \
        if (log_fp) {                                                                  \
            log_rotate();                                                              \
            time_t now = time(NULL);                                                   \
            struct tm *t = localtime(&now);                                            \
            char time_buf[32];                                                         \
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", t);              \
            fprintf(log_fp, "[%s] [tel] " fmt "\n", time_buf, ##__VA_ARGS__);          \
            fflush(log_fp);                                                            \
        }                                                                              \
    } while (0)

typedef struct {
    uint64_t uptime;
    uint64_t total_ram;
    uint64_t free_ram;
    uint64_t proc_count;
} tel_packet_t;

static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx,
                  uint32_t *edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(0));
}

static void append_feature(char *buf, size_t max_len, size_t *len, const char *name) {
    size_t needed = strlen(name) + (*len > 0 ? 1 : 0);
    if (*len >= max_len || needed >= max_len - *len) {
        return;
    }

    int n = snprintf(buf + *len, max_len - *len, "%s%s", (*len > 0) ? "," : "", name);
    if (n > 0) {
        *len += (size_t)n;
    }
}

static void get_simd_features(char *out_buf, size_t max_len) {
    out_buf[0] = '\0';
    size_t len = 0;
    bool is_x86 = false;

#if defined(__i386__) || defined(__x86_64__)
    is_x86 = true;
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;
    uint64_t xcr0 = 0;
    bool os_avx = false;

    if (max_leaf >= 1) {
        cpuid(1, &eax, &ebx, &ecx, &edx);
        if ((edx >> 23) & 1) {
            append_feature(out_buf, max_len, &len, "mmx");
        }
        if ((edx >> 25) & 1) {
            append_feature(out_buf, max_len, &len, "sse");
        }
        if ((edx >> 26) & 1) {
            append_feature(out_buf, max_len, &len, "sse2");
        }
        if ((ecx >> 0) & 1) {
            append_feature(out_buf, max_len, &len, "sse3");
        }
        if ((ecx >> 9) & 1) {
            append_feature(out_buf, max_len, &len, "ssse3");
        }
        if ((ecx >> 19) & 1) {
            append_feature(out_buf, max_len, &len, "sse4_1");
        }
        if ((ecx >> 20) & 1) {
            append_feature(out_buf, max_len, &len, "sse4_2");
        }

        if ((ecx & (1u << 26)) && (ecx & (1u << 27))) {
            uint32_t lo, hi;
            __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
            xcr0 = ((uint64_t)hi << 32) | lo;
        }
        os_avx = (ecx & (1u << 28)) && (xcr0 & 0x6) == 0x6;
        if (os_avx) {
            append_feature(out_buf, max_len, &len, "avx");
        }
    }

    if (max_leaf >= 7) {
        cpuid(7, &eax, &ebx, &ecx, &edx);
        if (os_avx && ((ebx >> 5) & 1)) {
            append_feature(out_buf, max_len, &len, "avx2");
        }
        if (os_avx && (xcr0 & 0xe6) == 0xe6 && ((ebx >> 16) & 1)) {
            append_feature(out_buf, max_len, &len, "avx512f");
        }
        if (os_avx && (xcr0 & 0xe6) == 0xe6 && ((ebx >> 17) & 1)) {
            append_feature(out_buf, max_len, &len, "avx512dq");
        }
        if (os_avx && (xcr0 & 0xe6) == 0xe6 && ((ebx >> 28) & 1)) {
            append_feature(out_buf, max_len, &len, "avx512cd");
        }
        if (os_avx && (xcr0 & 0xe6) == 0xe6 && ((ebx >> 30) & 1)) {
            append_feature(out_buf, max_len, &len, "avx512bw");
        }
        if (os_avx && (xcr0 & 0xe6) == 0xe6 && ((ebx >> 31) & 1)) {
            append_feature(out_buf, max_len, &len, "avx512vl");
        }
    }
#endif

    if (!is_x86 || len == 0) {
        FILE *f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char line[1024];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "Features", 8) == 0 ||
                    strncmp(line, "flags", 5) == 0) {
                    if (strstr(line, " neon") || strstr(line, " asimd")) {
                        append_feature(out_buf, max_len, &len, "neon");
                    }
                    if (strstr(line, " sve")) {
                        append_feature(out_buf, max_len, &len, "sve");
                    }
                    break;
                }
            }
            fclose(f);
        }
    }

    if (len == 0) {
        snprintf(out_buf, max_len, "basic");
    }
}

// Получить устройства из снимка ядра
// Прямое чтение портов PCI может пересечься с чтением ядра
static void get_pci_dump_raw(char *out_buf, size_t max_len) {
    if (out_buf == NULL || max_len == 0) {
        return;
    }

    out_buf[0] = '\0';
    size_t current_len = 0;
#ifdef STATS_LINUX
    // Linux предоставляет идентификаторы PCI через sysfs
    DIR *devices = opendir("/sys/bus/pci/devices");
    if (devices == NULL) {
        LOG("Cannot read PCI sysfs: %s", strerror(errno));
        return;
    }

    int dev_count = 0;
    struct dirent *entry;
    while ((entry = readdir(devices)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        unsigned ids[2] = {0};
        const char *fields[] = {"vendor", "device"};
        bool valid = true;
        for (size_t i = 0; i < 2; ++i) {
            char path[512];
            int n = snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/%s", entry->d_name, fields[i]);
            if (n < 0 || (size_t)n >= sizeof(path)) {
                valid = false;
                break;
            }

            FILE *file = fopen(path, "r");
            if (file == NULL) {
                valid = false;
                break;
            }

            int matched = fscanf(file, "%x", &ids[i]);
            fclose(file);
            if (matched != 1 || ids[i] > 0xfffe || (i == 0 && ids[i] == 0)) {
                valid = false;
                break;
            }
        }

        if (!valid) {
            continue;
        }

        int n = snprintf(out_buf + current_len, max_len - current_len,
            "%s%04x:%04x", current_len == 0 ? "" : ",", ids[0], ids[1]);
        if (n < 0 || (size_t)n >= max_len - current_len) {
            out_buf[current_len] = '\0';
            break;
        }

        current_len += (size_t)n;
        dev_count++;
    }

    closedir(devices);
#else
    FILE *pci = fopen("/proc/pci", "r");
    if (pci == NULL) {
        LOG("Cannot read /proc/pci: %s", strerror(errno));
        return;
    }

    int dev_count = 0;
    char line[128];

    while (fgets(line, sizeof(line), pci) != NULL) {
        unsigned segment, bus, slot, function, vendor, device;
        if (sscanf(line, "%x:%x:%x.%x %x:%x", &segment, &bus, &slot, &function,
                   &vendor, &device) != 6) {
            continue;
        }

        if (segment > 0xffff || bus > 0xff || slot > 0x1f || function > 7 ||
            vendor == 0 || vendor > 0xfffe || device > 0xfffe) {
            continue;
        }

        int written = snprintf(out_buf + current_len, max_len - current_len,
                               "%s%04x:%04x", current_len == 0 ? "" : ",", vendor, device);
        if (written < 0 || (size_t)written >= max_len - current_len) {
            out_buf[current_len] = '\0';
            break;
        }

        current_len += (size_t)written;
        dev_count++;
    }

    fclose(pci);
#endif
    LOG("PCI snapshot complete. Found %d valid devices.", dev_count);
}

static ssize_t udp_send(const char *host_ip, uint16_t port, const char *json);

// Отправить PCI-идентификаторы частями в пределах одного сеанса
static void send_pci_chunks(const char *server_ip, uint16_t port,
                            const char *hostname) {
    static char full_pci[4096];
    static bool scanned = false;
    if (!scanned) {
        get_pci_dump_raw(full_pci, sizeof(full_pci));
        scanned = full_pci[0] != '\0';
    }

    size_t total_len = strlen(full_pci);
    if (total_len == 0) {
        LOG("PCI dump skipped or failed.");
        return;
    }

    size_t device_count = 1;
    for (size_t i = 0; i < total_len; i++) {
        if (full_pci[i] == ',') {
            device_count++;
        }
    }

    size_t max_devices_per_chunk = 40;
    size_t total_chunks =
        (device_count + max_devices_per_chunk - 1) / max_devices_per_chunk;
    if (total_chunks == 0) {
        total_chunks = 1;
    }

    LOG("Sending PCI data in %zu chunks...", total_chunks);

    char work_pci[4096];
    strncpy(work_pci, full_pci, sizeof(work_pci));

    char *saveptr = NULL;
    char *token = strtok_r(work_pci, ",", &saveptr);

    char chunk_data[512] = {0};
    size_t current_len = 0;
    size_t chunk_num = 1;

    while (token != NULL) {
        size_t token_len = strlen(token);
        size_t needed = (current_len == 0) ? token_len : current_len + 1 + token_len;

        if (needed > max_devices_per_chunk * 10 - 1 && current_len > 0) {
            char json[1024];
            int n = snprintf(
                json, sizeof(json),
                "{\"type\":\"pci\",\"hostname\":\"%s\",\"report_id\":\"%s\",\"chunk\":%zu,\"total\":%zu,\"data\":\"%s\"}",
                hostname, hostname, chunk_num, total_chunks, chunk_data);

            if (n > 0 && (size_t)n < sizeof(json)) {
                LOG("PCI chunk %zu/%zu payload: [%s]", chunk_num, total_chunks,
                    chunk_data);
                udp_send(server_ip, port, json);
            }

            chunk_num++;
            current_len = 0;
            chunk_data[0] = '\0';
        }

        if (current_len == 0) {
            snprintf(chunk_data, sizeof(chunk_data), "%s", token);
            current_len = token_len;
        } else {
            snprintf(chunk_data + current_len, sizeof(chunk_data) - current_len, ",%s",
                     token);
            current_len += 1 + token_len;
        }

        token = strtok_r(NULL, ",", &saveptr);
    }

    if (current_len > 0) {
        char json[1024];
        int n = snprintf(
            json, sizeof(json),
            "{\"type\":\"pci\",\"hostname\":\"%s\",\"report_id\":\"%s\",\"chunk\":%zu,\"total\":%zu,\"data\":\"%s\"}",
            hostname, hostname, chunk_num, total_chunks, chunk_data);

        if (n > 0 && (size_t)n < sizeof(json)) {
            LOG("PCI chunk %zu/%zu payload: [%s]", chunk_num, total_chunks, chunk_data);
            udp_send(server_ip, port, json);
        }
    }
}

// Получить постоянный анонимный ID из локального ключа согласия
static bool derive_anon_id(const char *token, char out[65]) {
    if (token == NULL || out == NULL) {
        return false;
    }

    char input[96];
    int length = snprintf(input, sizeof(input), "synapse-monitor-node-v1:%s", token);
    if (length < 0 || (size_t)length >= sizeof(input)) {
        return false;
    }

    unsigned char digest[32];
    if (mbedtls_sha256((const unsigned char *)input, (size_t)length, digest, 0) != 0) {
        return false;
    }

    for (size_t index = 0; index < sizeof(digest); ++index) {
        snprintf(out + index * 2, 3, "%02x", digest[index]);
    }

    return true;
}

static char consent_token[65];

// Принимать только 256-битный случайный ключ в шестнадцатеричной записи
static bool valid_token(const char *token) {
    if (token == NULL || strlen(token) != 64) {
        return false;
    }
    for (size_t i = 0; i < 64; ++i) {
        if (!((token[i] >= '0' && token[i] <= '9') || (token[i] >= 'a' && token[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

// Проверить сертификат и получить отдельное подтверждение без перенаправлений
static bool consent_request(const char *operation, const char *token, const char *body) {
    if (!valid_token(token) || operation == NULL || body == NULL || strlen(body) > 2048) {
        return false;
    }
    struct tls transport;
    int rc = tls_init(&transport, 0, "/etc/ssl/certs/ca-certificates.crt", NULL);
    if (rc == 0) {
        rc = tls_connect(&transport, "synapseos.ru", "443");
    }
    char header[512];
    int n = snprintf(header, sizeof(header),
                     "POST /api/monitoring/%s HTTP/1.1\r\nHost: synapseos.ru\r\n"
                     "Authorization: Bearer %s\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                     operation, token, strlen(body));
    if (rc == 0 && n > 0 && (size_t)n < sizeof(header)) {
        rc = tls_write_all(&transport, header, (size_t)n);
        if (rc == 0) {
            rc = tls_write_all(&transport, body, strlen(body));
        }
    } else {
        rc = -1;
    }
    char response[8192];
    size_t used = 0;
    bool confirmed = false;
    while (rc == 0 && used + 1 < sizeof(response)) {
        int received = tls_read(&transport, response + used, sizeof(response) - used - 1);
        if (received <= 0) {
            break;
        }
        used += (size_t)received;
        response[used] = 0;
        if (strstr(response, "\r\n\r\n") != NULL) {
            confirmed = !strncmp(response, "HTTP/1.1 204 ", 13) ||
                        !strncmp(response, "HTTP/1.0 204 ", 13);
            break;
        }
    }
    tls_free(&transport);
    return confirmed;
}

// Сохранить состояние в существующем settings.ini без отдельного файла согласия
static bool consent_state(struct settings *s, const char *state, bool enabled) {
    return settings_set(s, settings_find("DataConsent", "State"), state) == 0 &&
           settings_set(s, settings_find("stats", "stats"), enabled ? "1" : "0") == 0 &&
           settings_save(s) == 0;
}

// Записать явный выбор до сети, повторить незавершённое действие без отправки отчётов
static int consent_change(bool accept, bool explicit_choice) {
    struct settings s;
    if (settings_load(&s, STATS_SETTINGS_ROOT)) {
        return 1;
    }
    const char *token = settings_get(&s, "DataConsent", "Token");
    const char *state = settings_get(&s, "DataConsent", "State");
    if (!explicit_choice && accept &&
        (strcmp(state, "pending") || strcmp(settings_get(&s, "DataConsent", "Version"), DATA_CONSENT_VERSION))) {
        return 1;
    }
    if (!explicit_choice && !accept && strcmp(state, "revoking") &&
        (strcmp(state, "active") || !strcmp(settings_get(&s, "stats", "stats"), "1"))) {
        return 0;
    }
    if (accept) {
        if (!strcmp(state, "revoking")) {
            fputs("Finish the pending withdrawal before giving a new consent\n", stderr);
            return 1;
        }
        if (strcmp(state, "pending") && strcmp(state, "active")) {
            unsigned char bytes[32];
            int fd = open("/dev/urandom", O_RDONLY);
            size_t used = 0;
            if (fd < 0) {
                return 1;
            }
            while (used < sizeof(bytes)) {
                ssize_t n = read(fd, bytes + used, sizeof(bytes) - used);
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                if (n <= 0) {
                    close(fd);
                    return 1;
                }
                used += (size_t)n;
            }
            close(fd);
            char generated[65];
            for (size_t i = 0; i < sizeof(bytes); ++i) {
                snprintf(generated + i * 2, 3, "%02x", bytes[i]);
            }
            if (settings_set(&s, settings_find("DataConsent", "Token"), generated)) {
                return 1;
            }
        }
        if (settings_set(&s, settings_find("DataConsent", "Version"), DATA_CONSENT_VERSION) ||
            !consent_state(&s, "pending", false)) {
            return 1;
        }
        token = settings_get(&s, "DataConsent", "Token");
        if (!consent_request("consent", token, "{\"version\":\"" DATA_CONSENT_VERSION "\"}")) {
            return 1;
        }
        return consent_state(&s, "active", true) ? 0 : 1;
    }
    if (!valid_token(token)) {
        return consent_state(&s, "revoked", false) ? 0 : 1;
    }
    // Отключение фиксируется до попытки связаться с сервером
    if (!consent_state(&s, "revoking", false)) {
        return 1;
    }
    if (!consent_request("revoke", token, "{}")) {
        return 1;
    }
    return consent_state(&s, "revoked", false) ? 0 : 1;
}

// Перечитывать локальный отзыв перед каждым пакетом и не передавать ключ по UDP
static ssize_t udp_send(const char *host_ip, uint16_t port, const char *json) {
    (void)host_ip;
    (void)port;
    if (print_packets) {
        puts(json);
        return (ssize_t)strlen(json);
    }
    struct settings s;
    if (settings_load(&s, STATS_SETTINGS_ROOT) || strcmp(settings_get(&s, "DataConsent", "State"), "active") ||
        strcmp(settings_get(&s, "stats", "stats"), "1") ||
        strcmp(settings_get(&s, "DataConsent", "Token"), consent_token)) {
        return -1;
    }
    return consent_request("report", consent_token, json) ? (ssize_t)strlen(json) : -1;
}

#include "performance.h"

// Показать состояние и идентификатор для обращения, не раскрывая секретный ключ
static int consent_status(void) {
    struct settings s;
    if (settings_load(&s, STATS_SETTINGS_ROOT)) {
        return 1;
    }
    printf("Consent state: %s\n", settings_get(&s, "DataConsent", "State"));
    const char *token = settings_get(&s, "DataConsent", "Token");
    if (valid_token(token)) {
        unsigned char digest[32];
        if (mbedtls_sha256((const unsigned char *)token, strlen(token), digest, 0)) {
            return 1;
        }
        fputs("Consent ID: ", stdout);
        for (size_t i = 0; i < sizeof(digest); ++i) {
            printf("%02x", digest[i]);
        }
        putchar('\n');
    }
    return 0;
}

// Запустить периодическую статистику либо локальную проверку счётчиков
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--performance")) {
        struct perf_sample first, second;
        if (!read_performance_file("/proc/performance", &first)) {
            fprintf(stderr, "/proc/performance is unavailable or unsupported\n");
            return 1;
        }
        sleep(1);
        if (!read_performance_file("/proc/performance", &second)) {
            return 1;
        }
        print_packets = true;
        send_performance(NULL, 0, "local", &second, &first, 1);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--consent-status")) {
        return consent_status();
    }
    if (argc == 2 && !strcmp(argv[1], "--consent-accept")) {
        return consent_change(true, true);
    }
    if (argc == 2 && !strcmp(argv[1], "--consent-revoke")) {
        return consent_change(false, true);
    }
    const char *server_ip = argc > 1 ? argv[1] : "synapseos.ru";
    char endpoint[64];
    uint16_t server_port = 55902;
    if (strlen(server_ip) >= sizeof(endpoint)) {
        fprintf(stderr, "Usage: %s [IPv4-or-hostname[:port]] [interval_seconds]\n", argv[0]);
        return 1;
    }
    strcpy(endpoint, server_ip);
    char *port_text = strchr(endpoint, ':');
    if (port_text) {
        *port_text++ = '\0';
        char *end;
        long port = strtol(port_text, &end, 10);
        if (!*port_text || *end || port < 1 || port > 65535) {
            fprintf(stderr, "Invalid UDP port\n");
            return 1;
        }
        server_port = (uint16_t)port;
    }
    server_ip = endpoint;
    if ((strcmp(server_ip, "synapseos.ru") && strcmp(server_ip, "185.177.216.71")) || server_port != 55902) {
        fputs("Consented reports use only https://synapseos.ru/api/monitoring/report\n", stderr);
        return 1;
    }

    unsigned int interval = 600;
    if (argc > 2) {
        char *end;
        long value = strtol(argv[2], &end, 10);
        if (!*argv[2] || *end || value < 1 || value > 86400) {
            fprintf(stderr, "Invalid monitoring interval (1..86400 seconds)\n");
            return 1;
        }
        interval = (unsigned int)value;
    }
    if (argc > 3) {
        fprintf(stderr, "Usage: %s [IPv4-or-hostname[:port]] [interval_seconds]\n", argv[0]);
        return 1;
    }

    struct settings initial;
    if (settings_load(&initial, STATS_SETTINGS_ROOT)) {
        return 1;
    }
    const char *initial_state = settings_get(&initial, "DataConsent", "State");
    if (!strcmp(initial_state, "revoking") ||
        (!strcmp(initial_state, "active") && strcmp(settings_get(&initial, "stats", "stats"), "1"))) {
        while (consent_change(false, false)) {
            sleep(30);
        }
        return 0;
    }
    if (!strcmp(initial_state, "pending")) {
        while (consent_change(true, false)) {
            struct settings current;
            if (settings_load(&current, STATS_SETTINGS_ROOT) ||
                strcmp(settings_get(&current, "DataConsent", "State"), "pending") ||
                strcmp(settings_get(&current, "DataConsent", "Version"), DATA_CONSENT_VERSION)) {
                return 1;
            }
            sleep(30);
        }
        if (settings_load(&initial, STATS_SETTINGS_ROOT)) {
            return 1;
        }
    }
    while (strcmp(settings_get(&initial, "DataConsent", "State"), "active") ||
           strcmp(settings_get(&initial, "DataConsent", "Version"), DATA_CONSENT_VERSION) ||
           !valid_token(settings_get(&initial, "DataConsent", "Token"))) {
        if (strcmp(settings_get(&initial, "stats", "stats"), "1")) {
            return 0;
        }

        sleep(30);
        if (settings_load(&initial, STATS_SETTINGS_ROOT)) {
            return 1;
        }
    }
    if (strcmp(settings_get(&initial, "stats", "stats"), "1")) {
        return 0;
    }
    snprintf(consent_token, sizeof(consent_token), "%s", settings_get(&initial, "DataConsent", "Token"));
    if (!consent_request("check", consent_token, "{}")) {
        return 1;
    }
    LOG("Consented report client starting");

    char anon_uuid[65];
    if (!derive_anon_id(consent_token, anon_uuid)) {
        LOG("Cannot derive anonymous monitoring ID");
        return 1;
    }
    LOG("Anonymous ID: %s", anon_uuid);

    char simd_features[512] = {0};
    get_simd_features(simd_features, sizeof(simd_features));
    LOG("Detected SIMD features: %s", simd_features);

    char simd_json[1024];
    int simd_n = snprintf(
        simd_json, sizeof(simd_json),
        "{\"type\":\"simd\",\"hostname\":\"%s\",\"report_id\":\"%s\",\"features\":\"%s\"}",
        anon_uuid, anon_uuid, simd_features);

    for (;;) {
        struct settings current;
        if (settings_load(&current, STATS_SETTINGS_ROOT)) {
            return 1;
        }
        if (strcmp(settings_get(&current, "stats", "stats"), "1") ||
            strcmp(settings_get(&current, "DataConsent", "State"), "active")) {
            while (consent_change(false, false)) {
                sleep(30);
            }
            return 0;
        }
        if (simd_n > 0 && (size_t)simd_n < sizeof(simd_json)) {
            udp_send(server_ip, server_port, simd_json);
        }
        send_pci_chunks(server_ip, server_port, anon_uuid);
        struct sysinfo s_i;
        if (sysinfo(&s_i) != 0) {
            LOG("sysinfo failed, retrying in %u sec. Error: %s", interval,
                strerror(errno));
            sleep(interval);
            continue;
        }

        tel_packet_t pkt = {0};
        pkt.uptime = (uint64_t)s_i.uptime;
        pkt.total_ram = (uint64_t)s_i.totalram * s_i.mem_unit;
        pkt.free_ram = (uint64_t)s_i.freeram * s_i.mem_unit;
        pkt.proc_count = (uint64_t)s_i.procs;

        LOG("System stats: Up=%llu, RAM=%lluMB, Free=%lluMB, Procs=%llu",
            (unsigned long long)pkt.uptime,
            (unsigned long long)pkt.total_ram / 1024 / 1024,
            (unsigned long long)pkt.free_ram / 1024 / 1024,
            (unsigned long long)pkt.proc_count);

        char json[1024];
        int n = snprintf(json, sizeof(json),
                         "{\"type\":\"sysinfo\",\"hostname\":\"%s\","
                         "\"interval\":%u,\"uptime\":%llu,"
                         "\"total_ram\":%llu,"
                         "\"free_ram\":%llu,"
                         "\"proc_count\":%llu}",
                         anon_uuid, interval, (unsigned long long)pkt.uptime,
                         (unsigned long long)pkt.total_ram,
                         (unsigned long long)pkt.free_ram,
                         (unsigned long long)pkt.proc_count);

        if (n < 0 || (size_t)n >= sizeof(json)) {
            LOG("JSON buffer overflow or formatting error.");
            sleep(interval);
            continue;
        }

        udp_send(server_ip, server_port, json);

        for (unsigned waited = 0; waited < interval; ++waited) {
            sleep(1);
            struct settings latest;
            if (settings_load(&latest, STATS_SETTINGS_ROOT) || strcmp(settings_get(&latest, "stats", "stats"), "1") ||
                strcmp(settings_get(&latest, "DataConsent", "State"), "active")) {
                break;
            }
        }
    }
    return 0;
}
