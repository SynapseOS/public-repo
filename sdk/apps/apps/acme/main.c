// main.c: Клиент ACME
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Подсистема: Клиент ACME в пользовательском пространстве
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

#define _GNU_SOURCE
#include "acme.h"
#include "../qemu-ga/json.h"
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#include <psa/crypto.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *staging = "https://acme-staging-v02.api.letsencrypt.org/directory";
struct client {
    char root[PATH_MAX], identifier[254], email[254], webroot[PATH_MAX];
    char directory[ACME_URL], profile[64], ca[PATH_MAX];
    char accountdir[PATH_MAX], certdir[PATH_MAX], newnonce[ACME_URL],
        newaccount[ACME_URL], neworder[ACME_URL];
    char nonce[256], kid[ACME_URL], jwk[2048], thumbprint[64];
    int test_http, accept_terms, lock;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
    mbedtls_pk_context account;
};
static char challenge_file[PATH_MAX];
static int held_lock = -1;
static void cleanup(void) {
    if (*challenge_file) {
        unlink(challenge_file);
    }
    if (held_lock >= 0) {
        close(held_lock);
    }
}
static void interrupted(int signal_number) {
    cleanup();
    _exit(128 + signal_number);
}
static void fatal(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    fputs("acme: ", stderr);
    vfprintf(stderr, format, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}
static void check(int rc, const char *what) {
    if (rc) {
        fatal("%s failed (%d)", what, rc);
    }
}
static void format(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) {
        fatal("path or message too long");
    }
}
static void copy(char *out, size_t cap, const char *s) {
    format(out, cap, "%s", s);
}
static int absolute_safe(const char *p) {
    if (*p != '/' || strpbrk(p, "\r\n\\") || strstr(p, "//")) {
        return 0;
    }

    for (const char *s = p + 1; *s;) {
        const char *end = strchr(s, '/');
        size_t n = end ? (size_t)(end - s) : strlen(s);
        if ((n == 1 && *s == '.') || (n == 2 && !strncmp(s, "..", 2))) {
            return 0;
        }
        if (!end) {
            break;
        }
        s = end + 1;
    }
    return 1;
}
static void rooted(struct client *c, char *out, size_t cap, const char *absolute) {
    if (!absolute_safe(absolute)) {
        fatal("unsafe absolute path");
    }
    format(out, cap, "%s%s", c->root, absolute);
}
static void directories(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    copy(tmp, sizeof(tmp), path);
    for (char *s = tmp + 1;; s++) {
        if (*s && *s != '/') {
            continue;
        }

        char saved = *s;
        *s = 0;
        struct stat st;
        if (lstat(tmp, &st)) {
            if (errno != ENOENT || mkdir(tmp, mode)) {
                fatal("mkdir %s: %s", tmp, strerror(errno));
            }
        } else if (!S_ISDIR(st.st_mode)) {
            fatal("%s is not a real directory", tmp);
        }
        *s = saved;
        if (!saved) {
            break;
        }
    }
}
static void write_all(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            fatal("write: %s", strerror(errno));
        }
        p += n;
        len -= n;
    }
}
static void durable_dir(const char *path) {
    char parent[PATH_MAX];
    copy(parent, sizeof(parent), path);
    char *slash = strrchr(parent, '/');
    if (!slash) {
        fatal("bad path");
    }
    *slash = 0;
    int fd = open(*parent ? parent : "/", O_RDONLY | O_DIRECTORY);
    if (fd < 0 || fsync(fd)) {
        fatal("sync parent directory: %s", strerror(errno));
    }
    close(fd);
}
static void atomic_file(const char *path, const void *data, size_t len, mode_t mode) {
    char tmp[PATH_MAX];
    format(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
    if (fd < 0) {
        fatal("create %s: %s", tmp, strerror(errno));
    }
    if (fchmod(fd, mode)) {
        fatal("chmod private file: %s", strerror(errno));
    }
    write_all(fd, data, len);
    if (fsync(fd)) {
        fatal("sync %s: %s", tmp, strerror(errno));
    }
    if (close(fd) || rename(tmp, path)) {
        fatal("commit %s: %s", path, strerror(errno));
    }
    durable_dir(path);
}
static char *read_file(const char *path, size_t limit, int optional) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        if (optional && errno == ENOENT) {
            return NULL;
        }
        fatal("open %s: %s", path, strerror(errno));
    }

    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (size_t)st.st_size > limit) {
        fatal("invalid file %s", path);
    }

    char *buf = malloc(limit + 1);
    if (!buf) {
        fatal("out of memory");
    }

    size_t used = 0;
    for (;;) {
        ssize_t n = read(fd, buf + used, limit - used);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            fatal("read %s", path);
        }
        if (!n) {
            break;
        }
        used += n;
        if (used == limit) {
            char extra;
            if (read(fd, &extra, 1) != 0) {
                fatal("oversized %s", path);
            }
            break;
        }
    }
    close(fd);
    buf[used] = 0;
    if (memchr(buf, 0, used)) {
        fatal("embedded NUL in %s", path);
    }
    return buf;
}
static void json_text(struct json *j, const char *body) {
    if (strstr(body, "\\u0000") || json_parse(j, body, strlen(body)) ||
        j->t[0].type != J_OBJECT) {
        fatal("invalid ACME JSON");
    }
    // Повторные ключи недопустимы для критичных полей
    for (int object = 0; object < j->count; object++) {
        if (j->t[object].type == J_OBJECT) {
            for (int a = object + 1; a < j->t[object].next; a = j->t[a + 1].next) {
                char key[256];
                if (json_string(j, a, key, sizeof(key)) < 0) {
                    fatal("invalid JSON key");
                }

                for (int b = j->t[a + 1].next; b < j->t[object].next;
                     b = j->t[b + 1].next) {
                    char other[256];
                    if (json_string(j, b, other, sizeof(other)) < 0 ||
                        !strcmp(key, other)) {
                        fatal("duplicate JSON key");
                    }
                }
            }
        }
    }
}
static int field(struct json *j, int object, const char *key, char *out, size_t cap,
                 int required) {
    int token = json_get(j, object, key);
    if (token < 0 && !required) {
        *out = 0;
        return -1;
    }
    if (json_string(j, token, out, cap) < 0) {
        fatal("missing/invalid field %s", key);
    }
    return token;
}
static void quoted(char *out, size_t cap, const char *s) {
    size_t n = 0;
    if (cap < 3) {
        fatal("JSON buffer too small");
    }
    out[n++] = '"';
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) {
            fatal("non-ASCII JSON parameter");
        }
        if (ch == '"' || ch == '\\') {
            if (n + 2 >= cap) {
                fatal("JSON parameter too long");
            }
            out[n++] = '\\';
        }
        if (n + 1 >= cap) {
            fatal("JSON parameter too long");
        }
        out[n++] = ch;
    }
    if (n + 2 > cap) {
        fatal("JSON parameter too long");
    }
    out[n++] = '"';
    out[n] = 0;
}
static char *base64url(const unsigned char *data, size_t len) {
    size_t cap = ((len + 2) / 3) * 4 + 1, written;
    char *out = malloc(cap);
    if (!out) {
        fatal("out of memory");
    }
    check(mbedtls_base64_encode((unsigned char *)out, cap, &written, data, len),
          "base64");
    for (size_t i = 0; i < written; i++) {
        if (out[i] == '+') {
            out[i] = '-';
        } else if (out[i] == '/') {
            out[i] = '_';
        }
    }

    while (written && out[written - 1] == '=') {
        written--;
    }
    out[written] = 0;
    return out;
}
static int token_safe(const char *s, size_t maximum) {
    size_t n = strlen(s);
    if (!n || n > maximum) {
        return 0;
    }

    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_') {
            return 0;
        }
    }
    return 1;
}
static void random_init(struct client *c) {
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->rng);
    mbedtls_pk_init(&c->account);
    check(psa_crypto_init() == PSA_SUCCESS ? 0 : -1, "PSA initialization");
    const unsigned char tag[] = "SynapseOS ACME account and server keys";
    check(mbedtls_ctr_drbg_seed(&c->rng, mbedtls_entropy_func, &c->entropy, tag,
                                sizeof(tag)),
          "random seed");
}
static void private_key(struct client *c, mbedtls_pk_context *key, const char *path) {
    char *text = read_file(path, 16384, 1);
    if (text) {
        check(psa_crypto_init() == PSA_SUCCESS ? 0 : -1, "PSA initialization");
        check(mbedtls_pk_parse_key(key, (unsigned char *)text, strlen(text) + 1, NULL,
                                   0, mbedtls_ctr_drbg_random, &c->rng),
              "read private key");
        free(text);
        if (mbedtls_pk_get_type(key) != MBEDTLS_PK_RSA) {
            fatal("only RSA private keys are supported");
        }

        struct stat st;
        if (lstat(path, &st) || (st.st_mode & 077) || st.st_uid != getuid()) {
            fatal("private key must be owned by this user with mode 0600");
        }
        return;
    }
    check(mbedtls_pk_setup(key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)),
          "RSA context");
    check(mbedtls_rsa_gen_key(mbedtls_pk_rsa(*key), mbedtls_ctr_drbg_random, &c->rng,
                              2048, 65537),
          "RSA key generation");
    unsigned char pem[8192];
    check(mbedtls_pk_write_key_pem(key, pem, sizeof(pem)), "private key PEM");
    atomic_file(path, pem, strlen((char *)pem), 0600);
    memset(pem, 0, sizeof(pem));
}
static void jwk(struct client *c) {
    mbedtls_mpi n, e;
    mbedtls_mpi_init(&n);
    mbedtls_mpi_init(&e);
    check(mbedtls_rsa_export(mbedtls_pk_rsa(c->account), &n, NULL, NULL, NULL, &e),
          "RSA public key");
    unsigned char bytes[512];
    size_t size = mbedtls_mpi_size(&n);
    check(mbedtls_mpi_write_binary(&n, bytes, size), "RSA modulus");
    char *ns = base64url(bytes, size);
    size = mbedtls_mpi_size(&e);
    check(mbedtls_mpi_write_binary(&e, bytes, size), "RSA exponent");
    char *es = base64url(bytes, size);
    format(c->jwk, sizeof(c->jwk), "{\"e\":\"%s\",\"kty\":\"RSA\",\"n\":\"%s\"}", es,
           ns);
    free(ns);
    free(es);
    mbedtls_mpi_free(&n);
    mbedtls_mpi_free(&e);
    unsigned char hash[32];
    check(mbedtls_sha256((unsigned char *)c->jwk, strlen(c->jwk), hash, 0),
          "JWK thumbprint");
    char *thumb = base64url(hash, sizeof(hash));
    copy(c->thumbprint, sizeof(c->thumbprint), thumb);
    free(thumb);
}
static void nonce_update(struct client *c, struct http_reply *r) {
    if (*r->nonce) {
        if (!token_safe(r->nonce, sizeof(c->nonce) - 1)) {
            fatal("invalid replay nonce");
        }
        copy(c->nonce, sizeof(c->nonce), r->nonce);
    }
}
static void nonce_fetch(struct client *c) {
    struct http_reply r;
    check(acme_http(c->newnonce, "HEAD", NULL, c->ca, c->test_http, &r),
          "nonce request");
    if (r.status < 200 || r.status >= 300) {
        fatal("nonce HTTP %d", r.status);
    }
    nonce_update(c, &r);
    acme_http_free(&r);
    if (!*c->nonce) {
        fatal("server omitted Replay-Nonce");
    }
}
static void signed_request(struct client *c, const char *url, const char *payload,
                           int use_jwk, struct http_reply *r) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!*c->nonce) {
            nonce_fetch(c);
        }

        char qu[ACME_URL * 2 + 8], kid[ACME_URL * 2 + 8], header[ACME_URL * 4 + 4096];
        quoted(qu, sizeof(qu), url);
        quoted(kid, sizeof(kid), c->kid);
        format(header, sizeof(header),
               "{\"alg\":\"RS256\",\"nonce\":\"%s\",\"url\":%s,%s:%s}", c->nonce, qu,
               use_jwk ? "\"jwk\"" : "\"kid\"", use_jwk ? c->jwk : kid);
        *c->nonce = 0;
        char *protected = base64url((unsigned char *)header, strlen(header)),
             *body = base64url((unsigned char *)payload, strlen(payload));
        size_t length = strlen(protected) + strlen(body) + 2;
        char *signing = malloc(length);
        if (!signing) {
            fatal("out of memory");
        }
        format(signing, length, "%s.%s", protected, body);
        unsigned char hash[32], signature[512];
        size_t siglen;
        check(psa_crypto_init() == PSA_SUCCESS ? 0 : -1, "PSA initialization");
        check(mbedtls_sha256((unsigned char *)signing, strlen(signing), hash, 0),
              "JWS hash");
        free(signing);
        check(mbedtls_pk_sign(&c->account, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                              signature, sizeof(signature), &siglen,
                              mbedtls_ctr_drbg_random, &c->rng),
              "JWS signature");
        char *sig = base64url(signature, siglen);
        length = strlen(protected) + strlen(body) + strlen(sig) + 128;
        char *request = malloc(length);
        if (!request) {
            fatal("out of memory");
        }
        format(request, length,
               "{\"protected\":\"%s\",\"payload\":\"%s\",\"signature\":\"%s\"}",
               protected, body, sig);
        free(protected);
        free(body);
        free(sig);
        int rc = acme_http(url, "POST", request, c->ca, c->test_http, r);
        free(request);
        check(rc, "signed request");
        nonce_update(c, r);
        if (r->status == 400 && r->body) {
            struct json j;
            char problem[256];
            json_text(&j, r->body);
            field(&j, 0, "type", problem, sizeof(problem), 0);
            if (!strcmp(problem, "urn:ietf:params:acme:error:badNonce") &&
                attempt < 2) {
                acme_http_free(r);
                continue;
            }
        }
        if (r->status >= 400) {
            fprintf(stderr, "acme: HTTP %d: %.1024s\n", r->status, r->body);
            acme_http_free(r);
            fatal("ACME request rejected");
        }
        if (r->status < 200 || r->status >= 300) {
            fatal("ACME redirect/unexpected HTTP %d", r->status);
        }
        return;
    }
    fatal("nonce retry limit reached");
}
static void discover(struct client *c) {
    struct http_reply r;
    check(acme_http(c->directory, "GET", NULL, c->ca, c->test_http, &r),
          "directory request");
    if (r.status != 200) {
        fatal("directory HTTP %d", r.status);
    }

    struct json j;
    json_text(&j, r.body);
    field(&j, 0, "newNonce", c->newnonce, sizeof(c->newnonce), 1);
    field(&j, 0, "newAccount", c->newaccount, sizeof(c->newaccount), 1);
    field(&j, 0, "newOrder", c->neworder, sizeof(c->neworder), 1);
    if (*c->profile) {
        int meta = json_get(&j, 0, "meta"), profiles = json_get(&j, meta, "profiles");
        if (json_get(&j, profiles, c->profile) < 0) {
            fatal("ACME directory does not advertise profile %s", c->profile);
        }
    }
    nonce_update(c, &r);
    acme_http_free(&r);
}
static void account_register(struct client *c) {
    char path[PATH_MAX];
    format(path, sizeof(path), "%s/account.key", c->accountdir);
    private_key(c, &c->account, path);
    jwk(c);
    format(path, sizeof(path), "%s/account.url", c->accountdir);
    char *saved = read_file(path, ACME_URL - 1, 1);
    if (saved) {
        copy(c->kid, sizeof(c->kid), saved);
        free(saved);
        if (!*c->kid) {
            fatal("empty account URL");
        }
        return;
    }
    if (!c->accept_terms) {
        fatal("new account requires --accept-terms (read the CA terms first)");
    }

    char email[600], payload[1024];
    quoted(email, sizeof(email), c->email);
    format(payload, sizeof(payload),
           "{\"termsOfServiceAgreed\":true,\"contact\":[\"mailto:%s\"]}", c->email);
    struct http_reply r;
    signed_request(c, c->newaccount, payload, 1, &r);
    if (!*r.location) {
        fatal("account response omitted Location");
    }
    copy(c->kid, sizeof(c->kid), r.location);
    struct json j;
    char status[32];
    json_text(&j, r.body);
    field(&j, 0, "status", status, sizeof(status), 1);
    if (strcmp(status, "valid")) {
        fatal("account is not valid");
    }
    atomic_file(path, c->kid, strlen(c->kid), 0600);
    acme_http_free(&r);
}
static int identifier_kind(const char *name, unsigned char *ip, size_t *length) {
    if (inet_pton(AF_INET, name, ip) == 1) {
        *length = 4;
        return 7;
    }
    if (inet_pton(AF_INET6, name, ip) == 1) {
        *length = 16;
        return 7;
    }

    size_t n = strlen(name);
    if (!n || n > 253) {
        fatal("invalid identifier");
    }

    for (const char *s = name; *s;) {
        const char *end = strchr(s, '.');
        size_t count = end ? (size_t)(end - s) : strlen(s);
        if (!count || count > 63 || !isalnum((unsigned char)*s) ||
            !isalnum((unsigned char)s[count - 1])) {
            fatal("invalid DNS identifier");
        }

        for (size_t i = 0; i < count; i++) {
            if (!isalnum((unsigned char)s[i]) && s[i] != '-') {
                fatal("only ASCII DNS identifiers are supported");
            }
        }
        if (!end) {
            break;
        }
        s = end + 1;
        if (!*s) {
            fatal("omit the final dot from the identifier");
        }
    }
    *length = n;
    return 2;
}
static int csr(struct client *c, mbedtls_pk_context *key, unsigned char *der,
               size_t capacity) {
    mbedtls_x509write_csr request;
    mbedtls_x509write_csr_init(&request);
    mbedtls_x509write_csr_set_md_alg(&request, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&request, key);
    check(mbedtls_x509write_csr_set_subject_name(&request, "O=SynapseOS"),
          "CSR subject");
    unsigned char ip[16], san[260];
    size_t bytes;
    int kind = identifier_kind(c->identifier, ip, &bytes);
    size_t at = 0;
    san[at++] = 0x30;
    size_t inner = bytes + (bytes >= 128 ? 3 : 2);
    if (inner >= 256) {
        san[at++] = 0x82;
        san[at++] = inner >> 8;
        san[at++] = inner;
    } else if (inner >= 128) {
        san[at++] = 0x81;
        san[at++] = inner;
    } else {
        san[at++] = inner;
    }
    san[at++] = 0x80 | kind;
    if (bytes >= 128) {
        san[at++] = 0x81;
        san[at++] = bytes;
    } else {
        san[at++] = bytes;
    }
    memcpy(san + at, kind == 7 ? (const void *)ip : (const void *)c->identifier, bytes);
    at += bytes;
    check(mbedtls_x509write_csr_set_extension(
              &request, MBEDTLS_OID_SUBJECT_ALT_NAME,
              MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0, san, at),
          "CSR SAN");
    check(psa_crypto_init() == PSA_SUCCESS ? 0 : -1, "PSA initialization");
    int n = mbedtls_x509write_csr_der(&request, der, capacity, mbedtls_ctr_drbg_random,
                                      &c->rng);
    mbedtls_x509write_csr_free(&request);
    if (n <= 0) {
        fatal("CSR encoding failed (%d)", n);
    }
    return n;
}
static void pause_poll(struct http_reply *r) {
    int delay = r->retry ? r->retry : 2;
    acme_http_free(r);
    sleep(delay);
}
static void authorize(struct client *c, const char *url) {
    struct http_reply r;
    signed_request(c, url, "", 0, &r);
    struct json j;
    char status[32];
    json_text(&j, r.body);
    field(&j, 0, "status", status, sizeof(status), 1);
    int identifier = json_get(&j, 0, "identifier");
    char value[254], kind[16];
    field(&j, identifier, "value", value, sizeof(value), 1);
    field(&j, identifier, "type", kind, sizeof(kind), 1);
    unsigned char ip[16];
    size_t iplen;
    int expected = identifier_kind(c->identifier, ip, &iplen);
    if (strcmp(value, c->identifier) || strcmp(kind, expected == 7 ? "ip" : "dns")) {
        fatal("authorization identifier mismatch");
    }
    if (!strcmp(status, "valid")) {
        acme_http_free(&r);
        return;
    }
    if (strcmp(status, "pending")) {
        fatal("authorization status %s", status);
    }

    int array = json_get(&j, 0, "challenges"), selected = -1;
    if (array < 0 || j.t[array].type != J_ARRAY) {
        fatal("missing challenges");
    }

    for (int n = array + 1; n < j.t[array].next; n = j.t[n].next) {
        char type[32];
        field(&j, n, "type", type, sizeof(type), 1);
        if (!strcmp(type, "http-01")) {
            if (selected >= 0) {
                fatal("duplicate HTTP-01 challenge");
            }
            selected = n;
        }
    }
    if (selected < 0) {
        fatal("CA did not offer HTTP-01");
    }

    char token[256], challenge[ACME_URL], directory[PATH_MAX], content[512];
    field(&j, selected, "token", token, sizeof(token), 1);
    field(&j, selected, "url", challenge, sizeof(challenge), 1);
    if (!token_safe(token, 200)) {
        fatal("unsafe challenge token");
    }
    format(directory, sizeof(directory), "%s/.well-known/acme-challenge", c->webroot);
    directories(directory, 0755);
    char candidate[PATH_MAX];
    format(candidate, sizeof(candidate), "%s/%s", directory, token);
    struct stat st;
    if (!lstat(candidate, &st) || errno != ENOENT) {
        fatal("challenge file already exists");
    }
    format(content, sizeof(content), "%s.%s", token, c->thumbprint);
    atomic_file(candidate, content, strlen(content), 0644);
    copy(challenge_file, sizeof(challenge_file), candidate);
    acme_http_free(&r);
    signed_request(c, challenge, "{}", 0, &r);
    acme_http_free(&r);
    for (int poll = 0; poll < 30; poll++) {
        signed_request(c, url, "", 0, &r);
        json_text(&j, r.body);
        field(&j, 0, "status", status, sizeof(status), 1);
        if (!strcmp(status, "valid")) {
            acme_http_free(&r);
            unlink(challenge_file);
            *challenge_file = 0;
            return;
        }
        if (strcmp(status, "pending") && strcmp(status, "processing")) {
            fatal("authorization failed: %s", status);
        }
        pause_poll(&r);
    }
    fatal("authorization polling limit reached");
}
static time_t x509_time(const mbedtls_x509_time *t) {
    struct tm tm = {.tm_year = t->year - 1900,
                    .tm_mon = t->mon - 1,
                    .tm_mday = t->day,
                    .tm_hour = t->hour,
                    .tm_min = t->min,
                    .tm_sec = t->sec};
    return timegm(&tm);
}
static int cert_matches(struct client *c, mbedtls_x509_crt *crt) {
    unsigned char ip[16];
    size_t bytes;
    int kind = identifier_kind(c->identifier, ip, &bytes);
    for (mbedtls_x509_sequence *san = &crt->subject_alt_names; san && san->buf.p;
         san = san->next) {
        if ((san->buf.tag & 31) == kind && san->buf.len == bytes &&
            !memcmp(san->buf.p,
                    kind == 7 ? (const void *)ip : (const void *)c->identifier,
                    bytes)) {
            return 1;
        }
    }
    return 0;
}
static void cert_load(const char *path, mbedtls_x509_crt *cert) {
    char *text = read_file(path, ACME_BODY, 0);
    mbedtls_x509_crt_init(cert);
    check(mbedtls_x509_crt_parse(cert, (unsigned char *)text, strlen(text) + 1),
          "certificate parse");
    free(text);
}
static int generation_safe(const char *generation) {
    return token_safe(generation, 100);
}
static int renewal_due(struct client *c) {
    char path[PATH_MAX];
    format(path, sizeof(path), "%s/current", c->certdir);
    char *current = read_file(path, 100, 1);
    if (!current) {
        return 1;
    }
    if (!generation_safe(current)) {
        fatal("invalid current generation");
    }
    format(path, sizeof(path), "%s/generations/%s/fullchain.pem", c->certdir, current);
    free(current);
    mbedtls_x509_crt cert;
    cert_load(path, &cert);
    if (!cert_matches(c, &cert)) {
        fatal("current certificate identifier mismatch");
    }
    time_t first = x509_time(&cert.valid_from), last = x509_time(&cert.valid_to),
           now = time(NULL);
    mbedtls_x509_crt_free(&cert);
    if (first < 0 || last <= first || now < first) {
        fatal("check system time/certificate validity before renewing");
    }
    return now >= last - (last - first) / 3;
}
static void issue(struct client *c) {
    discover(c);
    account_register(c);
    unsigned char ip[16];
    size_t iplen;
    int kind = identifier_kind(c->identifier, ip, &iplen);
    char payload[4096], id[600], profile[140];
    quoted(id, sizeof(id), c->identifier);
    quoted(profile, sizeof(profile), c->profile);
    if (*c->profile) {
        format(payload, sizeof(payload),
               "{\"identifiers\":[{\"type\":\"%s\",\"value\":%s}],\"profile\":%s}",
               kind == 7 ? "ip" : "dns", id, profile);
    } else {
        format(payload, sizeof(payload),
               "{\"identifiers\":[{\"type\":\"%s\",\"value\":%s}]}",
               kind == 7 ? "ip" : "dns", id);
    }

    struct http_reply r;
    signed_request(c, c->neworder, payload, 0, &r);
    char order[ACME_URL], finalize[ACME_URL], certificate[ACME_URL], status[32];
    if (!*r.location) {
        fatal("order response omitted Location");
    }
    copy(order, sizeof(order), r.location);
    struct json j;
    json_text(&j, r.body);
    field(&j, 0, "finalize", finalize, sizeof(finalize), 1);
    int auth = json_get(&j, 0, "authorizations");
    if (auth < 0 || j.t[auth].type != J_ARRAY || j.t[auth].children != 1) {
        fatal("expected one authorization");
    }

    char authorization[ACME_URL];
    if (json_string(&j, auth + 1, authorization, sizeof(authorization)) < 0) {
        fatal("invalid authorization URL");
    }
    acme_http_free(&r);
    authorize(c, authorization);
    int ready = 0;
    for (int poll = 0; poll < 30; poll++) {
        signed_request(c, order, "", 0, &r);
        json_text(&j, r.body);
        field(&j, 0, "status", status, sizeof(status), 1);
        if (!strcmp(status, "ready")) {
            ready = 1;
            acme_http_free(&r);
            break;
        }
        if (strcmp(status, "pending")) {
            fatal("order status before finalize: %s", status);
        }
        pause_poll(&r);
    }
    if (!ready) {
        fatal("order readiness polling limit reached");
    }

    char directory[PATH_MAX], keypath[PATH_MAX], certpath[PATH_MAX], current[PATH_MAX],
        generation[64];
    unsigned char random[8];
    check(mbedtls_ctr_drbg_random(&c->rng, random, sizeof(random)), "generation ID");
    format(generation, sizeof(generation), "g-%lld-%02x%02x%02x%02x%02x%02x%02x%02x",
           (long long)time(NULL), random[0], random[1], random[2], random[3], random[4],
           random[5], random[6], random[7]);
    format(directory, sizeof(directory), "%s/generations/%s", c->certdir, generation);
    directories(directory, 0700);
    format(keypath, sizeof(keypath), "%s/privkey.pem", directory);
    mbedtls_pk_context serverkey;
    mbedtls_pk_init(&serverkey);
    private_key(c, &serverkey, keypath);
    unsigned char der[8192];
    int len = csr(c, &serverkey, der, sizeof(der));
    char *encoded = base64url(der + sizeof(der) - len, len);
    char *csr_payload = malloc(strlen(encoded) + 32);
    if (!csr_payload) {
        fatal("out of memory");
    }
    format(csr_payload, strlen(encoded) + 32, "{\"csr\":\"%s\"}", encoded);
    free(encoded);
    signed_request(c, finalize, csr_payload, 0, &r);
    free(csr_payload);
    acme_http_free(&r);
    int valid = 0;
    for (int poll = 0; poll < 30; poll++) {
        signed_request(c, order, "", 0, &r);
        json_text(&j, r.body);
        field(&j, 0, "status", status, sizeof(status), 1);
        if (!strcmp(status, "valid")) {
            field(&j, 0, "certificate", certificate, sizeof(certificate), 1);
            acme_http_free(&r);
            valid = 1;
            break;
        }
        if (strcmp(status, "processing") && strcmp(status, "ready")) {
            fatal("order failed: %s", status);
        }
        pause_poll(&r);
    }
    if (!valid) {
        fatal("certificate polling limit reached");
    }
    signed_request(c, certificate, "", 0, &r);
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    check(psa_crypto_init() == PSA_SUCCESS ? 0 : -1, "PSA initialization");
    check(mbedtls_x509_crt_parse(&crt, (unsigned char *)r.body, r.size + 1),
          "issued certificate parse");
    check(mbedtls_pk_check_pair(&crt.pk, &serverkey, mbedtls_ctr_drbg_random, &c->rng),
          "issued key/certificate pair");
    time_t first = x509_time(&crt.valid_from), last = x509_time(&crt.valid_to),
           now = time(NULL);
    if (!cert_matches(c, &crt) || first > now || last <= now || last <= first) {
        fatal("issued certificate name/validity mismatch");
    }
    format(certpath, sizeof(certpath), "%s/fullchain.pem", directory);
    atomic_file(certpath, r.body, r.size, 0600);
    acme_http_free(&r);
    mbedtls_x509_crt_free(&crt);
    mbedtls_pk_free(&serverkey);
    format(current, sizeof(current), "%s/current", c->certdir);
    atomic_file(current, generation, strlen(generation), 0644);
    printf("acme: certificate activated for %s; expires %lld\n", c->identifier,
           (long long)last);
}
static void config_path(struct client *c, char *path) {
    format(path, PATH_MAX, "%s/config.json", c->certdir);
}
static void config_save(struct client *c) {
    char directory[ACME_URL * 2 + 8], webroot[PATH_MAX * 2 + 8], ca[PATH_MAX * 2 + 8],
        email[600], profile[140], path[PATH_MAX];
    quoted(directory, sizeof(directory), c->directory);
    quoted(webroot, sizeof(webroot), c->webroot + strlen(c->root));
    quoted(ca, sizeof(ca), c->ca + strlen(c->root));
    quoted(email, sizeof(email), c->email);
    quoted(profile, sizeof(profile), c->profile);
    char *text = malloc(sizeof(directory) + sizeof(webroot) + sizeof(ca) + 2048);
    if (!text) {
        fatal("out of memory");
    }
    format(
        text, sizeof(directory) + sizeof(webroot) + sizeof(ca) + 2048,
        "{\"directory\":%s,\"webroot\":%s,\"ca\":%s,\"email\":%s,\"profile\":%s,\"test_http\":%s}",
        directory, webroot, ca, email, profile, c->test_http ? "true" : "false");
    config_path(c, path);
    atomic_file(path, text, strlen(text), 0600);
    free(text);
}
static void config_load(struct client *c) {
    char path[PATH_MAX], absolute[PATH_MAX];
    config_path(c, path);
    char *text = read_file(path, 32768, 0);
    struct json j;
    json_text(&j, text);
    field(&j, 0, "directory", c->directory, sizeof(c->directory), 1);
    field(&j, 0, "email", c->email, sizeof(c->email), 1);
    field(&j, 0, "profile", c->profile, sizeof(c->profile), 1);
    field(&j, 0, "webroot", absolute, sizeof(absolute), 1);
    rooted(c, c->webroot, sizeof(c->webroot), absolute);
    field(&j, 0, "ca", absolute, sizeof(absolute), 1);
    rooted(c, c->ca, sizeof(c->ca), absolute);
    int test = json_get(&j, 0, "test_http"), saved;
    if (json_boolean(&j, test, &saved)) {
        fatal("invalid test mode in saved config");
    }
    if (saved && !c->test_http) {
        fatal("test certificate renewal requires explicit --test-http");
    }
    free(text);
}
static void help(void) {
    puts(
        "usage: acme issue NAME --email EMAIL --accept-terms [OPTIONS]\n       acme renew NAME [--root DIR] [--foreground] [--test-http]\nOPTIONS: --directory URL --profile NAME --webroot DIR --ca FILE --root DIR\nDefault CA: Let's Encrypt staging. One DNS name or IP, HTTP-01.\n--test-http permits local loopback fixtures only; renew checks expiry.");
}
int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        help();
        return 0;
    }
    if (argc < 3 || (strcmp(argv[1], "issue") && strcmp(argv[1], "renew"))) {
        help();
        return 2;
    }

    int renew = !strcmp(argv[1], "renew"), foreground = 0;
    struct client *c = calloc(1, sizeof(*c));
    if (!c) {
        fatal("out of memory");
    }
    copy(c->identifier, sizeof(c->identifier), argv[2]);
    for (char *s = c->identifier; *s; s++) {
        *s = tolower((unsigned char)*s);
    }

    unsigned char ip[16];
    size_t iplen;
    int kind = identifier_kind(c->identifier, ip, &iplen);
    copy(c->directory, sizeof(c->directory), staging);
    char webroot[PATH_MAX] = "/var/www",
         ca[PATH_MAX] = "/etc/ssl/certs/ca-certificates.crt";
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--foreground")) {
            foreground = 1;
            continue;
        }
        if (!strcmp(argv[i], "--accept-terms")) {
            c->accept_terms = 1;
            continue;
        }
        if (!strcmp(argv[i], "--test-http")) {
            c->test_http = 1;
            continue;
        }
        const char *option = argv[i];
        if (++i >= argc) {
            fatal("missing option value");
        }
        if (!strcmp(option, "--root")) {
            if (!realpath(argv[i], c->root)) {
                fatal("root path: %s", strerror(errno));
            }
            if (!strcmp(c->root, "/")) {
                *c->root = 0;
            }
        } else if (renew) {
            fatal(
                "renew uses saved configuration; only --root/--test-http are accepted");
        } else if (!strcmp(option, "--email")) {
            copy(c->email, sizeof(c->email), argv[i]);
        } else if (!strcmp(option, "--directory")) {
            copy(c->directory, sizeof(c->directory), argv[i]);
        } else if (!strcmp(option, "--profile")) {
            copy(c->profile, sizeof(c->profile), argv[i]);
        } else if (!strcmp(option, "--webroot")) {
            copy(webroot, sizeof(webroot), argv[i]);
        } else if (!strcmp(option, "--ca")) {
            copy(ca, sizeof(ca), argv[i]);
        } else {
            fatal("unknown option %s", option);
        }
    }
    if (renew && !foreground) {
        char logdir[PATH_MAX], logfile[PATH_MAX];
        rooted(c, logdir, sizeof(logdir), "/var/log");
        directories(logdir, 0700);
        rooted(c, logfile, sizeof(logfile), "/var/log/acme.log");
        int logfd = open(logfile, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
        if (logfd < 0 || dup2(logfd, STDOUT_FILENO) < 0 ||
            dup2(logfd, STDERR_FILENO) < 0) {
            fatal("cannot open ACME log");
        }
        if (logfd > STDERR_FILENO) {
            close(logfd);
        }
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }
    rooted(c, c->certdir, sizeof(c->certdir), "/etc/ssl/acme");
    directories(c->certdir, 0700);
    char path[PATH_MAX];
    format(path, sizeof(path), "%s/%s", c->certdir, c->identifier);
    copy(c->certdir, sizeof(c->certdir), path);
    directories(c->certdir, 0700);
    format(path, sizeof(path), "%s/lock", c->certdir);
    c->lock = open(path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (c->lock < 0 || flock(c->lock, LOCK_EX | LOCK_NB)) {
        fatal("another ACME operation is active, or lock cannot be opened");
    }
    held_lock = c->lock;
    atexit(cleanup);
    signal(SIGINT, interrupted);
    signal(SIGTERM, interrupted);
    if (renew) {
        config_load(c);
        if (!renewal_due(c)) {
            puts("acme: certificate is not due for renewal");
            return 0;
        }
        c->accept_terms = 1;
    } else {
        if (!strchr(c->email, '@') || strpbrk(c->email, "\"\\\r\n <>")) {
            fatal("provide a valid ASCII contact email");
        }
        rooted(c, c->webroot, sizeof(c->webroot), webroot);
        rooted(c, c->ca, sizeof(c->ca), ca);
        if (kind == 7 && !*c->profile) {
            copy(c->profile, sizeof(c->profile), "shortlived");
        }
        if (*c->profile && !token_safe(c->profile, 63)) {
            fatal("invalid ACME profile");
        }
        format(path, sizeof(path), "%s/current", c->certdir);
        char *existing = read_file(path, 100, 1);
        if (existing) {
            free(existing);
            fatal("certificate already installed; use acme renew");
        }
    }

    unsigned char hash[32];
    check(mbedtls_sha256((unsigned char *)c->directory, strlen(c->directory), hash, 0),
          "directory hash");
    char digest[65];
    for (unsigned i = 0; i < 32; i++) {
        snprintf(digest + 2 * i, 3, "%02x", hash[i]);
    }

    char base[PATH_MAX];
    rooted(c, base, sizeof(base), "/var/lib/acme/accounts");
    format(c->accountdir, sizeof(c->accountdir), "%s/%s", base, digest);
    directories(c->accountdir, 0700);
    format(path, sizeof(path), "%s/lock", c->accountdir);
    int accountlock = open(path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (accountlock < 0 || flock(accountlock, LOCK_EX | LOCK_NB)) {
        fatal("another ACME account operation is active");
    }
    random_init(c);
    config_save(c);
    issue(c);
    mbedtls_pk_free(&c->account);
    mbedtls_ctr_drbg_free(&c->rng);
    mbedtls_entropy_free(&c->entropy);
    close(accountlock);
    free(c);
    return 0;
}
