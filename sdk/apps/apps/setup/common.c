// common.c: Общие операции пакетного менеджера
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Работа с путями, метаданными, JSON и контрольными суммами
// Подсистема: пакетный менеджер setup
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

#include "setup.h"
#include <mbedtls/sha256.h>
#include <stdarg.h>
const char *root = "";
jmp_buf failure;
void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "setup: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    longjmp(failure, 1);
}
void path(char *out, const char *s) {
    if (snprintf(out, 4096, "%s/%s", root, *s == '/' ? s + 1 : s) >= 4096) {
        fail("path too long");
    }
}
void join_path(char *out, size_t cap, const char *parent, const char *leaf) {
    size_t a = strlen(parent), b = strlen(leaf);
    if (a + 1 + b >= cap) {
        fail("path too long");
    }
    memcpy(out, parent, a);
    out[a] = '/';
    memcpy(out + a + 1, leaf, b + 1);
}
int valid_name(const char *s) {
    if (!*s || strlen(s) >= 64) {
        return 0;
    }

    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ||
              strchr("-_.", *s))) {
            return 0;
        }
    }
    return 1;
}
int safe_path(const char *s) {
    if (!*s || *s == '/' || strlen(s) >= 512) {
        return 0;
    }

    for (const char *p = s; *p;) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (!n || (n == 1 && p[0] == '.') || (n == 2 && !memcmp(p, "..", 2))) {
            return 0;
        }

        for (size_t i = 0; i < n; i++) {
            if ((unsigned char)p[i] <= 32 || (unsigned char)p[i] >= 127 ||
                strchr("\\\"", p[i])) {
                return 0;
            }
        }
        if (!e) {
            break;
        }
        p = e + 1;
        if (!*p) {
            return 0;
        }
    }
    return 1;
}
void mkdirs(const char *s) {
    char p[4096];
    if (strlen(s) >= sizeof p) {
        fail("path too long");
    }
    strcpy(p, s);
    for (char *q = p + 1;; q++) {
        if (*q != '/' && *q) {
            continue;
        }

        char c = *q;
        *q = 0;
        struct stat st;
        if (lstat(p, &st)) {
            if (errno != ENOENT || mkdir(p, 0755)) {
                fail("mkdir %s: %s", p, strerror(errno));
            }
        } else if (!S_ISDIR(st.st_mode)) {
            fail("directory component is not a directory: %s", p);
        }
        *q = c;
        if (!c) {
            break;
        }
    }
}
void sync_parent(const char *s) {
    char p[4096];
    strcpy(p, s);
    char *q = strrchr(p, '/');
    if (!q) {
        strcpy(p, ".");
    } else if (q == p) {
        q[1] = 0;
    } else {
        *q = 0;
    }

    int fd = open(p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0 || fsync(fd)) {
        fail("directory sync %s: %s", p, strerror(errno));
    }
    close(fd);
}
void write_bytes(const char *p, const void *data, size_t len, unsigned mode) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, mode);
    if (fd < 0) {
        fail("write %s: %s", p, strerror(errno));
    }

    size_t pos = 0;
    while (pos < len) {
        ssize_t n = write(fd, (const char *)data + pos, len - pos);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            close(fd);
            fail("write %s: %s", p, strerror(errno));
        }
        pos += n;
    }
    if (fchmod(fd, mode) || fsync(fd)) {
        close(fd);
        fail("sync %s: %s", p, strerror(errno));
    }
    if (close(fd)) {
        fail("close %s", p);
    }
}
char *read_bytes(const char *p, size_t limit, size_t *len) {
    int fd = open(p, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        fail("read %s: %s", p, strerror(errno));
    }

    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > limit) {
        close(fd);
        fail("invalid or oversized file %s", p);
    }

    size_t size = st.st_size;
    char *b = malloc(size + 1);
    if (!b) {
        fail("out of memory");
    }

    size_t pos = 0;
    while (pos < size) {
        ssize_t n = read(fd, b + pos, size - pos);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            close(fd);
            free(b);
            fail("truncated file %s", p);
        }
        pos += n;
    }

    char extra;
    if (read(fd, &extra, 1) != 0) {
        fail("file changed %s", p);
    }
    close(fd);
    b[size] = 0;
    if (len) {
        *len = size;
    }
    return b;
}
void copy_file(const char *src, const char *dest, unsigned mode, int exclusive) {
    int in = open(src, O_RDONLY | O_NOFOLLOW);
    if (in < 0) {
        fail("open %s", src);
    }

    int out = open(
        dest, O_WRONLY | O_CREAT | O_NOFOLLOW | (exclusive ? O_EXCL : O_TRUNC), mode);
    if (out < 0) {
        close(in);
        fail("create %s: %s", dest, strerror(errno));
    }

    char b[16384];
    ssize_t n;
    while ((n = read(in, b, sizeof b)) > 0) {
        size_t pos = 0;
        while (pos < (size_t)n) {
            ssize_t m = write(out, b + pos, n - pos);
            if (m < 0 && errno == EINTR) {
                continue;
            }
            if (m <= 0) {
                fail("copy %s", dest);
            }
            pos += m;
        }
    }
    if (n < 0 || fchmod(out, mode) || fsync(out)) {
        fail("copy/sync %s: %s", dest, strerror(errno));
    }
    close(in);
    if (close(out)) {
        fail("close %s", dest);
    }
}
static void hex(const unsigned char *p, size_t n, char *s) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        s[2 * i] = h[p[i] >> 4];
        s[2 * i + 1] = h[p[i] & 15];
    }
    s[2 * n] = 0;
}
int unhex(const char *s, unsigned char *out, size_t len) {
    if (strlen(s) != 2 * len) {
        return -1;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned v = 0;
        for (int j = 0; j < 2; j++) {
            char c = s[i * 2 + j];
            if (c >= '0' && c <= '9') {
                v = v * 16 + c - '0';
            } else if (c >= 'a' && c <= 'f') {
                v = v * 16 + c - 'a' + 10;
            } else {
                return -1;
            }
        }
        out[i] = v;
    }
    return 0;
}
void sha_bytes(const void *p, size_t n, char out[65]) {
    unsigned char h[32];
    if (mbedtls_sha256(p, n, h, 0)) {
        fail("SHA256 failed");
    }
    hex(h, 32, out);
}
void sha_file(const char *p, char out[65]) {
    int fd = open(p, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        fail("hash %s: %s", p, strerror(errno));
    }
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    char b[16384];
    ssize_t n;
    while ((n = read(fd, b, sizeof b)) > 0) {
        mbedtls_sha256_update(&c, (void *)b, n);
    }
    close(fd);
    if (n < 0) {
        fail("hash read %s", p);
    }

    unsigned char h[32];
    mbedtls_sha256_finish(&c, h);
    mbedtls_sha256_free(&c);
    hex(h, 32, out);
}
struct json *parse_json(const char *s, size_t len) {
    struct json *j = malloc(sizeof *j);
    if (!j) {
        fail("out of memory");
    }
    if (json_parse(j, s, len)) {
        fail("invalid or overly complex JSON");
    }

