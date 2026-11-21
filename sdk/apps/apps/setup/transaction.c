// transaction.c: Применение и восстановление пакетных операций
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Журналирование изменений файлов и подготовка загрузочных записей
// Подсистема: пакетный менеджер setup
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 13.11.2026
// Доступ: общедоступный

#include "setup.h"
#include "../common/boot/limine-menu.h"
#include <sys/statvfs.h>
#include <elf.h>
#include <inttypes.h>
#include <strings.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#define MAXOPS 4096
struct operation {
    char relative[512], stage[4096], backup[4096];
    unsigned old_mode, mode;
    int had_old, remove, directory;
};
int transaction_started;
static struct operation *ops;
static int nops;
static char txn[2304];
static char *boot_config;
static size_t boot_config_len;
static int kernel_pending;
static char *boot_marker;
static size_t boot_marker_len;
static const char *boot_config_path = "boot/boot/limine/limine.conf";
static void remove_tree(const char *p) {
    struct stat st;
    if (lstat(p, &st)) {
        if (errno == ENOENT) {
            return;
        }
        fail("stat cleanup %s", p);
    }
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(p)) {
            fail("unlink cleanup %s", p);
        }
        return;
    }
    DIR *d = opendir(p);
    if (!d) {
        fail("read cleanup %s", p);
    }
    // Сначала читаем все имена, затем удаляем файлы
    // В VFS unlink сдвигает индексы записей и может нарушить продолжающийся обход
    char **children = NULL;
    size_t count = 0;
    struct dirent *e;
    for (;;) {
        errno = 0;
        e = readdir(d);
        if (!e) {
            int error = errno;
            closedir(d);
            if (error) {
                fail("read cleanup %s: %s", p, strerror(error));
            }
            break;
        }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }

        char child[4096];
        if ((size_t)snprintf(child, sizeof child, "%s/%s", p, e->d_name) >=
            sizeof child) {
            fail("cleanup path too long");
        }

        char **next = realloc(children, (count + 1) * sizeof *children);
        if (!next) {
            fail("out of memory during cleanup");
        }
        children = next;
        children[count] = strdup(child);
        if (!children[count]) {
            fail("out of memory during cleanup");
        }
        count++;
    }
    // Сначала удаляем каталоги содержимого
    // При ошибке оставляем журнал восстановления
    for (int directories = 1; directories >= 0; directories--) {
        for (size_t i = 0; i < count; i++) {
            if (!children[i]) {
                continue;
            }
            if (lstat(children[i], &st)) {
                fail("stat cleanup %s", children[i]);
            }
            if ((!!S_ISDIR(st.st_mode)) != directories) {
                continue;
            }
            remove_tree(children[i]);
            free(children[i]);
            children[i] = NULL;
        }
    }
    free(children);
    if (rmdir(p)) {
        fail("rmdir cleanup %s: %s", p, strerror(errno));
    }
}
static int old_owner(const char *s) {
    for (int i = 0; i < ninstalled; i++) {
        for (int k = 0; k < installed[i]->nfile; k++) {
            if (!strcmp(installed[i]->files[k].path, s) &&
                !installed[i]->files[k].type) {
                return i;
            }
        }
    }
    return -1;
}
static struct file *find_file(struct package *p, const char *s) {
    for (int k = 0; k < p->nfile; k++) {
        if (!strcmp(p->files[k].path, s)) {
            return p->files + k;
        }
    }
    return NULL;
}
static struct package *new_package(const char *name) {
    for (int i = 0; i < nchosen; i++) {
        if (!strcmp(chosen[i]->name, name)) {
            return chosen[i];
        }
    }
    return NULL;
}
static void check_parent(const char *full) {
    char buf[4096];
    strcpy(buf, full);
    char *slash = strrchr(buf, '/');
    if (!slash) {
        fail("invalid destination");
    }
    *slash = 0;
    for (char *p = buf + 1;; p++) {
        if (*p != '/' && *p) {
            continue;
        }

        char c = *p;
        *p = 0;
        struct stat st;
        if (!lstat(buf, &st) && !S_ISDIR(st.st_mode)) {
            fail("symlink/non-directory in destination %s", buf);
        }
        if (errno != ENOENT && lstat(buf, &st)) {
            fail("cannot inspect destination");
        }
        *p = c;
        if (!c) {
            break;
        }
    }
}
static struct operation *add(const char *relative) {
    if (nops == MAXOPS) {
        fail("too many file operations");
    }

    for (int i = 0; i < nops; i++) {
        if (!strcmp(ops[i].relative, relative)) {
            fail("conflicting destination %s", relative);
        }
    }

    struct operation *o = ops + nops;
    strcpy(o->relative, relative);
    snprintf(o->backup, sizeof o->backup, "%s/backup%d", txn, nops);
    nops++;
    char full[4096];
    path(full, relative);
    check_parent(full);
    char temporary[4096];
    if ((size_t)snprintf(temporary, sizeof temporary, "%s.setup-tmp", full) >=
        sizeof temporary) {
        fail("path too long");
    }

    struct stat st;
    if (!lstat(temporary, &st)) {
        fail("unmanaged temporary destination exists: %s", temporary);
    }
    if (!lstat(full, &st)) {
        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
            fail("destination is not a regular file/directory: %s", relative);
        }
        o->had_old = 1;
        o->old_mode = st.st_mode & 0777;
        int owner = old_owner(relative);
        if (owner >= 0 && !strcmp(installed[owner]->name, "sudo") && !strcmp(relative, "bin/sudo") &&
            S_ISREG(st.st_mode) && (st.st_mode & 07777) == 04755 && st.st_uid == geteuid())
            o->old_mode = 04755;
    } else if (errno != ENOENT) {
        fail("cannot inspect %s", relative);
    }
    return o;
}
// Открыть приватные данные до записи, независимо от umask вызывающей службы
static FILE *private_output(const char *dest) {
    int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        fail("create private package metadata");
    }
    if (fchmod(fd, 0600) != 0) {
        close(fd);
        fail("protect private package metadata");
    }
    FILE *stream = fdopen(fd, "wb");
    if (stream == NULL) {
        close(fd);
        fail("open private package metadata stream");
    }
    return stream;
}

