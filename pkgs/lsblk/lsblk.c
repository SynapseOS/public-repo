// lsblk.c: показать блочные устройства SynapseOS
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: synapse-x86_64-v1
// Файл: lsblk.c, подсистема: пользовательские утилиты
// Первое изменение: 30.10.2026, последнее изменение: 01.11.2026
// Статус доступа: общедоступный

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_DEVICES 256U

struct block_device {
    char name[96];
    char dev[32];
    char mountpoint[256];
    char fstype[32];
    uint64_t bytes;
    bool size_known;
    bool readonly;
    bool partition;
};

// Прочитать текстовый атрибут sysfs
static bool read_attribute(const char *root, const char *name, const char *attribute,
                           char *value, size_t length) {
    char path[PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/%s/%s", root, name, attribute);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return false;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }

    bool success = fgets(value, (int)length, file) != NULL;
    fclose(file);
    if (success) {
        value[strcspn(value, "\r\n")] = '\0';
    }

    return success;
}

// Разобрать целое число из sysfs
static bool read_number(const char *root, const char *name, const char *attribute, uint64_t *value) {
    char text[64];
    if (!read_attribute(root, name, attribute, text, sizeof(text))) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    unsigned long long number = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }

    *value = (uint64_t)number;
    return true;
}

// Найти устройство в собранном списке
static struct block_device *find_device(struct block_device *devices, size_t count, const char *name) {
    for (size_t index = 0; index < count; index++) {
        if (strcmp(devices[index].name, name) == 0) {
            return &devices[index];
        }
    }

    return NULL;
}

// Получить номер major:minor из stat для устройства devtmpfs
static void set_device_number(struct block_device *device, const struct stat *status) {
    unsigned long long number = (unsigned long long)status->st_rdev;
    if (number == 0U) {
        (void)snprintf(device->dev, sizeof(device->dev), "-");
        return;
    }

    unsigned major = (unsigned)(((number >> 8U) & 0xfffU) | ((number >> 32U) & 0xfffff000U));
    unsigned minor = (unsigned)((number & 0xffU) | ((number >> 12U) & 0xffffff00U));
    (void)snprintf(device->dev, sizeof(device->dev), "%u:%u", major, minor);
}

// Добавить опубликованные sysfs устройства
static size_t collect_sysfs(const char *root, struct block_device *devices) {
    DIR *directory = opendir(root);
    if (directory == NULL) {
        return 0;
    }

    size_t count = 0;
    struct dirent *item = NULL;
    while ((item = readdir(directory)) != NULL && count < MAX_DEVICES) {
        if (item->d_name[0] == '.') {
            continue;
        }

        struct block_device *device = &devices[count];
        memset(device, 0, sizeof(*device));
        if (snprintf(device->name, sizeof(device->name), "%s", item->d_name) >= (int)sizeof(device->name)) {
            continue;
        }

        char text[64];
        uint64_t sectors = 0;
        uint64_t readonly = 0;
        if (!read_attribute(root, device->name, "dev", text, sizeof(text)) ||
            !read_number(root, device->name, "size", &sectors)) {
            continue;
        }

        if (strlen(text) >= sizeof(device->dev)) {
            continue;
        }

        strcpy(device->dev, text);
        device->bytes = sectors > UINT64_MAX / 512U ? UINT64_MAX : sectors * 512U;
        device->size_known = true;
        if (read_number(root, device->name, "ro", &readonly)) {
            device->readonly = readonly != 0;
        }

        uint64_t partition = 0;
        device->partition = read_number(root, device->name, "partition", &partition) && partition != 0;
        count++;
    }

    closedir(directory);
    return count;
}

// Определить раздел по имени, если sysfs не опубликовала его тип
static bool is_partition_name(const char *name) {
    size_t length = strlen(name);
    if (length == 0 || name[length - 1] < '0' || name[length - 1] > '9') {
        return false;
    }

    if (strncmp(name, "nvme", 4) == 0 || strncmp(name, "mmcblk", 6) == 0 ||
        strncmp(name, "loop", 4) == 0) {
        const char *separator = strrchr(name, 'p');
        return separator != NULL && separator[1] != '\0';
    }

    return strncmp(name, "sd", 2) == 0 || strncmp(name, "vd", 2) == 0 ||
           strncmp(name, "hd", 2) == 0;
}

// Дополнить список устройствами devtmpfs, отсутствующими в sysfs
static size_t collect_devtmpfs(const char *root, struct block_device *devices, size_t count) {
    DIR *directory = opendir(root);
    if (directory == NULL) {
        return count;
    }

    struct dirent *item = NULL;
    while ((item = readdir(directory)) != NULL && count < MAX_DEVICES) {
        if (item->d_name[0] == '.' || find_device(devices, count, item->d_name) != NULL) {
            continue;
        }

        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", root, item->d_name) >= (int)sizeof(path)) {
            continue;
        }

        struct stat status;
        if (stat(path, &status) != 0 || !S_ISBLK(status.st_mode)) {
            continue;
        }

        struct block_device *device = &devices[count];
        memset(device, 0, sizeof(*device));
        if (snprintf(device->name, sizeof(device->name), "%s", item->d_name) >= (int)sizeof(device->name)) {
            continue;
        }

        set_device_number(device, &status);
        device->bytes = status.st_size > 0 ? (uint64_t)status.st_size : 0;
        device->size_known = status.st_size > 0;
        device->readonly = (status.st_mode & 0222) == 0;
        device->partition = is_partition_name(device->name);
        count++;
    }

    closedir(directory);
    return count;
}

