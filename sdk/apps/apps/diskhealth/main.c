// main.c: Состояние накопителей
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Запрос диагностических данных и вывод отчёта diskhealth
// Подсистема: пользовательские системные утилиты
// Первое изменение в истории Git: 13.10.2026
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

#include "report.h"
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
static bool digits(const char **p) {
    const char *start = *p;
    while (**p >= '0' && **p <= '9') {
        (*p)++;
    }
    return *p != start;
}
static bool whole_disk(const char *s) {
    if (!strncmp(s, "nvme", 4)) {
        s += 4;
        return digits(&s) && *s++ == 'n' && digits(&s) && !*s;
    }
    if ((s[0] != 'h' && s[0] != 's' && s[0] != 'v') || s[1] != 'd') {
        return false;
    }
    s += 2;
    const char *start = s;
    while (*s >= 'a' && *s <= 'z') {
        s++;
    }
    return s != start && !*s;
}
static int query(const char *name) {
    int fd = open(name, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", name, strerror(errno));
        return 2;
    }

    struct disk_health h;
    memset(&h, 0, sizeof(h));
    int r = ioctl(fd, DISK_HEALTH_GET, &h);
    int error = errno;
    close(fd);
    if (r < 0) {
        fprintf(stdout, "device=%s\nhealth=unavailable errno=%d (%s)\n", name, error,
                strerror(error));
        return 2;
    }
    return disk_health_report(stdout, name, &h);
}
int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        puts("Usage: diskhealth [DEVICE ...]\nWithout arguments, scan whole disks in "
             "/dev. Read-only NVMe/ATA diagnostics.\nExit: 0 no warning reported; 1 "
             "warning; 2 unavailable/incomplete.\nVirtual disks report the emulator's "
             "health, not the host physical disk.");
        return 0;
    }

    int result = 0;
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) {
                putchar('\n');
            }

            int r = query(argv[i]);
            if (r > result) {
                result = r;
            }
        }
        return result;
    }
    DIR *dir = opendir("/dev");
    if (!dir) {
        perror("/dev");
        return 2;
    }

    struct dirent *e;
    unsigned count = 0;
    while ((e = readdir(dir))) {
        if (whole_disk(e->d_name)) {
            char path[256];
            if (snprintf(path, sizeof(path), "/dev/%s", e->d_name) >=
                (int)sizeof(path)) {
                continue;
            }
            if (count++) {
                putchar('\n');
            }

            int r = query(path);
            if (r > result) {
                result = r;
            }
        }
    }
    closedir(dir);
    if (!count) {
        fprintf(stderr, "No whole disks found\n");
        return 2;
    }
    return result;
}
