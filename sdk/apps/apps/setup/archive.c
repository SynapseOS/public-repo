// archive.c: Чтение файлов пакетного архива
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Проверка заголовков tar и извлечение содержимого пакета
// Подсистема: пакетный менеджер setup
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 03.11.2026
// Доступ: общедоступный

#include "setup.h"
#include <mbedtls/sha256.h>
static uint64_t octal(const char *p, size_t n) {
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == 0)) {
        i++;
    }

    for (; i < n && p[i] && p[i] != ' '; i++) {
        if (p[i] < '0' || p[i] > '7' || v > (UINT64_MAX >> 3)) {
            fail("invalid tar number");
        }
        v = v * 8 + p[i] - '0';
    }

    for (; i < n; i++) {
        if (p[i] && p[i] != ' ') {
            fail("invalid tar number padding");
        }
    }
    return v;
}
static int zeros(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (p[i]) {
            return 0;
        }
    }
    return 1;
}

// Прочитать ограниченный диапазон без буфера размером со весь пакет
static int archive_read_at(int fd, uint64_t offset, void *buffer, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t count = pread(fd, (unsigned char *)buffer + done, size - done, (off_t)(offset + done));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return -1;
        }

        done += (size_t)count;
    }

    return 0;
}

// Проверить нулевое дополнение, включая весь остаток после двух конечных блоков
static int archive_zeros(int fd, uint64_t offset, uint64_t size) {
    unsigned char buffer[4096];
    while (size != 0) {
        size_t count = size < sizeof(buffer) ? (size_t)size : sizeof(buffer);
        if (archive_read_at(fd, offset, buffer, count) != 0 || !zeros(buffer, count)) {
            return 0;
        }

        offset += count;
        size -= count;
    }

    return 1;
}

// Закрыть архив перед выходом из незавершённой транзакции
static void archive_reject(int fd, const char *message) {
    close(fd);
    fail("%s", message);
}

// Записать проверяемые блоки только в закрытый каталог незавершённой транзакции
static const char *archive_payload(int fd, uint64_t offset, const struct file *file, const char *target) {
    unsigned char buffer[16384], hash[32], expected[32];
    const char *error = NULL;
    int output = -1;
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    if (unhex(file->hash, expected, sizeof(expected)) != 0 || mbedtls_sha256_starts(&context, 0) != 0) {
        error = "invalid payload hash";
        goto done;
    }

    if (target != NULL) {
        output = open(target, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, file->mode);
        if (output < 0) {
            error = "cannot create staged payload";
            goto done;
        }
    }

    uint64_t remaining = file->size;
    while (remaining != 0) {
        size_t count = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        if (archive_read_at(fd, offset, buffer, count) != 0 ||
            mbedtls_sha256_update(&context, buffer, count) != 0) {
            error = "truncated or unreadable payload";
            goto done;
        }

        size_t written = 0;
        while (output >= 0 && written < count) {
            ssize_t result = write(output, buffer + written, count - written);
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                error = "cannot write staged payload";
                goto done;
            }

            written += (size_t)result;
        }

        offset += count;
        remaining -= count;
    }

    if (mbedtls_sha256_finish(&context, hash) != 0 || memcmp(hash, expected, sizeof(hash)) != 0) {
        error = "payload SHA256 mismatch";
        goto done;
    }
    if (output >= 0 && (fchmod(output, file->mode) != 0 || fsync(output) != 0)) {
        error = "cannot sync staged payload";
    }

done:
    mbedtls_sha256_free(&context);
    if (output >= 0 && close(output) != 0 && error == NULL) {
        error = "cannot close staged payload";
    }
    if (error != NULL && target != NULL) {
        unlink(target);
    }

    return error;
}

