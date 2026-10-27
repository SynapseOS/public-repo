// logrotate.c: Ротация журналов служб
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Обход каталога журналов и копирование старого содержимого
// Подсистема: пользовательские системные утилиты
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

#include "../common/service_log.h"
#include <dirent.h>
int main(int argc, char **argv) {
    size_t limit = SERVICE_LOG_LIMIT;
    int first = 1;
    if (argc > 2 && !strcmp(argv[1], "--size")) {
        char *end;
        limit = strtoul(argv[2], &end, 10);
        if (*end || !limit) {
            return 2;
        }
        first = 3;
    }

    int bad = 0;
    for (int i = first; i < argc; i++) {
        if (service_log_rotate(argv[i], limit, SERVICE_LOG_KEEP) < 0) {
            perror(argv[i]);
            bad = 1;
        }
    }
    if (first == argc) {
        DIR *dir = opendir("/var/log");
        if (!dir) {
            if (errno != ENOENT) {
                perror("/var/log");
                bad = 1;
            }
        } else {
            struct dirent *entry;
            while ((entry = readdir(dir))) {
                size_t n = strlen(entry->d_name);
                if (n < 5 || strcmp(entry->d_name + n - 4, ".log")) {
                    continue;
                }

                char path[512];
                if (snprintf(path, sizeof(path), "/var/log/%s", entry->d_name) >=
                    (int)sizeof(path)) {
                    bad = 1;
                    continue;
                }

                struct stat st;
                if (lstat(path, &st) || !S_ISREG(st.st_mode)) {
                    continue;
                }
                if (service_log_rotate(path, limit, SERVICE_LOG_KEEP) < 0) {
                    perror(path);
                    bad = 1;
                }
            }
            closedir(dir);
        }
    }
    return bad;
}
