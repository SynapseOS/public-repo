// dns-refresh.h: Обновление DNS-конфигурации из снимка ядра
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Управляемый resolv.conf обновляется без перезаписи ручной конфигурации
// Подсистема: запуск и обслуживание пользовательских служб
// Первое изменение: до текущей уборки, исходная дата не установлена
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

// Обновляем resolv.conf для программ со старой libc из цикла обслуживания init
// Размер входного снимка ограничен DNS_CONFIG_LIMIT
#define DNS_CONFIG_LIMIT 4096
struct dns_refresh_state {
    char fallback[1024];
    int active;
};
static int dns_line_is(const char *line, size_t size, const char *word) {
    while (size && (*line == ' ' || *line == '\t')) {
        line++;
        size--;
    }
    size_t len = strlen(word);
    return size > len && !memcmp(line, word, len) && isspace((unsigned char)line[len]);
}
static int dns_managed(const char *config) {
    for (const char *p = config; *p;) {
        const char *end = strchr(p, '\n');
        if (!end) {
            end = p + strlen(p);
        }
        if (dns_line_is(p, end - p, "options")) {
            const char *w = p;
            while (w < end && (*w == ' ' || *w == '\t')) {
                w++;
            }
            w += 7;
            while (w < end) {
                while (w < end && isspace((unsigned char)*w)) {
                    w++;
                }
                if (w < end && *w == '#') {
                    break;
                }
                const char *z = w;
                while (z < end && !isspace((unsigned char)*z)) {
                    z++;
                }
                if (z - w == 12 && !memcmp(w, "synapse-dhcp", 12)) {
                    return 1;
                }
                w = z;
            }
        }
        p = *end ? end + 1 : end;
    }
    return 0;
}
static int dns_append(char *out, size_t cap, size_t *used, const char *p, size_t n) {
    if (n >= cap - *used) {
        return 0;
    }
    memcpy(out + *used, p, n);
    *used += n;
    out[*used] = 0;
    return 1;
}
// Если снимок содержит неверную запись, отклоняем его целиком
static int dns_validate_snapshot(const char *snapshot) {
    unsigned count = 0;
    for (const char *p = snapshot; *p;) {
        const char *end = strchr(p, '\n');
        if (!end) {
            return 0;
        }
        if (!dns_line_is(p, end - p, "nameserver") || ++count > 3) {
            return 0;
        }
        const char *word = p;
        while (word < end && (*word == ' ' || *word == '\t')) {
            word++;
        }
        word += 11;
        while (word < end && isspace((unsigned char)*word)) {
            word++;
        }
        const char *z = word;
        while (z < end && !isspace((unsigned char)*z)) {
            z++;
        }
        size_t len = z - word;
        char address[64];
        unsigned char raw[16];
        if (!len || len >= sizeof address) {
            return 0;
        }
        memcpy(address, word, len);
        address[len] = 0;
        while (z < end && isspace((unsigned char)*z)) {
            z++;
        }
        if (z != end || (inet_pton(AF_INET, address, raw) != 1 &&
                         inet_pton(AF_INET6, address, raw) != 1)) {
            return 0;
        }
        p = end + 1;
    }
    return 1;
}
static int dns_refresh_render(struct dns_refresh_state *state, const char *current,
                              const char *snapshot, char *out, size_t capacity) {
    if (!dns_managed(current)) {
        state->active = 0;
        state->fallback[0] = 0;
        return 0;
    }
    if (!dns_validate_snapshot(snapshot)) {
        return 0;
    }
    if (!state->active) {
        size_t used = 0;
        state->fallback[0] = 0;
        for (const char *p = current; *p;) {
            const char *end = strchr(p, '\n');
            if (!end) {
                end = p + strlen(p);
            }
            size_t n = end - p + (*end ? 1 : 0);
            if (dns_line_is(p, end - p, "nameserver") &&
                !dns_append(state->fallback, sizeof state->fallback, &used, p, n)) {
                return 0;
            }
            p = *end ? end + 1 : end;
        }
        state->active = 1;
    }
    const char *servers = *snapshot ? snapshot : state->fallback;
    size_t used = 0;
    int inserted = 0;
    out[0] = 0;
    for (const char *p = current; *p;) {
        const char *end = strchr(p, '\n');
        if (!end) {
            end = p + strlen(p);
        }
        if (dns_line_is(p, end - p, "nameserver")) {
            if (!inserted) {
                if (!dns_append(out, capacity, &used, servers, strlen(servers))) {
                    return 0;
                }
                inserted = 1;
            }
        } else if (!dns_append(out, capacity, &used, p, end - p + (*end ? 1 : 0))) {
            return 0;
        }
        p = *end ? end + 1 : end;
    }
    if (!inserted && *servers) {
        if (used && out[used - 1] != '\n' &&
            !dns_append(out, capacity, &used, "\n", 1)) {
            return 0;
        }
        if (!dns_append(out, capacity, &used, servers, strlen(servers))) {
            return 0;
        }
    }
    return strcmp(out, current) != 0;
}
static int dns_read_bounded(const char *path, char *output, size_t capacity,
                            struct stat *info) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        return 0;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        return 0;
    }
    size_t used = 0;
    int okay = 1;
    for (;;) {
        ssize_t n = read(fd, output + used, capacity - 1 - used);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            okay = 0;
            break;
        }
        if (!n) {
            break;
        }
        used += n;
        if (used == capacity - 1) {
            char extra;
            if (read(fd, &extra, 1) != 0) {
                okay = 0;
            }
            break;
        }
    }
    close(fd);
    if (!okay) {
        return 0;
    }
    output[used] = 0;
    if (memchr(output, 0, used)) {
        return 0;
    }
    if (info) {
        *info = st;
    }
    return 1;
}
static void dns_refresh_file(struct dns_refresh_state *state, const char *path,
                             const char *source) {
    char current[DNS_CONFIG_LIMIT], snapshot[256], next[DNS_CONFIG_LIMIT],
        verify[DNS_CONFIG_LIMIT];
    struct stat before, after;
    if (!dns_read_bounded(path, current, sizeof current, &before)) {
        return;
    }
    if (!dns_managed(current)) {
        state->active = 0;
        state->fallback[0] = 0;
        return;
    }
    // При отсутствии снимка или ошибке чтения оставляем текущий resolv.conf
    if (!dns_read_bounded(source, snapshot, sizeof snapshot, NULL)) {
        return;
    }
    if (!dns_refresh_render(state, current, snapshot, next, sizeof next)) {
        return;
    }
    char temp[512];
    if (snprintf(temp, sizeof temp, "%s.dhcp-XXXXXX", path) >= (int)sizeof temp) {
        return;
    }
    int fd = mkstemp(temp);
    if (fd < 0) {
        return;
    }
    int okay = !fchmod(fd, before.st_mode & 0777);
    size_t used = 0, len = strlen(next);
    while (okay && used < len) {
        ssize_t n = write(fd, next + used, len - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            okay = 0;
            break;
        }
        used += n;
    }
    if (close(fd)) {
        okay = 0;
    }
    // Проверяем внешние изменения, включая удаление маркера управления
    // rename заменяет файл целиком, без промежуточного усечения
    if (okay && dns_read_bounded(path, verify, sizeof verify, &after) &&
        before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
        !strcmp(current, verify)) {
        rename(temp, path);
    }
    unlink(temp);
}