static void db(const char *dest, struct package **items, int count) {
    FILE *f = private_output(dest);
    if (!f) {
        fail("write package database");
    }
    fputs("{\"format\":1,\"packages\":[", f);
    for (int i = 0; i < count; i++) {
        struct package *p = items[i];
        const char *source = p->installed ? p->url : repos[p->repo].id;
        if (!valid_name(source)) {
            fail("invalid installed source");
        }
        fprintf(f, "%s{\"source\":\"%s\",\"explicit\":%s,\"manifest\":", i ? "," : "",
                source, p->explicit ? "true" : "false");
        if (fwrite(p->manifest, 1, p->manifest_len, f) != p->manifest_len) {
            fail("write manifest database");
        }
        fputc('}', f);
    }
    fputs("]}\n", f);
    if (fflush(f) || fsync(fileno(f)) || fclose(f)) {
        fail("sync database");
    }
}
void serialize_db(const char *dest) {
    db(dest, chosen, nchosen);
}
static void replace(const char *src, const char *relative, unsigned mode) {
    char dest[4096], tmp[4096], parent[4096];
    path(dest, relative);
    check_parent(dest);
    strcpy(parent, dest);
    *strrchr(parent, '/') = 0;
    mkdirs(parent);
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.setup-tmp", dest) >= sizeof tmp) {
        fail("path too long");
    }

    struct stat st;
    if (!lstat(tmp, &st)) {
        if (!S_ISREG(st.st_mode)) {
            fail("unsafe transaction temporary file");
        }
        if (unlink(tmp)) {
            fail("remove temporary file");
        }
    }
    int privileged = mode == 04755;
    if (privileged && strcmp(relative, "bin/sudo")) fail("invalid setuid destination");
    copy_file(src, tmp, privileged ? 0600 : mode, 1);
    if (rename(tmp, dest)) {
        fail("rename %s: %s", dest, strerror(errno));
    }
    if (sudo_path(relative)) {
        int fd = open(dest, O_RDONLY | O_NOFOLLOW);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
            (geteuid() == 0 && st.st_gid != 0 && fchown(fd, 0, 0)) || fchmod(fd, mode) || fsync(fd))
            fail("set final permissions on %s", relative);
        close(fd);
    }
    sync_parent(dest);
}
static void journal(const char *dest) {
    FILE *f = private_output(dest);
    if (!f) {
        fail("create journal");
    }
    fprintf(f, "{\"format\":1,\"operations\":[");
    for (int i = 0; i < nops; i++) {
        struct operation *o = ops + i;
        fprintf(f, "%s{\"path\":\"%s\",\"had_old\":%s,\"mode\":%u,\"directory\":%s}",
                i ? "," : "", o->relative, o->had_old ? "true" : "false", o->old_mode,
                o->directory ? "true" : "false");
    }
    fputs("]}", f);
    if (fflush(f) || fsync(fileno(f)) || fclose(f)) {
        fail("flush journal");
    }
    sync_parent(dest);
}
void recover(void) {
    path(txn, "var/lib/setup/txn");
    struct stat st;
    if (lstat(txn, &st)) {
        if (errno == ENOENT) {
            return;
        }
        fail("inspect transaction");
    }

    char active[4096];
    snprintf(active, sizeof active, "%s/active", txn);
    if (lstat(active, &st)) {
        if (errno != ENOENT) {
            fail("inspect journal marker");
        }
        remove_tree(txn);
        fprintf(stderr, "Removed unapplied staging transaction\n");
        return;
    }

    char state[4096], next[4096], old[4096], journalpath[4096];
    path(state, "var/lib/setup/installed.json");
    snprintf(next, sizeof next, "%s/new.db", txn);
    snprintf(old, sizeof old, "%s/old.db", txn);
    char a[65], b[65];
    sha_file(next, a);
    int committed = 0;
    if (!lstat(state, &st)) {
        sha_file(state, b);
        committed = !strcmp(a, b);
    }
    if (!committed) {
        snprintf(journalpath, sizeof journalpath, "%s/journal.json", txn);
        size_t len;
        char *data = read_bytes(journalpath, META_LIMIT, &len);
        struct json *j = parse_json(data, len);
        object_fields(j, 0, "|format||operations|");
        if (jnum(j, 0, "format") != 1) {
            fail("invalid recovery journal");
        }

        int arr = json_get(j, 0, "operations");
        if (arr < 0 || j->t[arr].type != J_ARRAY) {
            fail("invalid recovery operations");
        }

        int tokens[MAXOPS], total = 0;
        for (int t = arr + 1; t < j->t[arr].next; t = j->t[t].next) {
            if (total == MAXOPS) {
                fail("recovery operation limit");
            }
            tokens[total++] = t;
        }

        for (int id = total - 1; id >= 0; id--) {
            int t = tokens[id];
            object_fields(j, t, "|path||had_old||mode||directory|");
            char relative[512], dest[4096], backup[4096];
            jstr(j, t, "path", relative, sizeof relative);
            if (!safe_path(relative) || id == MAXOPS) {
                fail("invalid recovery destination");
            }

            int had, directory;
            if (json_boolean(j, json_get(j, t, "had_old"), &had) ||
                json_boolean(j, json_get(j, t, "directory"), &directory)) {
                fail("invalid recovery flags");
            }

            uint64_t mode = jnum(j, t, "mode");
            if (mode > 0777 && !(mode == 04755 && had && !directory && !strcmp(relative, "bin/sudo"))) {
                fail("invalid recovery mode");
            }
            path(dest, relative);
            char temporary[4096];
            if ((size_t)snprintf(temporary, sizeof temporary, "%s.setup-tmp", dest) >=
                sizeof temporary) {
                fail("path too long");
            }
            if (unlink(temporary) && errno != ENOENT) {
                fail("recover temporary %s", temporary);
            }
            if (directory) {
                if (!had && rmdir(dest) && errno != ENOENT && errno != ENOTEMPTY) {
                    fail("recover directory %s", dest);
                }
                continue;
            }
            if (had) {
                snprintf(backup, sizeof backup, "%s/backup%d", txn, id);
                replace(backup, relative, mode);
            } else {
                if (unlink(dest)) {
                    if (errno == ENOENT) {
                        continue;
                    }
                    fail("recover unlink %s", dest);
                }
            }
            sync_parent(dest);
        }
        free(j);
        free(data);
        replace(old, "var/lib/setup/installed.json", 0600);
        fprintf(stderr, "Interrupted transaction rolled back\n");
    } else {
        fprintf(stderr, "Committed transaction recovered\n");
    }
    remove_tree(txn);
    sync_parent(txn);
}
// Проверить свободные кластеры с резервом для атомарной замены меню и маркера
int kernel_boot_has_space(const struct package *p) {
    char boot[4096], dest[4096], relative[512];
    struct statvfs space;
    struct stat st;

    path(boot, "boot");
    if (!p->nfile || statvfs(boot, &space) || !space.f_frsize) {
        return 1;
    }

    snprintf(relative, sizeof relative, "boot/boot/kernels/%s/kernel", p->version);
    path(dest, relative);
    uint64_t kernel_bytes = lstat(dest, &st) == 0 && S_ISREG(st.st_mode) ? 0 : p->files[0].size;
    uint64_t bytes = kernel_bytes + 1024U * 1024U;
    uint64_t blocks = bytes / space.f_frsize + (bytes % space.f_frsize != 0);
    return space.f_bavail >= blocks;
}

#define KERNEL_DIR "boot/boot/kernels"
#define KERNEL_LIMIT 256
struct kernel_item {
    char version[64];
    uint64_t bytes;
    int listed, protected, candidate;
};
struct clean_plan {
    char *menu, *next;
    size_t len;
    mode_t mode;
    struct kernel_item items[KERNEL_LIMIT];
    size_t count;
    uint64_t bytes;
    int current, rollback;
};

