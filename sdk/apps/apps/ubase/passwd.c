// passwd.c: Смена пароля с сохранением файла учётных записей
// Автор изменений SynapseOS: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux ELF64 musl
// Подсистема: учётные записи
// Первое изменение: дата исходной адаптации не установлена
// Последнее изменение: 24.11.2026
// Доступ: общедоступный
/* See LICENSE file for copyright and license details. */
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h>

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <shadow.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "config.h"
#include "passwd.h"
#include "text.h"
#include "util.h"

static int
open_tty(void)
{
	int fd;
	const char *tty;

	/* SynapseOS /dev/tty is a global console, while SSH uses a PTY. */
	if (isatty(STDIN_FILENO))return dup(STDIN_FILENO);
	if (isatty(STDERR_FILENO))return dup(STDERR_FILENO);
	fd = open("/dev/tty", O_RDWR);
	if (fd >= 0)
		return fd;

	tty = getenv("SHELL_TTY");
	if (tty && tty[0]) {
		fd = open(tty, O_RDWR);
		if (fd >= 0)
			return fd;
	}

	fd = open("/dev/console", O_RDWR);
	if (fd >= 0)
		return fd;

	fd = open("/dev/tty0", O_RDWR);
	return fd;
}

static volatile sig_atomic_t password_signal;

// Прервать ввод, чтобы обычный путь вернул режим терминала
static void password_interrupted(int number) {
    password_signal = number;
}

// Прочитать строку без эха и вернуть режим терминала перед обработкой сигнала
static char *tty_getpass(const char *prompt) {
    static char buf[BUFSIZ];
    struct termios old, hidden;
    const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    struct sigaction previous[4], action;
    unsigned installed = 0;
    int changed = 0, valid = 0;
    size_t length = 0;
    explicit_bzero(buf, sizeof(buf));
    int ttyfd = open_tty();
    if (ttyfd < 0 || tcgetattr(ttyfd, &old) != 0) {
        if (ttyfd >= 0) {
            close(ttyfd);
        }
        return NULL;
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = password_interrupted;
    sigemptyset(&action.sa_mask);
    password_signal = 0;
    for (; installed < 4; installed++) {
        if (sigaction(signals[installed], &action, &previous[installed]) != 0) {
            goto done;
        }
    }

    hidden = old;
    hidden.c_lflag = (hidden.c_lflag | ICANON) & ~(ECHO | ECHONL);
    if (tcsetattr(ttyfd, TCSAFLUSH, &hidden) != 0) {
        goto done;
    }
    changed = 1;
    fputs(prompt, stderr);
    fflush(stderr);

    while (password_signal == 0) {
        char byte;
        ssize_t count = read(ttyfd, &byte, 1);
        if (count < 0 && errno == EINTR && password_signal == 0) {
            continue;
        }
        if (count != 1) {
            errno = count == 0 ? EIO : errno;
            break;
        }
        if (byte == '\n' || byte == '\r') {
            valid = 1;
            break;
        }
        if (length >= sizeof(buf) - 1) {
            errno = EOVERFLOW;
            break;
        }
        buf[length++] = byte;
    }
    buf[length] = '\0';

done:
    {
        int error = errno;
        if (changed && tcsetattr(ttyfd, TCSAFLUSH, &old) != 0) {
            error = errno;
            valid = 0;
        }
        close(ttyfd);
        for (unsigned i = 0; i < installed; i++) {
            sigaction(signals[i], &previous[i], NULL);
        }
        fputc('\n', stderr);
        fflush(stderr);
        if (!valid || password_signal != 0) {
            explicit_bzero(buf, sizeof(buf));
        }
        errno = error;
    }
    if (password_signal != 0) {
        raise(password_signal);
        return NULL;
    }
    return valid ? buf : NULL;
}

static void
flush_tty(void)
{
	int fd = open_tty();
	if (fd >= 0) {
		tcflush(fd, TCIFLUSH);
		close(fd);
	}
}

// Заблокировать всю замену учётных данных на постоянном файле
static int lock_password_files(void) {
    int fd = open("/etc/.pwd.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }

    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != 0 ||
        info.st_nlink != 1 || (info.st_mode & 0777) != 0600) {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    while (flock(fd, LOCK_EX) != 0) {
        if (errno != EINTR) {
            int error = errno;
            close(fd);
            errno = error;
            return -1;
        }
    }

    return fd;
}

// Открыть только обычный файл root, не переходя по символьной ссылке
static FILE *open_account_file(const char *path) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }

    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != 0 || info.st_nlink != 1) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }

    FILE *file = fdopen(fd, "r");
    if (file == NULL) {
        int error = errno;
        close(fd);
        errno = error;
    }

    return file;
}

