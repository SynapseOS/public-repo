// sftp-server.c: Файловые операции SFTP v3
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Подсистема: Файловые операции SFTP v3 в пользовательском пространстве
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

// SFTP v3 через stdin/stdout после авторизации Dropbear
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#define MAX_PACKET 262144
#define MAX_HANDLES 64
static unsigned char input[MAX_PACKET], output[MAX_PACKET];
static size_t in_size, in_pos, out_size;
static int bad;
struct handle {
    uint32_t token;
    int fd, append;
    DIR *dir;
    char path[4096];
};
static struct handle handles[MAX_HANDLES];
static uint32_t next_token = 1;
static uint32_t get32(void) {
    if (in_size - in_pos < 4) {
        bad = 1;
        return 0;
    }

    unsigned char *p = input + in_pos;
    in_pos += 4;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t get64(void) {
    uint64_t hi = get32();
    return hi << 32 | get32();
}
static const unsigned char *get_string(uint32_t *length) {
    *length = get32();
    if (bad || *length > in_size - in_pos) {
        bad = 1;
        *length = 0;
        return input;
    }
    const unsigned char *p = input + in_pos;
    in_pos += *length;
    return p;
}
static void get_path(char path[4096]) {
    uint32_t length;
    const unsigned char *p = get_string(&length);
    if (bad || length >= 4096 || memchr(p, 0, length)) {
        bad = 1;
        path[0] = 0;
        return;
    }
    memcpy(path, p, length);
    path[length] = 0;
}
static void put32(uint32_t value) {
    if (out_size + 4 > sizeof(output)) {
        exit(1);
    }
    output[out_size++] = value >> 24;
    output[out_size++] = value >> 16;
    output[out_size++] = value >> 8;
    output[out_size++] = value;
}
static void put64(uint64_t value) {
    put32(value >> 32);
    put32(value);
}
static void put_string(const void *data, size_t size) {
    if (out_size + size + 4 > sizeof(output)) {
        exit(1);
    }
    put32(size);
    memcpy(output + out_size, data, size);
    out_size += size;
}
static void begin(int type, uint32_t id) {
    out_size = 5;
    output[4] = type;
    put32(id);
}
static int write_all(int fd, const void *data, size_t length) {
    const unsigned char *p = data;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        p += n;
        length -= n;
    }
    return 0;
}
static void finish(void) {
    uint32_t size = out_size - 4;
    output[0] = size >> 24;
    output[1] = size >> 16;
    output[2] = size >> 8;
    output[3] = size;
    if (write_all(1, output, out_size)) {
        exit(1);
    }
}
static void status(uint32_t id, uint32_t code, const char *message) {
    begin(101, id);
    put32(code);
    put_string(message, strlen(message));
    put_string("", 0);
    finish();
}
static void failure(uint32_t id) {
    uint32_t code = 4;
    if (errno == ENOENT || errno == ENOTDIR) {
        code = 2;
    } else if (errno == EACCES || errno == EPERM) {
        code = 3;
    } else if (errno == ENOSYS || errno == ENOTSUP) {
        code = 8;
    }
    status(id, code, strerror(errno));
}
static void attributes(const struct stat *st) {
    put32(15);
    put64(st->st_size);
    put32(st->st_uid);
    put32(st->st_gid);
    put32(st->st_mode);
    put32(st->st_atime);
    put32(st->st_mtime);
}
struct attrs {
    uint32_t flags, uid, gid, mode, atime, mtime;
    uint64_t size;
};
static struct attrs get_attrs(void) {
    struct attrs a = {0};
    a.flags = get32();
    if (a.flags & 1) {
        a.size = get64();
    }
    if (a.flags & 2) {
        a.uid = get32();
        a.gid = get32();
    }
    if (a.flags & 4) {
        a.mode = get32();
    }
    if (a.flags & 8) {
        a.atime = get32();
        a.mtime = get32();
    }
    if (a.flags & 0x80000000u) {
        uint32_t count = get32();
        if (count > 128) {
            bad = 1;
        }

        for (uint32_t i = 0; i < count && !bad; i++) {
            uint32_t len;
            get_string(&len);
            get_string(&len);
        }
    }
    if (a.flags & ~0x8000000fu) {
        bad = 1;
    }
    return a;
}
static struct handle *get_handle(void) {
    uint32_t length;
    const unsigned char *p = get_string(&length);
    if (bad || length != 4) {
        bad = 1;
        return NULL;
    }
    uint32_t token =
        (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    if (!token) {
        return NULL;
    }

    for (int i = 0; i < MAX_HANDLES; i++) {
        if (handles[i].token == token) {
            return &handles[i];
        }
    }
    return NULL;
}
static struct handle *allocate_handle(void) {
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (!handles[i].token) {
            struct handle *h = &handles[i];
            memset(h, 0, sizeof(*h));
            h->fd = -1;
            h->token = next_token++;
            if (!next_token) {
                next_token = 1;
            }
            return h;
        }
    }
    errno = EMFILE;
    return NULL;
}
static void return_handle(uint32_t id, struct handle *h) {
    unsigned char token[4] = {h->token >> 24, h->token >> 16, h->token >> 8, h->token};
    begin(102, id);
    put_string(token, 4);
    finish();
}
static int apply_attrs(const char *path, int fd, struct attrs a) {
    if (a.flags & 0x80000000u) {
        errno = ENOTSUP;
        return -1;
    }
    if ((a.flags & 2) && (a.uid || a.gid)) {
        errno = ENOTSUP;
        return -1;
    }
    if (a.flags & 1) {
        if (a.size > INT64_MAX) {
            errno = EINVAL;
            return -1;
        }
        if (fd >= 0 ? ftruncate(fd, a.size) : truncate(path, a.size)) {
            return -1;
        }
    }
    if ((a.flags & 4) &&
        (fd >= 0 ? fchmod(fd, a.mode & 0777) : chmod(path, a.mode & 0777))) {
        return -1;
    }
    if (a.flags & 8) {
        struct timespec ts[2] = {{a.atime, 0}, {a.mtime, 0}};
        if (fd >= 0 ? futimens(fd, ts) : utimensat(AT_FDCWD, path, ts, 0)) {
            return -1;
        }
    }
    return 0;
}
static void name_entry(const char *name, const struct stat *st) {
    put_string(name, strlen(name));
    put_string(name, strlen(name));
    attributes(st);
}
static void request(int type, uint32_t id) {
    char path[4096], other[4096];
    struct stat st;
    if (type == 3) { // Обработка открытия файла
        get_path(path);
        uint32_t flags = get32();
        struct attrs a = get_attrs();
        if (bad || flags & ~63u || !(flags & 3u)) {
            status(id, 5, "Invalid OPEN");
            return;
        }
        if (stat(path, &st) == 0 && !S_ISREG(st.st_mode)) {
            status(id, 3, "Only regular files");
            return;
        }

        int mode = (flags & 3u) == 3 ? O_RDWR : (flags & 2u) ? O_WRONLY : O_RDONLY;
        if (flags & 4u) {
            mode |= O_APPEND;
        }
        if (flags & 8u) {
            mode |= O_CREAT;
        }
        if (flags & 16u) {
            mode |= O_TRUNC;
        }
        if (flags & 32u) {
            mode |= O_EXCL;
        }

        struct handle *h = allocate_handle();
        if (!h) {
            failure(id);
            return;
        }
        h->append = (flags & 4u) != 0;
        h->fd = open(path, mode, (a.flags & 4) ? (a.mode & 0777) : 0644);
        if (h->fd < 0) {
            h->token = 0;
            failure(id);
            return;
        }
        // Права open применяются при создании, прочие атрибуты задаются отдельно
        return_handle(id, h);
        return;
    }
    if (type == 4 || type == 5 || type == 6 || type == 8 || type == 10 || type == 12) {
        struct handle *h = get_handle();
        if (bad) {
            status(id, 5, "Invalid handle packet");
            return;
        }
        if (!h) {
            status(id, 4, "Unknown handle");
            return;
        }
        if (type == 4) {
            int rc = h->dir ? closedir(h->dir) : close(h->fd);
            h->token = 0;
            if (rc) {
                failure(id);
            } else {
                status(id, 0, "OK");
            }
            return;
        }
        if (type == 12) {
            if (!h->dir) {
                status(id, 4, "Not a directory handle");
                return;
            }
            begin(104, id);
            size_t count_pos = out_size;
            put32(0);
            uint32_t count = 0;
            struct dirent *entry;
            while (count < 32 && (entry = readdir(h->dir))) {
                int n = snprintf(path, sizeof(path), "%s/%s", h->path, entry->d_name);
                if (n < 0 || n >= (int)sizeof(path) || lstat(path, &st)) {
                    continue;
                }
                name_entry(entry->d_name, &st);
                count++;
            }
            if (!count) {
                status(id, 1, "End of directory");
                return;
            }
            output[count_pos] = count >> 24;
            output[count_pos + 1] = count >> 16;
            output[count_pos + 2] = count >> 8;
            output[count_pos + 3] = count;
            finish();
            return;
        }
        if (h->fd < 0) {
            status(id, 4, "Not a file handle");
            return;
        }
        if (type == 8) {
            if (fstat(h->fd, &st)) {
                failure(id);
            } else {
                begin(105, id);
                attributes(&st);
                finish();
            }
            return;
        }
        if (type == 10) {
            struct attrs a = get_attrs();
            if (bad) {
                status(id, 5, "Invalid attributes");
            } else if (apply_attrs(NULL, h->fd, a)) {
                failure(id);
            } else {
                status(id, 0, "OK");
            }
            return;
        }
        uint64_t offset = get64();
        uint32_t size;
        const unsigned char *data = NULL;
        if (type == 6) {
            data = get_string(&size);
        } else {
            size = get32();
        }
        if (bad || offset > INT64_MAX) {
            status(id, 5, "Invalid transfer");
            return;
        }
        if (lseek(h->fd, (type == 6 && h->append) ? 0 : (off_t)offset,
                  (type == 6 && h->append) ? SEEK_END : SEEK_SET) < 0) {
            failure(id);
            return;
        }
        if (type == 6) {
            if (write_all(h->fd, data, size)) {
                failure(id);
            } else {
                status(id, 0, "OK");
            }
            return;
        }
        if (size > MAX_PACKET - 16) {
            size = MAX_PACKET - 16;
        }
        begin(103, id);
        put32(0);
        ssize_t n = read(h->fd, output + out_size, size);
        if (n < 0) {
            failure(id);
            return;
        }
        if (!n) {
            status(id, 1, "End of file");
            return;
        }
        output[9] = (uint32_t)n >> 24;
        output[10] = (uint32_t)n >> 16;
        output[11] = (uint32_t)n >> 8;
        output[12] = n;
        out_size += n;
        finish();
        return;
    }
    if (type == 7 || type == 9 || type == 11 || type == 13 || type == 14 ||
        type == 15 || type == 16 || type == 17 || type == 18 || type == 19 ||
        type == 20) {
        get_path(path);
        if (bad) {
            status(id, 5, "Invalid path");
            return;
        }
        if (type == 7 || type == 17) {
            if ((type == 7 ? lstat(path, &st) : stat(path, &st))) {
                failure(id);
            } else {
                begin(105, id);
                attributes(&st);
                finish();
            }
            return;
        }
        if (type == 9) {
            struct attrs a = get_attrs();
            if (bad) {
                status(id, 5, "Invalid attributes");
            } else if (apply_attrs(path, -1, a)) {
                failure(id);
            } else {
                status(id, 0, "OK");
            }
            return;
        }
        if (type == 11) {
            struct handle *h = allocate_handle();
            if (!h) {
                failure(id);
                return;
            }
            h->dir = opendir(path);
            if (!h->dir) {
                h->token = 0;
                failure(id);
                return;
            }
            snprintf(h->path, sizeof(h->path), "%s", path);
            return_handle(id, h);
            return;
        }
        if (type == 14) {
            struct attrs a = get_attrs();
            if (bad) {
                status(id, 5, "Invalid attributes");
            } else if (mkdir(path, (a.flags & 4) ? a.mode & 0777 : 0755)) {
                failure(id);
            } else {
                status(id, 0, "OK");
            }
            return;
        }
        if (type == 15 || type == 13) {
            if (type == 15 ? rmdir(path) : unlink(path)) {
                failure(id);
            } else {
                status(id, 0, "OK");
            }
            return;
        }
        if (type == 16) {
            if (!*path) {
                strcpy(path, ".");
            }

            char *real = realpath(path, NULL);
            if (!real) {
                failure(id);
                return;
            }
            if (stat(real, &st)) {
                free(real);
                failure(id);
                return;
            }
            begin(104, id);
            put32(1);
            name_entry(real, &st);
            free(real);
            finish();
            return;
        }
        if (type == 19) {
            ssize_t n = readlink(path, other, sizeof(other) - 1);
            if (n < 0) {
                failure(id);
                return;
            }
            other[n] = 0;
            memset(&st, 0, sizeof(st));
            begin(104, id);
            put32(1);
            name_entry(other, &st);
            finish();
            return;
        }
        get_path(other);
        if (bad) {
            status(id, 5, "Invalid destination");
            return;
        }
        if (type == 20) {
            if (symlink(path, other)) {
                failure(id);
            } else {
                status(id, 0, "OK");
            }
            return;
        }
        if (type == 18) {
            if (lstat(other, &st) == 0) {
                status(id, 4, "Destination exists");
                return;
            }
            if (rename(path, other)) {
                failure(id);
            } else {
                status(id, 0, "OK");
            }
            return;
        }
    }
    status(id, 8, "Operation not supported");
}
static int read_all(void *buf, size_t length) {
    unsigned char *p = buf;
    while (length) {
        ssize_t n = read(0, p, length);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        p += n;
        length -= n;
    }
    return 0;
}
int main(void) {
    int initialized = 0;
    for (;;) {
        unsigned char size[4];
        if (read_all(size, 4)) {
            break;
        }
        in_size = (uint32_t)size[0] << 24 | (uint32_t)size[1] << 16 |
                  (uint32_t)size[2] << 8 | size[3];
        if (!in_size || in_size > sizeof(input) || read_all(input, in_size)) {
            break;
        }

        int type = input[0];
        in_pos = 1;
        bad = 0;
        if (type == 1) {
            uint32_t version = get32();
            if (initialized || bad || version < 3) {
                break;
            }
            out_size = 5;
            output[4] = 2;
            put32(3);
            finish();
            initialized = 1;
            continue;
        }
        uint32_t id = get32();
        if (!initialized) {
            break;
        }
        if (bad) {
            status(id, 5, "Missing request ID");
            continue;
        }
        request(type, id);
    }

    for (int i = 0; i < MAX_HANDLES; i++) {
        if (handles[i].token) {
            if (handles[i].dir) {
                closedir(handles[i].dir);
            } else {
                close(handles[i].fd);
            }
        }
    }
    return 0;
}
