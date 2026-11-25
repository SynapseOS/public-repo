// ramroot.c: Подготовка корня в RAM перед штатным init
// Автор: Елчинян Арен Артурович 2026
// Архитектура: x86_64 и AArch64, ABI: общий POSIX и zlib
// Описание: Ограниченная распаковка gzip/newc через обычную VFS без контроллера накопителя
// Подсистема: загрузка пользовательского пространства
// Первое изменение: 25.11.2026
// Последнее изменение: 25.11.2026
// Доступ: общедоступный
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>
#define ROOT_LIMIT (UINT64_C(1024) * 1024 * 1024)
#define FILE_LIMIT (UINT32_C(256) * 1024 * 1024)
#define PATH_LIMIT 256
struct archive { gzFile file; uint64_t bytes; unsigned files; };

// Ограничить распакованный поток и отличить ранний EOF от полного архива
static bool read_exact(struct archive *a, void *out, unsigned bytes) {
    if (bytes > ROOT_LIMIT - a->bytes) {
        errno = EFBIG;
        return false;
    }
    unsigned done = 0;
    while (done < bytes) {
        int n = gzread(a->file, (uint8_t *)out + done, bytes - done);
        if (n <= 0) {
            errno = EINVAL;
            return false;
        }
        done += (unsigned)n;
        a->bytes += (unsigned)n;
    }
    return true;
}

// Проверить восьмизначное шестнадцатеричное поле newc
static bool field(const char *p, uint32_t *out) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned digit;
        if (p[i] >= '0' && p[i] <= '9') {
            digit = (unsigned)(p[i] - '0');
        } else if (p[i] >= 'a' && p[i] <= 'f') {
            digit = (unsigned)(p[i] - 'a') + 10;
        } else if (p[i] >= 'A' && p[i] <= 'F') {
            digit = (unsigned)(p[i] - 'A') + 10;
        } else {
            errno = EINVAL;
            return false;
        }
        value = (value << 4) | digit;
    }
    *out = value;
    return true;
}

// Имена архива остаются относительными и не выходят из выбранного корня
static bool valid_path(const char *name) {
    if (!name[0] || name[0] == '/') {
        return false;
    }
    for (const char *p = name; *p;) {
        const char *end = strchr(p, '/');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        if (!length || (length == 1 && p[0] == '.') || (length == 2 && p[0] == '.' && p[1] == '.')) {
            return false;
        }
        if (!end) {
            break;
        }
        p = end + 1;
        if (!*p) {
            return false;
        }
    }
    return true;
}