// Найти используемый shadow, не подменяя ошибку доступа другим файлом
static FILE *spw_get_file(const char *user, char path[PATH_MAX]) {
    int length = snprintf(path, PATH_MAX, "/etc/tcb/%s/shadow", user);
    if (length < 0 || length >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    FILE *file = open_account_file(path);
    if (file != NULL || (errno != ENOENT && errno != ENOTDIR)) {
        return file;
    }

    strcpy(path, "/etc/shadow");
    return open_account_file(path);
}

// Сохранить новый файл целиком, затем заменить старое имя и синхронизировать каталог
static int write_password_file(FILE *source, const char *path, const char *user, const char *hash) {
    char directory[PATH_MAX], temporary[PATH_MAX];
    const char *name = strrchr(path, '/');
    if (name == NULL || name == path || (size_t)(name - path) >= sizeof(directory)) {
        errno = EINVAL;
        return -1;
    }

    size_t directory_length = (size_t)(name - path);
    memcpy(directory, path, directory_length);
    directory[directory_length] = '\0';
    int length = snprintf(temporary, sizeof(temporary), "%s/.passwd-XXXXXX", directory);
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int parent = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0) {
        return -1;
    }

    struct stat info;
    if (fstat(parent, &info) != 0 || info.st_uid != 0 || (info.st_mode & 0022) != 0) {
        close(parent);
        errno = EINVAL;
        return -1;
    }

    int fd = mkstemp(temporary);
    if (fd < 0) {
        close(parent);
        return -1;
    }

    int result = -1;
    FILE *target = NULL;
    char *line = NULL;
    size_t capacity = 0;
    if (fchmod(fd, strcmp(path, "/etc/passwd") == 0 ? 0644 : 0600) != 0 ||
        fstat(fd, &info) != 0 || info.st_uid != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1) {
        goto done;
    }

    target = fdopen(fd, "w");
    if (target == NULL) {
        goto done;
    }
    fd = -1;
    size_t user_length = strlen(user);
    int found = 0;
    ssize_t size;
    while ((size = getline(&line, &capacity, source)) >= 0) {
        if (size > 16384) {
            errno = EOVERFLOW;
            goto done;
        }

        if (strncmp(line, user, user_length) == 0 && line[user_length] == ':') {
            char *rest = strchr(line + user_length + 1, ':');
            if (rest == NULL || found != 0) {
                errno = EINVAL;
                goto done;
            }
            found = 1;
            if (fprintf(target, "%s:%s%s", user, hash, rest) < 0) {
                goto done;
            }
        } else if (fputs(line, target) == EOF) {
            goto done;
        }
    }

    if (ferror(source) || found != 1 || fflush(target) != 0 || fsync(fileno(target)) != 0) {
        if (found != 1) {
            errno = EINVAL;
        }
        goto done;
    }

    if (fclose(target) != 0) {
        target = NULL;
        goto done;
    }
    target = NULL;
    if (rename(temporary, path) != 0 || fsync(parent) != 0) {
        goto done;
    }
    result = 0;

done:
    {
        int error = errno;
        free(line);
        if (target != NULL) {
            fclose(target);
        }
        if (fd >= 0) {
            close(fd);
        }
        if (result != 0) {
            unlink(temporary);
        }
        close(parent);
        errno = error;
    }
    return result;
}