    for (int t = 0; t < j->count; t++) {
        if (j->t[t].type == J_OBJECT) {
            char a[128], b[128];
            for (int i = t + 1; i < j->t[t].next; i = j->t[i + 1].next) {
                if (json_string(j, i, a, sizeof a) < 0) {
                    fail("invalid JSON key");
                }

                for (int k = j->t[i + 1].next; k < j->t[t].next; k = j->t[k + 1].next) {
                    if (json_string(j, k, b, sizeof b) < 0 || !strcmp(a, b)) {
                        fail("duplicate JSON field %s", a);
                    }
                }
            }
        }
    }
    return j;
}
void object_fields(struct json *j, int t, const char *allowed) {
    if (t < 0 || j->t[t].type != J_OBJECT) {
        fail("expected JSON object");
    }

    char s[128];
    for (int i = t + 1; i < j->t[t].next; i = j->t[i + 1].next) {
        json_string(j, i, s, sizeof s);
        char test[132];
        snprintf(test, sizeof test, "|%s|", s);
        if (!strstr(allowed, test)) {
            fail("unsupported field %s", s);
        }
    }
}
void jstr(struct json *j, int t, const char *key, char *out, size_t cap) {
    if (json_string(j, json_get(j, t, key), out, cap) < 0) {
        fail("missing or invalid %s", key);
    }
}
uint64_t jnum(struct json *j, int t, const char *key) {
    int64_t v;
    if (json_integer(j, json_get(j, t, key), &v) || v < 0) {
        fail("missing or invalid %s", key);
    }
    return v;
}
static int array(struct json *j, int t, const char *s) {
    int n = json_get(j, t, s);
    if (n < 0 || j->t[n].type != J_ARRAY) {
        fail("expected array %s", s);
    }
    return n;
}
static void strings(struct json *j, int a, char *out, size_t stride, int *count,
                    int max) {
    *count = 0;
    for (int t = a + 1; t < j->t[a].next; t = j->t[t].next) {
        if (*count >= max || json_string(j, t, out + stride * (*count), stride) < 0) {
            fail("invalid list");
        }
        (*count)++;
    }
}
struct package *parse_package(struct json *j, int t) {
    object_fields(j, t,
                  "|format||name||version||description||kind||arch||abi||depends||"
                  "conflicts||files|");
    if (jnum(j, t, "format") != 1) {
        fail("unsupported package format");
    }

