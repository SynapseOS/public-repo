// synapse-enumerate.c: Настоящие firmware manifests вместо Linux remoteproc
// Автор: Елчинян Арен Артурович 2026
// Архитектура: AArch64 и x86_64, ABI: POSIX filesystem
// Описание: Только обычные jsn/jsn.xz, ограниченная глубина и отсутствие symlink traversal
// Подсистема: публичные пакеты телефона
// Первое изменение: 25.11.2026
// Последнее изменение: 25.11.2026
// Доступ: общедоступный
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct syn_pd_budget { unsigned directories, entries, maps; };

// Boundaries относятся к firmware manifest discovery, payload для радио не читается
static bool syn_pd_extension(const char *name) {
    size_t size = strlen(name);
    return (size > 4 && !strcmp(name + size - 4, ".jsn")) ||
        (size > 7 && !strcmp(name + size - 7, ".jsn.xz"));
}

// Каждый открытый fd принадлежит текущему recursion frame и закрывается до возврата
static int syn_pd_walk(struct assoc *set, int fd, const char *path, unsigned depth, struct syn_pd_budget *budget) {
    DIR *directory = fdopendir(fd);
    if (!directory) { close(fd); return -1; }
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (++budget->entries > 8192) { errno = E2BIG; result = -1; break; }
        struct stat status;
        if (fstatat(dirfd(directory), entry->d_name, &status, AT_SYMLINK_NOFOLLOW)) { result = -1; break; }
        if (!S_ISDIR(status.st_mode) && !S_ISREG(status.st_mode)) continue;
        char child[PATH_MAX];
        int size = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (size < 0 || (size_t)size >= sizeof(child)) { errno = ENAMETOOLONG; result = -1; break; }
        if (S_ISDIR(status.st_mode)) {
            if (depth >= 8 || ++budget->directories > 256) { errno = E2BIG; result = -1; break; }
            int child_fd = openat(dirfd(directory), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child_fd < 0 || syn_pd_walk(set, child_fd, child, depth + 1, budget)) { result = -1; break; }
        } else if (syn_pd_extension(entry->d_name)) {
            if (status.st_size <= 0 || status.st_size > 4 * 1024 * 1024 || ++budget->maps > 256) {
                errno = E2BIG; result = -1; break;
            }
            assoc_set(set, child, NULL);
        }
    }
    closedir(directory);
    return result;
}

// Root задаётся только платформой, нет фальшивых карт или Linux sysfs fallback
static int syn_pd_enumerate_at(struct assoc *set, const char *root) {
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct syn_pd_budget budget = {.directories = 1};
    return syn_pd_walk(set, fd, root, 0, &budget);
}

static int pd_enumerate_jsons(struct assoc *set) {
    return syn_pd_enumerate_at(set, "/lib/firmware");
}