// Новые учётные записи публикуются после shadow и групп, под тем же замком что passwd
struct account_edit {
    const char *path;
    char tmp[64];
    FILE *out;
    int fd;
};

static struct account_edit edits[] = {
    {.path = "/etc/group", .fd = -1},
    {.path = "/etc/shadow", .fd = -1},
    {.path = "/etc/passwd", .fd = -1},
};

static void account_cleanup(void) {
    for (size_t i = 0; i < sizeof edits / sizeof edits[0]; i++) {
        if (edits[i].tmp[0]) {
            unlink(edits[i].tmp);
        }
    }
}

static void account_error(const char *message) {
    eprintf("%s\n", message);
}

static int account_name(const char *name) {
    return name && *name && strlen(name) <= 32 && !strspn(name, "0123456789") &&
           strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == strlen(name) &&
           *name != '-';
}

static unsigned account_number(const char *s) {
    if (!*s || strspn(s, "0123456789") != strlen(s)) {
        account_error("invalid numeric identifier");
    }
    errno = 0;
    unsigned long n = strtoul(s, NULL, 10);
    if (errno || n == 0 || n >= UINT32_MAX) {
        account_error("identifier must be between 1 and 4294967294");
    }
    return (unsigned)n;
}

static FILE *account_stage(struct account_edit *e, FILE **source) {
    *source = open_account_file(e->path);
    if (!*source) {
        eprintf("open %s:", e->path);
    }
    struct stat info;
    if (fstat(fileno(*source), &info) || (info.st_mode & 0022)) {
        account_error("unsafe account database permissions");
    }
    strcpy(e->tmp, "/etc/.account-XXXXXX");
    e->fd = mkstemp(e->tmp);
    if (e->fd < 0 || fchown(e->fd, info.st_uid, info.st_gid) || fchmod(e->fd, info.st_mode & 0777)) {
        eprintf("stage %s:", e->path);
    }
    e->out = fdopen(e->fd, "w");
    if (!e->out) {
        eprintf("fdopen:");
    }
    return e->out;
}

static void account_finish(struct account_edit *e, FILE *source) {
    if (ferror(source) || fclose(source) || fflush(e->out) || fsync(e->fd) || fclose(e->out)) {
        eprintf("write %s:", e->path);
    }
}

static void account_commit(int parent) {
    char baks[sizeof edits / sizeof edits[0]][64];
    size_t n = sizeof edits / sizeof edits[0];
    int err = 0;

    for (size_t i = 0; i < n; i++) {
        baks[i][0] = 0;
        if (!edits[i].tmp[0]) {
            continue;
        }
        snprintf(baks[i], sizeof baks[i], "%s.bak.%ld", edits[i].path, (long)getpid());
        unlink(baks[i]);
        if (link(edits[i].path, baks[i]) != 0) {
            err = errno;
            for (size_t j = 0; j < i; j++) {
                if (baks[j][0]) {
                    unlink(baks[j]);
                }
            }
            errno = err;
            eprintf("backup %s:", edits[i].path);
        }
    }

    for (size_t i = 0; i < n; i++) {
        if (!edits[i].tmp[0]) {
            continue;
        }
        if (rename(edits[i].tmp, edits[i].path) != 0) {
            err = errno;
            for (size_t j = 0; j < i; j++) {
                if (baks[j][0]) {
                    rename(baks[j], edits[j].path);
                }
            }
            for (size_t j = 0; j < n; j++) {
                if (baks[j][0]) {
                    unlink(baks[j]);
                }
            }
            fsync(parent);
            errno = err;
            eprintf("publish %s:", edits[i].path);
        }
        edits[i].tmp[0] = 0;
    }

    for (size_t i = 0; i < n; i++) {
        if (baks[i][0]) {
            unlink(baks[i]);
        }
    }
    if (fsync(parent) != 0) {
        eprintf("sync /etc:");
    }
}

