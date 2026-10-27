// thermal.c: Чтение температурных зон
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Просмотр доступных зон ACPI и их состояния
// Подсистема: пользовательские системные утилиты
// Первое изменение: до текущей уборки, исходная дата не установлена
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

#include <synapse/thermal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
static int show(int raw) {
    struct synapse_thermal_sample samples[SYNAPSE_THERMAL_MAX_SAMPLES];
    int n = synapse_thermal_read(samples, SYNAPSE_THERMAL_MAX_SAMPLES);
    if (n < 0) {
        fprintf(stderr, "thermal: %s\n", strerror(errno));
        return 1;
    }
    if (!n) {
        puts("No ACPI thermal zones reported by firmware.");
        return 1;
    }
    puts(raw ? "zone temperature_millicelsius errno"
             : "ACPI zone                         Temperature");
    for (int i = 0; i < n; i++) {
        if (raw) {
            printf("%s %d %d\n", samples[i].path, samples[i].temperature_millicelsius,
                   samples[i].error);
        } else if (samples[i].error) {
            printf("%-32s unavailable: %s\n", samples[i].path,
                   strerror(samples[i].error));
        } else {
            long long value = samples[i].temperature_millicelsius,
                      magnitude = value < 0 ? -value : value;
            printf("%-32s %s%lld.%03lld C\n", samples[i].path, value < 0 ? "-" : "",
                   magnitude / 1000, magnitude % 1000);
        }
    }
    return 0;
}
int main(int argc, char **argv) {
    unsigned interval = 0;
    int raw = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--raw")) {
            raw = 1;
        } else if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--watch")) {
            interval = 2;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                char *end;
                unsigned long n = strtoul(argv[++i], &end, 10);
                if (*end || !n || n > 3600) {
                    goto usage;
                }
                interval = n;
            }
        } else {
            goto usage;
        }
    }
    do {
        if (interval) {
            printf("\n--- ACPI temperatures, epoch %lld ---\n", (long long)time(NULL));
        }

        int result = show(raw);
        fflush(stdout);
        if (!interval) {
            return result;
        }
        sleep(interval);
    } while (1);
usage:
    fprintf(stderr, "usage: thermal [--raw] [-w [seconds: 1..3600]]\n");
    return 2;
}
