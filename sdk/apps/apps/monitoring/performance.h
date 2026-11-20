// performance.h: Счётчики и статистика системы
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: Счётчики и статистика системы в пользовательском пространстве
// Первое изменение: 13.10.2026
// Последнее изменение: 20.11.2026
// Доступ: общедоступный

#define PERF_MAX_CPUS 256
struct perf_cpu {
    unsigned id;
    uint64_t busy, idle, switches, interrupts;
};
struct perf_sample {
    uint64_t uptime;
    size_t count;
    struct perf_cpu cpus[PERF_MAX_CPUS];
};
static bool read_performance_file(const char *path, struct perf_sample *sample) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    memset(sample, 0, sizeof(*sample));
    char line[256];
    unsigned version = 0;
    size_t announced = 0;
    bool valid = true;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long a, b, c, d;
        unsigned id;
        if (sscanf(line, "version %u", &version) == 1) {
            continue;
        }
        if (sscanf(line, "uptime_us %llu", &a) == 1) {
            sample->uptime = a;
            continue;
        }
        if (sscanf(line, "cpu_count %zu", &announced) == 1) {
            continue;
        }
        if (line[0] == '#') {
            continue;
        }
        if (sscanf(line, "cpu %u %llu %llu %llu %llu", &id, &a, &b, &c, &d) != 5 ||
            sample->count >= PERF_MAX_CPUS || id != sample->count) {
            valid = false;
            break;
        }
        sample->cpus[sample->count++] = (struct perf_cpu){id, a, b, c, d};
    }
    valid = valid && !ferror(f) && version == 1 && sample->count &&
            announced == sample->count;
    fclose(f);
    return valid;
}

static int cpu_busy_permille(const struct perf_cpu *current,
                             const struct perf_cpu *previous) {
    if (!previous || previous->id != current->id || current->busy < previous->busy ||
        current->idle < previous->idle) {
        return -1;
    }
    uint64_t busy = current->busy - previous->busy,
             idle = current->idle - previous->idle;
    if (UINT64_MAX - busy < idle || !(busy + idle)) {
        return -1;
    }
    uint64_t total = busy + idle;
    if (total > UINT64_MAX / 1000) {
        busy >>= 10;
        total >>= 10;
    }
    return (int)(busy * 1000 / total);
}

static void send_performance(const char *ip, uint16_t port, const char *hostname,
                             const struct perf_sample *current,
                             const struct perf_sample *previous, uint64_t sequence) {
    const size_t per_packet = 4;
    size_t chunks = (current->count + per_packet - 1) / per_packet;
    for (size_t start = 0; start < current->count; start += per_packet) {
        char json[1200];
        int n = snprintf(
            json, sizeof(json),
            "{\"type\":\"performance\",\"version\":1,\"hostname\":\"%s\","
            "\"sequence\":%llu,\"uptime_us\":%llu,\"sample_us\":%llu,\"cpu_count\":%zu,"
            "\"chunk\":%zu,\"total\":%zu,\"cpus\":[",
            hostname, (unsigned long long)sequence, (unsigned long long)current->uptime,
            (unsigned long long)(previous && current->uptime >= previous->uptime
                                     ? current->uptime - previous->uptime
                                     : 0),
            current->count, start / per_packet + 1, chunks);
        if (n < 0 || (size_t)n >= sizeof(json)) {
            return;
        }

        size_t used = n;
        for (size_t i = start; i < current->count && i < start + per_packet; i++) {
            const struct perf_cpu *c = &current->cpus[i];
            const struct perf_cpu *p =
                previous && i < previous->count ? &previous->cpus[i] : NULL;
            int utilization = cpu_busy_permille(c, p);
            char value[16];
            if (utilization < 0) {
                strcpy(value, "null");
            } else {
                snprintf(value, sizeof(value), "%d", utilization);
            }
            n = snprintf(json + used, sizeof(json) - used,
                         "%s{\"id\":%u,\"busy_us\":%llu,\"idle_us\":%llu,"
                         "\"switches\":%llu,\"interrupts\":%llu,\"busy_permille\":%s}",
                         i == start ? "" : ",", c->id, (unsigned long long)c->busy,
                         (unsigned long long)c->idle, (unsigned long long)c->switches,
                         (unsigned long long)c->interrupts, value);
            if (n < 0 || (size_t)n >= sizeof(json) - used) {
                return;
            }
            used += n;
        }
        if (used + 3 > sizeof(json)) {
            return;
        }
        memcpy(json + used, "]}", 3);
        https_send(ip, port, json);
    }
}