static unsigned unused_id(int group) {
    for (unsigned i = 1000; i < 60000; i++) {
        if (group ? getgrgid(i) == NULL : getpwuid(i) == NULL) {
            return i;
        }
    }
    account_error("no free identifier");
    return 0;
}

static int account_match(const char *line, const char *name) {
    size_t n = strlen(name);
    return strncmp(line, name, n) == 0 && line[n] == ':';
}

static void append_record(struct account_edit *e, const char *name, const char *record) {
    FILE *in;
    FILE *out = account_stage(e, &in);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int newline = 1;
    while ((n = getline(&line, &cap, in)) >= 0) {
        if (account_match(line, name)) {
            account_error("duplicate account database entry");
        }
        if (fputs(line, out) == EOF) {
            eprintf("write %s:", e->path);
        }
        newline = n > 0 && line[n - 1] == '\n';
    }
    if ((!newline && fputc('\n', out) == EOF) || fputs(record, out) == EOF) {
        eprintf("write %s:", e->path);
    }
    free(line);
    account_finish(e, in);
}

static void change_groups(const char *name, const char *list, unsigned create_gid) {
    char *groups = list ? estrdup(list) : NULL;
    char *items[128];
    size_t count = 0, seen = 0;
    for (char *p = groups, *part; p && (part = strsep(&p, ","));) {
        if (!account_name(part) || count == sizeof items / sizeof items[0] || !getgrnam(part)) {
            account_error("unknown or invalid supplementary group");
        }
        for (size_t i = 0; i < count; i++) {
            if (!strcmp(items[i], part)) {
                account_error("duplicate supplementary group");
            }
        }
        items[count++] = part;
    }
    FILE *in;
    FILE *out = account_stage(&edits[0], &in);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int newline = 1;
    while ((n = getline(&line, &cap, in)) >= 0) {
        newline = n > 0 && line[n - 1] == '\n';
        size_t i = 0;
        while (i < count && !account_match(line, items[i])) {
            i++;
        }
        if (i == count) {
            if (fputs(line, out) == EOF) {
                eprintf("write group:");
            }
            continue;
        }
        seen++;
        char *members = strrchr(line, ':');
        if (!members) {
            account_error("invalid group record");
        }
        members++;
        line[strcspn(line, "\n")] = 0;
        int exists = 0;
        size_t length = strlen(name);
        for (char *p = members; *p;) {
            size_t len = strcspn(p, ",");
            if (len == length && !strncmp(p, name, len)) {
                exists = 1;
            }
            p += len + (p[len] == ',');
        }
        if (fprintf(out, "%s%s%s\n", line, exists || !*members ? "" : ",", exists ? "" : name) < 0) {
            eprintf("write group:");
        }
        newline = 1;
    }
    if (seen != count) {
        account_error("supplementary group not found in local group file");
    }
    if (create_gid && ((!newline && fputc('\n', out) == EOF) ||
                       fprintf(out, "%s:x:%u:\n", name, create_gid) < 0)) {
        eprintf("write group:");
    }
    free(line);
    free(groups);
    account_finish(&edits[0], in);
}

static void change_shell(const char *name, const char *shell) {
    FILE *in;
    FILE *out = account_stage(&edits[2], &in);
    char *line = NULL;
    size_t cap = 0;
    int found = 0;
    while (getline(&line, &cap, in) >= 0) {
        if (account_match(line, name)) {
            char *last = strrchr(line, ':');
            if (!last || found++) {
                account_error("ambiguous passwd entry");
            }
            last[1] = 0;
            if (fprintf(out, "%s%s\n", line, shell) < 0) {
                eprintf("write passwd:");
            }
        } else if (fputs(line, out) == EOF) {
            eprintf("write passwd:");
        }
    }
    if (!found) {
        account_error("user not found in local passwd file");
    }
    free(line);
    account_finish(&edits[2], in);
}

