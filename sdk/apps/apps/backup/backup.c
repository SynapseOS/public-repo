// backup.c: Локальные резервные копии
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Создание и проверка архивов перед сохранением или восстановлением
// Подсистема: установка и начальная настройка системы
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

// Проверяем архив перед сохранением и извлечением
#define _XOPEN_SOURCE 700
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

static char temporary_archive[PATH_MAX];
static void cleanup(void) {
    if (temporary_archive[0]) {
        unlink(temporary_archive);
    }
}
static void fail(const char *s) {
    fprintf(stderr, "backup: %s%s%s\n", s, errno ? ": " : "",
            errno ? strerror(errno) : "");
    exit(1);
}
static void invalid(const char *s) {
    errno = 0;
    fail(s);
}
static int readblock(int fd, unsigned char *b) {
    size_t n = 0;
    while (n < 512) {
        ssize_t r = read(fd, b + n, 512 - n);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0) {
            fail("read archive");
        }
        if (!r) {
            return n ? -1 : 0;
        }
        n += (size_t)r;
    }
    return 1;
}
static uint64_t octal(const unsigned char *p, size_t n) {
    uint64_t v = 0;
    size_t i = 0;
    int digits = 0;
    while (i < n && p[i] == ' ') {
        i++;
    }

    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
        if (v > (UINT64_MAX - 7) / 8) {
            invalid("numeric overflow");
        }
        v = v * 8 + p[i] - '0';
        digits = 1;
    }

    for (; i < n; i++) {
        if (p[i] != 0 && p[i] != ' ') {
            invalid("invalid numeric field");
        }
    }
    if (!digits) {
        invalid("empty numeric field");
    }
    return v;
}
static int safe_path(const char *s) {
    if (!*s || *s == '/' || *s == '-') {
        return 0;
    }

    while (*s) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (!n || (n == 2 && s[0] == '.' && s[1] == '.')) {
            return 0;
        }
        if (n == 1 && *s == '.') {
            if (!e) {
                return 0;
            }
        }
        if (!e) {
            break;
        }
        s = e + 1;
        if (!*s) {
            break;
        }
    }
    return 1;
}
static int scan(int fd, int show) {
    unsigned char b[512];
    int entries = 0, zeros = 0, r;
    if (lseek(fd, 0, SEEK_SET) < 0) {
        fail("seek archive");
    }

    while ((r = readblock(fd, b)) == 1) {
        size_t i;
        int blank = 1;
        for (i = 0; i < 512; i++) {
            if (b[i]) {
                blank = 0;
                break;
            }
        }
        if (blank) {
            zeros++;
            if (zeros >= 2) {
                while ((r = readblock(fd, b)) == 1) {
                    for (i = 0; i < 512; i++) {
                        if (b[i]) {
                            invalid("data after archive end");
                        }
                    }
                }
                if (r < 0) {
                    invalid("truncated archive padding");
                }
                return entries;
            }
            continue;
        }
        if (zeros) {
            invalid("missing second end block");
        }
        uint64_t sum = 0, stored = octal(b + 148, 8);
        for (i = 0; i < 512; i++) {
            sum += (i >= 148 && i < 156) ? ' ' : b[i];
        }
        if (sum != stored) {
            invalid("archive header checksum mismatch");
        }
        if (memcmp(b + 257, "ustar", 5) != 0) {
            invalid("only ustar archives are supported");
        }

        char name[257];
        size_t pn = strnlen((char *)b + 345, 155), nn = strnlen((char *)b, 100);
        if (pn + nn + !!pn >= sizeof(name)) {
            invalid("archive path too long");
        }
        memcpy(name, b + 345, pn);
        if (pn) {
            name[pn++] = '/';
        }
        memcpy(name + pn, b, nn);
        name[pn + nn] = 0;
        if (!safe_path(name)) {
            invalid("unsafe archive path");
        }
        if (b[156] != '0' && b[156] != 0 && b[156] != '5') {
            invalid("links, devices and extended tar headers are unsupported");
        }
        uint64_t size = octal(b + 124, 12);
        (void)octal(b + 100, 8);
        if (b[156] == '5' && size) {
            invalid("directory with data");
        }
        if (show) {
            puts(name);
        }
        entries++;
        uint64_t blocks = size / 512 + !!(size % 512);
        while (blocks--) {
            if (readblock(fd, b) != 1) {
                invalid("truncated archive data");
            }
        }
    }
    invalid("archive missing end blocks");
    return 0;
}
static int run_tar(char **args, int input) {
    pid_t p = fork();
    if (p < 0) {
        fail("fork tar");
    }
    if (!p) {
        if (input >= 0) {
            if (dup2(input, STDIN_FILENO) < 0) {
                _exit(126);
            }
            if (input != STDIN_FILENO) {
                close(input);
            }
        }
        execv("/bin/tar", args);
        perror("backup: /bin/tar");
        _exit(127);
    }

    int status;
    while (waitpid(p, &status, 0) < 0) {
        if (errno != EINTR) {
            fail("wait tar");
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
static void empty_directory(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (errno != ENOENT) {
            fail("destination");
        }
        if (mkdir(path, 0700) < 0) {
            fail("create destination");
        }
    } else if (!S_ISDIR(st.st_mode)) {
        invalid("restore destination must be a directory, not a link");
    }
    DIR *d = opendir(path);
    if (!d) {
        fail("open destination");
    }

    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            invalid("restore destination must be empty");
        }
    }
    closedir(d);
}
static void preflight(const char *path, dev_t device, unsigned depth) {
    if (depth > 128) {
        invalid("directory nesting too deep");
    }

    struct stat st;
    if (lstat(path, &st) < 0) {
        fail(path);
    }
    if (st.st_dev != device) {
        invalid("mounted filesystem inside selected path; back it up separately");
    }
    if (S_ISREG(st.st_mode)) {
        if (st.st_nlink > 1) {
            invalid("hard-linked source file unsupported");
        }

        int f = open(path, O_RDONLY);
        if (f < 0) {
            fail(path);
        }
        close(f);
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        invalid(
            "backup supports regular files and directories only (no links/devices)");
    }
    DIR *d = opendir(path);
    if (!d) {
        fail(path);
    }

    struct dirent *e;
    char child[PATH_MAX];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >=
            (int)sizeof(child)) {
            invalid("path too long");
        }
        preflight(child, device, depth + 1);
    }
    closedir(d);
}
static void help(void) {
    puts("backup create ARCHIVE [--root DIR] [--include-boot] [PATH ...]\n"
         "backup list ARCHIVE | backup test ARCHIVE\n"
         "backup restore ARCHIVE EMPTY-DIRECTORY\n"
         "Defaults: etc var/lib/setup root var/www. Uncompressed tar; no "
         "links/devices.");
}
int main(int argc, char **argv) {
    atexit(cleanup);
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        help();
        return argc < 2 ? 1 : 0;
    }
    if (!strcmp(argv[1], "test") || !strcmp(argv[1], "list") ||
        !strcmp(argv[1], "restore")) {
        int restore = !strcmp(argv[1], "restore");
        if (argc != (restore ? 4 : 3)) {
            help();
            return 1;
        }

        int fd = open(argv[2], O_RDONLY);
        if (fd < 0) {
            fail("open archive");
        }

        int entries = scan(fd, !strcmp(argv[1], "list"));
        if (restore) {
            char dest[PATH_MAX];
            empty_directory(argv[3]);
            if (!realpath(argv[3], dest)) {
                fail("destination path");
            }
            if (lseek(fd, 0, SEEK_SET) < 0) {
                fail("seek archive");
            }

            char *args[] = {"tar", "-x", "-p", "-C", dest, NULL};
            if (!run_tar(args, fd)) {
                invalid("restore failed (destination may contain partial files)");
            }
        }
        close(fd);
        if (strcmp(argv[1], "list")) {
            printf("backup: %s OK (%d entries)\n", argv[1], entries);
        }
        return 0;
    }
    if (strcmp(argv[1], "create") || argc < 3) {
        help();
        return 1;
    }
    const char *root = "/";
    int boot = 0, count = 0;
    char *paths[128];
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--root")) {
            if (++i >= argc) {
                invalid("--root needs a directory");
            }
            root = argv[i];
        } else if (!strcmp(argv[i], "--include-boot")) {
            boot = 1;
        } else {
            if (count >= 120) {
                invalid("too many paths");
            }
            paths[count++] = argv[i];
        }
    }

    char destination[PATH_MAX], parent[PATH_MAX], temp[PATH_MAX], resolved[PATH_MAX];
    if (strlen(argv[2]) >= sizeof(parent)) {
        invalid("archive path too long");
    }
    strcpy(parent, argv[2]);
    char *slash = strrchr(parent, '/');
    const char *base = slash ? slash + 1 : argv[2];
    char filename[PATH_MAX];
    strcpy(filename, base);
    if (!*filename || !strcmp(filename, ".") || !strcmp(filename, "..")) {
        invalid("invalid archive filename");
    }
    if (slash) {
        if (slash == parent) {
            slash[1] = 0;
        } else {
            *slash = 0;
        }
    } else {
        strcpy(parent, ".");
    }
    if (!realpath(parent, resolved)) {
        fail("archive parent");
    }
    if (snprintf(destination, sizeof(destination), "%s/%s", resolved, filename) >=
        (int)sizeof(destination)) {
        invalid("archive path too long");
    }
    if (!realpath(root, resolved)) {
        fail("backup root");
    }
    if (chdir(resolved) < 0) {
        fail("backup root");
    }
    if (!count) {
        char *defaults[] = {"etc", "var/lib/setup", "root", "var/www"};
        struct stat st;
        for (int i = 0; i < 4; i++) {
            if (!lstat(defaults[i], &st)) {
                paths[count++] = defaults[i];
            }
        }
    }
    if (!count) {
        invalid("no backup paths found");
    }

    for (int i = 0; i < count; i++) {
        char *p = paths[i];
        if (!safe_path(p) || !strcmp(p, ".") || !strncmp(p, "./", 2)) {
            invalid("paths must be relative to root without dot components");
        }
        const char *blocked[] = {"proc", "dev", "sys", "tmp", "boot"};
        for (int j = 0; j < 5; j++) {
            if ((j != 4 || !boot) && !strncmp(p, blocked[j], strlen(blocked[j])) &&
                (p[strlen(blocked[j])] == 0 || p[strlen(blocked[j])] == '/')) {
                invalid("excluded system path (boot requires --include-boot)");
            }
        }

        char absolute[PATH_MAX];
        if (!realpath(p, absolute)) {
            fail(p);
        }

        size_t n = strlen(absolute);
        if (!strncmp(destination, absolute, n) &&
            (destination[n] == 0 || destination[n] == '/')) {
            invalid("archive destination lies inside a selected backup path");
        }

        struct stat st;
        if (lstat(p, &st) < 0) {
            fail(p);
        }

        char ancestors[PATH_MAX];
        if (strlen(p) >= sizeof(ancestors)) {
            invalid("path too long");
        }
        strcpy(ancestors, p);
        for (char *a = ancestors; *a; a++) {
            if (*a == '/') {
                *a = 0;
                struct stat parentst;
                if (lstat(ancestors, &parentst) < 0) {
                    fail(ancestors);
                }
                if (!S_ISDIR(parentst.st_mode)) {
                    invalid("path ancestor is not a real directory");
                }
                *a = '/';
            }
        }
        preflight(p, st.st_dev, 0);
    }
    if (snprintf(temp, sizeof(temp), "%s.tmp.XXXXXX", destination) >=
        (int)sizeof(temp)) {
        invalid("archive path too long");
    }

    int fd = mkstemp(temp);
    if (fd < 0) {
        fail("temporary archive");
    }
    strcpy(temporary_archive, temp);
    close(fd);
    char *args[140];
    int n = 0;
    args[n++] = "tar";
    args[n++] = "-c";
    args[n++] = "-f";
    args[n++] = temp;
    for (int i = 0; i < count; i++) {
        args[n++] = paths[i];
    }
    args[n] = NULL;
    if (!run_tar(args, -1)) {
        unlink(temp);
        invalid("tar creation failed; previous archive retained");
    }
    fd = open(temp, O_RDWR);
    if (fd < 0) {
        unlink(temp);
        fail("open created archive");
    } // sbase tar не добавляет завершающие блоки, поэтому записываем их сами
    unsigned char end[1024] = {0};
    if (lseek(fd, 0, SEEK_END) < 0 ||
        write(fd, end, sizeof(end)) != (ssize_t)sizeof(end)) {
        fail("write archive end");
    }

    int entries = scan(fd, 0);
    if (fchmod(fd, 0600) < 0 || fsync(fd) < 0) {
        close(fd);
        unlink(temp);
        fail("sync archive");
    }
    close(fd);
    if (rename(temp, destination) < 0) {
        unlink(temp);
        fail("publish archive");
    }
    temporary_archive[0] = 0;
    printf("backup: created %s (%d entries)\n", destination, entries);
    return 0;
}