// Открыть родителя по дескрипторам, не следуя ссылкам из ранее распакованных записей
static int parent_fd(int root, char *name, char **leaf) {
    int fd = dup(root);
    char *p = name;
    while (fd >= 0) {
        char *slash = strchr(p, '/');
        if (!slash) {
            *leaf = p;
            return fd;
        }
        *slash = 0;
        int next = openat(fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && mkdirat(fd, p, 0755) == 0) {
            next = openat(fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        close(fd);
        fd = next;
        p = slash + 1;
    }
    return -1;
}

// Относительная ссылка также не может подняться выше корня
static bool valid_link(const char *path, const char *target) {
    if (!target[0] || target[0] == '/') {
        return false;
    }
    unsigned depth = 0;
    bool descended = false;
    for (const char *p = path; *p; ++p) {
        depth += *p == '/';
    }
    for (const char *p = target; *p;) {
        const char *slash = strchr(p, '/');
        size_t length = slash ? (size_t)(slash - p) : strlen(p);
        if (length == 2 && p[0] == '.' && p[1] == '.') {
            // После перехода в другое имя оно уже может оказаться ссылкой на каталог
            if (!depth || descended) {
                return false;
            }
            --depth;
        } else if (length && !(length == 1 && p[0] == '.')) {
            ++depth;
            descended = true;
        }
        if (!slash) {
            break;
        }
        p = slash + 1;
    }
    return true;
}

// Дополнение до четырёх байтов не содержит полезных данных
static bool padding(struct archive *a) {
    unsigned bytes = (unsigned)(-a->bytes & 3u);
    uint8_t pad[3] = {0};
    return !bytes || read_exact(a, pad, bytes);
}

// Создать только новый файл, не заменяя работающий init или ранее существующую библиотеку
static bool regular_file(struct archive *a, int parent, const char *name, uint32_t size, unsigned mode) {
    int fd = openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    bool ok = true;
    uint8_t buffer[16384];
    while (ok && size) {
        unsigned count = size < sizeof(buffer) ? size : sizeof(buffer);
        ok = read_exact(a, buffer, count);
        unsigned done = 0;
        while (ok && done < count) {
            ssize_t n = write(fd, buffer + done, count - done);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                ok = false;
            } else {
                done += (unsigned)n;
            }
        }
        size -= count;
    }
    if (ok) {
        ok = fchmod(fd, mode & 0777u) == 0;
    }
    int saved = errno;
    if (close(fd) && ok) {
        ok = false;
        saved = errno;
    }
    if (!ok) {
        (void)unlinkat(parent, name, 0);
    }
    errno = saved;
    return ok;
}

// Проверить конец сжатого потока, включая CRC gzip после TRAILER
static bool finish(struct archive *a) {
    uint8_t pad[512];
    unsigned tail = 0;
    for (;;) {
        int count = gzread(a->file, pad, sizeof(pad));
        if (count < 0 || tail > 4096 || (count == 0 && !gzeof(a->file))) {
            errno = EINVAL;
            return false;
        }
        if (!count) {
            return true;
        }
        tail += (unsigned)count;
        for (int i = 0; i < count; ++i) {
            if (pad[i]) {
                errno = EINVAL;
                return false;
            }
        }
    }
}

// Распаковать ограниченное подмножество newc в уже смонтированную общую VFS
static bool unpack(const char *source, const char *destination) {
    int root = open(destination, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct archive a = {.file = gzopen(source, "rb")};
    if (root < 0 || !a.file) {
        if (root >= 0) {
            close(root);
        }
        if (a.file) {
            gzclose(a.file);
        }
        return false;
    }
    bool ok = false;
    for (;;) {
        char header[110], name[PATH_LIMIT];
        uint32_t mode, size, namesize, links;
        if (++a.files > 65536 || !read_exact(&a, header, sizeof(header)) || memcmp(header, "070701", 6) ||
            !field(header + 14, &mode) || !field(header + 38, &links) ||
            !field(header + 54, &size) || !field(header + 94, &namesize) ||
            !namesize || namesize > sizeof(name) || size > FILE_LIMIT ||
            !read_exact(&a, name, namesize) || name[namesize - 1] || strlen(name) != namesize - 1 || !padding(&a)) {
            errno = EINVAL;
            break;
        }
        if (!strcmp(name, "TRAILER!!!")) {
            ok = !size && finish(&a);
            break;
        }
        if (!valid_path(name)) {
            errno = EINVAL;
            break;
        }
        char original[PATH_LIMIT];
        memcpy(original, name, namesize);
        char *leaf = NULL;
        int parent = parent_fd(root, name, &leaf);
        if (parent < 0) {
            break;
        }
        bool created = false;
        if (S_ISDIR(mode) && !size) {
            created = mkdirat(parent, leaf, mode & 0777u) == 0;
            if (!created && errno == EEXIST) {
                int fd = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                created = fd >= 0;
                if (fd >= 0) {
                    close(fd);
                }
            }
        } else if (S_ISREG(mode) && links == 1) {
            created = regular_file(&a, parent, leaf, size, mode);
        } else if (S_ISLNK(mode) && links == 1 && size && size < PATH_LIMIT) {
            char target[PATH_LIMIT];
            if (read_exact(&a, target, size)) {
                target[size] = 0;
                created = strlen(target) == size && valid_link(original, target) &&
                    symlinkat(target, parent, leaf) == 0;
            }
        } else {
            errno = EINVAL;
        }
        close(parent);
        if (!created || !padding(&a)) {
            break;
        }
    }
    int saved = errno;
    if (gzclose(a.file) != Z_OK) {
        ok = false;
        saved = EINVAL;
    }
    close(root);
    errno = saved;
    if (ok) {
        printf("RAMROOT_READY files=%u bytes=%llu\n", a.files - 1, (unsigned long long)a.bytes);
        fflush(stdout);
    }
    return ok;
}

// В обычном запуске сохранить PID 1, а графический сеанс оставить отдельным процессом
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--extract")) {
        if (unpack(argv[2], argv[3])) {
            return 0;
        }
        perror("ramroot: extract");
        return 1;
    }
    if (argc != 1 || getpid() != 1) {
        fputs("usage: ramroot --extract ARCHIVE DIRECTORY\n", stderr);
        return 2;
    }
    bool ready = unpack("/etc/boot-root.cpio.gz", "/");
    if (!ready) {
        perror("ramroot: recovery init");
    } else if (access("/bin/boot-session", X_OK) == 0) {
        pid_t child = fork();
        if (!child) {
            execl("/bin/boot-session", "boot-session", (char *)NULL);
            perror("ramroot: boot-session");
            _exit(127);
        }
        if (child < 0) {
            perror("ramroot: fork");
        }
    }
    execl("/bin/init", "init", (char *)NULL);
    perror("ramroot: init");
    for (;;) {
        pause();
    }
}
