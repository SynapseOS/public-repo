// http.c: Клиент ACME
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Подсистема: Клиент ACME в пользовательском пространстве
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

#define _GNU_SOURCE
#include "acme.h"
#include "../common/tls/tls.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct stream {
    struct tls tls;
    int secure, fd;
    long long deadline;
    unsigned char data[8192];
    size_t at, size;
};
static long long ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        return 0;
    }
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int receive(struct stream *s) {
    if (ms() >= s->deadline) {
        return -1;
    }

    int n = s->secure ? tls_read(&s->tls, s->data, sizeof(s->data))
                      : (int)recv(s->fd, s->data, sizeof(s->data), 0);
    if (n <= 0) {
        return -1;
    }
    s->at = 0;
    s->size = (size_t)n;
    return 0;
}
static int byte(struct stream *s) {
    if (s->at == s->size && receive(s)) {
        return -1;
    }
    return s->data[s->at++];
}
static int line(struct stream *s, char *out, size_t cap, size_t *budget) {
    size_t at = 0;
    int c;
    while ((c = byte(s)) >= 0) {
        if (!*budget || at + 1 >= cap || !c ||
            (c < 32 && c != '\r' && c != '\t' && c != '\n')) {
            return -1;
        }
        --*budget;
        if (c == '\n') {
            if (!at || out[at - 1] != '\r') {
                return -1;
            }
            out[at - 1] = 0;
            return 0;
        }
        out[at++] = c;
    }
    return -1;
}
static int exact(struct stream *s, char *out, size_t bytes) {
    while (bytes) {
        if (s->at == s->size && receive(s)) {
            return -1;
        }

        size_t n = s->size - s->at;
        if (n > bytes) {
            n = bytes;
        }
        memcpy(out, s->data + s->at, n);
        s->at += n;
        out += n;
        bytes -= n;
    }
    return 0;
}
static int integer(const char *text, int base, unsigned long *out) {
    char *end;
    if (!*text || !isalnum((unsigned char)*text)) {
        return -1;
    }
    errno = 0;
    *out = strtoul(text, &end, base);
    return errno || *end ? -1 : 0;
}
static int connect_plain(const char *host, const char *port) {
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICHOST},
                    *addresses;
    if (getaddrinfo(host, port, &hints, &addresses)) {
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
            close(fd);
            fd = -1;
            continue;
        }

        int rc = connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc && errno == EINPROGRESS) {
            struct pollfd p = {fd, POLLOUT, 0};
            int error = 0;
            socklen_t length = sizeof(error);
            rc = poll(&p, 1, 15000) > 0 &&
                         !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) &&
                         !error
                     ? 0
                     : -1;
        }
        if (!rc && !fcntl(fd, F_SETFL, 0)) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd >= 0) {
        struct timeval tv = {15, 0};
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv))) {
            close(fd);
            return -1;
        }
    }
    return fd;
}
static int send_all(struct stream *s, const char *data, size_t length) {
    if (s->secure) {
        return tls_write_all(&s->tls, data, length);
    }

    while (length) {
        ssize_t n = send(s->fd, data, length, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        data += n;
        length -= n;
    }
    return 0;
}
void acme_http_free(struct http_reply *reply) {
    free(reply->body);
    reply->body = NULL;
}
int acme_http(const char *url, const char *method, const char *payload, const char *ca,
              int test_http, struct http_reply *r) {
    memset(r, 0, sizeof(*r));
    struct stream s = {.fd = -1, .deadline = ms() + 45000};
    char host[256], port[8], authority[264], path[ACME_URL], request[ACME_URL + 1024];
    const char *start;
    if (!strncmp(url, "https://", 8)) {
        s.secure = 1;
        start = url + 8;
        strcpy(port, "443");
    } else if (test_http && !strncmp(url, "http://", 7)) {
        start = url + 7;
        strcpy(port, "80");
    } else {
        fputs("acme: only HTTPS ACME URLs are accepted\n", stderr);
        return -1;
    }
    if (strlen(url) >= ACME_URL || strpbrk(url, "\r\n\t #\\") || strpbrk(start, "@")) {
        return -1;
    }

    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        if (*p < 33 || *p > 126) {
            return -1;
        }
    }
    const char *slash = strchr(start, '/');
    size_t n = slash ? (size_t)(slash - start) : strlen(start);
    if (!n || n >= sizeof(authority)) {
        return -1;
    }
    memcpy(authority, start, n);
    authority[n] = 0;
    strcpy(path, slash ? slash : "/");
    char *colon;
    if (authority[0] == '[') {
        char *end = strchr(authority, ']');
        if (!end || (end[1] && end[1] != ':')) {
            return -1;
        }
        n = (size_t)(end - authority - 1);
        if (n >= sizeof(host)) {
            return -1;
        }
        memcpy(host, authority + 1, n);
        host[n] = 0;
        colon = end[1] ? end + 1 : NULL;
    } else {
        colon = strchr(authority, ':');
        n = colon ? (size_t)(colon - authority) : strlen(authority);
        if (n >= sizeof(host)) {
            return -1;
        }
        memcpy(host, authority, n);
        host[n] = 0;
    }
    if (colon) {
        unsigned long value;
        if (strlen(colon + 1) >= sizeof(port) || integer(colon + 1, 10, &value) ||
            !value || value > 65535) {
            return -1;
        }
        strcpy(port, colon + 1);
    }
    if (!s.secure && strcmp(host, "127.0.0.1") && strcmp(host, "::1")) {
        fputs("acme: --test-http is limited to numeric loopback\n", stderr);
        return -1;
    }

    int initialized = 0, result = -1;
    if (s.secure) {
        int rc = tls_init(&s.tls, 0, ca, NULL);
        initialized = 1;
        if (!rc) {
            rc = tls_connect(&s.tls, host, port);
        }
        if (rc) {
            tls_error("acme HTTPS", rc);
            goto done;
        }
    } else {
        s.fd = connect_plain(host, port);
        if (s.fd < 0) {
            goto done;
        }
    }

    size_t bytes = payload ? strlen(payload) : 0;
    int size = snprintf(
        request, sizeof(request),
        "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: SynapseOS-acme/1\r\nAccept: application/json, application/pem-certificate-chain\r\nConnection: close\r\n%sContent-Length: %zu\r\n\r\n",
        method, path, authority,
        payload ? "Content-Type: application/jose+json\r\n" : "", bytes);
    if (size < 0 || (size_t)size >= sizeof(request) || send_all(&s, request, size) ||
        (bytes && send_all(&s, payload, bytes))) {
        goto done;
    }

    char text[8192];
    size_t budget = 65536;
    unsigned long length = 0;
    int have_length = 0, chunked = 0;
    if (line(&s, text, sizeof(text), &budget) ||
        (strncmp(text, "HTTP/1.1 ", 9) && strncmp(text, "HTTP/1.0 ", 9)) ||
        !isdigit((unsigned char)text[9]) || !isdigit((unsigned char)text[10]) ||
        !isdigit((unsigned char)text[11]) || text[12] != ' ') {
        goto done;
    }
    r->status = atoi(text + 9);
    for (;;) {
        if (line(&s, text, sizeof(text), &budget)) {
            goto done;
        }
        if (!*text) {
            break;
        }

        char *value = strchr(text, ':');
        if (!value || *text == ' ' || *text == '\t') {
            goto done;
        }
        *value++ = 0;
        while (*value == ' ' || *value == '\t') {
            value++;
        }

        char *end = value + strlen(value);
        while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = 0;
        }
        if (!strcasecmp(text, "Content-Length")) {
            if (have_length++ || integer(value, 10, &length) || length > ACME_BODY) {
                goto done;
            }
        } else if (!strcasecmp(text, "Transfer-Encoding")) {
            if (chunked++ || strcasecmp(value, "chunked")) {
                goto done;
            }
        } else if (!strcasecmp(text, "Content-Encoding") &&
                   strcasecmp(value, "identity")) {
            goto done;
        } else if (!strcasecmp(text, "Replay-Nonce")) {
            if (*r->nonce || strlen(value) >= sizeof(r->nonce)) {
                goto done;
            }
            strcpy(r->nonce, value);
        } else if (!strcasecmp(text, "Location")) {
            if (*r->location || strlen(value) >= sizeof(r->location)) {
                goto done;
            }
            strcpy(r->location, value);
        } else if (!strcasecmp(text, "Retry-After")) {
            unsigned long retry;
            if (!integer(value, 10, &retry)) {
                r->retry = retry > 10 ? 10 : retry < 1 ? 1 : (int)retry;
            }
        }
    }
    if (have_length && chunked) {
        goto done;
    }
    r->body = malloc(ACME_BODY + 1);
    if (!r->body) {
        goto done;
    }
    if (!strcmp(method, "HEAD") || r->status == 204) {
        r->size = 0;
    } else if (have_length) {
        if (exact(&s, r->body, length)) {
            goto done;
        }
        r->size = length;
    } else if (chunked) {
        for (;;) {
            budget = 8192;
            if (line(&s, text, sizeof(text), &budget)) {
                goto done;
            }

            char *extension = strchr(text, ';');
            if (extension) {
                *extension = 0;
            }

            unsigned long chunk;
            if (integer(text, 16, &chunk) || chunk > ACME_BODY - r->size) {
                goto done;
            }
            if (!chunk) {
                budget = 65536;
                do {
                    if (line(&s, text, sizeof(text), &budget)) {
                        goto done;
                    }
                } while (*text);
                break;
            }
            if (exact(&s, r->body + r->size, chunk) || byte(&s) != '\r' ||
                byte(&s) != '\n') {
                goto done;
            }
            r->size += chunk;
        }
    } else {
        fputs("acme: response needs Content-Length or chunked framing\n", stderr);
        goto done;
    }
    r->body[r->size] = 0;
    if (memchr(r->body, 0, r->size)) {
        goto done;
    }
    result = 0;
done:
    if (initialized) {
        tls_free(&s.tls);
    } else if (s.fd >= 0) {
        close(s.fd);
    }
    if (result) {
        acme_http_free(r);
        fputs("acme: HTTP transport failed\n", stderr);
    }
    return result;
}
