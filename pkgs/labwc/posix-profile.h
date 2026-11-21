// posix-profile.h: Параметры POSIX для процессов графического сеанса
// Автор: Елчинян Арен Артурович, 2026. AMD64, ABI POSIX
// Подсистема: графическая оболочка. Первое и последнее изменение: 21.11.2026. Общедоступно
#pragma once
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>

struct posix_profile {
    int mask;
    unsigned files;
};

// Менеджер входа создаёт закрытый каталог UID до смены пользователя.
static inline int prepare_posix_profile(uid_t uid, gid_t gid) {
    const char *parents[] = {"/synapse", "/synapse/subsys", "/synapse/subsys/posix"};
    struct stat st;
    for (unsigned i = 0; i < sizeof(parents) / sizeof(parents[0]); i++) {
        if ((mkdir(parents[i], 0755) && errno != EEXIST) || lstat(parents[i], &st) ||
            !S_ISDIR(st.st_mode) || st.st_uid || (st.st_mode & 0022)) return -1;
    }
    char dir[128];
    snprintf(dir, sizeof(dir), "/synapse/subsys/posix/%u", (unsigned)uid);
    if (!mkdir(dir, 0700)) {
        if (lstat(dir, &st)) return -1;
        return st.st_uid == uid && st.st_gid == gid ? 0 : chown(dir, uid, gid);
    }
    return errno == EEXIST && !lstat(dir, &st) && S_ISDIR(st.st_mode) &&
        st.st_uid == uid && !(st.st_mode & 0077) ? 0 : -1;
}

static inline void posix_profile_path(char path[128]) {
    snprintf(path, 128, "/synapse/subsys/posix/%u/session", (unsigned)getuid());
}

// Перенос выполняет сам владелец при первом входе. Старый профиль доступен
// для чтения и при обновлении только оболочки, до установки менеджера входа.
static inline struct posix_profile read_posix_profile(const char *home) {
    struct posix_profile result = {-1, 0};
    char path[128], old[1024], extra;
    posix_profile_path(path);
    FILE *file = fopen(path, "r");
    if (!file && errno == ENOENT && home && *home == '/' &&
        snprintf(old, sizeof(old), "%s/.config/synapse/posix-session", home) < (int)sizeof(old)) {
        struct stat st;
        if (!lstat(old, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid()) {
            // Жёсткая ссылка публикует прежний файл один раз при одновременном входе.
            if (!link(old, path)) unlink(old);
            file = fopen(path, "r");
            if (!file) file = fopen(old, "r");
        }
    }
    if (file) {
        int mask;
        unsigned files;
        if (fscanf(file, "mask=%d files=%u %c", &mask, &files, &extra) == 2 &&
            mask >= -1 && mask <= 0777 && (!files || files >= 128)) {
            result.mask = mask;
            result.files = files;
        }
        fclose(file);
    }
    return result;
}

// Применить маску создания и мягкий предел, сохранив жёсткий предел родителя
static inline void apply_posix_profile(const char *home) {
    struct posix_profile profile = read_posix_profile(home);
    if (profile.mask >= 0) {
        umask((mode_t)profile.mask);
    }
    if (profile.files) {
        struct rlimit limit;
        if (getrlimit(RLIMIT_NOFILE, &limit) == 0) {
            limit.rlim_cur = profile.files < limit.rlim_max ? profile.files : limit.rlim_max;
            if (setrlimit(RLIMIT_NOFILE, &limit) != 0) {
                perror("labwc: RLIMIT_NOFILE");
            }
        }
    }
}
