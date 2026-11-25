// synapse-translate.c: Файловая граница BSD сервера прошивок Qualcomm
// Автор: Елчинян Арен Артурович 2026
// Архитектура: AArch64 и x86_64, ABI: POSIX
// Описание: Обычные файлы прошивок и временный каталог, без Linux sysfs и /dev/mem
// Подсистема: публичные пакеты
// Первое изменение: 25.11.2026
// Последнее изменение: 25.11.2026
// Доступ: общедоступный
// SPDX-License-Identifier: BSD-3-Clause
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int sanitize_path(const char *path);
int translate_open(const char *path, int flags);

// Сетевые имена не могут содержать пустые, текущие или родительские компоненты
int sanitize_path(const char *path) {
    if (!path || !*path || strnlen(path, PATH_MAX) >= PATH_MAX) {
        errno = EINVAL;
        return -1;
    }
    const char *part = path + (*path == '/');
    while (*part) {
        const char *end = strchr(part, '/');
        size_t count = end ? (size_t)(end - part) : strlen(part);
        if (!count || count > NAME_MAX || (count == 1 && part[0] == '.') ||
            (count == 2 && part[0] == '.' && part[1] == '.')) {
            errno = EACCES;
            return -1;
        }
        if (!end) {
            return 0;
        }
        part = end + 1;
        if (!*part) {
            errno = EACCES;
            return -1;
        }
    }
    errno = EACCES;
    return -1;
}

// Родительские каталоги открываются отдельно, symlink не может вывести запрос за пределы root
static int below(const char *root, const char *file, int flags, bool writable) {
    char path[PATH_MAX];
    if (!root || root[0] != '/' || sanitize_path(file)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(path, file, strlen(file) + 1);
    int directory = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0 && writable && errno == ENOENT) {
        if (mkdir(root, 0700) && errno != EEXIST) {
            return -1;
        }
        directory = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (directory < 0) {
        return -1;
    }
    char *part = path;
    for (;;) {
        char *slash = strchr(part, '/');
        if (!slash) {
            int fd = openat(directory, part, flags | O_CLOEXEC | O_NOFOLLOW, 0600);
            int saved = errno;
            close(directory);
            if (fd < 0) {
                errno = saved;
                return -1;
            }
            struct stat info;
            if (fstat(fd, &info) || !S_ISREG(info.st_mode)) {
                close(fd);
                errno = EACCES;
                return -1;
            }
            return fd;
        }
        *slash = 0;
        int next = openat(directory, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && writable && errno == ENOENT && (flags & O_CREAT)) {
            if (!mkdirat(directory, part, 0700) || errno == EEXIST) {
                next = openat(directory, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            }
        }
        int saved = errno;
        close(directory);
        if (next < 0) {
            errno = saved;
            return -1;
        }
        directory = next;
        part = slash + 1;
    }
}

// Пути прошивок читаются из закреплённого дерева, readwrite обслуживается в отдельном RAM-каталоге
int translate_open(const char *path, int flags) {
    static const char *const prefixes[] = {
        "/readonly/firmware/image/", "/readonly/vendor/firmware_mnt/image/", "/readonly/vendor/firmware/"
    };
    if (sanitize_path(path)) {
        return -1;
    }
    const char *root = getenv("SYNAPSE_FIRMWARE_ROOT");
    if (!root || !*root) {
        root = "/lib/firmware";
    }
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        size_t count = strlen(prefixes[i]);
        if (!strncmp(path, prefixes[i], count)) {
            if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND))) {
                errno = EROFS;
                return -1;
            }
            return below(root, path + count, flags, false);
        }
    }
    static const char modem[] = "/readonly/firmware/modem_pr/";
    if (!strncmp(path, modem, sizeof(modem) - 1)) {
        if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND))) {
            errno = EROFS;
            return -1;
        }
        return below(root, path + strlen("/readonly/firmware/"), flags, false);
    }
    static const char rw[] = "/readwrite/";
    if (!strncmp(path, rw, sizeof(rw) - 1)) {
        root = getenv("SYNAPSE_TQFTP_RW_ROOT");
        if (!root || !*root) {
            root = "/run/tqftpserv";
            if (mkdir("/run", 0755) && errno != EEXIST) {
                return -1;
            }
        }
        return below(root, path + sizeof(rw) - 1, flags, true);
    }
    errno = ENOENT;
    return -1;
}
