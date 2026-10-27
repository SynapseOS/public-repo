// hwinfo.c: Сведения об устройствах компьютера
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Чтение PCI, ACPI и блочных устройств через нативный API
// Подсистема: пользовательские системные утилиты
// Первое изменение: до текущей уборки, исходная дата не установлена
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <synapse/gpu.h>
#include <synapse/block.h>
static void report(const char *title, const char *path) {
    printf("\n[%s]\n", title);
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("Unavailable: %s (%s)\n", path, strerror(errno));
        return;
    }

    char buf[4096];
    size_t n, total = 0;
    while ((n = fread(buf, 1, sizeof(buf), f))) {
        fwrite(buf, 1, n, stdout);
        total += n;
    }
    if (ferror(f)) {
        printf("Read error: %s\n", strerror(errno));
    } else if (!total) {
        puts("(none)");
    }
    fclose(f);
}
static void disks(void) {
    puts("\n[Block devices]");
    DIR *dir = opendir("/dev");
    if (!dir) {
        perror("/dev");
        return;
    }

    struct dirent *entry;
    struct stat st;
    while ((entry = readdir(dir))) {
        char path[512];
        snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
        if (stat(path, &st) || !S_ISBLK(st.st_mode)) {
            continue;
        }
        const char *state = "status unsupported";
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            if (!ioctl(fd, SYN_BLOCK_ONLINE, 0)) {
                state = "online";
            } else if (errno == ENODEV) {
                state = "disconnected";
            } else if (errno == EIO) {
                state = "I/O failed";
            }
            close(fd);
        } else {
            state = "cannot open";
        }
        printf("%-24s %lld bytes  %s\n", path, (long long)st.st_size, state);
    }
    closedir(dir);
}
static void graphics(void) {
    puts("\n[Graphics]");
    int fd = open("/dev/fb0", O_RDONLY);
    struct syn_gpu_info info = {0};
    if (fd < 0) {
        perror("/dev/fb0");
        return;
    }
    if (ioctl(fd, SYN_FB_GPU_INFO, &info) < 0) {
        printf("GPU query unavailable: %s\n", strerror(errno));
    } else {
        printf("Backend: %.32s\nMode: %ux%u, pitch %u\nCapabilities: native=%u "
               "cursor=%u backlight=%u mirror=%u; pipes=0x%x\n",
               info.name, info.width, info.height, info.pitch,
               !!(info.capabilities & SYN_GPU_NATIVE_SCANOUT),
               !!(info.capabilities & SYN_GPU_HW_CURSOR),
               !!(info.capabilities & SYN_GPU_PWM_BACKLIGHT),
               !!(info.capabilities & SYN_GPU_MIRROR), info.pipe_mask);
    }
    close(fd);
}
int main(int argc, char **argv) {
    const char *section = argc == 1                                    ? "all"
                          : argc == 3 && !strcmp(argv[1], "--section") ? argv[2]
                                                                       : NULL;
    const char *names[] = {"system",  "memory", "pci",    "usb",   "modules",
                           "thermal", "acpi",   "errors", "disks", "graphics"};
    const char *paths[] = {"/proc/version", "/proc/meminfo",  "/proc/pci",
                           "/proc/usb",     "/proc/modules",  "/proc/thermal",
                           "/proc/acpi",    "/proc/hw_errors"};
    int found = 0;
    if (section) {
        for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++) {
            if (!strcmp(section, "all") || !strcmp(section, names[i])) {
                found = 1;
                if (i == 8) {
                    disks();
                } else if (i == 9) {
                    graphics();
                } else {
                    report(names[i], paths[i]);
                }
                if (i == 0) {
                    report("CPU", "/proc/cpuinfo");
                    report("Boot command line", "/proc/cmdline");
                    report("Machine", "/proc/dmi/product_name");
                }
            }
        }
    }
    if (!found) {
        fprintf(stderr,
                "usage: hwinfo [--section "
                "system|memory|pci|usb|modules|thermal|acpi|errors|disks|graphics]\n");
        return 2;
    }
    return ferror(stdout) ? 1 : 0;
}
