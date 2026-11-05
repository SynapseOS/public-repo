// repository.c: Чтение каталогов пакетных репозиториев
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Получение каталога, проверка подписи и учёт источников пакетов
// Подсистема: пакетный менеджер setup
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 03.11.2026
// Доступ: общедоступный

#include "setup.h"
#include <netdb.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
struct repo repos[MAXREPO];
int nrepo;
struct package *available[MAXPKG], *installed[MAXPKG], *chosen[MAXPKG];
int navailable, ninstalled, nchosen, dry_run, yes;
int dropbear_ed25519_verify(const unsigned char *, unsigned long, const unsigned char *,
                            unsigned long, const unsigned char *);
static void repo_index(struct repo *r, int ri, const char *data, size_t len,
                       const char *sig, size_t siglen, uint64_t floor, int add);
static long long millis(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int waitfd(int fd, int events, long long end) {
    while (millis() < end) {
        struct pollfd p = {fd, events, 0};
        int n = poll(&p, 1, end - millis());
        if (n > 0) {
            return 0;
        }
        if (n < 0 && errno != EINTR) {
            break;
        }
    }
    return -1;
}
static void http(const char *url, const char *dest, size_t limit) {
    char host[256], port[8] = "80", authority[272];
    const char *s = url + 7, *slash = strchr(s, '/');
    size_t n = slash ? (size_t)(slash - s) : strlen(s);
    if (!n || n >= sizeof host) {
        fail("invalid HTTP URL");
    }
    memcpy(authority, s, n);
    authority[n] = 0;
    strcpy(host, authority);
    char *colon = strchr(host, ':');
    if (colon) {
        *colon++ = 0;
        char *end;
        unsigned long p = strtoul(colon, &end, 10);
        if (*end || !p || p > 65535) {
            fail("invalid HTTP port");
        }
        snprintf(port, sizeof port, "%lu", p);
    }
    if (strpbrk(authority, "@\\[] \r\n\t")) {
        fail("invalid HTTP authority");
    }
    s = slash ? slash : "/";
    for (const char *q = s; *q; q++) {
        if ((unsigned char)*q <= 32 || (unsigned char)*q >= 127) {
            fail("invalid HTTP path");
        }
    }

    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM},
                    *a = NULL;
    if (getaddrinfo(host, port, &hints, &a)) {
        fail("DNS failed for %s", host);
    }

    int fd = -1;
    long long end = millis() + 15000;
    for (struct addrinfo *p = a; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);
        int r = connect(fd, p->ai_addr, p->ai_addrlen);
        if (r == 0 || (errno == EINPROGRESS && !waitfd(fd, POLLOUT, end))) {
            int e = 0;
            socklen_t z = sizeof e;
            if (!getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &z) && !e) {
                break;
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(a);
    if (fd < 0) {
        fail("HTTP connection failed");
    }

    char req[4096];
    n = snprintf(req, sizeof req,
                 "GET %s HTTP/1.0\r\nHost: %s\r\nAccept-Encoding: "
                 "identity\r\nConnection: close\r\n\r\n",
                 s, authority);
    if (n >= sizeof req) {
        fail("URL too long");
    }

    size_t pos = 0;
    while (pos < n) {
        ssize_t r = send(fd, req + pos, n - pos, 0);
        if (r > 0) {
            pos += r;
        } else if ((errno != EINTR && errno != EAGAIN) || waitfd(fd, POLLOUT, end)) {
            close(fd);
            fail("HTTP send failed");
        }
    }

    char header[32768];
    pos = 0;
    while (pos + 1 < sizeof header) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r > 0) {
            header[pos++] = c;
            header[pos] = 0;
            if (pos >= 4 && !memcmp(header + pos - 4, "\r\n\r\n", 4)) {
                break;
            }
        } else if (r == 0 ||
                   ((errno != EINTR && errno != EAGAIN) || waitfd(fd, POLLIN, end))) {
            close(fd);
            fail("HTTP header truncated");
        }
    }
    if (pos < 4 || memcmp(header + pos - 4, "\r\n\r\n", 4)) {
        fail("HTTP header too large");
    }
    if (strncmp(header, "HTTP/1.0 200 ", 13) && strncmp(header, "HTTP/1.1 200 ", 13)) {
        fail("HTTP response is not 200");
    }

    int have = 0;
    uint64_t length = 0;
    char *save;
    strtok_r(header, "\r\n", &save);
    for (char *line = strtok_r(NULL, "\r\n", &save); line;
         line = strtok_r(NULL, "\r\n", &save)) {
        char *c = strchr(line, ':');
        if (!c) {
            fail("invalid HTTP header");
        }
        *c++ = 0;
        while (*c == ' ' || *c == '\t') {
            c++;
        }
        if (!strcasecmp(line, "Content-Length")) {
            char *e;
            if (have++ || !(*c >= '0' && *c <= '9')) {
                fail("ambiguous HTTP length");
            }
            errno = 0;
            length = strtoull(c, &e, 10);
            if (errno || *e || length > limit) {
                fail("invalid HTTP length");
            }
        } else if (!strcasecmp(line, "Transfer-Encoding") ||
                   (!strcasecmp(line, "Content-Encoding") &&
                    strcasecmp(c, "identity"))) {
            fail("unsupported HTTP encoding; serve static files");
        }
    }
    if (!have) {
        fail("HTTP Content-Length required");
    }

    int out = open(dest, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (out < 0) {
        fail("create download %s", dest);
    }

    char b[16384];
    uint64_t total = 0;
    struct download_progress progress;
    progress_start(&progress, length);
    while (total < length) {
        size_t want = length - total;
        if (want > sizeof b) {
            want = sizeof b;
        }
        ssize_t r = recv(fd, b, want, 0);
        if (r > 0) {
            size_t w = 0;
            while (w < (size_t)r) {
                ssize_t x = write(out, b + w, r - w);
                if (x <= 0) {
                    fail("download write failed");
                }
                w += x;
            }
            total += r;
            progress_update(&progress, total, 0);
        } else if (r == 0 ||
                   ((errno != EINTR && errno != EAGAIN) || waitfd(fd, POLLIN, end))) {
            fail("HTTP body truncated or timed out");
        }
    }
    close(fd);
    if (fsync(out) || close(out)) {
        fail("download sync failed");
    }
    progress_update(&progress, total, 1);
}
void fetch(const char *url, const char *dest, size_t limit) {
    fprintf(stderr, "Fetching %s\n", url);
    for (int i = 0; i < MAXREPO; i++) {
        struct repo *r = repos + i;
        size_t n = strlen(r->url);
        if (*r->token_file && n && !strncmp(url, r->url, n) && url[n] == '/') {
            private_fetch(url, dest, limit, r->token_file);
            return;
        }
    }
    if (!strncmp(url, "file://", 7)) {
        struct stat st;
        if (stat(url + 7, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            (uint64_t)st.st_size > limit) {
            fail("invalid local download");
        }
        copy_file(url + 7, dest, 0600, 1);
    } else if (!strncmp(url, "http://", 7)) {
        http(url, dest, limit);
    } else if (!strncmp(url, "https://", 8)) {
        char max[32], exe[4096];
        snprintf(max, sizeof max, "%zu", limit);
        path(exe, "bin/https");
        // Хостовый клиент обновляет примонтированный корень без запуска гостевого
        // HTTPS-клиента Подмена команды разрешена только при явном указании корневого
        // каталога
        const char *host_client = *root ? getenv("SETUP_HTTPS_CLIENT") : NULL;
        if (host_client && *host_client) {
            if (*host_client != '/' || strlen(host_client) >= sizeof exe) {
                fail("SETUP_HTTPS_CLIENT must be an absolute executable path");
            }
            strcpy(exe, host_client);
        }

        pid_t pid = fork();
        if (pid < 0) {
            fail("fork HTTPS client");
        }
        if (!pid) {
            execl(exe, "https", "--max-bytes", max, "-o", dest, url, (char *)NULL);
            _exit(127);
        }

        int status;
        struct download_progress progress;
        size_t url_len = strlen(url);
        progress_start(&progress,
                       url_len >= 5 && !strcmp(url + url_len - 5, ".spkg") ? limit : 0);
        char pattern[4096];
        if ((size_t)snprintf(pattern, sizeof pattern, "%s.part.*", dest) >=
            sizeof pattern) {
            fail("download path too long");
        }

        for (;;) {
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid) {
                break;
            }
            if (result < 0 && errno != EINTR) {
                fail("wait HTTPS client");
            }
            glob_t files = {0};
            uint64_t bytes = 0;
            struct stat st;
            if (!lstat(dest, &st) && S_ISREG(st.st_mode) && st.st_size > 0) {
                bytes = st.st_size;
            }
            if (!glob(pattern, 0, NULL, &files)) {
                for (size_t i = 0; i < files.gl_pathc; i++) {
                    if (!lstat(files.gl_pathv[i], &st) && S_ISREG(st.st_mode) &&
                        st.st_size > 0 && (uint64_t)st.st_size > bytes) {
                        bytes = st.st_size;
                    }
                }
            }
            globfree(&files);
            progress_update(&progress, bytes, 0);
            struct timespec delay = {0, 200000000};
            nanosleep(&delay, NULL);
        }

        struct stat st;
        if (WIFEXITED(status) && !WEXITSTATUS(status) && !stat(dest, &st)) {
            progress_update(&progress, st.st_size, 1);
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            fail("HTTPS download failed");
        }
    } else {
        fail("unsupported repository transport");
    }
}