static int kernel_revision(const char *v) {
    for (int i = 0; i < 4; i++) {
        if (*v < '0' || *v > '9' || (*v == '0' && v[1] >= '0' && v[1] <= '9')) {
            return 0;
        }
        uint64_t n = 0;
        while (*v >= '0' && *v <= '9') {
            n = n * 10 + (unsigned)(*v++ - '0');
            if (n > UINT32_MAX) {
                return 0;
            }
        }
        const char *sep = i < 2 ? "." : i == 2 ? "-r" : "";
        size_t len = strlen(sep);
        if (strncmp(v, sep, len)) {
            return 0;
        }
        v += len;
    }
    return !*v;
}

static int same_release(const char *version, const char *release) {
    size_t len = strlen(release);
    return !strcmp(version, release) || (!strncmp(version, release, len) && !strncmp(version + len, "-r", 2));
}

static char *trim_line(char *line) {
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t n = strlen(line);
    while (n && strchr(" \t\r", line[n - 1])) {
        line[--n] = 0;
    }
    return line;
}

// procfs может сообщать нулевой st_size, читаем содержимое через read
static int boot_identity(char *cmdline, char *release) {
    char file[4096];
    path(file, "proc/cmdline");
    int fd = open(file, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        return 0;
    }
    size_t used = 0;
    while (used < 4095) {
        ssize_t n = read(fd, cmdline + used, 4095 - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            close(fd);
            return 0;
        }
        if (!n) {
            break;
        }
        used += (size_t)n;
    }
    close(fd);
    if (!used || used == 4095 || memchr(cmdline, 0, used)) {
        return 0;
    }
    if (cmdline[used - 1] == '\n') {
        used--;
    }
    cmdline[used] = 0;
    char *trimmed = trim_line(cmdline);
    memmove(cmdline, trimmed, strlen(trimmed) + 1);
    if (!*root) {
        struct utsname uts;
        if (uname(&uts) || strlen(uts.release) >= 64) {
            return 0;
        }
        strcpy(release, uts.release);
    } else {
        path(file, "proc/version");
        FILE *f = fopen(file, "r");
        if (!f) {
            return 0;
        }
        int found = fscanf(f, "SynapseOS version %63s", release);
        fclose(f);
        if (found != 1) {
            return 0;
        }
    }
    return 1;
}

static int boot_token(const char *cmdline, const char *key, char value[64]) {
    size_t key_len = strlen(key);
    value[0] = 0;
    while (*cmdline) {
        cmdline += strspn(cmdline, " \t");
        size_t len = strcspn(cmdline, " \t");
        if (len >= key_len && !memcmp(cmdline, key, key_len)) {
            size_t n = len - key_len;
            if (*value || !n || n >= 64) {
                return 0;
            }
            memcpy(value, cmdline + key_len, n);
            value[n] = 0;
        }
        cmdline += len;
    }
    return !*value || kernel_revision(value);
}

static uint64_t allocated_bytes(const struct stat *st, unsigned long unit, int synapse_fat) {
    if (!synapse_fat) {
        return (uint64_t)st->st_blocks * 512;
    }
    uint64_t n = (uint64_t)st->st_size;
    return (n / unit + (n % unit != 0)) * unit;
}

// Удаляем только каталоги штатного формата, без симлинков, вложений и чужих файлов
static int inspect_kernel(const char *full, unsigned long unit, int synapse_fat, uint64_t *bytes) {
    struct stat directory, file;
    if (lstat(full, &directory) || !S_ISDIR(directory.st_mode)) {
        return 0;
    }
    DIR *d = opendir(full);
    if (!d) {
        return 0;
    }
    int count = 0, invalid = 0;
    struct dirent *e;
    for (;;) {
        errno = 0;
        e = readdir(d);
        if (!e) {
            invalid |= errno != 0;
            break;
        }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        char payload[4096];
        join_path(payload, sizeof payload, full, "kernel");
        if (strcmp(e->d_name, "kernel") || lstat(payload, &file) || !S_ISREG(file.st_mode) ||
            file.st_nlink != 1 || file.st_size <= 0 || file.st_dev != directory.st_dev) {
            invalid = 1;
            break;
        }
        count++;
    }
    closedir(d);
    if (invalid || count != 1) {
        return 0;
    }
    *bytes = allocated_bytes(&file, unit, synapse_fat) + allocated_bytes(&directory, unit, synapse_fat);
    return 1;
}

static const char *next_boot_entry(const char *p) {
    for (; *p; p++) {
        if (*p == '\n' && p[1] == '/') {
            return p + 1;
        }
    }
    return p;
}

// Разбираем только плоское меню с первой записью по умолчанию
// Неизвестные поля и любые ссылки из других записей защищают версию
static int parse_boot_entry(char *entry, struct clean_plan *plan, const char *cmdline,
                            const char *release, int *running, int *base) {
    int id = -1, paths = 0, protocol = 0, commands = 0, matched = 0, recovery = 0, extra = 0;
    int base_path = 0, other_path = 0;
    char *cursor = entry;
    char *title = trim_line(strsep(&cursor, "\n"));
    if (strcasestr(title, "recovery") || strcasestr(title, "rescue")) {
        recovery = 1;
    }
    for (char *line; (line = strsep(&cursor, "\n")) != NULL;) {
        line = trim_line(line);
        if (!strncmp(line, "path:", 5)) {
            const char *value = trim_line(line + 5);
            paths++;
            if (!strcmp(value, "boot():/boot/kernel")) {
                base_path = 1;
            } else if (!strcmp(value, "boot():/boot/kernel.pre-necro")) {
                other_path = 1;
            }
            static const char prefix[] = "boot():/boot/kernels/";
            if (!base_path && !other_path) {
                const char *leaf = strncmp(value, prefix, sizeof prefix - 1) ? NULL : value + sizeof prefix - 1;
                const char *suffix = leaf ? strchr(leaf, '/') : NULL;
                char version[64];
                if (!suffix || suffix == leaf || (size_t)(suffix - leaf) >= sizeof version ||
                    strcmp(suffix, "/kernel")) {
                    return -2;
                }
                memcpy(version, leaf, (size_t)(suffix - leaf));
                version[suffix - leaf] = 0;
                if (!kernel_revision(version)) {
                    return -2;
                }
            }
            for (size_t j = 0; j < plan->count; j++) {
                char expected[128];
                snprintf(expected, sizeof expected, "boot():/boot/kernels/%s/kernel", plan->items[j].version);
                if (!strcmp(value, expected)) {
                    id = (int)j;
                    break;
                }
            }
        } else if (!strcmp(line, "protocol: limine")) {
            protocol++;
        } else if (!strncmp(line, "cmdline:", 8)) {
            commands++;
            matched = !strcmp(trim_line(line + 8), cmdline);
        } else if (*line == '/' || !strncmp(line, "module_path:", 12)) {
            return -2;
        } else if (*line && strncmp(line, "comment:", 8) && strncmp(line, "kaslr:", 6)) {
            extra = 1;
        }
    }
    if (paths != 1 || protocol != 1) {
        return -2;
    }
    *base = base_path;
    if ((base_path || other_path || id < 0) && commands == 1 && matched) {
        *running = 1;
    }
    if (id >= 0) {
        struct kernel_item *k = &plan->items[id];
        k->listed = 1;
        if (commands == 1 && matched && same_release(k->version, release)) {
            *running = 1;
        }
        char expected[96];
        snprintf(expected, sizeof expected, "/SynapseOS %s", k->version);
        if (!recovery && !extra && commands <= 1 && !strcmp(title, expected)) {
            return id;
        }
    }
    return -1;
}

