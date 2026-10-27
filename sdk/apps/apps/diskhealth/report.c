// report.c: Вывод состояния накопителя
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Разбор и вывод диагностических данных NVMe и ATA
// Подсистема: пользовательские системные утилиты
// Первое изменение в истории Git: 13.10.2026
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

#include "report.h"
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
static uint16_t le16(const uint8_t *p) {
    return p[0] | (uint16_t)p[1] << 8;
}
static uint32_t le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le48(const uint8_t *p) {
    uint64_t n = 0;
    for (unsigned i = 0; i < 6; i++) {
        n |= (uint64_t)p[i] << (8 * i);
    }
    return n;
}
static void text(FILE *f, const char *label, const uint8_t *p, size_t n, bool swap) {
    char value[81];
    if (n > 80) {
        n = 80;
    }

    for (size_t i = 0; i < n; i++) {
        unsigned c = p[swap ? (i ^ 1) : i];
        value[i] = c >= 32 && c < 127 ? c : c ? '?' : ' ';
    }

    while (n && value[n - 1] == ' ') {
        n--;
    }
    value[n] = 0;
    fprintf(f, "%s=%s\n", label, value);
}
// Счётчики NVMe имеют размер 128 бит в little endian, выводим их без усечения
static void counter(FILE *f, const char *label, const uint8_t *p) {
    uint8_t v[16];
    memcpy(v, p, 16);
    char digits[40];
    unsigned count = 0;
    do {
        unsigned remainder = 0, nonzero = 0;
        for (int i = 15; i >= 0; i--) {
            unsigned n = (remainder << 8) | v[i];
            v[i] = n / 10;
            remainder = n % 10;
            nonzero |= v[i];
        }
        digits[count++] = '0' + remainder;
        if (!nonzero) {
            break;
        }
    } while (count < sizeof(digits));
    fprintf(f, "%s=", label);
    while (count) {
        fputc(digits[--count], f);
    }
    fputc('\n', f);
}
static void temperature(FILE *f, const char *label, unsigned k) {
    if (!k) {
        fprintf(f, "%s=unknown\n", label);
        return;
    }

    int milli = (int)k * 1000 - 273150;
    unsigned magnitude = milli < 0 ? -milli : milli;
    fprintf(f, "%s=%s%u.%03u C\n", label, milli < 0 ? "-" : "", magnitude / 1000,
            magnitude % 1000);
}
static bool page_valid(const uint8_t *p) {
    uint8_t sum = 0;
    for (unsigned i = 0; i < 512; i++) {
        sum += p[i];
    }
    return !sum && le16(p) != 0 && le16(p) != 65535;
}
int disk_health_report(FILE *f, const char *device, const struct disk_health *h) {
    fprintf(f, "device=%s\n", device);
    if (h->version != DISK_HEALTH_VERSION) {
        fprintf(f, "error=unsupported ABI version %u\n", h->version);
        return 2;
    }
    fprintf(f, "protocol=%s scope=%s query_errno=%u\n",
            h->protocol == DISK_HEALTH_NVME  ? "NVMe"
            : h->protocol == DISK_HEALTH_ATA ? "ATA"
                                             : "unknown",
            h->protocol == DISK_HEALTH_NVME ? "controller" : "device", h->error);
    if (h->flags & DISK_HEALTH_IDENTITY) {
        bool ata = h->protocol == DISK_HEALTH_ATA;
        text(f, "model", h->identity + (ata ? 54 : 24), 40, ata);
        text(f, "serial", h->identity + (ata ? 20 : 4), 20, ata);
        text(f, "firmware", h->identity + (ata ? 46 : 64), 8, ata);
    }
    if (!(h->flags & DISK_HEALTH_SUPPORTED)) {
        fprintf(f, "health=unavailable\n");
        return 2;
    }
    if (!(h->flags & DISK_HEALTH_ENABLED)) {
        fprintf(f, "health=disabled (no state changes requested)\n");
        return 2;
    }

    bool warning = !!(h->flags & DISK_HEALTH_FAILING);
    if ((h->flags & DISK_HEALTH_DATA_VALID) && h->protocol == DISK_HEALTH_NVME) {
        warning |= h->data[0] != 0 || h->data[5] >= 100;
    }
    if ((h->flags & DISK_HEALTH_DATA_VALID) &&
        (h->flags & DISK_HEALTH_THRESHOLDS_VALID) && h->protocol == DISK_HEALTH_ATA &&
        page_valid(h->data) && page_valid(h->thresholds)) {
        for (unsigned i = 0; i < 30; i++) {
            const uint8_t *a = h->data + 2 + i * 12;
            if (!a[0] || !(le16(a + 1) & 1)) {
                continue;
            }

            for (unsigned j = 0; j < 30; j++) {
                const uint8_t *t = h->thresholds + 2 + j * 12;
                if (t[0] == a[0] && t[1] && a[3] <= t[1]) {
                    warning = true;
                }
            }
        }
    }
    fprintf(f, "health=%s\n",
            h->flags & DISK_HEALTH_STATUS_VALID
                ? (warning ? "WARNING" : "no warning reported")
                : "unknown");
    if (!(h->flags & DISK_HEALTH_DATA_VALID)) {
        fprintf(f, "data=unavailable\n");
        return warning ? 1 : 2;
    }
    const uint8_t *p = h->data;
    if (h->protocol == DISK_HEALTH_NVME) {
        unsigned critical = p[0];
        fprintf(f, "critical_warning=0x%02x\n", critical);
        warning |= critical != 0;
        static const char *reasons[] = {
            "spare below threshold",  "temperature threshold",
            "reliability degraded",   "media read-only",
            "volatile backup failed", "persistent memory read-only/unreliable"};
        for (unsigned i = 0; i < 6; i++) {
            if (critical & (1u << i)) {
                fprintf(f, "warning=%s\n", reasons[i]);
            }
        }
        temperature(f, "temperature", le16(p + 1));
        fprintf(f, "available_spare=%u%% spare_threshold=%u%% percentage_used=%u%%\n",
                p[3], p[4], p[5]);
        if (p[5] >= 100) {
            warning = true;
            fprintf(f, "warning=estimated endurance consumed\n");
        }
        const char *labels[] = {
            "data_units_read",     "data_units_written",      "host_read_commands",
            "host_write_commands", "controller_busy_minutes", "power_cycles",
            "power_on_hours",      "unsafe_shutdowns",        "media_errors",
            "error_log_entries"};
        for (unsigned i = 0; i < 10; i++) {
            counter(f, labels[i], p + 32 + i * 16);
        }
        fprintf(f, "data_unit_bytes=512000 (rounded up by controller)\n");
        fprintf(f, "warning_temperature_minutes=%u critical_temperature_minutes=%u\n",
                le32(p + 192), le32(p + 196));
        for (unsigned i = 0; i < 8; i++) {
            if (le16(p + 200 + 2 * i)) {
                char label[32];
                snprintf(label, sizeof(label), "temperature_sensor_%u", i + 1);
                temperature(f, label, le16(p + 200 + 2 * i));
            }
        }
    } else if (h->protocol == DISK_HEALTH_ATA) {
        if (!page_valid(p)) {
            fprintf(f, "error=invalid SMART data checksum/revision\n");
            return 2;
        }

        bool thresholds =
            (h->flags & DISK_HEALTH_THRESHOLDS_VALID) && page_valid(h->thresholds);
        fprintf(f, "smart_revision=%u thresholds=%s\n", le16(p),
                thresholds ? "available" : "unavailable");
        fprintf(f, "id flags current worst threshold raw48 threshold_state\n");
        bool seen[256] = {0};
        for (unsigned i = 0; i < 30; i++) {
            const uint8_t *a = p + 2 + i * 12;
            unsigned id = a[0];
            if (!id) {
                continue;
            }
            if (seen[id]) {
                fprintf(f, "error=duplicate SMART attribute %u\n", id);
                return 2;
            }
            seen[id] = true;
            unsigned threshold = 0;
            bool found = false;
            if (thresholds) {
                for (unsigned j = 0; j < 30; j++) {
                    if (h->thresholds[2 + j * 12] == id) {
                        threshold = h->thresholds[3 + j * 12];
                        found = true;
                        break;
                    }
                }
            }

            bool failing = found && threshold && a[3] <= threshold;
            if (failing && (le16(a + 1) & 1)) {
                warning = true; // Атрибут предупреждения об отказе
            }
            fprintf(f, "%u 0x%04x %u %u ", id, le16(a + 1), a[3], a[4]);
            if (found) {
                fprintf(f, "%u", threshold);
            } else {
                fputc('-', f);
            }
            fprintf(f, " %" PRIu64 " %s\n", le48(a + 5),
                    !found       ? "unknown"
                    : !threshold ? "not-set"
                    : failing    ? "reached"
                                 : "above");
        }
        fprintf(f, "raw48 values and attribute meanings are vendor-specific; no "
                   "guessed temperature/wear conversion\n");
    } else {
        fprintf(f, "error=unsupported protocol\n");
        return 2;
    }
    return warning ? 1 : h->error || !(h->flags & DISK_HEALTH_STATUS_VALID) ? 2 : 0;
}
