// repository.c: Чтение каталогов пакетных репозиториев
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Получение каталога, проверка подписи и учёт источников пакетов
// Подсистема: пакетный менеджер setup
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 21.10.2026
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
            object_fields(j, 0, "|format||generation|");
            if (jnum(j, 0, "format") != 1 || jnum(j, 0, "generation") < floor) {
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

// Загрузить индекс и подпись одного поколения с одного зеркала
static void fetch_snapshot(struct repo *r, uint64_t generation,
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
        if (!fetch_attempt(url, sig, 128, NULL, 0, 0)) {
            strcpy(r->url, r->mirrors[i]);
            return;
        }
        unlink(index);
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
    object_fields(j, 0, "|format||repo||generation||created||expires||packages|");
    uint64_t format = jnum(j, 0, "format");
    if (format != 1 && format != 2) {
        fail("wrong index format/repository");
    }
    static const unsigned char domain[] = "SynapseOS repository index v2";
    unsigned char message[sizeof domain + 32];
    const unsigned char *signed_data = (const unsigned char *)data;
    size_t signed_len = len;
    if (format == 2) {
        char hash[65];
        sha_bytes(data, len, hash);
        memcpy(message, domain, sizeof domain); // Копируем также нулевой байт домена
        if (unhex(hash, message + sizeof domain, 32)) {
            fail("invalid SHA256 index digest");
        }
        signed_data = message;
        signed_len = sizeof message;
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
        object_fields(j, t, "|manifest||sha256||size||url|");
        int m = json_get(j, t, "manifest");
        struct package *p = parse_package(j, m);
        p->repo = ri;
        jstr(j, t, "sha256", p->hash, sizeof p->hash);
        jstr(j, t, "url", p->url, sizeof p->url);
        p->size = jnum(j, t, "size");
        unsigned char h[32];
        char expected[128];
        snprintf(expected, sizeof expected, "packages/%s.spkg", p->hash);
        if (unhex(p->hash, h, 32) || strcmp(expected, p->url) ||
            p->size > PACKAGE_LIMIT || p->size < 1024) {
            fail("invalid package transport metadata");
        }
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
            fetch_snapshot(r, generation, temp, tempsig);
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