static void free_clean_plan(struct clean_plan *plan) {
    free(plan->menu);
    free(plan->next);
    free(plan);
}

static int has_boot_default(const char *menu, const char *end) {
    int defaults = 0;
    while (menu < end) {
        const char *line_end = strchr(menu, '\n');
        if (!line_end || line_end > end) {
            line_end = end;
        }
        while (menu < line_end && (*menu == ' ' || *menu == '\t')) {
            menu++;
        }
        if (!strncmp(menu, "default_entry:", 14)) {
            const char *value = menu + 14;
            while (value < line_end && (*value == ' ' || *value == '\t')) {
                value++;
            }
            if (value == line_end || *value++ != '1') {
                return 0;
            }
            while (value < line_end && strchr(" \t\r", *value)) {
                value++;
            }
            if (value != line_end || ++defaults > 1) {
                return 0;
            }
        }
        menu = line_end < end ? line_end + 1 : end;
    }
    return 1;
}

// Подсказка не должна прерывать установку пакетов при недоступном меню
static char *read_boot_file(const char *path_name, size_t limit, size_t *len) {
    int fd = open(path_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > limit) {
        close(fd);
        return NULL;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        close(fd);
        return NULL;
    }
    *len = 0;
    while (*len < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + *len, (size_t)st.st_size - *len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        *len += (size_t)n;
    }
    char extra;
    int valid = *len == (size_t)st.st_size && read(fd, &extra, 1) == 0 && !memchr(buf, 0, *len);
    close(fd);
    if (!valid) {
        free(buf);
        return NULL;
    }
    buf[*len] = 0;
    return buf;
}

static int boot_directory(const char *relative) {
    char file[4096];
    struct stat st;
    path(file, relative);
    return !lstat(file, &st) && S_ISDIR(st.st_mode);
}

static int build_clean_plan(struct clean_plan *plan, int update_probe) {
    char file[4096], cmdline[4096], release[64], marker[64] = "";
    if (!boot_identity(cmdline, release) || !boot_directory("boot") || !boot_directory("boot/boot") ||
        !boot_directory(KERNEL_DIR) || !boot_directory("boot/boot/limine")) {
        return 0;
    }
    char actual[64], previous[64];
    if (!boot_token(cmdline, "setup_kernel=", actual) || !boot_token(cmdline, "setup_rollback=", previous)) {
        return 0;
    }
    path(file, "boot/syngitf.txt");
    size_t len;
    char *metadata = read_boot_file(file, 4096, &len);
    if (!metadata) {
        return 0;
    }
    int valid = 1;
    char *cursor = metadata;
    for (char *line; (line = strsep(&cursor, "\n")) != NULL;) {
        if (!strncmp(line, "version=", 8)) {
            const char *v = trim_line(line + 8);
            if (*marker || strlen(v) >= sizeof marker || !*v) {
                valid = 0;
                break;
            }
            strcpy(marker, v);
        }
    }
    free(metadata);
    if (!valid || !kernel_revision(marker) || !same_release(marker, release) || (*actual && strcmp(actual, marker))) {
        return 0;
    }

    struct statvfs space;
    struct statfs fs;
    path(file, "boot");
    if (statvfs(file, &space) || !space.f_frsize || statfs(file, &fs)) {
        return 0;
    }
    int synapse_fat = !*root && fs.f_type == 0x4d44;
    path(file, KERNEL_DIR);
    DIR *dir = opendir(file);
    if (!dir) {
        return 0;
    }
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(dir);
        if (!e) {
            int error = errno;
            closedir(dir);
            if (error) {
                return 0;
            }
            break;
        }
        if (strlen(e->d_name) >= sizeof marker || !kernel_revision(e->d_name)) {
            continue;
        }
        if (plan->count == KERNEL_LIMIT) {
            closedir(dir);
            return 0;
        }
        char full[4096];
        join_path(full, sizeof full, file, e->d_name);
        struct kernel_item *k = &plan->items[plan->count];
        if (inspect_kernel(full, space.f_frsize, synapse_fat, &k->bytes)) {
            strcpy(k->version, e->d_name);
            plan->count++;
        }
    }

    path(file, boot_config_path);
    struct stat st;
    if (lstat(file, &st) || !S_ISREG(st.st_mode)) {
        return 0;
    }
    plan->mode = st.st_mode & 0777;
    plan->menu = read_boot_file(file, 65536, &len);
    if (!plan->menu || strchr(plan->menu, '~') || strstr(plan->menu, "..") ||
        strstr(plan->menu, SESSION_BEGIN)) {
        return 0;
    }
    const char *first = plan->menu[0] == '/' ? plan->menu : next_boot_entry(plan->menu);
    if (!*first || !has_boot_default(plan->menu, first)) {
        return 0;
    }
    int current = -1, recovery = 0, running_entries = 0;
    for (size_t j = 0; j < plan->count; j++) {
        const char *ref = strcasestr(plan->menu, plan->items[j].version);
        if (ref && ref < first) {
            plan->items[j].protected = 1;
        }
    }
    for (const char *entry = first; *entry;) {
        if (entry[1] == '/') {
            return 0;
        }
        const char *end = next_boot_entry(entry);
        size_t size = (size_t)(end - entry);
        char *raw = strndup(entry, size);
        char *copy = strndup(entry, size);
        if (!raw || !copy) {
            fail("out of memory");
        }
        int running = 0, base = 0;
        int id = parse_boot_entry(copy, plan, cmdline, release, &running, &base);
        free(copy);
        if (id == -2) {
            free(raw);
            return 0;
        }
        running_entries += running;
        if (entry == first) {
            // Маркер установки не доказывает, что ядро уже загружено
            if (!running || id < 0 || strcmp(plan->items[id].version, marker)) {
                free(raw);
                return 0;
            }
            current = id;
        }
        recovery |= base;
        for (size_t j = 0; j < plan->count; j++) {
            if (((int)j != id && strcasestr(raw, plan->items[j].version)) ||
                (running && (int)j == id)) {
                plan->items[j].protected = 1;
            }
        }
        free(raw);
        entry = end;
    }
    path(file, "boot/boot/kernel");
    if (current < 0 || running_entries != 1) {
        return 0;
    }
    plan->current = current;
    if (update_probe) {
        return 1;
    }
    if (!*actual || !*previous || !recovery || lstat(file, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return 0;
    }

    int rollback = -1;
    for (size_t j = 0; j < plan->count; j++) {
        struct kernel_item *k = &plan->items[j];
        if (k->listed && !strcmp(k->version, previous) && version_cmp(k->version, marker) < 0) {
            rollback = (int)j;
        }
    }
    if (rollback < 0) {
        return 0;
    }
    plan->current = current;
    plan->rollback = rollback;
    plan->items[rollback].protected = 1;
    for (size_t j = 0; j < plan->count; j++) {
        struct kernel_item *k = &plan->items[j];
        if (k->listed && !k->protected && version_cmp(k->version, previous) < 0) {
            k->candidate = 1;
            plan->bytes += k->bytes;
        }
    }

    plan->next = malloc(len + 1);
    if (!plan->next) {
        fail("out of memory");
    }
    plan->len = (size_t)(first - plan->menu);
    memcpy(plan->next, plan->menu, plan->len);
    for (const char *entry = first; *entry;) {
        const char *end = next_boot_entry(entry);
        size_t size = (size_t)(end - entry);
        char *copy = strndup(entry, size);
        if (!copy) {
            fail("out of memory");
        }
        int running = 0, base = 0;
        int id = parse_boot_entry(copy, plan, cmdline, release, &running, &base);
        free(copy);
        if (id < 0 || !plan->items[id].candidate) {
            memcpy(plan->next + plan->len, entry, size);
            plan->len += size;
        }
        entry = end;
    }
    plan->next[plan->len] = 0;
    return 1;
}

