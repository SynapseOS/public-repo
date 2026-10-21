// transaction.c: Применение и восстановление пакетных операций
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Журналирование изменений файлов и подготовка загрузочных записей
// Подсистема: пакетный менеджер setup
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 21.10.2026
// Доступ: общедоступный

#include "setup.h"
#include "../common/boot/limine-menu.h"
#include <sys/statvfs.h>
#include <elf.h>
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
    } else if (errno != ENOENT) {
        fail("cannot inspect %s", relative);
    }
    return o;
}
static void db(const char *dest, struct package **items, int count) {
    FILE *f = fopen(dest, "wb");
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
    copy_file(src, tmp, mode, 1);
    if (rename(tmp, dest)) {
        fail("rename %s: %s", dest, strerror(errno));
    }
    sync_parent(dest);
}
static void journal(const char *dest) {
    FILE *f = fopen(dest, "wb");
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

            unsigned mode = jnum(j, t, "mode");
            if (mode & ~0777) {
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

    char full[4096], boot[4096];
    path(boot, "boot");
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
    int replaced_path = 0, limine_protocol = 0;
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

    struct statvfs space;
    if (!statvfs(boot, &space) && space.f_frsize &&
        space.f_bavail < (p->files[0].size + 1024 * 1024) / space.f_frsize + 1) {
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
        header.e_ident[EI_VERSION] != EV_CURRENT || header.e_machine != EM_X86_64 ||
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
                    if (!lstat(full, &st)) {
                        if (S_ISREG(st.st_mode)) {
                            sha_file(full, h);
                            if (!strcmp(h, f->hash)) {
                                continue;
                            }
                        }
                        fail("config companion exists: %s", relative);
                    }
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
            copy_file(full, ops[i].backup, ops[i].old_mode, 1);
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
    puts("Transaction complete");
    if (kernel_pending) {
        puts(
            "New kernel prepared. Reboot to apply; previous kernels remain in Limine.");
    }
}