// Проверить загрузку в отдельном процессе, чтобы ошибка одного зеркала не прерывала setup
static int fetch_attempt(const char *url, const char *dest, size_t limit,
                         const char *hash, uint64_t floor, int current) {
    unlink(dest);
    pid_t pid = fork();
    if (pid < 0) {
        fail("fork mirror download");
    }
    if (pid == 0) {
        if (setjmp(failure)) {
            _exit(1);
        }
        if (strncmp(url, "https://", 8)) {
            alarm(current ? 20 : hash != NULL ? 600 : 60);
        }
        fetch(url, dest, limit);
        if (hash != NULL) {
            char actual[65];
            sha_file(dest, actual);
            if (strcmp(actual, hash)) {
                _exit(1);
            }
        }
        if (current) {
            size_t len;
            char *data = read_bytes(dest, 4096, &len);
            struct json *j = parse_json(data, len);
            // Указатель не подписан, доверяем только поколению проверенного ниже снимка
            if (j->t[0].type != J_OBJECT || jnum(j, 0, "format") < 1 ||
                jnum(j, 0, "generation") < floor) {
                _exit(1);
            }
        }
        _exit(0);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fail("wait mirror download");
        }
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }
    unlink(dest);
    return -1;
}

// Выбрать наиболее свежий доступный каталог, а при равенстве самый быстрый ответ
static uint64_t select_mirror(struct repo *r, const char *current, uint64_t floor) {
    char selected[sizeof r->url] = "";
    uint64_t best_generation = 0;
    long long best_time = LLONG_MAX;
    int found = 0;

    for (int i = 0; i < r->nmirrors; i++) {
        char url[2048];
        const char *base = r->mirrors[i];
        const char *query = (!strncmp(base, "https://", 8) || !strncmp(base, "http://", 7))
                                ? "?catalog=large-v1" : "";
        if ((size_t)snprintf(url, sizeof url, "%s/current.json%s", base, query) >= sizeof url) {
            fail("mirror URL too long");
        }

        long long start = millis();
        if (fetch_attempt(url, current, 4096, NULL, floor, 1)) {
            fprintf(stderr, "Repository %s: mirror %s unavailable\n", r->id, base);
            continue;
        }

        size_t len;
        char *data = read_bytes(current, 4096, &len);
        struct json *j = parse_json(data, len);
        uint64_t generation = jnum(j, 0, "generation");
        long long elapsed = millis() - start;
        free(j);
        free(data);
        if (!found || generation > best_generation ||
            (generation == best_generation && elapsed < best_time)) {
            strcpy(selected, base);
            best_generation = generation;
            best_time = elapsed;
            found = 1;
        }
    }

    unlink(current);
    if (!found) {
        fail("no available mirror for %s", r->id);
    }
    strcpy(r->url, selected);
    r->mirror_checked = 1;
    fprintf(stderr, "Repository %s: selected mirror %s (%lld ms)\n", r->id, r->url, best_time);
    return best_generation;
}

