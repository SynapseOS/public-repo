// postgres-start.c: Подготовка и запуск PostgreSQL от выделенного пользователя
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux amd64
//
// Подсистема: серверные пакеты
// Первое изменение: 13.11.2026, последнее изменение: 14.11.2026
// Доступ: общедоступный

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char home[] = "/var/lib/postgresql/16";
static const char data[] = "/var/lib/postgresql/16/data";

static _Noreturn void io_error(const char *operation) {
    perror(operation);
    exit(1);
}

static _Noreturn void refuse(const char *reason) {
    fprintf(stderr, "postgres: %s\n", reason);
    exit(1);
}

static void append_account(const char *path, const char *record) {
    int source = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat info;
    if (source < 0 || fstat(source, &info) < 0) {
        io_error(path);
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != 0 || (info.st_mode & 0022) != 0 || info.st_nlink != 1) {
        refuse("unsafe account file");
    }
    FILE *input = fdopen(source, "r");
    if (!input) {
        io_error("postgres: fdopen");
    }
    char temp[] = "/etc/.postgres-account.XXXXXX";
    int fd = mkstemp(temp);
    if (fd < 0) {
        io_error("postgres: mkstemp");
    }
    FILE *output = fdopen(fd, "w");
    int ok = output && fchown(fd, info.st_uid, info.st_gid) == 0 && fchmod(fd, info.st_mode & 0777) == 0;
    int last = '\n';
    if (ok) {
        for (int c; (c = fgetc(input)) != EOF; last = c) {
            if (fputc(c, output) == EOF) {
                ok = 0;
                break;
            }
        }
        ok = ok && !ferror(input) && (last == '\n' || fputc('\n', output) != EOF) &&
             fputs(record, output) != EOF && fflush(output) == 0 && fsync(fd) == 0;
    }
    fclose(input);
    if (output) {
        if (fclose(output) != 0) {
            ok = 0;
        }
    } else {
        close(fd);
    }
    if (!ok || rename(temp, path) != 0) {
        unlink(temp);
        refuse("cannot update account file");
    }
    int directory = open("/etc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0 || fsync(directory) != 0) {
        io_error("postgres: sync /etc");
    }
    close(directory);
}

static unsigned allocate_id(int group) {
    // Перебирать локальные записи без обращения к отсутствующему NSCD
    unsigned char used[1000] = {0};
    FILE *input = fopen(group ? "/etc/group" : "/etc/passwd", "re");
    if (!input) {
        io_error("postgres: read account file");
    }
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, input) >= 0) {
        char *field = strchr(line, ':');
        if (field) {
            field = strchr(field + 1, ':');
        }
        if (!field) {
            continue;
        }
        char *end;
        unsigned long id = strtoul(field + 1, &end, 10);
        if (end != field + 1 && (*end == ':' || *end == '\n') && id >= 2600 && id < 3600) {
            used[id - 2600] = 1;
        }
    }
    if (ferror(input)) {
        io_error("postgres: read account file");
    }
    fclose(input);
    free(line);
    for (unsigned i = 0; i < sizeof used; i++) {
        if (!used[i]) {
            return i + 2600;
        }
    }
    refuse("no free system account ID");
}

static void provision_account(void) {
    int lock = open("/etc/.pwd.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat info;
    if (lock < 0 || fstat(lock, &info) < 0) {
        io_error("postgres: account lock");
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != 0 || info.st_nlink != 1 || (info.st_mode & 0777) != 0600) {
        refuse("unsafe account lock");
    }
    while (flock(lock, LOCK_EX) < 0) {
        if (errno != EINTR) {
            io_error("postgres: lock accounts");
        }
    }

    char record[256];
    struct passwd *account = getpwnam("postgres");
    struct group *group = getgrnam("postgres");
    if (account && (account->pw_uid == 0 || !group || group->gr_gid == 0 || account->pw_gid != group->gr_gid)) {
        refuse("existing postgres account must use the non-root postgres group");
    }
    if (!group) {
        unsigned gid = allocate_id(1);
        snprintf(record, sizeof record, "postgres:x:%u:\n", gid);
        append_account("/etc/group", record);
        group = getgrnam("postgres");
    }
    if (!group || group->gr_gid == 0) {
        refuse("postgres group is unavailable");
    }
    if (!account) {
        snprintf(record, sizeof record, "postgres:!:%u:%u:PostgreSQL:%s:/bin/sh\n",
                 allocate_id(0), group->gr_gid, home);
        append_account("/etc/passwd", record);
    }
    close(lock);
}

static void make_directory(const char *path, mode_t mode, uid_t uid, gid_t gid) {
    if (mkdir(path, mode) == 0) {
        if ((geteuid() == 0 && chown(path, uid, gid) < 0) || chmod(path, mode) < 0) {
            io_error(path);
        }
        return;
    }
    if (errno != EEXIST) {
        io_error(path);
    }
    struct stat info;
    if (lstat(path, &info) < 0) {
        io_error(path);
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != uid || info.st_gid != gid ||
        (info.st_mode & (mode == 0700 ? 0077 : 0022))) {
        refuse("unexpected owner or permissions on the PostgreSQL directories");
    }
}

static void initialize_cluster(void) {
    struct stat info;
    if (lstat("/var/lib/postgresql/16/data/PG_VERSION", &info) == 0) {
        if (!S_ISREG(info.st_mode) || info.st_uid != geteuid()) {
            refuse("unsafe PG_VERSION");
        }
        return;
    }
    if (errno != ENOENT) {
        io_error("postgres: PG_VERSION");
    }
    DIR *directory = opendir(data);
    if (!directory) {
        io_error(data);
    }
    errno = 0;
    for (struct dirent *entry; (entry = readdir(directory)) != NULL;) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            refuse("data directory is not empty and has no PG_VERSION");
        }
    }
    if (errno != 0 || closedir(directory) != 0) {
        io_error(data);
    }

    pid_t pid = fork();
    if (pid < 0) {
        io_error("postgres: fork initdb");
    }
    if (pid == 0) {
        const char *initdb = "/usr/lib/postgresql/16/bin/initdb";
        execl(initdb, initdb, "-D", data, "--no-locale",
              "--encoding=UTF8", "--auth=trust", "--auth-host=scram-sha-256", (char *)NULL);
        io_error("postgres: exec initdb");
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            io_error("postgres: wait initdb");
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        refuse("initdb failed");
    }
}

int main(void) {
    umask(077);
    if (geteuid() == 0) {
        provision_account();
    }
    struct passwd *account = getpwnam("postgres");
    if (!account || account->pw_uid == 0 || account->pw_gid == 0) {
        refuse("non-root postgres account is unavailable");
    }
    uid_t uid = account->pw_uid;
    gid_t gid = account->pw_gid;
    if (geteuid() == 0) {
        make_directory("/var/lib/postgresql", 0755, 0, 0);
        make_directory(home, 0700, uid, gid);
        if (setgroups(0, NULL) || setgid(gid) || setuid(uid)) {
            io_error("postgres: set credentials");
        }
    }
    if (geteuid() != uid || getegid() != gid || chdir(home)) {
        refuse("cannot enter the postgres account or working directory");
    }
    make_directory(data, 0700, uid, gid);
    initialize_cluster();

    const char *binary = "/usr/lib/postgresql/16/bin/postgres";
    execl(binary, binary, "-D", data, "-h", "127.0.0.1",
          "-k", home, "-c", "unix_socket_permissions=0700", "-c", "dynamic_shared_memory_type=posix",
          (char *)NULL);
    io_error("postgres: exec");
}