void kernel_clean_hint(void) {
    struct clean_plan *plan = calloc(1, sizeof *plan);
    if (plan && build_clean_plan(plan, 0) && plan->bytes) {
        fprintf(stderr, "Run setup clean-kernels --dry-run: %" PRIu64 " bytes can be freed in /boot\n",
                plan->bytes);
    } else {
        fputs("Run setup clean-kernels --dry-run to inspect /boot; no reclaimable space confirmed\n", stderr);
    }
    if (plan) {
        free_clean_plan(plan);
    }
}

void clean_kernels(void) {
    struct clean_plan *plan = calloc(1, sizeof *plan);
    if (!plan) {
        fail("out of memory");
    }
    if (!build_clean_plan(plan, 0)) {
        free_clean_plan(plan);
        fail("cannot confirm running kernel, rollback and recovery from the boot menu; nothing removed");
    }
    printf("Running kernel (protected): %s\nRollback kernel (protected): %s\nRecovery entries: protected\n",
           plan->items[plan->current].version, plan->items[plan->rollback].version);
    for (size_t j = 0; j < plan->count; j++) {
        const struct kernel_item *k = &plan->items[j];
        if (k->candidate) {
            printf("Obsolete: %s (%" PRIu64 " allocated bytes)\n", k->version, k->bytes);
        }
    }
    printf("Reclaimable from kernel directories: %" PRIu64 " bytes\n", plan->bytes);
    if (dry_run || !plan->bytes) {
        free_clean_plan(plan);
        return;
    }
    if (!yes) {
        char answer[32];
        fprintf(stderr, "Remove these kernels? [y/N] ");
        if (!fgets(answer, sizeof answer, stdin) || (answer[0] != 'y' && answer[0] != 'Y')) {
            free_clean_plan(plan);
            fail("cancelled");
        }
    }

    char file[4096], temporary[4096];
    path(file, boot_config_path);
    size_t len;
    char *current = read_bytes(file, 65536, &len);
    if (len != strlen(plan->menu) || memcmp(current, plan->menu, len)) {
        fail("boot menu changed; nothing removed");
    }
    free(current);
    if (strcmp(plan->menu, plan->next)) {
        path(temporary, "boot/boot/limine/limine.conf.setup-clean");
        int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, plan->mode);
        if (fd < 0) {
            fail("cannot stage boot menu: %s", strerror(errno));
        }
        FILE *output = fdopen(fd, "wb");
        if (!output) {
            close(fd);
            unlink(temporary);
            fail("cannot open staged boot menu");
        }
        int error = fwrite(plan->next, 1, plan->len, output) != plan->len ||
                    fflush(output) || fchmod(fd, plan->mode) || fsync(fd);
        if (fclose(output) || error || rename(temporary, file)) {
            unlink(temporary);
            fail("cannot replace boot menu; no kernels removed");
        }
    }
    // Меню должно быть записано на диск до удаления файлов, на которые оно ссылалось
    sync_parent(file);
    char directory[4096];
    path(directory, KERNEL_DIR);
    for (size_t j = 0; j < plan->count; j++) {
        if (!plan->items[j].candidate) {
            continue;
        }
        char full[4096], payload[4096];
        join_path(full, sizeof full, directory, plan->items[j].version);
        join_path(payload, sizeof payload, full, "kernel");
        if (unlink(payload) || rmdir(full)) {
            fail("cannot remove obsolete kernel %s: %s", plan->items[j].version, strerror(errno));
        }
        sync_parent(full);
    }
    free_clean_plan(plan);
}