// Прочитать точки монтирования из procfs
static void collect_mounts(const char *path, struct block_device *devices, size_t count) {
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), file) != NULL) {
        char source[256];
        char mountpoint[256];
        char fstype[32];
        if (sscanf(line, "%255s %255s %31s", source, mountpoint, fstype) != 3) {
            continue;
        }

        const char *name = strrchr(source, '/');
        if (name == NULL) {
            continue;
        }

        struct block_device *device = find_device(devices, count, name + 1);
        if (device != NULL) {
            (void)snprintf(device->mountpoint, sizeof(device->mountpoint), "%s", mountpoint);
            (void)snprintf(device->fstype, sizeof(device->fstype), "%s", fstype);
        }
    }

    fclose(file);
}

// Сравнить имена устройств для устойчивого вывода
static int compare_devices(const void *left, const void *right) {
    const struct block_device *first = left;
    const struct block_device *second = right;
    return strcmp(first->name, second->name);
}

// Отформатировать ёмкость диска
static void format_size(uint64_t bytes, bool exact, char *text, size_t length) {
    if (exact) {
        (void)snprintf(text, length, "%llu", (unsigned long long)bytes);
        return;
    }

    static const char *units[] = {"B", "K", "M", "G", "T", "P"};
    double amount = (double)bytes;
    size_t unit = 0;
    while (amount >= 1024.0 && unit + 1U < sizeof(units) / sizeof(units[0])) {
        amount /= 1024.0;
        unit++;
    }

    (void)snprintf(text, length, unit == 0 ? "%.0f%s" : "%.1f%s", amount, units[unit]);
}

// Показать справку по ключам
static void usage(FILE *stream) {
    fprintf(stream, "Usage: lsblk [-b] [-n] [-p] [--list] [-o NAME,FSTYPE] [DEVICE]\n");
}

// Перечислить блочные устройства
int main(int argc, char **argv) {
    bool exact = false;
    bool headings = true;
    bool full_paths = false;
    bool name_fstype = false;
    const char *target = NULL;
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "-b") == 0) {
            exact = true;
        } else if (strcmp(argv[index], "-n") == 0) {
            headings = false;
        } else if (strcmp(argv[index], "-p") == 0) {
            full_paths = true;
        } else if (strcmp(argv[index], "--list") == 0) {
            continue;
        } else if (strcmp(argv[index], "--noheadings") == 0) {
            headings = false;
        } else if (strcmp(argv[index], "-o") == 0 && index + 1 < argc &&
                   strcmp(argv[index + 1], "NAME,FSTYPE") == 0) {
            name_fstype = true;
            index++;
        } else if (strcmp(argv[index], "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (argv[index][0] == '/' && target == NULL) {
            const char *basename = strrchr(argv[index], '/');
            target = basename == NULL ? argv[index] : basename + 1;
        } else {
            usage(stderr);
            return 2;
        }
    }

    const char *sysfs = getenv("SYNAPSE_BLOCK_ROOT");
    const char *devtmpfs = getenv("SYNAPSE_DEV_ROOT");
    const char *mounts = getenv("SYNAPSE_PROC_MOUNTS");
    if (sysfs == NULL || *sysfs == '\0') {
        sysfs = "/sys/block";
    }
    if (devtmpfs == NULL || *devtmpfs == '\0') {
        devtmpfs = "/dev";
    }
    if (mounts == NULL || *mounts == '\0') {
        mounts = "/proc/mounts";
    }

    struct block_device devices[MAX_DEVICES];
    size_t count = collect_sysfs(sysfs, devices);
    count = collect_devtmpfs(devtmpfs, devices, count);
    collect_mounts(mounts, devices, count);
    qsort(devices, count, sizeof(devices[0]), compare_devices);

    if (headings) {
        puts(name_fstype ? "NAME FSTYPE" : "NAME         MAJ:MIN   SIZE RO TYPE  MOUNTPOINT");
    }

    size_t displayed = 0;
    for (size_t index = 0; index < count; index++) {
        if (target != NULL && strncmp(devices[index].name, target, strlen(target)) != 0) {
            continue;
        }

        displayed++;
        if (name_fstype) {
            printf("%s %s\n", devices[index].name, devices[index].fstype);
            continue;
        }

        char size[32];
        if (devices[index].size_known) {
            format_size(devices[index].bytes, exact, size, sizeof(size));
        } else {
            (void)snprintf(size, sizeof(size), "-");
        }

        printf("%s%-12s %-7s %6s %u %-5s %s\n", full_paths ? "/dev/" : "", devices[index].name,
               devices[index].dev, size, devices[index].readonly ? 1U : 0U,
               devices[index].partition ? "part" : "disk", devices[index].mountpoint);
    }

    if (displayed == 0) {
        fprintf(stderr, "lsblk: no block devices found in %s or %s\n", sysfs, devtmpfs);
        return 1;
    }

    return 0;
}