// Проверить каждый член USTAR без выделения памяти размером с архив или файл
void extract(struct package *p, const char *archive, const char *stage) {
    int fd = open(archive, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        fail("cannot open package archive");
    }

    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        (uint64_t)info.st_size > PACKAGE_LIMIT || (uint64_t)info.st_size != p->size || info.st_size % 512 != 0) {
        archive_reject(fd, "archive length mismatch");
    }

    size_t len = (size_t)info.st_size;

    int seen[MAXFILES] = {0}, manifest = 0, ended = 0;
    size_t offset = 0;
    mkdirs(stage);
    while (offset + 512 <= len) {
        unsigned char h[512];
        if (archive_read_at(fd, offset, h, sizeof(h)) != 0) {
            archive_reject(fd, "truncated tar header");
        }
        if (zeros(h, 512)) {
            if (offset + 1024 > len || !archive_zeros(fd, offset, len - offset)) {
                archive_reject(fd, "invalid tar end marker");
            }
            ended = 1;
            break;
        }
        uint64_t sum = 0;
        for (int i = 0; i < 512; i++) {
            sum += (i >= 148 && i < 156) ? 32 : h[i];
        }
        if (sum != octal((void *)h + 148, 8)) {
            archive_reject(fd, "tar checksum mismatch");
        }
        if (memcmp(h + 257, "ustar", 5) || h[262] != 0) {
            archive_reject(fd, "only USTAR packages are accepted");
        }
        uint64_t size = octal((void *)h + 124, 12), mode = octal((void *)h + 100, 8);
        if (size > PACKAGE_LIMIT || size > len - offset - 512 || mode & ~0777 ||
            octal((void *)h + 108, 8) || octal((void *)h + 116, 8)) {
            archive_reject(fd, "invalid tar metadata");
        }

        char name[512], part[101], prefix[156];
        if (!memchr(h, 0, 100) || !memchr(h + 345, 0, 155)) {
            archive_reject(fd, "unterminated tar path");
        }
        memcpy(part, h, 100);
        part[100] = 0;
        memcpy(prefix, h + 345, 155);
        prefix[155] = 0;
        if (*prefix) {
            snprintf(name, sizeof name, "%s/%s", prefix, part);
        } else {
            snprintf(name, sizeof name, "%s", part);
        }

        int type = h[156] == '5' ? 1 : 0;
        if (h[156] != 0 && h[156] != '0' && h[156] != '5') {
            archive_reject(fd, "links/devices/PAX records are forbidden");
        }
        if (type && *name && name[strlen(name) - 1] == '/') {
            name[strlen(name) - 1] = 0;
        }
        if (!safe_path(name)) {
            archive_reject(fd, "unsafe tar path");
        }
        if (!strcmp(name, "manifest.json")) {
            if (offset || manifest || type || size > META_LIMIT) {
                archive_reject(fd, "manifest must be the first regular tar member");
            }

            char *body = malloc((size_t)size + 1);
            if (body == NULL) {
                archive_reject(fd, "out of memory for package manifest");
            }
            if (archive_read_at(fd, offset + 512, body, (size_t)size) != 0) {
                free(body);
                archive_reject(fd, "truncated package manifest");
            }
            body[size] = 0;
            struct json *j = parse_json(body, size);
            struct package *m = parse_package(j, 0);
            if (!package_same(p, m)) {
                archive_reject(fd, "manifest does not match signed index");
            }
            free(m->manifest);
            free(m->files);
            free(m);
            free(j);
            free(body);
            manifest = 1;
        } else {
            if (!manifest || strncmp(name, "payload/", 8)) {
                archive_reject(fd, "unexpected tar member");
            }

            int found = -1;
            for (int i = 0; i < p->nfile; i++) {
                if (!strcmp(name + 8, p->files[i].path)) {
                    found = i;
                }
            }
            if (found < 0 || seen[found]++) {
                archive_reject(fd, "undeclared or duplicate tar member");
            }

            struct file *f = p->files + found;
            if (f->type != type || f->size != size || f->mode != mode) {
                archive_reject(fd, "payload metadata mismatch");
            }

            char target[4096];
            if (!type) {
                snprintf(target, sizeof(target), "%s/f%d", stage, found);
            }

            const char *error = archive_payload(fd, offset + 512, f, type ? NULL : target);
            if (error != NULL) {
                archive_reject(fd, error);
            }
        }

        size_t padded = (size + 511) & ~(size_t)511;
        if (padded > len - offset - 512) {
            archive_reject(fd, "truncated tar padding");
        }
        if (!archive_zeros(fd, offset + 512 + size, padded - size)) {
            archive_reject(fd, "nonzero tar padding");
        }
        offset += 512 + padded;
    }
    if (!ended || !manifest) {
        archive_reject(fd, "incomplete archive");
    }

    for (int i = 0; i < p->nfile; i++) {
        if (!seen[i]) {
            close(fd);
            fail("missing payload member %s", p->files[i].path);
        }
    }
    if (close(fd) != 0) {
        fail("cannot close package archive");
    }
}