// Ядро хранится в отдельном каталоге версии на корневой файловой системе
// Обновление ядра и загрузочного маркера разрешено только официальному пакету
static void plan_kernel(struct package *p, int index) {
    if (kernel_pending || strcmp(p->name, "kernel") || !repos[p->repo].official) {
        fail("kernel update requires one official kernel package");
    }

    char payload[512];
    snprintf(payload, sizeof payload, "usr/share/setup/kernels/%s/kernel", p->version);
    if ((p->nfile != 1 && p->nfile != 2) || strcmp(p->files[0].path, payload) ||
        p->files[0].type || p->files[0].config) {
        fail("invalid kernel payload layout");
    }
    if (p->nfile == 2) {
        snprintf(payload, sizeof payload, "usr/share/setup/kernels/%s/build.txt",
                 p->version);
        if (strcmp(p->files[1].path, payload) || p->files[1].type ||
            p->files[1].config) {
            fail("invalid kernel build metadata");
        }
    }

    char full[4096];
    path(full, "boot/syngitf.txt");
    size_t marker_len;
    char *marker = read_bytes(full, 4096, &marker_len);
    if (strlen(marker) != marker_len || !marker_len) {
        fail("invalid /boot/syngitf.txt");
    }
    // Сохраняем root= и остальные параметры компьютера из загрузочного маркера
    // Данные сборки берём из пакета
    boot_marker = calloc(1, marker_len + 512);
    if (!boot_marker) {
        fail("out of memory");
    }

    for (size_t pos = 0; pos < marker_len;) {
        size_t end = pos;
        while (end < marker_len && marker[end] != '\n') {
            end++;
        }
        if (strncmp(marker + pos, "commit=", 7) &&
            strncmp(marker + pos, "version=", 8)) {
            memcpy(boot_marker + boot_marker_len, marker + pos, end - pos);
            boot_marker_len += end - pos;
            boot_marker[boot_marker_len++] = '\n';
        }
        pos = end + (end < marker_len);
    }
    if (p->nfile == 1) {
        const char *commit =
            !strncmp(marker, "commit=", 7) ? marker : strstr(marker, "\ncommit=");
        if (commit && *commit == '\n') {
            commit++;
        }
        if (!commit) {
            fail("missing boot build identity");
        }
        const char *end = strchr(commit, '\n');
        size_t n = end ? (size_t)(end - commit) : strlen(commit);
        memcpy(boot_marker + boot_marker_len, commit, n);
        boot_marker_len += n;
        boot_marker[boot_marker_len++] = '\n';
        boot_marker_len +=
            snprintf(boot_marker + boot_marker_len, marker_len + 512 - boot_marker_len,
                     "version=%s\n", p->version);
    }
    free(marker);
    char previous[64] = "";
    if (!*root) {
        struct clean_plan *plan = calloc(1, sizeof *plan);
        if (plan && build_clean_plan(plan, 1) && version_cmp(plan->items[plan->current].version, p->version) < 0) {
            strcpy(previous, plan->items[plan->current].version);
        }
        if (plan) {
            free_clean_plan(plan);
        }
    }
    path(full, boot_config_path);
    size_t len;
    char *config = read_bytes(full, 65536, &len);
    if (strlen(config) != len || !strstr(config, "protocol: limine") ||
        !strstr(config, "boot():/boot/")) {
        fail("unsupported Limine configuration");
    }

    size_t plain_len;
    char *plain = limine_session_menu(config, 0, &plain_len);
    if (!plain) {
        fail("invalid managed Limine menu");
    }
    free(config);
    config = plain;
    len = plain_len;
    // Сохраняем общие параметры и старые загрузочные записи
    // Новую запись добавляем первой, оставляя прежнее ядро доступным для выбора
    size_t first = 0;
    while (first < len &&
           !(config[first] == '/' && (!first || config[first - 1] == '\n'))) {
        first++;
    }
    if (first == len) {
        fail("no boot entries in Limine configuration");
    }

    size_t title_end = first;
    while (title_end < len && config[title_end] != '\n') {
        title_end++;
    }

    size_t section_end = title_end;
    while (section_end < len && !(config[section_end] == '/' && section_end > first &&
                                  config[section_end - 1] == '\n')) {
        section_end++;
    }

    size_t capacity = len * 2 + 1024;
    boot_config = calloc(1, capacity);
    if (!boot_config) {
        fail("out of memory");
    }
    memcpy(boot_config, config, first);
    size_t used = first;
    used +=
        snprintf(boot_config + used, capacity - used, "/SynapseOS %s\n", p->version);
    int replaced_path = 0, limine_protocol = 0, has_cmdline = 0;
    for (size_t pos = title_end < len ? title_end + 1 : len; pos < section_end;) {
        size_t end = pos;
        while (end < section_end && config[end] != '\n') {
            end++;
        }

        size_t trim = pos;
        while (trim < end && (config[trim] == ' ' || config[trim] == '\t')) {
            trim++;
        }
        if (end - trim >= 16 && !memcmp(config + trim, "protocol: limine", 16)) {
            limine_protocol = 1;
        }
        if (end - trim >= 5 && !memcmp(config + trim, "path:", 5)) {
            used += snprintf(boot_config + used, capacity - used,
                             "    path: boot():/boot/kernels/%s/kernel\n", p->version);
            replaced_path++;
        } else if (end - trim >= 8 && !memcmp(config + trim, "cmdline:", 8)) {
            if (has_cmdline++) {
                fail("first boot entry contains multiple command lines");
            }
            used += snprintf(boot_config + used, capacity - used, "    cmdline:");
            for (size_t j = trim + 8; j < end;) {
                while (j < end && strchr(" \t\r", config[j])) {
                    j++;
                }
                size_t start = j;
                while (j < end && !strchr(" \t\r", config[j])) {
                    j++;
                }
                if (j > start && (j - start < 13 || memcmp(config + start, "setup_kernel=", 13)) &&
                    (j - start < 15 || memcmp(config + start, "setup_rollback=", 15))) {
                    used += snprintf(boot_config + used, capacity - used, " %.*s", (int)(j - start), config + start);
                }
            }
            used += snprintf(boot_config + used, capacity - used, " setup_kernel=%s", p->version);
            if (*previous) {
                used += snprintf(boot_config + used, capacity - used, " setup_rollback=%s", previous);
            }
            boot_config[used++] = '\n';
        } else {
            size_t count = end - pos + (end < section_end);
            memcpy(boot_config + used, config + pos, count);
            used += count;
        }
        pos = end + (end < section_end);
    }
    if (!limine_protocol || replaced_path != 1) {
        fail("first boot entry must contain one Limine kernel path");
    }
    if (!has_cmdline) {
        if (used && boot_config[used - 1] != '\n') {
            boot_config[used++] = '\n';
        }
        used += snprintf(boot_config + used, capacity - used, "    cmdline: setup_kernel=%s", p->version);
        if (*previous) {
            used += snprintf(boot_config + used, capacity - used, " setup_rollback=%s", previous);
        }
        boot_config[used++] = '\n';
    }
    if (used && boot_config[used - 1] != '\n') {
        boot_config[used++] = '\n';
    }
    memcpy(boot_config + used, config + first, len - first);
    boot_config_len = used + len - first;
    char needle[256];
    snprintf(needle, sizeof needle, "path: boot():/boot/kernels/%s/kernel", p->version);
    int existing_entry = strstr(config, needle) != NULL;
    if (existing_entry) {
        memcpy(boot_config, config, len);
        boot_config_len = len;
    }
    boot_config[boot_config_len] = 0;
    free(config);
    char relative[512];
    snprintf(relative, sizeof relative, "boot/boot/kernels/%s/kernel", p->version);
    path(full, relative);
    struct stat st;
    int kernel_exists = !lstat(full, &st);
    if (kernel_exists) {
        char digest[65];
        sha_file(full, digest);
        if (strcmp(digest, p->files[0].hash)) {
            fail("kernel version already exists with different content");
        }
    }

    if (!kernel_boot_has_space(p)) {
        kernel_clean_hint();
        fail("insufficient space on boot partition");
    }
    if (!kernel_exists) {
        struct operation *o = add(relative);
        o->mode = 0644;
        snprintf(o->stage, sizeof o->stage, "%s/pkg%d/f0", txn, index);
    }
    kernel_pending = 1;
}

static void validate_kernel(const char *stage) {
    size_t n;
    unsigned char *body = (void *)read_bytes(stage, PACKAGE_LIMIT, &n);
    if (n < sizeof(Elf64_Ehdr)) {
        fail("truncated kernel ELF");
    }
    Elf64_Ehdr header;
    memcpy(&header, body, sizeof header);
    if (memcmp(header.e_ident, ELFMAG, SELFMAG) ||
        header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_ident[EI_VERSION] != EV_CURRENT || header.e_machine != SETUP_ELF_MACHINE ||
        header.e_version != EV_CURRENT ||
        (header.e_type != ET_EXEC && header.e_type != ET_DYN) ||
        header.e_ehsize != sizeof header || header.e_phentsize != sizeof(Elf64_Phdr) ||
        !header.e_phnum || header.e_phnum > 1024 || header.e_phoff > n ||
        header.e_phnum > (n - header.e_phoff) / sizeof(Elf64_Phdr)) {
        fail("invalid x86_64 kernel ELF");
    }

    int entry = 0;
    for (unsigned i = 0; i < header.e_phnum; i++) {
        Elf64_Phdr ph;
        memcpy(&ph, body + header.e_phoff + i * sizeof ph, sizeof ph);
        if (ph.p_type == PT_INTERP || ph.p_type == PT_DYNAMIC) {
            fail("kernel must be statically linked");
        }
        if (ph.p_type != PT_LOAD) {
            continue;
        }
        if (ph.p_offset > n || ph.p_filesz > n - ph.p_offset ||
            ph.p_filesz > ph.p_memsz || ph.p_memsz > UINT64_MAX - ph.p_vaddr ||
            (ph.p_align > 1 && (ph.p_align & (ph.p_align - 1)))) {
            fail("invalid kernel segment");
        }
        if ((ph.p_flags & PF_X) && header.e_entry >= ph.p_vaddr &&
            header.e_entry - ph.p_vaddr < ph.p_memsz) {
            entry = 1;
        }
    }
    free(body);
    if (!entry) {
        fail("kernel entry point is outside executable segments");
    }
}

