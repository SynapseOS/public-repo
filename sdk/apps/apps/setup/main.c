// main.c: Команды пакетного менеджера setup
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Разбор аргументов, просмотр каталога и последовательное обновление пакетов
// Подсистема: пользовательский пакетный менеджер
// Первое изменение в истории Git: 09.10.2026
// Последнее изменение: 21.10.2026
// Доступ: общедоступный

#include "setup.h"
#include <sys/wait.h>
static void help(void) {
    char notice[4096];
    path(notice, "var/lib/setup/updates.txt");
    FILE *pending = fopen(notice, "r");
    if (pending) {
        char line[256];
        if (fgets(line, sizeof line, pending)) {
            fputs(line, stdout);
        }
        fclose(pending);
    }
    puts("setup <command> [packages]\n"
         "  update    search [TEXT]  info NAME    ls [-i|-u]\n"
         "  install   reinstall     upgrade      remove    verify\n"
         "  auto      check [--notify]         recover       repo list     auth "
         "[status|logout]\n"
         "Options: --yes, --dry-run");
}
static int catalog_best(int index) {
    struct package *p = available[index];
    for (int j = 0; j < navailable; j++) {
        struct package *other = available[j];
        if (j == index || strcmp(p->name, other->name)) {
            continue;
        }

        int priority = repos[other->repo].priority - repos[p->repo].priority;
        int version = version_cmp(other->version, p->version);
        if (priority > 0 || (!priority && (version > 0 || (!version && j < index)))) {
            return 0;
        }
    }
    return 1;
}
// Каждый этап использует обычную блокировку, журнал и проверку подписи
// Новый процесс после обновления setup запустит установленный бинарник
static void auto_step(const char *exe, const char *cmd, int argc, char **names,
                      int preview) {
    char *child[MAXPKG + 8];
    int n = 0;
    child[n++] = (char *)exe;
    if (*root) {
        child[n++] = "--root";
        child[n++] = (char *)root;
    }
    child[n++] = (char *)cmd;
    child[n++] = "--yes";
    if (preview) {
        child[n++] = "--dry-run";
    }

    for (int i = 0; i < argc; i++) {
        child[n++] = names[i];
    }
    child[n] = NULL;
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        fail("auto: cannot start %s: %s", cmd, strerror(errno));
    }
    if (!pid) {
        execvp(exe, child);
        perror("setup auto exec");
        _exit(127);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fail("auto: wait failed: %s", strerror(errno));
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fail("auto stopped: %s failed; see the error above", cmd);
    }
}
static int needs_repair(struct package *p) {
    int damaged = 0;
    for (int i = 0; i < p->nfile; i++) {
        struct file *f = p->files + i;
        char full[4096], hash[65];
        struct stat st;
        path(full, f->path);
        if (lstat(full, &st)) {
            if (errno != ENOENT) {
                fail("cannot inspect %s: %s", f->path, strerror(errno));
            }
            fprintf(stderr, "Missing: %s\n", f->path);
            damaged = 1;
            continue;
        }
        if (!((f->type && S_ISDIR(st.st_mode)) || (!f->type && S_ISREG(st.st_mode)))) {
            fail("auto: unsafe destination type: %s; inspect it manually", f->path);
        }
        // Существующая конфигурация и её права принадлежат пользователю
        if (f->config || f->type) {
            continue;
        }
        sha_file(full, hash);
        if (strcmp(hash, f->hash)) {
            fprintf(stderr, "Damaged: %s\n", f->path);
            damaged = 1;
        }
    }
    return damaged;
}
static void automatic(const char *invoked) {
    char exe[4096];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0) {
        exe[len] = 0;
    } else {
        if (strlen(invoked) >= sizeof exe) {
            fail("setup executable path too long");
        }
        strcpy(exe, invoked);
    }
    if (dry_run) {
        puts("Auto preview: cached catalog only; no recovery, downloads or writes.");
        auto_step(exe, "upgrade", 0, NULL, 1);
    } else {
        auto_step(exe, "recover", 0, NULL, 0);
        auto_step(exe, "update", 0, NULL, 0);
        auto_step(exe, "upgrade", 0, NULL, 0);
    }
    load_state();
    char *repair[MAXPKG];
    int count = 0;
    for (int i = 0; i < ninstalled; i++) {
        if (needs_repair(installed[i])) {
            repair[count++] = installed[i]->name;
        }
    }
    if (count) {
        auto_step(exe, "reinstall", count, repair, dry_run);
    }
    if (dry_run) {
        puts("Auto preview complete.");
        return;
    }

    for (int i = 0; i < ninstalled; i++) {
        if (needs_repair(installed[i])) {
            fail("auto: %s still needs repair", installed[i]->name);
        }
    }
    puts("Auto complete: installed packages updated and package files checked; user "
         "configs preserved.");
}
int main(int argc, char **argv) {
    volatile int lock = -1, recovering = 0;
    char **args = calloc(argc, sizeof *args);
    int count = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root")) {
            if (++i == argc) {
                return 2;
            }
            root = argv[i];
            if (*root != '/' || strlen(root) > 2000) {
                return 2;
            }
        } else if (!strcmp(argv[i], "--yes")) {
            yes = 1;
        } else if (!strcmp(argv[i], "--dry-run")) {
            dry_run = 1;
        } else {
            args[count++] = argv[i];
        }
    }
    if (!count) {
        help();
        return 0;
    }
    if (setjmp(failure)) {
        if (lock >= 0 && !recovering && transaction_started) {
            recovering = 1;
            recover();
        }
        if (lock >= 0) {
            close(lock);
        }
        return 1;
    }
    const char *cmd = args[0];
    if (!strcmp(cmd, "h") || !strcmp(cmd, "?") || !strcmp(cmd, "help") ||
        !strcmp(cmd, "--help")) {
        help();
        return 0;
    }
    if (!strcmp(cmd, "auth")) {
        auth_command(count - 1, args + 1);
        return 0;
    }
    if (!strcmp(cmd, "plus")) {
        plus_command(count - 1, args + 1);
        return 0;
    }
    if (!strcmp(cmd, "auto")) {
        if (count != 1) {
            fail("auto takes no package arguments");
        }
        automatic(argv[0]);
        return 0;
    }

    int install = !strcmp(cmd, "install") || !strcmp(cmd, "a") || !strcmp(cmd, "add") ||
                  !strcmp(cmd, "i"),
        remove = !strcmp(cmd, "remove") || !strcmp(cmd, "r") || !strcmp(cmd, "del") ||
                 !strcmp(cmd, "rm") || !strcmp(cmd, "d"),
        upgrade = !strcmp(cmd, "upgrade"), update = !strcmp(cmd, "update"),
        recovery = !strcmp(cmd, "recover"), reinstall = !strcmp(cmd, "reinstall");
    int check = !strcmp(cmd, "check");
    int mutate = check || update || recovery ||
                 ((install || remove || upgrade || reinstall) && !dry_run);
    if (mutate) {
        if (geteuid() != 0 && !*root) {
            fail("administrative privileges required");
        }

        char dir[4096], p[4096];
        path(dir, "var/lib/setup");
        mkdirs(dir);
        path(p, "var/lib/setup/lock");
        int candidate = open(p, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
        if (candidate < 0) {
            fail("setup lock is unavailable");
        }
        if (flock(candidate, LOCK_EX | LOCK_NB)) {
            close(candidate);
            fail("another setup operation is running or lock is unavailable");
        }
        lock = candidate;
        if (!recovery) {
            char txn[4096];
            path(txn, "var/lib/setup/txn");
            struct stat st;
            if (!lstat(txn, &st)) {
                fail("unfinished transaction: run setup recover");
            }
        } else {
            recovering = 1;
            recover();
            return 0;
        }
    }
    load_state();
    if (check) {
        int notify = count == 2 && !strcmp(args[1], "--notify");
        if (count != 1 && !notify) {
            fail("usage: setup check [--notify]");
        }

        char notice[4096];
        path(notice, "var/lib/setup/updates.txt");
        unlink(notice); // После ошибки проверки убираем старое предложение обновления
        load_repos(1);
        if (!nrepo) {
            fail("no repositories configured");
        }
        solve(0, NULL, 1);
        int updates = 0;
        int changed[MAXPKG] = {0};
        for (int i = 0; i < nchosen; i++) {
            struct package *p = chosen[i], *old = NULL;
            if (p->installed) {
                continue;
            }

            for (int j = 0; j < ninstalled; j++) {
                if (!strcmp(installed[j]->name, p->name)) {
                    old = installed[j];
                }
            }
            if (!old || !package_same(old, p)) {
                changed[i] = 1;
                updates++;
            }
        }
        if (updates) {
            char text[256];
            snprintf(text, sizeof text,
                     "[setup] %d package update(s) available. Run: setup upgrade\n",
                     updates);
            fputs(text, stdout);
            for (int i = 0; i < nchosen; i++) {
                if (changed[i]) {
                    printf("  %s %s\n", chosen[i]->name, chosen[i]->version);
                }
            }
            write_bytes(notice, text, strlen(text), 0644);
            if (notify && !*root) {
                // Только уведомляем об обновлении, без запроса ввода или установки
                int tty = open("/dev/tty0", O_WRONLY | O_NOCTTY | O_NONBLOCK);
                if (tty >= 0) {
                    write(tty, "\n", 1);
                    write(tty, text, strlen(text));
                    close(tty);
                }
            }
        } else {
            puts("All installed packages are up to date.");
        }
        return 0;
    }
    if (update) {
        if (count != 1) {
            fail("update takes no package arguments");
        }
        load_repos(1);
        if (!nrepo) {
            fail("no repositories configured; see setup repo list");
        }
        return 0;
    }
    if (!strcmp(cmd, "l") || !strcmp(cmd, "ls") || !strcmp(cmd, "list")) {
        int installed_only =
            count == 2 && (!strcmp(args[1], "--installed") || !strcmp(args[1], "-i"));
        int not_installed = count == 2 && (!strcmp(args[1], "--not-installed") ||
                                           !strcmp(args[1], "-u"));
        if (count > 2 || (count == 2 && !installed_only && !not_installed &&
                          strcmp(args[1], "--available"))) {
            fail("usage: setup ls [-i|-u]");
        }
        if (installed_only) {
            for (int i = 0; i < ninstalled; i++) {
                printf("%s %s %s%s\n", installed[i]->name, installed[i]->version,
                       installed[i]->url,
                       installed[i]->explicit ? "" : " (dependency)");
            }
            return 0;
        }
        load_repos(0);
        for (int i = 0; i < navailable; i++) {
            struct package *p = available[i];
            int have = 0;
            for (int j = 0; j < ninstalled; j++) {
                if (!strcmp(p->name, installed[j]->name)) {
                    have = 1;
                }
            }
            if (have && not_installed) {
                continue;
            }
            if (catalog_best(i)) {
                printf("%s %s [%s]%s\n", p->name, p->version, repos[p->repo].id,
                       have ? " (installed)" : "");
            }
        }
        return 0;
    }
    if (!strcmp(cmd, "verify")) {
        verify(count - 1, args + 1);
        return 0;
    }
    if (remove) {
        if (count < 2) {
            fail("remove needs package names");
        }
        plan_remove(count - 1, args + 1);
        transact();
        return 0;
    }
    load_repos(!strcmp(cmd, "repo") ? -1 : 0);
    if (!strcmp(cmd, "repo")) {
        if (count != 2 || strcmp(args[1], "list")) {
            fail("usage: setup repo list");
        }

        for (int i = 0; i < nrepo; i++) {
            printf("%s %s key=%s priority=%d generation=%llu%s\n", repos[i].id,
                   repos[i].url, repos[i].key, repos[i].priority,
                   (unsigned long long)repos[i].generation,
                   repos[i].official ? " official" : "");
            for (int k = 1; k < repos[i].nmirrors; k++) {
                printf("  mirror %s\n", repos[i].mirrors[k]);
            }
        }
        if (!nrepo) {
            puts("No repositories configured");
        }
        return 0;
    }
    if (!strcmp(cmd, "search") || !strcmp(cmd, "info")) {
        if ((strcmp(cmd, "search") && count != 2) || count > 2) {
            fail("%s needs one argument", cmd);
        }
        const char *query = count == 2 ? args[1] : "";
        int found = 0;
        for (int i = 0; i < navailable; i++) {
            struct package *p = available[i];
            if (!catalog_best(i)) {
                continue;
            }
            if ((!strcmp(cmd, "info") && !strcmp(p->name, query)) ||
                (!strcmp(cmd, "search") &&
                 (strstr(p->name, query) || strstr(p->description, query)))) {
                printf("%s %s [%s] %s\n", p->name, p->version, repos[p->repo].id,
                       p->description);
                if (!strcmp(cmd, "info")) {
                    printf("  %s %s, %llu bytes, %s\n", p->arch, p->abi,
                           (unsigned long long)p->size, p->kind);
                    for (int d = 0; d < p->ndep; d++) {
                        printf("  requires %s\n", p->dep[d]);
                    }
                }
                found++;
            }
        }
        if (!found && !strcmp(cmd, "info")) {
            fail("package not found");
        }
        return 0;
    }
    if (install || upgrade || reinstall) {
        if ((install || reinstall) && count < 2) {
            fail("%s needs package names", cmd);
        }
        if (upgrade || reinstall) {
            for (int a = 1; a < count; a++) {
                int found = 0;
                for (int i = 0; i < ninstalled; i++) {
                    if (!strcmp(args[a], installed[i]->name)) {
                        found = 1;
                    }
                }
                if (!found) {
                    fail("%s package is not installed: %s", cmd, args[a]);
                }
            }
        }
        solve(count - 1, args + 1, upgrade || reinstall);
        if (reinstall) {
            for (int a = 1; a < count; a++) {
                for (int i = 0; i < nchosen; i++) {
                    struct package *p = chosen[i];
                    if (strcmp(p->name, args[a])) {
                        continue;
                    }
                    if (p->installed) {
                        struct package *archive = NULL;
                        for (int j = 0; j < navailable; j++) {
                            if (!strcmp(p->name, available[j]->name) &&
                                !strcmp(p->version, available[j]->version) &&
                                !strcmp(p->url, repos[available[j]->repo].id) &&
                                (!archive || repos[available[j]->repo].priority >
                                                 repos[archive->repo].priority)) {
                                archive = available[j];
                            }
                        }
                        if (!archive) {
                            fail("no repository archive for %s %s", p->name,
                                 p->version);
                        }
                        archive->explicit = p->explicit;
                        chosen[i] = p = archive;
                    }
                    p->reinstall = 1;
                }
            }
        }
        transact();
        return 0;
    }
    fail("unknown command: %s", cmd);
}
