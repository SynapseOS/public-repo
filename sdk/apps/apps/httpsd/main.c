// main.c: HTTPS-сервер
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Подсистема: HTTPS-сервер в пользовательском пространстве
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

#define _GNU_SOURCE
#include "../common/tls/tls.h"
#include "../common/service_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <signal.h>
#include <ctype.h>

static int hex(unsigned char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}
// Читаем активную версию один раз, чтобы ключ и цепочка относились к одной версии
static int credentials(const char *acme, const char **cert, const char **key,
                       char certpath[4096], char keypath[4096]) {
    if (!acme) {
        return 0;
    }

    char path[4096], generation[101];
    int n = snprintf(path, sizeof(path), "%s/current", acme);
    if (n < 0 || n >= (int)sizeof(path)) {
        return -1;
    }

    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        return -1;
    }
    ssize_t size = read(fd, generation, sizeof(generation));
    close(fd);
    if (size <= 0 || size >= 100) {
        return -1;
    }

    for (ssize_t i = 0; i < size; i++) {
        unsigned char c = generation[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return -1;
        }
    }
    generation[size] = 0;
    n = snprintf(certpath, 4096, "%s/generations/%s/fullchain.pem", acme, generation);
    if (n < 0 || n >= 4096) {
        return -1;
    }
    n = snprintf(keypath, 4096, "%s/generations/%s/privkey.pem", acme, generation);
    if (n < 0 || n >= 4096) {
        return -1;
    }
    *cert = certpath;
    *key = keypath;
    return 0;
}
static int decode(char *path) {
    char *dst = path;
    for (char *src = path; *src && *src != '?' && *src != '#'; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '%') {
            if (!src[1] || !src[2]) {
                return -1;
            }

            int a = hex(src[1]), b = hex(src[2]);
            if (a < 0 || b < 0) {
                return -1;
            }
            c = (a << 4) | b;
            src += 2;
        }
        if (c < 32 || c == 127 || c == '\\') {
            return -1;
        }
        *dst++ = (char)c;
    }
    *dst = 0;
    return path[0] == '/' ? 0 : -1;
}
static int open_file(int root, char *path) {
    if (decode(path)) {
        return -1;
    }

    int directory = dup(root);
    if (directory < 0) {
        return -1;
    }

    char *p = path + 1;
    if (!*p) {
        int file = openat(directory, "index.html", O_RDONLY | O_NOFOLLOW);
        close(directory);
        return file;
    }

    for (;;) {
        char *slash = strchr(p, '/');
        if (slash) {
            *slash = 0;
        }
        if (!*p || !strcmp(p, ".") || !strcmp(p, "..")) {
            close(directory);
            return -1;
        }

        int next =
            openat(directory, p, O_RDONLY | O_NOFOLLOW | (slash ? O_DIRECTORY : 0));
        close(directory);
        if (next < 0) {
            return -1;
        }
        if (!slash) {
            return next;
        }
        directory = next;
        p = slash + 1;
        if (!*p) {
            int file = openat(directory, "index.html", O_RDONLY | O_NOFOLLOW);
            close(directory);
            return file;
        }
    }
}
static const char *mime(const char *path) {
    const char *p = strrchr(path, '.');
    if (!p) {
        return "application/octet-stream";
    }
    if (!strcasecmp(p, ".html") || !strcasecmp(p, ".htm")) {
        return "text/html; charset=utf-8";
    }
    if (!strcasecmp(p, ".txt")) {
        return "text/plain; charset=utf-8";
    }
    if (!strcasecmp(p, ".json")) {
        return "application/json";
    }
    if (!strcasecmp(p, ".css")) {
        return "text/css";
    }
    if (!strcasecmp(p, ".js")) {
        return "text/javascript";
    }
    if (!strcasecmp(p, ".png")) {
        return "image/png";
    }
    if (!strcasecmp(p, ".jpg") || !strcasecmp(p, ".jpeg")) {
        return "image/jpeg";
    }
    if (!strcasecmp(p, ".svg")) {
        return "image/svg+xml";
    }
    return "application/octet-stream";
}
static int header(struct tls *t, int status, const char *reason,
                  unsigned long long size, const char *type) {
    char buffer[512];
    int n = snprintf(
        buffer, sizeof buffer,
        "HTTP/1.1 %d %s\r\nContent-Length: %llu\r\nContent-Type: %s\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n",
        status, reason, size, type);
    return n < 0 || n >= (int)sizeof buffer ? -1 : tls_write_all(t, buffer, n);
}
static void handle(struct tls *t, int root) {
    char request[16385];
    size_t size = 0;
    char *finish = NULL;
    while (size < sizeof request - 1) {
        int n = tls_read(t, request + size, sizeof request - 1 - size);
        if (n <= 0) {
            return;
        }
        if (memchr(request + size, 0, n)) {
            return;
        }
        size += n;
        request[size] = 0;
        if ((finish = strstr(request, "\r\n\r\n"))) {
            break;
        }
    }
    if (!finish) {
        header(t, 431, "Request Header Fields Too Large", 0, "text/plain");
        return;
    }

    char *end = strstr(request, "\r\n");
    if (!end) {
        return;
    }
    *end = 0;
    char method[8], path[4096], version[16], extra;
    if (sscanf(request, "%7s %4095s %15s %c", method, path, version, &extra) != 3 ||
        (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))) {
        header(t, 400, "Bad Request", 0, "text/plain");
        return;
    }

    int head = !strcmp(method, "HEAD");
    if (!head && strcmp(method, "GET")) {
        header(t, 405, "Method Not Allowed", 0, "text/plain");
        return;
    }
    const char *type = mime(path);
    int file = open_file(root, path);
    struct stat st;
    if (file < 0) {
        fprintf(stderr, "httpsd: file lookup failed: %s\n", strerror(errno));
    }
    if (file < 0 || fstat(file, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) {
        if (file >= 0) {
            close(file);
        }
        header(t, 404, "Not Found", 0, "text/plain");
        return;
    }
    if (!header(t, 200, "OK", st.st_size, type) && !head) {
        unsigned char buffer[16384];
        off_t remaining = st.st_size;
        while (remaining > 0) {
            size_t size =
                remaining > (off_t)sizeof buffer ? sizeof buffer : (size_t)remaining;
            ssize_t n = read(file, buffer, size);
            if (n <= 0 || tls_write_all(t, buffer, n)) {
                break;
            }
            remaining -= n;
        }
    }
    close(file);
}
int main(int argc, char **argv) {
    const char *root = "/var/www", *cert = NULL, *key = NULL, *acme = NULL;
    unsigned port = 443;
    int logging = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cert") && i + 1 < argc) {
            cert = argv[++i];
        } else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
            key = argv[++i];
        } else if (!strcmp(argv[i], "--acme") && i + 1 < argc) {
            acme = argv[++i];
        } else if (!strcmp(argv[i], "--root") && i + 1 < argc) {
            root = argv[++i];
        } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            char *end;
            unsigned long n = strtoul(argv[++i], &end, 10);
            if (*end || !n || n > 65535) {
                return 2;
            }
            port = n;
        } else if (!strcmp(argv[i], "--foreground")) {
            logging = 0;
        } else if (!strcmp(argv[i], "--help")) {
            puts(
                "httpsd (--cert PEM --key PEM | --acme DIR) [--root /var/www] [--port 443] [--foreground]\nStatic HTTPS GET/HEAD server; up to 16 concurrent connections.\nLogs to /var/log/httpsd.log unless --foreground is used.");
            return 0;
        } else {
            return 2;
        }
    }
    if ((acme && (cert || key)) || (!acme && (!cert || !key))) {
        fprintf(stderr, "httpsd: select --acme or --cert and --key (see --help)\n");
        return 2;
    }
    // Проверяем ключ и сертификат в отдельном процессе
    // Обработчик соединения инициализирует свой DRBG после fork
    pid_t check = fork();
    if (check < 0) {
        return 1;
    }
    if (!check) {
        char cp[4096], kp[4096];
        if (credentials(acme, &cert, &key, cp, kp)) {
            _exit(1);
        }

        struct tls t;
        int rc = tls_init(&t, 1, cert, key);
        if (rc) {
            tls_error("httpsd credentials", rc);
        }
        tls_free(&t);
        _exit(rc ? 1 : 0);
    }

    int status;
    if (waitpid(check, &status, 0) != check || !WIFEXITED(status) ||
        WEXITSTATUS(status)) {
        return 1;
    }

    int directory = open(root, O_RDONLY | O_DIRECTORY);
    if (directory < 0) {
        perror("httpsd root");
        return 1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("httpsd socket");
        return 1;
    }

    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY};
    if (bind(sock, (void *)&address, sizeof address) || listen(sock, 16)) {
        perror("httpsd listen");
        return 1;
    }
    if (logging && service_log_start("httpsd", 1)) {
        perror("httpsd log");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    fprintf(stderr, "httpsd: serving %s on port %u\n", root, port);
    unsigned children = 0;
    for (;;) {
        while (waitpid(-1, &status, WNOHANG) > 0) {
            if (children) {
                children--;
            }
        }
        if (children >= 16) {
            if (waitpid(-1, &status, 0) > 0) {
                children--;
            }
            continue;
        }

        int client = accept(sock, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("httpsd accept");
            return 1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(client);
            continue;
        }
        if (!pid) {
            close(sock);
            char cp[4096], kp[4096];
            if (credentials(acme, &cert, &key, cp, kp)) {
                close(client);
                _exit(1);
            }

            struct tls t;
            int rc = tls_init(&t, 1, cert, key);
            if (!rc) {
                rc = tls_accept(&t, client);
            } else {
                close(client);
            }
            if (rc) {
                tls_error("httpsd handshake", rc);
            } else {
                handle(&t, directory);
                mbedtls_ssl_close_notify(&t.ssl);
            }
            tls_free(&t);
            close(directory);
            _exit(rc ? 1 : 0);
        }
        children++;
        close(client);
    }
}