static int has_graphical_manager(struct package **packages, int count) {
    for (int i = 0; i < count; i++) {
        for (int f = 0; f < packages[i]->nfile; f++) {
            if (!packages[i]->files[f].type &&
                (!strcmp(packages[i]->files[f].path, "bin/clearui") ||
                 !strcmp(packages[i]->files[f].path, "bin/desktop"))) {
                return 1;
            }
        }
    }
    return 0;
}
static void plan_session_menu(void) {
    int graphical = has_graphical_manager(chosen, nchosen);
    int manager_changed = 0;
    for (int i = 0; i < nchosen; i++) {
        if (!chosen[i]->installed && has_graphical_manager(&chosen[i], 1)) {
            manager_changed = 1;
        }
    }
    if (!kernel_pending && !manager_changed &&
        graphical == has_graphical_manager(installed, ninstalled)) {
        return;
    }

    char full[4096];
    path(full, boot_config_path);
    if (!boot_config) {
        if (access(full, F_OK)) {
            return; // У Live-корня нет постоянного загрузочного раздела
        }
        boot_config = read_bytes(full, 65536, &boot_config_len);
        if (strlen(boot_config) != boot_config_len) {
            fail("invalid Limine configuration");
        }
    }

    size_t length;
    char *next = limine_session_menu(boot_config, graphical, &length);
    if (!next) {
        fail("cannot update Limine session menu");
    }
    if (!kernel_pending && length == boot_config_len &&
        !memcmp(next, boot_config, length)) {
        free(next);
        free(boot_config);
        boot_config = NULL;
        return;
    }
    free(boot_config);
    boot_config = next;
    boot_config_len = length;
}