// Проверить скачанный каталог в дочернем процессе перед выбором зеркала
static int verify_snapshot_attempt(struct repo *r, uint64_t generation,
                                   uint64_t floor, const char *index, const char *sig) {
    pid_t pid = fork();
    if (pid < 0) {
        fail("fork snapshot verification");
    }
    if (pid == 0) {
        if (setjmp(failure)) {
            _exit(1);
        }

        size_t len, siglen;
        char *data = read_bytes(index, META_LIMIT, &len);
        char *signature = read_bytes(sig, 128, &siglen);
        repo_index(r, -1, data, len, signature, siglen, floor, 0);
        _exit(r->generation == generation ? 0 : 1);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fail("wait snapshot verification");
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

// Загрузить проверенный индекс и подпись одного поколения с одного зеркала
static void fetch_snapshot(struct repo *r, uint64_t generation, uint64_t floor,
                           const char *index, const char *sig) {
    int selected = 0;
    for (int i = 0; i < r->nmirrors; i++) {
        if (!strcmp(r->url, r->mirrors[i])) {
            selected = i;
            break;
        }
    }

    for (int step = 0; step < r->nmirrors; step++) {
        int i = (selected + step) % r->nmirrors;
        char url[2048];
        if ((size_t)snprintf(url, sizeof url, "%s/snapshots/%llu/index.json",
                             r->mirrors[i], (unsigned long long)generation) >= sizeof url) {
            fail("snapshot URL too long");
        }
        if (fetch_attempt(url, index, META_LIMIT, NULL, 0, 0)) {
            continue;
        }
        if ((size_t)snprintf(url, sizeof url, "%s/snapshots/%llu/index.sig",
                             r->mirrors[i], (unsigned long long)generation) >= sizeof url) {
            fail("signature URL too long");
        }
        if (!fetch_attempt(url, sig, 128, NULL, 0, 0) &&
            !verify_snapshot_attempt(r, generation, floor, index, sig)) {
            strcpy(r->url, r->mirrors[i]);
            return;
        }
        fprintf(stderr, "Repository %s: mirror %s has no valid snapshot\n",
                r->id, r->mirrors[i]);
        unlink(index);
        unlink(sig);
    }

    fail("no mirror has snapshot %llu for %s", (unsigned long long)generation, r->id);
}

// Получить архив с проверкой SHA-256, переходя к другому зеркалу при ошибке
void fetch_package(struct repo *r, const char *relative, const char *dest,
                   size_t limit, const char *hash) {
    if (r->nmirrors > 1 && !r->mirror_checked) {
        char current[4096];
        path(current, "var/cache/setup");
        size_t length = strlen(current);
        if ((size_t)snprintf(current + length, sizeof current - length,
                             "/%s/current.download", r->id) >= sizeof current - length) {
            fail("mirror probe path too long");
        }
        select_mirror(r, current, r->generation);
    }

    int selected = 0;
    for (int i = 0; i < r->nmirrors; i++) {
        if (!strcmp(r->url, r->mirrors[i])) {
            selected = i;
            break;
        }
    }

    for (int step = 0; step < r->nmirrors; step++) {
        int i = step == 0 ? selected : (selected + step) % r->nmirrors;
        char url[2048];
        if ((size_t)snprintf(url, sizeof url, "%s/%s", r->mirrors[i], relative) >= sizeof url) {
            fail("package URL too long");
        }
        if (!fetch_attempt(url, dest, limit, hash, 0, 0)) {
            strcpy(r->url, r->mirrors[i]);
            return;
        }
        fprintf(stderr, "Repository %s: package mirror %s unavailable or hash mismatch\n",
                r->id, r->mirrors[i]);
    }
    fail("all package mirrors failed for %s", relative);
}
static void atomic_text(const char *dest, const void *data, size_t len) {
    char tmp[4096];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.new", dest) >= sizeof tmp) {
        fail("path too long");
    }
    unlink(tmp);
    write_bytes(tmp, data, len, 0600);
    if (rename(tmp, dest)) {
        fail("rename metadata: %s", strerror(errno));
    }
    sync_parent(dest);
}
// Проверить подпись и содержимое снимка, затем добавить поддерживаемые пакеты
static void repo_index(struct repo *r, int ri, const char *data, size_t len,
                       const char *sig, size_t siglen, uint64_t floor, int add) {
    if (siglen != 128) {
        fail("signature must be 64 bytes in hex");
    }

    unsigned char pk[32], signature[64];
    char keypath[4096];
    path(keypath, "etc/setup/keys");
    strcat(keypath, "/");
    strcat(keypath, r->key);
    strcat(keypath, ".pub");
    size_t keylen;
    char *key = read_bytes(keypath, 256, &keylen);
    while (keylen && (key[keylen - 1] == '\n' || key[keylen - 1] == '\r')) {
        key[--keylen] = 0;
    }

    struct json *j = parse_json(data, len);
    uint64_t format = jnum(j, 0, "format");
    if (format != 1 && format != 2 && format != 3) {
        fail("wrong index format/repository");
    }
    const char *allowed = format == 3 ?
        "|format||repo||generation||created||expires||packages||required_features||extensions|" :
        "|format||repo||generation||created||expires||packages|";
    object_fields(j, 0, allowed);
    if (format == 3) {
        int required = json_get(j, 0, "required_features");
        int extensions = json_get(j, 0, "extensions");
        if (required < 0 || j->t[required].type != J_ARRAY || j->t[required].children != 0 ||
            (extensions >= 0 && j->t[extensions].type != J_OBJECT)) {
            fail("unsupported repository features");
        }
    }
    static const unsigned char domain_v2[] = "SynapseOS repository index v2";
    static const unsigned char domain_v3[] = "SynapseOS repository index v3";
    unsigned char message[sizeof domain_v3 + 32];
    const unsigned char *signed_data = (const unsigned char *)data;
    size_t signed_len = len;
    if (format >= 2) {
        char hash[65];
        sha_bytes(data, len, hash);
        const unsigned char *domain = format == 2 ? domain_v2 : domain_v3;
        size_t domain_len = sizeof domain_v3;
        memcpy(message, domain, domain_len);
        if (unhex(hash, message + domain_len, 32)) {
            fail("invalid SHA256 index digest");
        }
        signed_data = message;
        signed_len = domain_len + 32;
    }
    if (unhex(key, pk, 32) || unhex(sig, signature, 64) ||
        dropbear_ed25519_verify(signed_data, signed_len, signature, 64, pk)) {
        fail("invalid Ed25519 signature for %s", r->id);
    }
    free(key);
    char id[64];
    jstr(j, 0, "repo", id, sizeof id);
    if (strcmp(id, r->id)) {
        fail("wrong index format/repository");
    }
    uint64_t gen = jnum(j, 0, "generation"), created = jnum(j, 0, "created"),
             expires = jnum(j, 0, "expires");
    time_t now = time(NULL);
    if (now < 1700000000) {
        fail("clock is not trustworthy; set system time first");
    }
    if (gen < floor || gen < r->minimum) {
        fail("repository generation rollback");
    }
    if (created > (uint64_t)now + 300 || expires <= (uint64_t)now ||
        expires <= created) {
        fail("expired or future repository index");
    }
    r->generation = gen;
    r->expires = expires;
    int a = json_get(j, 0, "packages");
    if (a < 0 || j->t[a].type != J_ARRAY) {
        fail("invalid package index");
    }

    for (int t = a + 1; t < j->t[a].next; t = j->t[t].next) {
        object_fields(j, t, format == 3 ?
            "|manifest||sha256||size||url||extensions|" : "|manifest||sha256||size||url|");
        int extensions = json_get(j, t, "extensions");
        if (extensions >= 0 && j->t[extensions].type != J_OBJECT) {
            fail("invalid package extensions");
        }
        int m = json_get(j, t, "manifest");
        if (m < 0 || j->t[m].type != J_OBJECT) {
            fail("invalid package manifest");
        }

        int64_t package_format;
        if (json_integer(j, json_get(j, m, "format"), &package_format) ||
            package_format < 1) {
            fail("invalid package format");
        }

        char hash[65], url[128];
        jstr(j, t, "sha256", hash, sizeof hash);
        jstr(j, t, "url", url, sizeof url);
        uint64_t size = jnum(j, t, "size");
        unsigned char h[32];
        char expected[128];
        snprintf(expected, sizeof expected, "packages/%s.spkg", hash);
        if (unhex(hash, h, 32) || strcmp(expected, url) ||
            size > PACKAGE_LIMIT || size < 1024) {
            fail("invalid package transport metadata");
        }

        if (package_format != 1) {
            continue;
        }

        struct package *p = parse_package(j, m);
        p->repo = ri;
        strcpy(p->hash, hash);
        strcpy(p->url, url);
        p->size = size;
        if (add) {
            if (navailable >= MAXPKG) {
                fail("too many available packages");
            }

            for (int i = 0; i < navailable; i++) {
                if (available[i]->repo == ri && !strcmp(p->name, available[i]->name) &&
                    !strcmp(p->version, available[i]->version)) {
                    fail("duplicate package version");
                }
            }
            available[navailable++] = p;
        } else {
            free(p->files);
            free(p->manifest);
            free(p);
        }
    }
    free(j);
}
void load_repos(int update) {
    char dir[4096];
    path(dir, "etc/setup/repos");
    DIR *d = opendir(dir);
    if (!d) {
        if (errno == ENOENT) {
            return;
        }
        fail("open repositories");
    }

    struct dirent *ent;
    while ((ent = readdir(d))) {
        size_t n = strlen(ent->d_name);
        if (n < 6 || strcmp(ent->d_name + n - 5, ".json")) {
            continue;
        }
        if (nrepo == MAXREPO) {
            fail("too many repositories");
        }

        char config[4096];
        join_path(config, sizeof config, dir, ent->d_name);
        size_t len;
        char *data = read_bytes(config, 65536, &len);
        struct json *j = parse_json(data, len);
        object_fields(j, 0,
                      "|id||url||mirrors||key||prefix||priority||official||enabled||min_"
                      "generation||token_file|");
        int enabled, official;
        if (json_boolean(j, json_get(j, 0, "enabled"), &enabled) ||
            json_boolean(j, json_get(j, 0, "official"), &official)) {
            fail("invalid repository flags");
        }
        if (!enabled) {
            free(j);
            free(data);
            continue;
        }

        struct repo *r = repos + nrepo;
        jstr(j, 0, "id", r->id, sizeof r->id);
        jstr(j, 0, "url", r->url, sizeof r->url);
        strcpy(r->mirrors[0], r->url);
        r->nmirrors = 1;
        int mirrors = json_get(j, 0, "mirrors");
        if (mirrors >= 0) {
            if (j->t[mirrors].type != J_ARRAY) {
                fail("invalid repository mirrors");
            }
            for (int t = mirrors + 1; t < j->t[mirrors].next; t = j->t[t].next) {
                if (r->nmirrors >= MAXMIRROR ||
                    json_string(j, t, r->mirrors[r->nmirrors], sizeof r->mirrors[0]) < 0) {
                    fail("invalid repository mirrors");
                }
                r->nmirrors++;
            }
        }
        jstr(j, 0, "key", r->key, sizeof r->key);
        jstr(j, 0, "prefix", r->prefix, sizeof r->prefix);
        if (json_get(j, 0, "token_file") >= 0) {
            jstr(j, 0, "token_file", r->token_file, sizeof r->token_file);
            if (!safe_path(r->token_file) ||
                strncmp(r->token_file, "etc/setup/auth/", 15) ||
                strncmp(r->url, "https://", 8)) {
                fail("authenticated repositories require HTTPS and etc/setup/auth "
                     "credentials");
            }
        }
        uint64_t priority = jnum(j, 0, "priority");
        if (priority > 10000) {
            fail("repository priority out of range");
        }
        r->priority = priority;
        r->minimum = jnum(j, 0, "min_generation");
        r->official = official;
        if (!*r->url || !valid_name(r->id) || !valid_name(r->key) ||
            r->priority > 10000 || strpbrk(r->url, "\r\n \t") ||
            r->url[strlen(r->url) - 1] == '/') {
            fail("invalid repository config");
        }
        if (r->nmirrors > 1 && *r->token_file) {
            fail("authenticated repositories do not support mirrors");
        }
        for (int i = 0; i < r->nmirrors; i++) {
            const char *mirror = r->mirrors[i];
            size_t length = strlen(mirror);
            if (!length || mirror[length - 1] == '/' || strpbrk(mirror, "\r\n \t") ||
                (strncmp(mirror, "https://", 8) && strncmp(mirror, "http://", 7) &&
                 strncmp(mirror, "file://", 7))) {
                fail("invalid repository mirror");
            }
            for (int k = 0; k < i; k++) {
                if (!strcmp(mirror, r->mirrors[k])) {
                    fail("duplicate repository mirror");
                }
            }
        }

        for (int i = 0; i < nrepo; i++) {
            if (!strcmp(repos[i].id, r->id)) {
                fail("duplicate repository ID");
            }
        }
        free(j);
        free(data);
        char state[4096];
        path(state, "var/lib/setup/repos");
        if (update == 1) {
            mkdirs(state);
        }
        strcat(state, "/");
        strcat(state, r->id);
        struct stat st;
        uint64_t floor = r->minimum;
        int has = !lstat(state, &st);
        if (has) {
            data = read_bytes(state, 64, &len);
            char *end;
            errno = 0;
            floor = strtoull(data, &end, 10);
            if (errno || *end) {
                fail("invalid repository generation state");
            }
            free(data);
        }
        if (update < 0) {
            r->generation = has ? floor : 0;
            nrepo++;
            continue;
        }
        if (*r->token_file) {
            char statusfile[4096], statusurl[2048];
            path(statusfile, "var/cache/setup");
            mkdirs(statusfile);
            strcat(statusfile, "/plus-status.download");
            snprintf(statusurl, sizeof statusurl, "%s/status.json", r->url);
            unlink(statusfile);
            fetch(statusurl, statusfile, 4096);
            data = read_bytes(statusfile, 4096, &len);
            j = parse_json(data, len);
            int active;
            if (json_boolean(j, json_get(j, 0, "active"), &active)) {
                fail("invalid subscription status");
            }
            free(j);
            free(data);
            unlink(statusfile);
            if (!active) {
                fprintf(stderr,
                        "Repository %s: subscription inactive; installed packages "
                        "retained, updates disabled\n",
                        r->id);
                nrepo++;
                continue;
            }
        }
        uint64_t generation = floor;
        char cachebase[4096];
        path(cachebase, "var/cache/setup");
        strcat(cachebase, "/");
        strcat(cachebase, r->id);
        char temp[4096], tempsig[4096], current[4096];
        if (update == 1) {
            mkdirs(cachebase);
            snprintf(current, sizeof current, "%s/current.download", cachebase);
            generation = select_mirror(r, current, floor);
            snprintf(temp, sizeof temp, "%s/index.download", cachebase);
            snprintf(tempsig, sizeof tempsig, "%s/sig.download", cachebase);
            fetch_snapshot(r, generation, floor, temp, tempsig);
        } else {
            if (!has) {
                nrepo++;
                continue;
            }
            snprintf(temp, sizeof temp, "%s/%llu/index.json", cachebase,
                     (unsigned long long)generation);
            snprintf(tempsig, sizeof tempsig, "%s/%llu/index.sig", cachebase,
                     (unsigned long long)generation);
        }

        size_t siglen;
        data = read_bytes(temp, META_LIMIT, &len);
        char *sig = read_bytes(tempsig, 128, &siglen);
        repo_index(r, nrepo, data, len, sig, siglen, floor, 1);
        if (r->generation != generation) {
            fail("snapshot generation does not match pointer");
        }
        if (update == 1) {
            char destdir[4096], index[4096], sigpath[4096];
            snprintf(destdir, sizeof destdir, "%s/%llu", cachebase,
                     (unsigned long long)generation);
            mkdirs(destdir);
            join_path(index, sizeof index, destdir, "index.json");
            join_path(sigpath, sizeof sigpath, destdir, "index.sig");
            if (!lstat(index, &st)) {
                char h1[65], h2[65];
                sha_file(index, h1);
                sha_bytes(data, len, h2);
                if (strcmp(h1, h2)) {
                    fail("immutable snapshot changed");
                }
            } else {
                write_bytes(index, data, len, 0600);
            }
            write_bytes(sigpath, sig, siglen, 0600);
            sync_parent(index);
            char number[64];
            int z =
                snprintf(number, sizeof number, "%llu", (unsigned long long)generation);
            atomic_text(state, number, z);
            unlink(temp);
            unlink(tempsig);
            fprintf(stderr, "Repository %s: generation %llu verified\n", r->id,
                    (unsigned long long)generation);
        }
        free(data);
        free(sig);
        nrepo++;
    }
    closedir(d);
}
void load_state(void) {
    char p[4096];
    path(p, "var/lib/setup/installed.json");
    struct stat st;
    if (lstat(p, &st)) {
        if (errno == ENOENT) {
            return;
        }
        fail("read installed state");
    }

    size_t len;
    char *data = read_bytes(p, META_LIMIT, &len);
    struct json *j = parse_json(data, len);
    object_fields(j, 0, "|format||packages|");
    if (jnum(j, 0, "format") != 1) {
        fail("unsupported installed database");
    }

    int a = json_get(j, 0, "packages");
    if (a < 0 || j->t[a].type != J_ARRAY) {
        fail("invalid installed database");
    }

    for (int t = a + 1; t < j->t[a].next; t = j->t[t].next) {
        if (ninstalled == MAXPKG) {
            fail("too many installed packages");
        }
        object_fields(j, t, "|source||explicit||manifest|");
        struct package *pkg = parse_package(j, json_get(j, t, "manifest"));
        pkg->installed = 1;
        pkg->repo = -1;
        char source[64];
        jstr(j, t, "source", source,
             sizeof source); // В записи установленного пакета сохраняем источник в поле
                             // url
        // Адрес загрузки в такой записи не используется
        strcpy(pkg->url, source);
        if (json_boolean(j, json_get(j, t, "explicit"), &pkg->explicit)) {
            fail("invalid installed flags");
        }

        for (int i = 0; i < ninstalled; i++) {
            if (!strcmp(installed[i]->name, pkg->name)) {
                fail("duplicate installed package");
            }
        }
        installed[ninstalled++] = pkg;
    }
    free(j);
    free(data);
}

// Выдать только общие сведения, без путей файлов, хэшей и данных авторизации
static void public_string(FILE *stream, const char *value) {
    fputc('"', stream);
    for (const unsigned char *p = (const unsigned char *)value; *p != 0; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', stream);
            fputc(*p, stream);
        } else if (*p < 32) {
            fprintf(stream, "\\u%04x", (unsigned)*p);
        } else {
            fputc(*p, stream);
        }
    }

    fputc('"', stream);
}

