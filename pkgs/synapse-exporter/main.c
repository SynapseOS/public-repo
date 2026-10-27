/*
    main.c: Экспорт системных счётчиков для Prometheus
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI: Linux amd64 ABI

    Описание: Отдаёт снимки CPU, памяти, процессов, сети и диска
    Подсистема: мониторинг
    Первое изменение: 23.10.2026
    Последнее изменение: 23.10.2026
    Доступ: общедоступный
*/

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <lumine/process.h>

#define METRICS_SIZE 131072
#define PROCESS_LIMIT 512

struct metrics {
    char data[METRICS_SIZE];
    size_t length;
    bool failed;
};

// Дописать строку метрик с проверкой свободного места
static void add(struct metrics *metrics, const char *format, ...) {
    if (metrics->failed) {
        return;
    }

    va_list args;
    va_start(args, format);
    int written = vsnprintf(metrics->data + metrics->length,
        sizeof(metrics->data) - metrics->length, format, args);
    va_end(args);

    if (written < 0 || (size_t)written >= sizeof(metrics->data) - metrics->length) {
        metrics->failed = true;
        return;
    }

    metrics->length += (size_t)written;
}

// Снять накопленные счётчики каждого процессора
static bool cpu_metrics(struct metrics *metrics) {
    FILE *file = fopen("/proc/performance", "r");
    if (file == NULL) {
        return false;
    }

    add(metrics, "# TYPE synapse_cpu_busy_seconds_total counter\n");
    add(metrics, "# TYPE synapse_cpu_idle_seconds_total counter\n");
    add(metrics, "# TYPE synapse_cpu_context_switches_total counter\n");
    add(metrics, "# TYPE synapse_cpu_interrupts_total counter\n");

    char line[256];
    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned cpu;
        unsigned long long busy, idle, switches, interrupts;
        if (sscanf(line, "cpu %u %llu %llu %llu %llu", &cpu, &busy, &idle,
            &switches, &interrupts) != 5) {
            continue;
        }

        add(metrics, "synapse_cpu_busy_seconds_total{cpu=\"%u\"} %.6f\n", cpu,
            (double)busy / 1000000.0);
        add(metrics, "synapse_cpu_idle_seconds_total{cpu=\"%u\"} %.6f\n", cpu,
            (double)idle / 1000000.0);
        add(metrics, "synapse_cpu_context_switches_total{cpu=\"%u\"} %llu\n", cpu, switches);
        add(metrics, "synapse_cpu_interrupts_total{cpu=\"%u\"} %llu\n", cpu, interrupts);
    }

    bool okay = !ferror(file);
    fclose(file);
    return okay;
}

// Перевести поля /proc/meminfo в байты
static bool memory_metrics(struct metrics *metrics) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (file == NULL) {
        return false;
    }

    add(metrics, "# TYPE synapse_memory_bytes gauge\n");

    char line[256];
    while (fgets(line, sizeof(line), file) != NULL) {
        char name[40];
        unsigned long long kib;
        if (sscanf(line, "%39[^:]: %llu kB", name, &kib) != 2) {
            continue;
        }

        add(metrics, "synapse_memory_bytes{kind=\"%s\"} %llu\n", name, kib * 1024ULL);
    }

    bool okay = !ferror(file);
    fclose(file);
    return okay;
}

// Снять память и накопленное время CPU по процессам
static bool process_metrics(struct metrics *metrics) {
    struct lumine_process_info_v3 *processes = calloc(PROCESS_LIMIT, sizeof(*processes));
    if (processes == NULL) {
        return false;
    }

    long count = syscall(LUMINE_PROCESS_SNAPSHOT_V3, processes, PROCESS_LIMIT, sizeof(*processes));
    if (count < 0 || count > PROCESS_LIMIT) {
        free(processes);
        return false;
    }

    add(metrics, "# TYPE synapse_process_info gauge\n");
    add(metrics, "# TYPE synapse_process_resident_bytes gauge\n");
    add(metrics, "# TYPE synapse_process_kernel_bytes gauge\n");
    add(metrics, "# TYPE synapse_process_cpu_seconds_total counter\n");
    add(metrics, "# TYPE synapse_process_snapshot_truncated gauge\n");
    add(metrics, "synapse_process_snapshot_truncated %d\n", count == PROCESS_LIMIT ? 1 : 0);

    for (long index = 0; index < count; index++) {
        const struct lumine_process_info_v3 *process = &processes[index];
        char name[sizeof(process->base.base.name) * 2 + 1];
        size_t position = 0;

        for (size_t offset = 0; offset < sizeof(process->base.base.name) &&
            process->base.base.name[offset] != '\0'; offset++) {
            char character = process->base.base.name[offset];
            if (character == '\\' || character == '"') {
                name[position++] = '\\';
                name[position++] = character;
            } else if (character == '\n') {
                name[position++] = '\\';
                name[position++] = 'n';
            } else {
                name[position++] = character;
            }
        }

        name[position] = '\0';
        add(metrics, "synapse_process_info{pid=\"%lld\",name=\"%s\"} 1\n",
            (long long)process->base.base.pid, name);
        add(metrics, "synapse_process_resident_bytes{pid=\"%lld\"} %llu\n",
            (long long)process->base.base.pid,
            (unsigned long long)process->base.resident_bytes);
        add(metrics, "synapse_process_kernel_bytes{pid=\"%lld\"} %llu\n",
            (long long)process->base.base.pid,
            (unsigned long long)process->kernel_bytes);
        add(metrics, "synapse_process_cpu_seconds_total{pid=\"%lld\"} %.6f\n",
            (long long)process->base.base.pid, (double)process->base.cpu_us / 1000000.0);
    }

    free(processes);
    return count < PROCESS_LIMIT;
}