static void account_usage(const char *command) {
    fprintf(stderr, "usage: %s\n", command);
    exit(2);
}

static int account_main(const char *cmd, int argc, char **argv) {
    int add = !strcmp(cmd, "useradd") || !strcmp(cmd, "adduser");
    int group = !strcmp(cmd, "groupadd");
    const char *usage_text = add ? "useradd [-m] [-s SHELL] [-u UID] [-g GROUP] [-G GROUPS] USER" :
        group ? "groupadd [-g GID] GROUP" : "usermod [-a -G GROUPS] [-s SHELL] USER";
    const char *shell = NULL, *supp = NULL, *primary = NULL, *uid_arg = NULL;
    int make_home = 0, append = 0, option;
    while ((option = getopt(argc, argv, add ? "ms:u:g:G:" : group ? "g:" : "as:G:")) != -1) {
        switch (option) {
        case 'm': make_home = 1; break;
        case 'a': append = 1; break;
        case 's': shell = optarg; break;
        case 'G': supp = optarg; break;
        case 'g': primary = optarg; break;
        case 'u': uid_arg = optarg; break;
        default: account_usage(usage_text);
        }
    }
    if (optind + 1 != argc || !account_name(argv[optind]) ||
        (!add && !group && ((!shell && !supp) || append != (supp != NULL)))) {
        account_usage(usage_text);
    }
    if (shell && (*shell != '/' || strpbrk(shell, ":\r\n") || access(shell, X_OK))) {
        account_error("shell must be an executable absolute path");
    }
    if (getuid() != 0 || geteuid() != 0) {
        account_error("only root may manage accounts");
    }
    umask(077);
    int lock = lock_password_files();
    if (lock < 0) {
        eprintf("lock account database:");
    }
    int parent = open("/etc", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat info;
    if (parent < 0 || fstat(parent, &info) || info.st_uid != 0 || (info.st_mode & 0022)) {
        account_error("unsafe /etc directory");
    }
    if (atexit(account_cleanup)) {
        account_error("cannot register cleanup");
    }
    const char *name = argv[optind];
    char record[PATH_MAX + 128];
    if (group) {
        unsigned gid = primary ? account_number(primary) : unused_id(1);
        if (getgrnam(name) || getgrgid(gid)) {
            account_error("group or GID already exists");
        }
        snprintf(record, sizeof record, "%s:x:%u:\n", name, gid);
        append_record(&edits[0], name, record);
    } else if (add) {
        unsigned uid = uid_arg ? account_number(uid_arg) : unused_id(0);
        if (getpwnam(name) || getpwuid(uid)) {
            account_error("user or UID already exists");
        }
        struct group *gr;
        unsigned private_gid = 0, gid;
        if (primary) {
            gr = account_name(primary) ? getgrnam(primary) : getgrgid(account_number(primary));
            if (!gr) {
                account_error("primary group does not exist");
            }
            gid = gr->gr_gid;
        } else {
            gr = getgrnam(name);
            if (gr) {
                account_error("group already exists; select it explicitly with -g");
            }
            private_gid = gid = unused_id(1);
        }
        if (gid == 0) {
            account_error("root group cannot be the primary group");
        }
        if (supp || private_gid) {
            change_groups(name, supp, private_gid);
        }
        snprintf(record, sizeof record, "%s:!:::::::\n", name);
        append_record(&edits[1], name, record);
        char home[64];
        snprintf(home, sizeof home, "/home/%s", name);
        int n = snprintf(record, sizeof record, "%s:x:%u:%u::%s:%s\n",
                         name, uid, gid, home, shell ? shell : "/bin/sh");
        if (n < 0 || (size_t)n >= sizeof record) {
            account_error("passwd entry too long");
        }
        append_record(&edits[2], name, record);
        if (make_home) {
            int homes = open("/home", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (homes < 0 && errno == ENOENT && mkdir("/home", 0755) == 0) {
                chmod("/home", 0755);
                homes = open("/home", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            }
            if (homes < 0 || fstat(homes, &info) || info.st_uid != 0 || (info.st_mode & 0022) ||
                mkdirat(homes, name, 0700)) {
                eprintf("create home directory:");
            }
            if (fchownat(homes, name, uid, gid, AT_SYMLINK_NOFOLLOW) || fsync(homes)) {
                int error = errno;
                unlinkat(homes, name, AT_REMOVEDIR);
                errno = error;
                eprintf("set home owner:");
            }
            close(homes);
        }
    } else {
        if (!getpwnam(name)) {
            account_error("no such user");
        }
        if (supp) {
            change_groups(name, supp, 0);
        }
        if (shell) {
            change_shell(name, shell);
        }
    }
    account_commit(parent);
    close(parent);
    close(lock);
    return 0;
}

// Получить полную случайную соль для имеющегося SHA-512 crypt
static int
gensalt(char *s)
{
	static const char b64[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	uint8_t buf[12];
	uint32_t n;
	int i;

    size_t done = 0;
    while (done < sizeof(buf)) {
        ssize_t count = syscall(SYS_getrandom, buf + done, sizeof(buf) - done, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (count == 0) {
                errno = EIO;
            }
            explicit_bzero(buf, sizeof(buf));
            return -1;
        }
        done += (size_t)count;
    }
	for (i = 0; i < 12; i += 3) {
		n = buf[i] << 16 | buf[i+1] << 8 | buf[i+2];
		*s++ = b64[n%64]; n /= 64;
		*s++ = b64[n%64]; n /= 64;
		*s++ = b64[n%64]; n /= 64;
		*s++ = b64[n];
	}
	*s++ = '\0';
    explicit_bzero(buf, sizeof(buf));
    return 0;
}

// Показать поддерживаемый способ запуска
static void
usage(void)
{
	eprintf("usage: %s [--stdin] [username [password]]\n", argv0);
}

// Сменить пароль после проверки аккаунта и полного подтверждения ввода
int main(int argc, char *argv[]) {
    const char *cmd = strrchr(argv[0], '/');
    cmd = cmd ? cmd + 1 : argv[0];
    if (!strcmp(cmd, "useradd") || !strcmp(cmd, "adduser") ||
        !strcmp(cmd, "usermod") || !strcmp(cmd, "groupadd")) {
        argv0 = (char *)cmd;
        return account_main(cmd, argc, argv);
    }

    char *cryptpass = NULL, *inpass = NULL, *encoded;
    char salt[sizeof(PW_CIPHER) + 16] = PW_CIPHER;
    char account_path[PATH_MAX];
    struct passwd *account;
    struct spwd *shadow = NULL;
    FILE *file = NULL;
    int lock_fd = -1, status = 1;
    const char *target_user = NULL, *direct_password = NULL;
    char stdin_password[256];
    bool password_stdin = false;

    explicit_bzero(stdin_password, sizeof(stdin_password));
    if (argc == 3 && !strcmp(argv[1], "--stdin")) {
        password_stdin = true;
        argv[1] = argv[2];
        argc = 2;
    }

    ARGBEGIN {
    default:
        usage();
        goto cleanup;
    } ARGEND;
    if (argc > 2 || (password_stdin && argc != 1)) {
        usage();
        goto cleanup;
    }
    if (argc >= 1) {
        target_user = argv[0];
    }
    if (argc == 2) {
        direct_password = argv[1];
    }
    if (password_stdin) {
        if (fgets(stdin_password, sizeof(stdin_password), stdin) == NULL) {
            weprintf("read password from stdin:\n");
            goto cleanup;
        }
        stdin_password[strcspn(stdin_password, "\r\n")] = 0;
        direct_password = stdin_password;
    }

    pw_init();
    umask(077);
    lock_fd = lock_password_files();
    if (lock_fd < 0) {
        weprintf("lock password files:");
        goto cleanup;
    }

    account = target_user != NULL ? getpwnam(target_user) : getpwuid(getuid());
    if (account == NULL || account->pw_name == NULL || account->pw_passwd == NULL) {
        weprintf("account is unavailable\n");
        goto cleanup;
    }
    if (strcmp(account->pw_passwd, "x") == 0) {
        shadow = getspnam(account->pw_name);
        if (shadow == NULL || shadow->sp_pwdp == NULL) {
            weprintf("shadow account is unavailable\n");
            goto cleanup;
        }
    }

    if (direct_password != NULL) {
        if (getuid() != 0) {
            weprintf("only root can set password non-interactively\n");
            goto cleanup;
        }
    } else {
        flush_tty();
        if (getuid() != 0) {
            const char *prior = shadow != NULL ? shadow->sp_pwdp : account->pw_passwd;
            if (prior[0] == '!' || prior[0] == '*') {
                weprintf("denied\n");
                goto cleanup;
            }
            if (prior[0] != '\0') {
                inpass = tty_getpass("Old password: ");
                if (inpass == NULL || inpass[0] == '\0') {
                    weprintf("no password supplied\n");
                    goto cleanup;
                }
                encoded = crypt(inpass, prior);
                explicit_bzero(inpass, strlen(inpass));
                if (encoded == NULL || encoded[0] == '*' || strcmp(encoded, prior) != 0) {
                    weprintf("incorrect password\n");
                    goto cleanup;
                }
            }
        }
    }

    if (direct_password != NULL) {
        inpass = (char *)direct_password;
    } else {
        inpass = tty_getpass("Enter new password: ");
    }
    if (inpass == NULL || inpass[0] == '\0') {
        weprintf("no password supplied\n");
        goto cleanup;
    }
    if (gensalt(salt + strlen(salt)) != 0) {
        weprintf("getrandom:");
        goto cleanup;
    }
    encoded = crypt(inpass, salt);
    explicit_bzero(inpass, strlen(inpass));
    if (encoded == NULL || encoded[0] == '*') {
        weprintf("crypt failed\n");
        goto cleanup;
    }
    cryptpass = strdup(encoded);
    if (cryptpass == NULL) {
        weprintf("copy password hash:");
        goto cleanup;
    }

    if (direct_password == NULL) {
        flush_tty();
        inpass = tty_getpass("Retype new password: ");
        if (inpass == NULL || inpass[0] == '\0') {
            weprintf("no password supplied\n");
            goto cleanup;
        }
        encoded = crypt(inpass, salt);
        explicit_bzero(inpass, strlen(inpass));
        if (encoded == NULL || encoded[0] == '*' || strcmp(cryptpass, encoded) != 0) {
            weprintf("passwords do not match\n");
            goto cleanup;
        }
    }

    file = spw_get_file(account->pw_name, account_path);
    if (file == NULL && (errno == ENOENT || errno == ENOTDIR)) {
        strcpy(account_path, "/etc/passwd");
        file = open_account_file(account_path);
    }
    if (file == NULL || write_password_file(file, account_path, account->pw_name, cryptpass) != 0) {
        weprintf("update password file:");
        goto cleanup;
    }
    status = 0;

cleanup:
    if (inpass != NULL) {
        explicit_bzero(inpass, strlen(inpass));
    }
    if (file != NULL && fclose(file) != 0) {
        status = 1;
    }
    if (cryptpass != NULL) {
        explicit_bzero(cryptpass, strlen(cryptpass));
        free(cryptpass);
    }
    if (lock_fd >= 0) {
        close(lock_fd);
    }
    explicit_bzero(salt, sizeof(salt));
    explicit_bzero(stdin_password, sizeof(stdin_password));
    return status;
}