// Записать разрешённые поля manifest, не копируя исходный JSON целиком
static void public_manifest(FILE *stream, const struct package *package) {
    const char *keys[] = {"name", "version", "description", "kind", "arch", "abi"};
    const char *values[] = {package->name, package->version, package->description,
                           package->kind, package->arch, package->abi};
    fputs("{\"format\":1", stream);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        fprintf(stream, ",\"%s\":", keys[i]);
        public_string(stream, values[i]);
    }

    fputs(",\"depends\":[", stream);
    for (int i = 0; i < package->ndep; i++) {
        if (i != 0) {
            fputc(',', stream);
        }
        public_string(stream, package->dep[i]);
    }

    fputs("],\"conflicts\":[", stream);
    for (int i = 0; i < package->nconf; i++) {
        if (i != 0) {
            fputc(',', stream);
        }
        public_string(stream, package->conflict[i]);
    }
    fputs("],\"files\":[]}", stream);
}

// Создать только общеизвестные каталоги usr/share с точными правами при любом umask
static void public_directory(const char *relative) {
    char full[4096];
    path(full, relative);
    if (mkdir(full, 0755) != 0 && errno != EEXIST) {
        fail("create public catalog directory: %s", strerror(errno));
    }

    int fd = open(full, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    struct stat st;
    if (fd < 0) {
        fail("open public catalog directory: %s", strerror(errno));
    }
    int bad = fstat(fd, &st) != 0 || st.st_uid != geteuid();
    if (!bad) {
        bad = fchmod(fd, 0755) != 0;
    }
    close(fd);
    if (bad) {
        fail("unsafe public catalog directory");
    }
}

// Убрать прежний снимок до изменения состояния, чтобы после отказа не показывать старые данные
void invalidate_public_catalog(void) {
    int fd = open(*root != 0 ? root : "/", O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        fail("open public catalog root");
    }
    const char *parts[] = {"usr", "share", "setup"};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        int next = openat(fd, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        int error = errno;
        close(fd);
        if (next < 0) {
            if (error == ENOENT) {
                return;
            }
            fail("unsafe public catalog directory");
        }
        fd = next;
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_uid != geteuid() || fchmod(fd, 0755) != 0) {
            close(fd);
            fail("unsafe public catalog directory");
        }
    }
    int result = unlinkat(fd, "catalog.json", 0);
    int error = errno;
    if (result == 0 && fsync(fd) != 0) {
        result = -1;
        error = errno;
    }
    close(fd);
    if (result != 0 && error != ENOENT) {
        fail("invalidate public catalog: %s", strerror(error));
    }
}