// Снять сетевые байты и пакеты из отдельного снимка ядра
static bool network_metrics(struct metrics *metrics) {
    FILE *file = fopen("/proc/net-stats", "r");
    if (file == NULL) {
        return false;
    }

    add(metrics, "# TYPE synapse_network_bytes_total counter\n");
    add(metrics, "# TYPE synapse_network_packets_total counter\n");

    char line[256];
    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned device;
        unsigned long long receive_bytes, transmit_bytes, receive_packets, transmit_packets;
        if (sscanf(line, "%u %llu %llu %llu %llu", &device, &receive_bytes,
            &transmit_bytes, &receive_packets, &transmit_packets) != 5) {
            continue;
        }

        add(metrics, "synapse_network_bytes_total{device=\"%u\",direction=\"receive\"} %llu\n",
            device, receive_bytes);
        add(metrics, "synapse_network_bytes_total{device=\"%u\",direction=\"transmit\"} %llu\n",
            device, transmit_bytes);
        add(metrics, "synapse_network_packets_total{device=\"%u\",direction=\"receive\"} %llu\n",
            device, receive_packets);
        add(metrics, "synapse_network_packets_total{device=\"%u\",direction=\"transmit\"} %llu\n",
            device, transmit_packets);
    }

    bool okay = !ferror(file);
    fclose(file);
    return okay;
}

// Снять ёмкость и доступный объём корневой файловой системы
static bool filesystem_metrics(struct metrics *metrics) {
    struct statvfs info;
    if (statvfs("/", &info) < 0) {
        return false;
    }

    add(metrics, "# TYPE synapse_filesystem_size_bytes gauge\n");
    add(metrics, "# TYPE synapse_filesystem_available_bytes gauge\n");
    add(metrics, "synapse_filesystem_size_bytes{mountpoint=\"/\"} %llu\n",
        (unsigned long long)info.f_blocks * info.f_frsize);
    add(metrics, "synapse_filesystem_available_bytes{mountpoint=\"/\"} %llu\n",
        (unsigned long long)info.f_bavail * info.f_frsize);
    return true;
}

// Собрать ответ для одного обращения
static void collect(struct metrics *metrics) {
    memset(metrics, 0, sizeof(*metrics));
    bool okay = cpu_metrics(metrics);
    okay = memory_metrics(metrics) && okay;
    okay = process_metrics(metrics) && okay;
    okay = network_metrics(metrics) && okay;
    okay = filesystem_metrics(metrics) && okay;
    add(metrics, "# TYPE synapse_exporter_collect_success gauge\n");
    add(metrics, "synapse_exporter_collect_success %d\n", okay ? 1 : 0);
}

// Передать весь HTTP-ответ, включая короткие записи в сокет
static bool send_all(int client, const char *data, size_t length) {
    while (length > 0) {
        ssize_t written = send(client, data, length, 0);
        if (written < 0 && errno == EINTR) {
            continue;
        }

        if (written <= 0) {
            return false;
        }

        data += written;
        length -= (size_t)written;
    }

    return true;
}

// Проверить запрос и отдать текстовые метрики
static void serve(int client) {
    char request[1024];
    ssize_t received = recv(client, request, sizeof(request) - 1, 0);
    if (received <= 0) {
        return;
    }

    request[received] = 0;
    if (strncmp(request, "GET /metrics ", 13) != 0) {
        static const char response[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(client, response, sizeof(response) - 1);
        return;
    }

    struct metrics metrics;
    collect(&metrics);
    if (metrics.failed) {
        static const char response[] = "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(client, response, sizeof(response) - 1);
        return;
    }

    char header[256];
    int length = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n", metrics.length);
    if (length < 0 || (size_t)length >= sizeof(header)) {
        return;
    }

    if (send_all(client, header, (size_t)length)) {
        send_all(client, metrics.data, metrics.length);
    }
}

// Открыть HTTP-сокет и обслуживать последовательные запросы
int main(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        perror("synapse-exporter: socket");
        return 1;
    }

    int reuse = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("synapse-exporter: reuseaddr");
        close(listener);
        return 1;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(9100);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(listener, 16) < 0) {
        perror("synapse-exporter: listen");
        close(listener);
        return 1;
    }

    for (;;) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("synapse-exporter: accept");
            break;
        }

        serve(client);
        close(client);
    }

    close(listener);
    return 1;
}
