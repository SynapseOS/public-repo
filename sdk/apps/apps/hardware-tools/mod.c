// mod.c: Управление модулями ядра
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Загрузка, выгрузка и безопасная замена образа модуля
// Подсистема: пользовательские системные утилиты
// Первое изменение: до текущей уборки, исходная дата не установлена
// Последнее изменение: 15.10.2026
// Доступ: общедоступный

// Загрузка, выгрузка и замена модулей через нативный API без запуска оболочки
#include <elf.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define UNLOAD_SAFE 1u
struct native_info {
    uint64_t magic;
    uint32_t abi, flags;
    char name[64], version[32], license[32];
    uint64_t init, fini;
    char arch[16];
};
struct image {
    unsigned char *data;
    size_t size;
    struct native_info info;
};
// Проверить имя модуля перед построением пути
static int valid_name(const char *s) {
    size_t n = strlen(s);
    if (!n || n >= 64) {
        return 0;
    }

    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) {
            return 0;
        }
    }
    return 1;
}
// Проверить границы участка внутри файла
static int range(size_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= size - offset;
}
// Прочитать ELF и проверить метаданные до загрузки или замены модуля
static int read_image(const char *arg, struct image *image) {
    char path[512];
    if (strchr(arg, '/')) {
        if (strlen(arg) >= sizeof(path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(path, arg);
    } else {
        if (!valid_name(arg)) {
            errno = EINVAL;
            return -1;
        }
        snprintf(path, sizeof(path), "/lib/modules/%s.so", arg);
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st)) {
        close(fd);
        return -1;
    }
    if (st.st_size < (off_t)sizeof(Elf64_Ehdr) || st.st_size > 8 * 1024 * 1024) {
        close(fd);
        errno = ENOEXEC;
        return -1;
    }
    image->size = st.st_size;
    image->data = malloc(image->size);
    if (!image->data) {
        close(fd);
        return -1;
    }

    size_t done = 0;
    while (done < image->size) {
        ssize_t n = read(fd, image->data + done, image->size - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            close(fd);
            free(image->data);
            image->data = NULL;
            errno = EIO;
            return -1;
        }
        done += n;
    }
    close(fd);
    Elf64_Ehdr eh;
    memcpy(&eh, image->data, sizeof(eh));
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_machine != EM_X86_64 ||
        eh.e_shentsize != sizeof(Elf64_Shdr) || !eh.e_shnum ||
        eh.e_shstrndx >= eh.e_shnum ||
        !range(image->size, eh.e_shoff, (uint64_t)eh.e_shnum * sizeof(Elf64_Shdr))) {
        goto invalid;
    }
    Elf64_Shdr strings;
    memcpy(&strings, image->data + eh.e_shoff + (size_t)eh.e_shstrndx * sizeof(strings),
           sizeof(strings));
    if (!range(image->size, strings.sh_offset, strings.sh_size)) {
        goto invalid;
    }

    for (unsigned i = 0; i < eh.e_shnum; i++) {
        Elf64_Shdr sh;
        memcpy(&sh, image->data + eh.e_shoff + i * sizeof(sh), sizeof(sh));
        if (sh.sh_name >= strings.sh_size) {
            continue;
        }
        const char *name = (void *)(image->data + strings.sh_offset + sh.sh_name);
        size_t remaining = strings.sh_size - sh.sh_name;
        if (!memchr(name, 0, remaining)) {
            goto invalid;
        }
        if (strcmp(name, ".syn_modinfo")) {
            continue;
        }
        if (sh.sh_size < sizeof(image->info) ||
            !range(image->size, sh.sh_offset, sh.sh_size)) {
            goto invalid;
        }
        memcpy(&image->info, image->data + sh.sh_offset, sizeof(image->info));
        if (image->info.magic != 0x53594e4d4f4431ULL || image->info.abi != 2 ||
            !memchr(image->info.name, 0, 64) || !memchr(image->info.version, 0, 32) ||
            !memchr(image->info.license, 0, 32) || !memchr(image->info.arch, 0, 16) ||
            strcmp(image->info.arch, "amd64") || !valid_name(image->info.name)) {
            goto invalid;
        }
        return 0;
    }
invalid:
    free(image->data);
    image->data = NULL;
    errno = ENOEXEC;
    return -1;
}
// Найти загруженный модуль и показать сведения из снимка реестра
static int status(const char *name, int verbose) {
    FILE *f = fopen("/proc/modules", "r");
    if (!f) {
        return -1;
    }

    char line[512], module[64], state[32], version[32], license[32], arch[16];
    unsigned refs, safe, abi;
    unsigned long long size;
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        strcpy(arch, "unknown");
        abi = 0;
        int fields = sscanf(line, "%63s %llu %u %*s %31s %*s %31s %31s %u %15s %u",
                            module, &size, &refs, state, version, license, &safe, arch, &abi);
        if (fields != 7 && fields != 9) {
            continue;
        }
        if (strcmp(module, name)) {
            continue;
        }
        found = 1;
        if (verbose) {
            printf("Name: %s\nStatus: %s\nVersion: %s\nLicense: %s\nMemory: %llu "
                   "bytes\nReferences: %u\nArchitecture: %s\nABI: %u\nUnload: %s\n",
                   module, state, version, license, size, refs, arch, abi,
                   !safe      ? "blocked: driver teardown not declared"
                   : refs > 1 ? "blocked: module is in use"
                              : "supported");
        }
        break;
    }

    int error = ferror(f);
    fclose(f);
    if (error) {
        return -1;
    }
    return found;
}
// Вывести список модулей с архитектурой и версией формата
static int list(void) {
    FILE *f = fopen("/proc/modules", "r");
    if (!f) {
        return -1;
    }
    puts("Name Size References Users Status Address Version License UnloadSafe Arch ABI");
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f))) {
        fwrite(buf, 1, n, stdout);
    }

    int bad = ferror(f) || ferror(stdout);
    fclose(f);
    return bad ? -1 : 0;
}
// Выполнить выбранное действие над модулем
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "list")) {
        if (!list()) {
            return 0;
        }
        perror("mod list");
        return 1;
    }
    if (argc != 3) {
        goto usage;
    }
    const char *command = argv[1], *arg = argv[2];
    struct image image = {0};
    int info = !strcmp(command, "info") || !strcmp(command, "status"),
        load = !strcmp(command, "load"), unload = !strcmp(command, "unload"),
        restart = !strcmp(command, "restart") || !strcmp(command, "reboot") ||
                  !strcmp(command, "reload");
    if (!info && !load && !unload && !restart) {
        goto usage;
    }
    if (info && !strchr(arg, '/')) {
        int found = status(arg, 1);
        if (found > 0) {
            return 0;
        }
        if (found < 0) {
            perror("mod status");
            return 1;
        }
    }
    if (unload) {
        if (!valid_name(arg)) {
            errno = EINVAL;
            goto error;
        }
        if (syscall(SYS_delete_module, arg, 0) < 0) {
            goto error;
        }
        printf("mod: %s unloaded\n", arg);
        return 0;
    }
    // Проверяем и сохраняем новый образ до изменения работающего модуля
    if (read_image(arg, &image)) {
        goto error;
    }
    if (info) {
        printf("Name: %s\nStatus: not loaded\nVersion: %s\nLicense: %s\nArchitecture: %s\nABI: "
               "%u\nUnload contract: %s\n",
               image.info.name, image.info.version, image.info.license, image.info.arch, image.info.abi,
               image.info.flags & UNLOAD_SAFE ? "supported" : "not declared");
        free(image.data);
        return 0;
    }
    if (restart && syscall(SYS_delete_module, image.info.name, 0) < 0) {
        goto error;
    }
    if (syscall(SYS_init_module, image.data, image.size, "") < 0) {
        if (restart) {
            fprintf(stderr,
                    "mod: previous instance unloaded; replacement load failed\n");
        }
        goto error;
    }
    printf("mod: %s %s\n", image.info.name, restart ? "restarted" : "loaded");
    free(image.data);
    return 0;
error:
    fprintf(stderr, "mod %s %s: %s\n", command, arg, strerror(errno));
    if (errno == EBUSY) {
        fprintf(stderr, "Use mod info to inspect references and the unload contract; "
                        "no forced unload is performed.\n");
    }
    free(image.data);
    return 1;
usage:
    fprintf(stderr, "usage: mod list | mod info|status|load|unload|restart|reboot "
                    "<name or path>\n");
    return 2;
}