// Опубликовать разобранное состояние и уже проверенные каталоги под общей блокировкой setup
void publish_public_catalog(struct package **state, int count) {
    public_directory("usr");
    public_directory("usr/share");
    public_directory("usr/share/setup");
    char full[4096], temp[4096];
    path(full, "usr/share/setup/catalog.json");
    path(temp, "usr/share/setup/catalog.json.new");
    if (unlink(temp) != 0 && errno != ENOENT) {
        fail("remove public catalog staging file");
    }

    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        fail("create public catalog: %s", strerror(errno));
    }
    FILE *stream = fdopen(fd, "wb");
    if (stream == NULL) {
        close(fd);
        unlink(temp);
        fail("open public catalog stream");
    }

    fputs("{\"format\":1,\"repositories\":[", stream);
    for (int i = 0; i < nrepo; i++) {
        fprintf(stream, "%s{\"id\":", i != 0 ? "," : "");
        public_string(stream, repos[i].id);
        fprintf(stream, ",\"priority\":%d}", repos[i].priority);
    }
    fputs("],\"available\":[", stream);
    for (int i = 0; i < navailable; i++) {
        struct package *package = available[i];
        fprintf(stream, "%s{\"source\":", i != 0 ? "," : "");
        public_string(stream, repos[package->repo].id);
        fprintf(stream, ",\"size\":%llu,\"manifest\":", (unsigned long long)package->size);
        public_manifest(stream, package);
        fputc('}', stream);
    }
    fputs("],\"installed\":[", stream);
    for (int i = 0; i < count; i++) {
        struct package *package = state[i];
        const char *source = package->installed ? package->url : repos[package->repo].id;
        if (!valid_name(source)) {
            fclose(stream);
            unlink(temp);
            fail("invalid public catalog source");
        }
        fprintf(stream, "%s{\"source\":", i != 0 ? "," : "");
        public_string(stream, source);
        fprintf(stream, ",\"explicit\":%s,\"manifest\":", package->explicit ? "true" : "false");
        public_manifest(stream, package);
        fputc('}', stream);
    }
    fputs("]}\n", stream);
    long bytes = ftell(stream);
    int bad = bytes < 0 || (uint64_t)bytes > META_LIMIT || ferror(stream);
    if (!bad) {
        bad = fflush(stream) != 0 || fchmod(fd, 0644) != 0 || fsync(fd) != 0;
    }
    if (fclose(stream) != 0) {
        bad = 1;
    }
    if (bad || rename(temp, full) != 0) {
        unlink(temp);
        fail("public catalog publication failed; package state may already be committed");
    }
    sync_parent(full);
}

