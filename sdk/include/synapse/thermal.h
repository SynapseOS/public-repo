/*
    thermal.h: Чтение температур через /proc/thermal
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: Linux (ELF64)

    Описание: Пользовательский помощник чтения снимка температур
    Подсистема: диагностика оборудования
    Первое изменение: не установлено по истории Git
    Последнее изменение: 14.10.2026
    Доступ: коммерческий
*/

#ifndef SYNAPSE_THERMAL_H
#define SYNAPSE_THERMAL_H

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#define SYNAPSE_THERMAL_MAX_SAMPLES 32
struct synapse_thermal_sample {
    char path[128];
    int32_t temperature_millicelsius;
    int error;
};

// Прочитать свежий снимок температур, вернуть число записей либо -1 и errno
// Температуру можно использовать только при нулевом error у соответствующей записи
static inline int synapse_thermal_read(struct synapse_thermal_sample *samples,
                                       size_t capacity) {
    if (!samples || !capacity || capacity > SYNAPSE_THERMAL_MAX_SAMPLES) {
        errno = EINVAL;
        return -1;
    }

    FILE *file = fopen("/proc/thermal", "r");
    if (!file) {
        return -1;
    }

    char header[128];
    if (!fgets(header, sizeof(header), file)) {
        fclose(file);
        errno = EIO;
        return -1;
    }

    int count = 0;
    while ((size_t)count < capacity) {
        long long value;
        int status;
        int fields = fscanf(file, "%127s %lld %d", samples[count].path, &value, &status);
        if (fields == EOF && !ferror(file)) {
            break;
        }
        if (fields != 3 || value < INT32_MIN || value > INT32_MAX || status < 0) {
            fclose(file);
            errno = EIO;
            return -1;
        }
        samples[count].temperature_millicelsius = (int32_t)value;
        samples[count].error = status;
        count++;
    }

    fclose(file);
    return count;
}
#endif