    struct package *p = calloc(1, sizeof *p);
    if (!p) {
        fail("out of memory");
    }
    jstr(j, t, "name", p->name, sizeof p->name);
    jstr(j, t, "version", p->version, sizeof p->version);
    jstr(j, t, "description", p->description, sizeof p->description);
    jstr(j, t, "kind", p->kind, sizeof p->kind);
    jstr(j, t, "arch", p->arch, sizeof p->arch);
    jstr(j, t, "abi", p->abi, sizeof p->abi);
    if (!valid_name(p->name)) {
        fail("invalid package name");
    }
    version_cmp(p->version, p->version);
    if (strcmp(p->kind, "tool") && strcmp(p->kind, "application") &&
        strcmp(p->kind, "library") && strcmp(p->kind, "kernel")) {
        fail("invalid package kind");
    }
    strings(j, array(j, t, "depends"), (char *)p->dep, sizeof p->dep[0], &p->ndep,
            MAXDEP);
    strings(j, array(j, t, "conflicts"), (char *)p->conflict, sizeof p->conflict[0],
            &p->nconf, MAXDEP);
    for (int i = 0; i < p->nconf; i++) {
        if (!valid_name(p->conflict[i])) {
            fail("invalid conflict name");
        }
    }

    int a = array(j, t, "files");
    if (j->t[a].children > MAXFILES) {
        fail("too many package files");
    }
    p->files = calloc(j->t[a].children ? j->t[a].children : 1, sizeof *p->files);
    if (!p->files) {
        fail("out of memory");
    }

    for (int k = a + 1; k < j->t[a].next; k = j->t[k].next) {
        if (p->nfile == MAXFILES) {
            fail("too many package files");
        }

        struct file *f = p->files + p->nfile++;
        object_fields(j, k, "|path||type||size||sha256||mode||config||target|");
        jstr(j, k, "path", f->path, sizeof f->path);
        char type[32];
        jstr(j, k, "type", type, sizeof type);
        if (!safe_path(f->path) ||
            (strncmp(f->path, "bin/", 4) && strncmp(f->path, "lib/", 4) &&
             strncmp(f->path, "usr/share/", 10) && strncmp(f->path, "usr/lib/", 8) &&
             strncmp(f->path, "usr/include/", 12) &&
             strncmp(f->path, "usr/libexec/", 12) && strncmp(f->path, "etc/", 4))) {
            fail("forbidden package path %s", f->path);
        }
        if (!strncmp(f->path, "etc/setup/", 10) ||
            !strcmp(f->path, "etc/settings.ini") ||
            !strncmp(f->path, "etc/dropbear/", 13) || !strcmp(f->path, "etc/shadow") ||
            !strcmp(f->path, "etc/passwd")) {
            fail("protected path %s", f->path);
        }
        uint64_t mode = jnum(j, k, "mode");
        if (mode & ~0777ULL) {
            fail("unsafe permissions");
        }
        f->mode = mode;
        if (f->mode & ~0777) {
            fail("unsafe permissions");
        }
        f->size = jnum(j, k, "size");
        if (f->size > PACKAGE_LIMIT) {
            fail("file too large");
        }
        jstr(j, k, "sha256", f->hash, sizeof f->hash);
        unsigned char digest[32];
        if (unhex(f->hash, digest, 32)) {
            fail("invalid hash");
        }

        int conf = 0;
        if (json_boolean(j, json_get(j, k, "config"), &conf)) {
            fail("invalid config flag");
        }
        f->config = conf;
        if (conf && strncmp(f->path, "etc/", 4)) {
            fail("config outside etc");
        }
        if (!strcmp(type, "file")) {
            f->type = 0;
        } else if (!strcmp(type, "directory")) {
            f->type = 1;
        } else {
            fail("unsupported payload type %s", type);
        }

        for (int i = 0; i < p->nfile - 1; i++) {
            if (!strcmp(f->path, p->files[i].path)) {
                fail("duplicate payload path");
            }
        }
    }
    p->manifest_len = j->t[t].end - j->t[t].start;
    p->manifest = strndup(j->text + j->t[t].start, p->manifest_len);
    return p;
}
int package_same(struct package *a, struct package *b) {
    if (strcmp(a->name, b->name) || strcmp(a->version, b->version) ||
        strcmp(a->abi, b->abi) || strcmp(a->arch, b->arch) ||
        strcmp(a->kind, b->kind) || a->ndep != b->ndep || a->nconf != b->nconf ||
        a->nfile != b->nfile) {
        return 0;
    }

    for (int i = 0; i < a->ndep; i++) {
        if (strcmp(a->dep[i], b->dep[i])) {
            return 0;
        }
    }

    for (int i = 0; i < a->nconf; i++) {
        if (strcmp(a->conflict[i], b->conflict[i])) {
            return 0;
        }
    }

    for (int i = 0; i < a->nfile; i++) {
        struct file *f = a->files + i, *g = b->files + i;
        if (strcmp(f->path, g->path) || strcmp(f->hash, g->hash) ||
            f->mode != g->mode || f->size != g->size || f->config != g->config ||
            f->type != g->type) {
            return 0;
        }
    }
    return 1;
}