// Разрешить имя источника без адресов серверов и ссылок на приватные credentials
static int public_repo(const char *id, int priority) {
    if (!valid_name(id)) {
        fail("invalid public catalog repository");
    }
    for (int i = 0; i < nrepo; i++) {
        if (strcmp(repos[i].id, id) == 0) {
            return i;
        }
    }
    if (nrepo == MAXREPO || priority < 0 || priority > 10000) {
        fail("public catalog repository limit");
    }
    strcpy(repos[nrepo].id, id);
    repos[nrepo].priority = priority;
    return nrepo++;
}

// Прочитать только безопасный снимок, не открывая installed.json, подписанные кэши и auth
void load_public_catalog(void) {
    char full[4096];
    path(full, "usr/share/setup/catalog.json");
    struct stat st;
    uid_t owner = 0;
    if (*root != 0) {
        struct stat directory;
        if (stat(root, &directory) != 0 || !S_ISDIR(directory.st_mode)) {
            fail("invalid public catalog root");
        }
        owner = directory.st_uid;
    }
    if (lstat(full, &st) != 0) {
        fail("public catalog unavailable; administrator must run setup update or recover");
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != owner || (st.st_mode & 0022) != 0) {
        fail("unsafe public catalog ownership or permissions");
    }

    size_t len;
    char *data = read_bytes(full, META_LIMIT, &len);
    struct json *json = parse_json(data, len);
    object_fields(json, 0, "|format||repositories||available||installed|");
    if (jnum(json, 0, "format") != 1) {
        fail("unsupported public catalog");
    }
    int array = json_get(json, 0, "repositories");
    if (array < 0 || json->t[array].type != J_ARRAY) {
        fail("invalid public repositories");
    }
    for (int token = array + 1; token < json->t[array].next; token = json->t[token].next) {
        object_fields(json, token, "|id||priority|");
        char id[64];
        jstr(json, token, "id", id, sizeof(id));
        uint64_t priority = jnum(json, token, "priority");
        if (priority > 10000) {
            fail("public repository priority out of range");
        }
        (void)public_repo(id, (int)priority);
    }

    const char *sections[] = {"available", "installed"};
    for (int section = 0; section < 2; section++) {
        array = json_get(json, 0, sections[section]);
        if (array < 0 || json->t[array].type != J_ARRAY) {
            fail("invalid public package list");
        }
        for (int token = array + 1; token < json->t[array].next; token = json->t[token].next) {
            object_fields(json, token, section == 0 ? "|source||size||manifest|" :
                          "|source||explicit||manifest|");
            struct package *package = parse_package(json, json_get(json, token, "manifest"));
            if (package->nfile != 0) {
                fail("private file metadata in public catalog");
            }
            char source[64];
            jstr(json, token, "source", source, sizeof(source));
            if (!valid_name(source)) {
                fail("invalid public source");
            }
            if (section == 0) {
                if (navailable == MAXPKG) {
                    fail("public catalog package limit");
                }
                package->size = jnum(json, token, "size");
                if (package->size > PACKAGE_LIMIT) {
                    fail("public package size limit");
                }
                package->repo = public_repo(source, 0);
                available[navailable++] = package;
            } else {
                if (ninstalled == MAXPKG) {
                    fail("public installed package limit");
                }
                if (json_boolean(json, json_get(json, token, "explicit"), &package->explicit)) {
                    fail("invalid public installed flag");
                }
                package->installed = 1;
                strcpy(package->url, source);
                installed[ninstalled++] = package;
            }
        }
    }

    // Установленные компоненты видны и до первого получения сетевого индекса
    for (int i = 0; i < ninstalled; i++) {
        int found = 0;
        for (int j = 0; j < navailable; j++) {
            if (strcmp(installed[i]->name, available[j]->name) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            if (navailable == MAXPKG) {
                fail("public catalog package limit");
            }
            installed[i]->repo = public_repo(installed[i]->url, 0);
            available[navailable++] = installed[i];
        }
    }
    free(json);
    free(data);
}
