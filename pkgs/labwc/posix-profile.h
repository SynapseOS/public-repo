// posix-profile.h: Параметры POSIX для процессов графического сеанса
// Автор: Елчинян Арен Артурович, 2026. AMD64, ABI POSIX
// Подсистема: графическая оболочка. Первое изменение: 21.11.2026. Последнее изменение: 22.11.2026. Общедоступно
#pragma once
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>

#define SYNAPSE_PR_SET_MEMORY_POPULATE 102006

struct posix_profile {
    int mask;
    unsigned files;
    unsigned memory_mib;
    unsigned populate;
    int nice;
    unsigned stack_mib;
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
    struct posix_profile result = {-1, 0, 0, 0, 0, 0};
    char path[128], old[1024];
    posix_profile_path(path);
    FILE *file = fopen(path, "r");
    if (!file && errno == ENOENT && home && *home == '/' &&
        snprintf(old, sizeof(old), "%s/.config/synapse/posix-session", home) < (int)sizeof(old)) {
        struct stat st;
        if (!lstat(old, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid()) {
            // Жёсткая ссылка публикует прежний файл один раз при одновременном входе
            if (!link(old, path)) unlink(old);
            file = fopen(path, "r");
            if (!file) file = fopen(old, "r");
        }
    }
    if (file) {
        char line[128];
        while (fgets(line, sizeof(line), file)) {
            unsigned value;
            if (sscanf(line, "mask=%i", &result.mask) == 1) {
                continue;
            }
            if (sscanf(line, "files=%u", &value) == 1 && (!value || value >= 128)) {
                result.files = value;
                continue;
            }
            if (sscanf(line, "memory=%u", &value) == 1 && value <= 1048576) {
                result.memory_mib = value;
                continue;
            }
            if (sscanf(line, "populate=%u", &value) == 1 && value <= 1) {
                result.populate = value;
                continue;
            }
            if (sscanf(line, "nice=%i", &result.nice) == 1) {
                continue;
            }
            if (sscanf(line, "stack=%u", &value) == 1 && value <= 1048576) {
                result.stack_mib = value;
            }
        }
        fclose(file);
    }
    if (result.mask < -1 || result.mask > 0777) {
        result.mask = -1;
    }
    if (result.nice < -20 || result.nice > 19) {
        result.nice = 0;
    }
    return result;
}

// Применить маску создания и мягкие пределы, сохранив жёсткие пределы родителя
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
    if (profile.memory_mib) {
        struct rlimit limit;
        const rlim_t requested = (rlim_t)profile.memory_mib * 1024 * 1024;
        if (getrlimit(RLIMIT_AS, &limit) == 0) {
            limit.rlim_cur = requested < limit.rlim_max ? requested : limit.rlim_max;
            if (setrlimit(RLIMIT_AS, &limit) != 0) {
                perror("labwc: RLIMIT_AS");
            }
        }
    }
    if (profile.stack_mib) {
        struct rlimit limit;
        const rlim_t requested = (rlim_t)profile.stack_mib * 1024 * 1024;
        if (getrlimit(RLIMIT_STACK, &limit) == 0) {
            limit.rlim_cur = requested < limit.rlim_max ? requested : limit.rlim_max;
            if (setrlimit(RLIMIT_STACK, &limit) != 0) {
                perror("labwc: RLIMIT_STACK");
            }
        }
    }
    if (setpriority(PRIO_PROCESS, 0, profile.nice) != 0) {
        perror("labwc: setpriority");
    }
    if (prctl(SYNAPSE_PR_SET_MEMORY_POPULATE, profile.populate, 0, 0, 0) != 0 && errno != ENOSYS) {
        perror("labwc: memory populate");
    }
}
