// main.c: Загрузка файлов по HTTPS
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Подсистема: Загрузка файлов по HTTPS в пользовательском пространстве
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 24.10.2026
// Доступ: общедоступный

#define _GNU_SOURCE
#include "../common/tls/tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>

struct input {
    struct tls *tls;
    unsigned char data[16384];
    size_t pos, size;
};
static int byte(struct input *in) {
    if (in->pos == in->size) {
        int n = tls_read(in->tls, in->data, sizeof in->data);
        if (n <= 0) {
            return -1;
        }
        in->pos = 0;
        in->size = n;
    }
    return in->data[in->pos++];
}
static int line(struct input *in, char *out, size_t cap, size_t *budget) {
    size_t n = 0;
    int c;
    while ((c = byte(in)) >= 0) {
        if (!*budget || n + 1 >= cap) {
            return -1;
        }
        --*budget;
        if (c == '\n') {
            if (!n || out[n - 1] != '\r') {
                return -1;
            }
            out[n - 1] = 0;
            return 0;
        }
        if (!c || (c < 32 && c != '\r' && c != '\t')) {
            return -1;
        }
        out[n++] = (char)c;
    }
    return -1;
}
static int write_all(int fd, const void *p, size_t size) {
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        p = (const char *)p + n;
        size -= (size_t)n;
    }
    return 0;
}
static int exact(struct input *in, int out, uint64_t size) {
    enum { WRITE_BATCH = 128 * 1024 };
    unsigned char *batch = malloc(WRITE_BATCH);
    size_t used = 0;
    int result = 0;

    while (size) {
        if (in->pos == in->size) {
            int n = tls_read(in->tls, in->data, sizeof in->data);
            if (n <= 0) {
                result = -1;
                break;
            }
            in->pos = 0;
            in->size = n;
        }

        size_t n = in->size - in->pos;
        if (n > size) {
            n = (size_t)size;
        }

        if (batch != NULL) {
            if (n > WRITE_BATCH - used) {
                n = WRITE_BATCH - used;
            }
            memcpy(batch + used, in->data + in->pos, n);
            used += n;
            if (used == WRITE_BATCH) {
                if (write_all(out, batch, used)) {
                    result = -1;
                    break;
                }
                used = 0;
            }
        } else if (write_all(out, in->data + in->pos, n)) {
            result = -1;
            break;
        }

        in->pos += n;
        size -= n;
    }

    if (result == 0 && used != 0 && write_all(out, batch, used)) {
        result = -1;
    }

    free(batch);
    return result;
}
static int number(const char *s, int base, uint64_t *result) {
    if (!*s || *s == '+' || *s == '-' || *s == ' ' || *s == '\t') {
        return -1;
    }
    errno = 0;
    char *end;
    unsigned long long n = strtoull(s, &end, base);
    if (errno || *end) {
        return -1;
    }
    *result = n;
    return 0;
}
static int response(struct tls *t, int out, uint64_t limit) {
    struct input in = {.tls = t};
    char text[8192];
    size_t budget = 65536;
    if (line(&in, text, sizeof text, &budget)) {
        return -1;
    }
    if (strncmp(text, "HTTP/1.1 200 ", 13) && strncmp(text, "HTTP/1.0 200 ", 13)) {
        fprintf(stderr, "https: response rejected: %s\n", text);
        return -1;
    }
    uint64_t length = 0, total = 0;
    int have_length = 0, chunked = 0;
    for (;;) {
        if (line(&in, text, sizeof text, &budget)) {
            return -1;
        }
        if (!*text) {
            break;
        }

        char *colon = strchr(text, ':');
        if (!colon || text[0] == ' ' || text[0] == '\t') {
            return -1;
        }
        *colon++ = 0;
        while (*colon == ' ' || *colon == '\t') {
            colon++;
        }

        char *end = colon + strlen(colon);
        while (end > colon && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = 0;
        }
        if (!strcasecmp(text, "Content-Length")) {
            if (have_length || number(colon, 10, &length) || length > limit) {
                return -1;
            }
            have_length = 1;
        } else if (!strcasecmp(text, "Transfer-Encoding")) {
            if (chunked || strcasecmp(colon, "chunked")) {
                return -1;
            }
            chunked = 1;
        } else if (!strcasecmp(text, "Content-Encoding") &&
                   strcasecmp(colon, "identity")) {
            return -1;
        }
    }
    if (*text || (have_length && chunked)) {
        return -1;
    }
    if (have_length) {
        return exact(&in, out, length);
    }
    if (chunked) {
        for (;;) {
            size_t chunk_budget = 8192;
            if (line(&in, text, sizeof text, &chunk_budget)) {
                return -1;
            }

            char *extension = strchr(text, ';');
            if (extension) {
                *extension = 0;
            }
            uint64_t size;
            if (number(text, 16, &size) || size > limit - total) {
                return -1;
            }
            if (!size) {
                budget = 65536;
                do {
                    if (line(&in, text, sizeof text, &budget)) {
                        return -1;
                    }
                } while (*text);
                return 0;
            }
            if (exact(&in, out, size) || byte(&in) != '\r' || byte(&in) != '\n') {
                return -1;
            }
            total += size;
        }
    }

    for (;;) {
        if (in.pos < in.size) {
            size_t n = in.size - in.pos;
            if (n > limit - total || write_all(out, in.data + in.pos, n)) {
                return -1;
            }
            total += n;
            in.pos = in.size;
        }

        int n = tls_read(t, in.data, sizeof in.data);
        // Ответ без указанной длины завершается после проверенного TLS close_notify
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return 0;
        }
        if (n <= 0) {
            return -1;
        }
        in.pos = 0;
        in.size = n;
    }
}
static int publish(const char *tmp, const char *dest) {
    struct stat existing;
    if (!lstat(dest, &existing)) {
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT) {
        return -1;
    }
    if (!rename(tmp, dest)) {
        return 0;
    }
    if (errno != ENOSYS) {
        return -1;
    }
    // Создаём новый файл, не заменяя существующий
    // Проверка TLS завершена до копирования
    int in = open(tmp, O_RDONLY);
    if (in < 0) {
        return -1;
    }

    int out = open(dest, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (out < 0) {
        close(in);
        return -1;
    }

    unsigned char buffer[16384];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buffer, sizeof buffer)) > 0) {
        if (write_all(out, buffer, n)) {
            rc = -1;
            break;
        }
    }
    if (n < 0 || fsync(out)) {
        rc = -1;
    }
    close(in);
    if (close(out)) {
        rc = -1;
    }
    if (rc) {
        unlink(dest);
    }
    return rc;
}
int main(int argc, char **argv) {
    const char *ca = "/etc/ssl/certs/ca-certificates.crt", *output = NULL, *url = NULL;
    uint64_t limit = 512ULL * 1024 * 1024;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ca") && i + 1 < argc) {
            ca = argv[++i];
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            output = argv[++i];
        } else if (!strcmp(argv[i], "--max-bytes") && i + 1 < argc) {
            if (number(argv[++i], 10, &limit)) {
                return 2;
            }
        } else if (!strcmp(argv[i], "--help")) {
            puts(
                "https [--ca FILE] [--max-bytes N] [-o FILE] https://HOST[:PORT]/PATH\nChecks CA, hostname and certificate dates. Redirects are rejected.");
            return 0;
        } else if (argv[i][0] != '-' && !url) {
            url = argv[i];
        } else {
            return 2;
        }
    }
    if (!url || strncmp(url, "https://", 8)) {
        fprintf(stderr, "https: an HTTPS URL is required (see --help)\n");
        return 2;
    }

    char host[256], port[6] = "443", authority[272], path[4096];
    const char *start = url + 8, *slash = strpbrk(start, "/?#");
    size_t n = slash ? (size_t)(slash - start) : strlen(start);
    if (!n || n >= sizeof host) {
        return 2;
    }
    memcpy(authority, start, n);
    authority[n] = 0;
    if (strchr(authority, '@') || strchr(authority, '[') || strchr(authority, '\\')) {
        return 2;
    }

    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)authority[i] <= 32 || (unsigned char)authority[i] >= 127) {
            return 2;
        }
    }
    strcpy(host, authority);
    char *colon = strchr(host, ':');
    if (colon) {
        *colon++ = 0;
        uint64_t p;
        if (number(colon, 10, &p) || !p || p > 65535) {
            return 2;
        }
        snprintf(port, sizeof port, "%llu", (unsigned long long)p);
    }
    if (!*host || strlen(host) >= sizeof host - 1) {
        return 2;
    }
    if (!slash) {
        strcpy(path, "/");
    } else if (*slash == '/') {
        snprintf(path, sizeof path, "%s", slash);
    } else {
        snprintf(path, sizeof path, "/%s", slash);
    }
    if (slash && strlen(slash) + 2 >= sizeof path) {
        return 2;
    }

    char *fragment = strchr(path, '#');
    if (fragment) {
        *fragment = 0;
    }

    for (char *p = path; *p; p++) {
        if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127) {
            return 2;
        }
    }

    char tmp[4096];
    int fd;
    if (output) {
        if (strlen(output) + 20 >= sizeof tmp) {
            return 2;
        }
        snprintf(tmp, sizeof tmp, "%s.part.XXXXXX", output);
    } else {
        strcpy(tmp, "/tmp/https.XXXXXX");
    }
    fd = mkstemp(tmp);
    if (fd < 0) {
        perror("https temporary file");
        return 1;
    }

    struct tls t;
    int rc = tls_init(&t, 0, ca, NULL);
    if (!rc) {
        rc = tls_connect(&t, host, port);
    }
    if (rc) {
        tls_error("https TLS", rc);
        goto done;
    }

    char request[4608];
    n = (size_t)snprintf(
        request, sizeof request,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: SynapseOS-https/1\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
        path, authority);
    if (n >= sizeof request || (rc = tls_write_all(&t, request, n))) {
        goto done;
    }
    if ((rc = response(&t, fd, limit))) {
        fprintf(stderr, "https: incomplete, invalid or oversized response\n");
        goto done;
    }
    if (fsync(fd)) {
        rc = -1;
        perror("https save");
        goto done;
    }
    if (close(fd)) {
        fd = -1;
        rc = -1;
        perror("https close");
        goto done;
    }
    fd = -1;
    if (output) {
        rc = publish(tmp, output);
        if (rc) {
            perror("https publish");
        }
    } else {
        fd = open(tmp, O_RDONLY);
        if (fd < 0) {
            rc = -1;
            goto done;
        }

        unsigned char data[16384];
        ssize_t count;
        while ((count = read(fd, data, sizeof data)) > 0) {
            if (write_all(STDOUT_FILENO, data, count)) {
                rc = -1;
                break;
            }
        }
        if (count < 0) {
            rc = -1;
        }
    }
done:
    if (fd >= 0) {
        close(fd);
    }
    unlink(tmp);
    tls_free(&t);
    return rc ? 1 : 0;
}