void transact(void) {
    path(txn, "var/lib/setup/txn");
    ops = calloc(MAXOPS, sizeof *ops);
    if (!ops) {
        fail("out of memory");
    }

    int changes = 0;
    uint64_t downloads = 0, required = 0;
    for (int i = 0; i < nchosen; i++) {
        struct package *p = chosen[i];
        if (p->installed) {
            continue;
        }

        struct package *previous = NULL;
        for (int k = 0; k < ninstalled; k++) {
            if (!strcmp(installed[k]->name, p->name)) {
                previous = installed[k];
            }
        }
        if (previous && package_same(previous, p) && !p->reinstall) {
            continue;
        }
        fprintf(stderr, "%s %s %s (%s)\n",
                p->reinstall ? "Reinstall"
                : previous   ? "Upgrade"
                             : "Install",
                p->name, p->version, repos[p->repo].id);
        downloads += p->size;
        changes++;
        if (!strcmp(p->kind, "kernel")) {
            plan_kernel(p, i);
        }

        for (int k = 0; k < p->nfile; k++) {
            struct file *f = p->files + k;
            required += f->size;
            int owner = old_owner(f->path);
            if (owner >= 0 && strcmp(installed[owner]->name, p->name)) {
                fail("file %s belongs to %s", f->path, installed[owner]->name);
            }

            for (int a = 0; a < i; a++) {
                for (int b = 0; b < chosen[a]->nfile; b++) {
                    if (!f->type && !chosen[a]->files[b].type &&
                        !strcmp(f->path, chosen[a]->files[b].path)) {
                        fail("file ownership conflict %s", f->path);
                    }
                }
            }

            char relative[512];
            strcpy(relative, f->path);
            char full[4096];
            path(full, relative);
            struct stat st;
            int exists = !lstat(full, &st);
            if (exists && f->config && sudo_path(relative) && !sudo_metadata(f, &st))
                fail("review permissions and ownership of existing sudo configuration: %s", relative);
            if (exists && ((f->type && !S_ISDIR(st.st_mode)) ||
                           (!f->type && !S_ISREG(st.st_mode)))) {
                fail("destination type conflict: %s", f->path);
            }
            if (exists && !f->type && owner < 0) {
                char h[65];
                if (!f->config) {
                    if (strcmp(p->kind, "kernel")) {
                        fail("unmanaged destination exists: %s", f->path);
                    }
                    sha_file(full, h);
                    if (strcmp(h, f->hash)) {
                        fail("retained kernel content mismatch: %s", f->path);
                    }
                }
            }
            if (f->config && exists && (previous || owner < 0)) {
                struct file *oldfile = previous ? find_file(previous, f->path) : NULL;
                char h[65];
                sha_file(full, h);
                if (!oldfile || strcmp(h, oldfile->hash)) {
                    if (!strcmp(f->hash, h)) {
                        continue;
                    }
                    if (strlen(relative) + 10 >= sizeof relative) {
                        fail("config path too long");
                    }
                    strcat(relative, ".setup-new");
                    path(full, relative);
                    int already_staged = 0;
                    size_t companion_len = strlen(relative);
                    for (int alternate = 0;; alternate++) {
                        if (lstat(full, &st)) {
                            if (errno != ENOENT) {
                                fail("cannot inspect config companion: %s", relative);
                            }
                            if (old_owner(relative) < 0) {
                                break;
                            }
                        } else if (S_ISREG(st.st_mode) && old_owner(relative) < 0) {
                            sha_file(full, h);
                            if (!strcmp(h, f->hash)) {
                                already_staged = 1;
                                break;
                            }
                        }
                        if (alternate >= 999 ||
                            snprintf(relative + companion_len, sizeof relative - companion_len,
                                     ".%d", alternate + 1) >= (int)(sizeof relative - companion_len)) {
                            fail("no available config companion path: %s", f->path);
                        }
                        path(full, relative);
                    }
                    if (already_staged) {
                        continue;
                    }
                    fprintf(stderr, "Preserving %s; new package configuration: %s\n", f->path, relative);
                }
            }

            struct operation *o = add(relative);
            o->mode = f->mode;
            o->directory = f->type;
            snprintf(o->stage, sizeof o->stage, "%s/pkg%d/f%d", txn, i, k);
            if (o->had_old && !o->directory) {
                path(full, relative);
                if (lstat(full, &st) || !S_ISREG(st.st_mode)) {
                    fail("invalid existing file");
                }
                required += st.st_size;
            }
        }
    }

    for (int i = 0; i < ninstalled; i++) {
        struct package *p = installed[i], *replacement = new_package(p->name);
        if (replacement == p) {
            continue;
        }
        if (!replacement) {
            fprintf(stderr, "Remove %s %s\n", p->name, p->version);
            changes++;
        }

        for (int k = 0; k < p->nfile; k++) {
            struct file *f = p->files + k;
            if (!strcmp(p->kind, "kernel") || f->type ||
                (replacement && find_file(replacement, f->path))) {
                continue;
            }

            char full[4096], h[65];
            path(full, f->path);
            struct stat st;
            if (lstat(full, &st)) {
                if (errno == ENOENT) {
                    continue;
                }
                fail("inspect removal");
            }
            if (f->config) {
                sha_file(full, h);
                if (strcmp(h, f->hash)) {
                    fprintf(stderr, "Preserve modified config %s\n", f->path);
                    continue;
                }
            }

            struct operation *o = add(f->path);
            o->remove = 1;
            required += st.st_size;
        }
    }
    plan_session_menu();
    if (kernel_pending) {
        struct operation *marker = add("boot/syngitf.txt");
        marker->mode = 0644;
        snprintf(marker->stage, sizeof marker->stage, "%s/syngitf.next", txn);
    }
    if (boot_config) {
        struct operation *o = add(boot_config_path);
        o->mode = 0644;
        snprintf(o->stage, sizeof o->stage, "%s/limine.next", txn);
    }
    fprintf(stderr,
            "Plan: %d package changes, %d file operations, %llu download bytes\n",
            changes, nops, (unsigned long long)downloads);
    if (dry_run) {
        return;
    }
    if (!changes) {
        puts("Already up to date");
        return;
    }
    if (!yes) {
        if (!isatty(0)) {
            fail("noninteractive operation requires --yes");
        }
        fprintf(stderr, "Apply this plan? [y/N] ");
        char answer[32];
        if (!fgets(answer, sizeof answer, stdin) ||
            (answer[0] != 'y' && answer[0] != 'Y')) {
            fail("cancelled");
        }
    }

    char state_dir[4096];
    path(state_dir, "var/lib/setup");
    struct statvfs space;
    if (statvfs(state_dir, &space)) {
        fail("cannot determine filesystem space: %s", strerror(errno));
    }
    required += downloads + 8 * 1024 * 1024;
    if (space.f_frsize && space.f_bavail < required / space.f_frsize + 1) {
        fail("insufficient disk space for staging and rollback");
    }
    mkdirs(txn);
    if (chmod(txn, 0700) != 0) {
        fail("protect transaction directory");
    }
    transaction_started = 1;
    if (boot_config) {
        char next_config[4096];
        snprintf(next_config, sizeof next_config, "%s/limine.next", txn);
        write_bytes(next_config, boot_config, boot_config_len, 0644);
    }

    for (int i = 0; i < nchosen; i++) {
        struct package *p = chosen[i];
        if (p->installed) {
            continue;
        }

        int has = 0;
        for (int k = 0; k < nops; k++) {
            char prefix[4096];
            snprintf(prefix, sizeof prefix, "%s/pkg%d/", txn, i);
            if (!strncmp(ops[k].stage, prefix, strlen(prefix))) {
                has = 1;
            }
        }
        if (!has) {
            continue;
        }

        char archive[4096], stage[4096], hash[65];
        snprintf(archive, sizeof archive, "%s/package%d.spkg", txn, i);
        fetch_package(&repos[p->repo], p->url, archive, p->size, p->hash);
        sha_file(archive, hash);
        if (strcmp(hash, p->hash)) {
            fail("package SHA256 mismatch: %s", p->name);
        }
        snprintf(stage, sizeof stage, "%s/pkg%d", txn, i);
        extract(p, archive, stage);
        if (!strcmp(p->kind, "kernel")) {
            char elf[4096];
            join_path(elf, sizeof elf, stage, "f0");
            validate_kernel(elf);
            if (p->nfile == 2 && kernel_pending) {
                char metadata[4096];
                join_path(metadata, sizeof metadata, stage, "f1");
                size_t n;
                char *build = read_bytes(metadata, 256, &n);
                char expected[128];
                snprintf(expected, sizeof expected, "version=%s\n", p->version);
                if (strlen(build) != n || strncmp(build, "commit=", 7) ||
                    !strstr(build, expected)) {
                    fail("invalid kernel build identity");
                }

                size_t end = 7;
                while (end < n && build[end] != '\n') {
                    char c = build[end++];
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                        fail("invalid kernel commit");
                    }
                }
                if (end - 7 != 16 && end - 7 != 40) {
                    fail("invalid kernel commit length");
                }
                if (end >= n || strcmp(build + end + 1, expected)) {
                    fail("invalid kernel build metadata fields");
                }
                memcpy(boot_marker + boot_marker_len, build, n);
                boot_marker_len += n;
                free(build);
            }
        }
        fprintf(stderr, "Verified %s %s\n", p->name, p->version);
    }
    if (kernel_pending) {
        char next_marker[4096];
        snprintf(next_marker, sizeof next_marker, "%s/syngitf.next", txn);
        write_bytes(next_marker, boot_marker, boot_marker_len, 0644);
    }

    char old[4096], next[4096], journalpath[4096], active[4096];
    snprintf(old, sizeof old, "%s/old.db", txn);
    db(old, installed, ninstalled);
    snprintf(next, sizeof next, "%s/new.db", txn);
    serialize_db(next);
    for (int i = 0; i < nops; i++) {
        if (ops[i].had_old && !ops[i].directory) {
            char full[4096];
            path(full, ops[i].relative);
            copy_file(full, ops[i].backup, 0600, 1);
        }
    }
    snprintf(journalpath, sizeof journalpath, "%s/journal.json", txn);
    journal(journalpath);
    snprintf(active, sizeof active, "%s/active", txn);
    write_bytes(active, "1", 1, 0600);
    sync_parent(active); // Имитация отказа разрешена только в тесте с отдельным
                         // корневым каталогом
    const char *fault = *root ? getenv("SETUP_TEST_STOP_AFTER") : NULL;
    int stop = fault ? atoi(fault) : -1;
    if (stop == 0) {
        _exit(99);
    }

    for (int i = 0; i < nops; i++) {
        struct operation *o = ops + i;
        char full[4096];
        path(full, o->relative);
        if (o->directory) {
            mkdirs(full);
            if (!o->had_old && chmod(full, o->mode)) {
                fail("directory permissions %s", full);
            }
            sync_parent(full);
        } else if (o->remove) {
            if (unlink(full) && errno != ENOENT) {
                fail("remove %s", full);
            }
            sync_parent(full);
        } else {
            replace(o->stage, o->relative, o->mode);
        }
        if (!o->remove && !o->directory && !strncmp(o->relative, "boot/", 5)) {
            char expected[65], actual[65];
            sha_file(o->stage, expected);
            sha_file(full, actual);
            if (strcmp(expected, actual)) {
                fail("boot file readback mismatch: %s", o->relative);
            }
        }
        fprintf(stderr, "Applied %d/%d: %s\n", i + 1, nops, o->relative);
        if (stop == i + 1) {
            _exit(99);
        }
    }
    replace(next, "var/lib/setup/installed.json", 0600);
    remove_tree(txn);
    sync_parent(txn);
    transaction_started = 0;
    publish_public_catalog(chosen, nchosen);
    puts("Transaction complete");
    if (kernel_pending) {
        puts(
            "New kernel prepared. Reboot to apply; previous kernels remain in Limine.");
    }
}
