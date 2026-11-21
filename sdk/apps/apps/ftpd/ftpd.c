// ftpd.c: Пассивный FTP-сервер
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Подсистема: Пассивный FTP-сервер в пользовательском пространстве
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 20.11.2026
// Доступ: общедоступный

// FTP по IPv4, пассивная передача для аккаунтов администратора
// RFC 959 и EPSV из RFC 2428, без анонимного входа и TLS
#include "../common/service_log.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pwd.h>
#include <shadow.h>
#include <crypt.h>
#include <dirent.h>
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>

static int first_port = 40000, last_port = 40009;
static int ready(int fd, short events, int seconds) {
    struct pollfd p = {fd, events, 0};
    int n;
    do {
        n = poll(&p, 1, seconds * 1000);
    } while (n < 0 && errno == EINTR);
    return n > 0 && (p.revents & (events | POLLHUP));
}
static int send_all(int fd, const char *buf, size_t len) {
    while (len) {
        if (!ready(fd, POLLOUT, 60)) {
            return -1;
        }
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        buf += n;
        len -= n;
    }
    return 0;
}
static int reply(int fd, int code, const char *fmt, ...) {
    char text[4096], line[4200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    int n = snprintf(line, sizeof(line), "%d %s\r\n", code, text);
    return send_all(fd, line, n);
}
static int listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_port = htons(port),
                               .sin_addr = {.s_addr = INADDR_ANY}};
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 8)) {
        close(fd);
        return -1;
    }
    return fd;
}
static int read_line(int fd, char *buf, size_t cap) {
    size_t len = 0;
    int overflow = 0;
    for (;;) {
        if (!ready(fd, POLLIN, 300)) {
            return -1;
        }

        unsigned char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        if (c == '\n') {
            break;
        }
        if (c == '\r') {
            continue;
        }
        if (c < 32 || c == 127 || len >= cap - 1) {
            overflow = 1;
        } else {
            buf[len++] = c;
        }
    }
    buf[len] = 0;
    return overflow ? -2 : (int)len;
}
static int authenticate(const char *user, const char *password) {
    struct passwd *pw = getpwnam(user);
    // Изоляция UID неполная, разрешаем только аккаунты администратора
    if (!pw || pw->pw_uid != 0 || !*password) {
        return 0;
    }
    const char *hash = pw->pw_passwd;
    if (!strcmp(hash, "x")) {
        struct spwd *sp = getspnam(user);
        if (!sp) {
            return 0;
        }
        hash = sp->sp_pwdp;
    }
    if (!hash || hash[0] != '\x24') {
        return 0;
    }

    char *result = crypt(password, hash);
    if (!result) {
        return 0;
    }

    size_t len = strlen(hash);
    if (strlen(result) != len) {
        return 0;
    }

    unsigned char diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (unsigned char)(result[i] ^ hash[i]);
    }
    return diff == 0;
}
static int data_accept(int passive, struct in_addr peer) {
    if (passive < 0 || !ready(passive, POLLIN, 10)) {
        return -1;
    }

    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = accept(passive, (struct sockaddr *)&addr, &len);
    if (fd >= 0 && addr.sin_addr.s_addr != peer.s_addr) {
        close(fd);
        return -1;
    }
    return fd;
}
static int list_entry(int fd, const char *name, const char *path, int names) {
    if (strchr(name, '\r') || strchr(name, '\n')) {
        return 0;
    }

    char line[4600];
    int n;
    if (names) {
        n = snprintf(line, sizeof(line), "%s\r\n", name);
    } else {
        struct stat st;
        if (lstat(path, &st)) {
            return -1;
        }

        char mode[11] = "----------";
        if (S_ISDIR(st.st_mode)) {
            mode[0] = 'd';
        } else if (S_ISLNK(st.st_mode)) {
            mode[0] = 'l';
        }

        for (int i = 0; i < 9; i++) {
            if (st.st_mode & (1 << (8 - i))) {
                mode[i + 1] = "rwx"[i % 3];
            }
        }

        struct tm tm;
        gmtime_r(&st.st_mtime, &tm);
        char date[32];
        strftime(date, sizeof(date), "%b %d %H:%M", &tm);
        n = snprintf(line, sizeof(line), "%s 1 0 0 %lld %s %s\r\n", mode,
                     (long long)st.st_size, date, name);
    }
    return n < 0 || n >= (int)sizeof(line) ? -1 : send_all(fd, line, n);
}
static int listing(int data, const char *path, int names) {
    struct stat st;
    if (stat(path, &st)) {
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        const char *name = strrchr(path, '/');
        return list_entry(data, name ? name + 1 : path, path, names);
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }

    struct dirent *entry;
    int bad = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }

        char full[4096];
        int n = snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        if (n < 0 || n >= (int)sizeof(full) ||
            list_entry(data, entry->d_name, full, names)) {
            bad = 1;
            break;
        }
    }
    closedir(dir);
    return bad ? -1 : 0;
}
static int transfer(int data, int file, int upload, int ascii) {
    char buf[8192], converted[16384];
    ssize_t n;
    int pending_cr = 0, prev_cr = 0;
    for (;;) {
        if (upload && !ready(data, POLLIN, 60)) {
            return -1;
        }
        n = upload ? recv(data, buf, sizeof(buf), 0) : read(file, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            return -1;
        }
        if (!n) {
            break;
        }
        const char *out = buf;
        size_t len = n;
        if (ascii) {
            len = 0;
            out = converted;
            for (ssize_t i = 0; i < n; i++) {
                char c = buf[i];
                if (upload) {
                    if (pending_cr) {
                        if (c != '\n') {
                            converted[len++] = '\r';
                        }
                        pending_cr = 0;
                    }
                    if (c == '\r') {
                        pending_cr = 1;
                    } else {
                        converted[len++] = c;
                    }
                } else {
                    if (c == '\n' && !prev_cr) {
                        converted[len++] = '\r';
                    }
                    converted[len++] = c;
                    prev_cr = c == '\r';
                }
            }
        }
        if (upload ? log_write_all(file, out, len) : send_all(data, out, len)) {
            return -1;
        }
    }
    if (upload && pending_cr && log_write_all(file, "\r", 1)) {
        return -1;
    }
    return 0;
}
// Собрать канонический абсолютный путь из рабочего каталога и аргумента команды
static void ftp_resolve(const char *cwd, const char *arg, char *out, size_t cap) {
    char combined[8192];
    if (arg[0] == '/') {
        (void)snprintf(combined, sizeof(combined), "%s", arg);
    } else {
        (void)snprintf(combined, sizeof(combined), "%s/%s", cwd, arg);
    }

    size_t used = 0;
    out[used++] = '/';
    out[used] = 0;
    char *save = NULL;
    for (char *part = strtok_r(combined, "/", &save); part != NULL;
         part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".") || !*part) {
            continue;
        }

        if (!strcmp(part, "..")) {
            if (used > 1) {
                used--;
                while (used > 1 && out[used - 1] != '/') {
                    used--;
                }
                if (used > 1 && out[used - 1] == '/') {
                    used--;
                }
                out[used] = 0;
            }
            continue;
        }

        size_t need = strlen(part) + 2U;
        if (used + need >= cap) {
            break;
        }

        if (used > 1) {
            out[used++] = '/';
        }
        strcpy(out + used, part);
        used += strlen(part);
        out[used] = 0;
    }
}

// Запись запрещена в системные и загрузочные каталоги, чтение не ограничено
static bool ftp_write_allowed(const char *path) {
    static const char *const denied[] = {
        "/boot", "/etc", "/bin", "/sbin", "/usr", "/lib", "/lib64", "/lib32",
        "/libexec", "/proc", "/sys", "/dev", "/var", "/root", "/home", "/run",
        "/mnt", "/media", "/opt", "/srv", "/lost+found", NULL
    };

    for (size_t i = 0; denied[i] != NULL; i++) {
        size_t n = strlen(denied[i]);
        if (!strncmp(path, denied[i], n) && (path[n] == '/' || path[n] == '\0')) {
            return false;
        }
    }
    return true;
}

// Ответить отказом, если команда записи целится в запрещённый каталог
static int ftp_write_guard(int control, const char *arg) {
    char cwd[4096], resolved[8192];
    if (!getcwd(cwd, sizeof(cwd))) {
        reply(control, 550, "Cannot get directory");
        return -1;
    }

    ftp_resolve(cwd, arg, resolved, sizeof(resolved));
    if (!ftp_write_allowed(resolved)) {
        reply(control, 550, "Permission denied");
        return -1;
    }
    return 0;
}

static void session(int control, struct sockaddr_in peer) {
    int logged = 0, passive = -1, ascii = 1, failures = 0;
    off_t restart = 0;
    char user[64] = "", line[4096];
    reply(control, 220, "Lumine FTP ready");
    for (;;) {
        int n = read_line(control, line, sizeof(line));
        if (n == -1) {
            break;
        }
        if (n == -2) {
            reply(control, 500, "Command too long or invalid");
            continue;
        }

        char *arg = strchr(line, ' ');
        if (arg) {
            *arg++ = 0;
        } else {
            arg = line + strlen(line);
        }

        for (char *p = line; *p; p++) {
            *p = toupper((unsigned char)*p);
        }
        if (!strcmp(line, "QUIT")) {
            reply(control, 221, "Goodbye");
            break;
        }
        if (!strcmp(line, "USER")) {
            logged = 0;
            user[0] = 0;
            restart = 0;
            if (passive >= 0) {
                close(passive);
                passive = -1;
            }

            size_t len = strlen(arg);
            int valid = len && len < sizeof(user);
            for (size_t i = 0; i < len; i++) {
                if (!isalnum((unsigned char)arg[i]) && arg[i] != '_' && arg[i] != '-' &&
                    arg[i] != '.') {
                    valid = 0;
                }
            }
            if (valid) {
                strcpy(user, arg);
            }
            reply(control, 331, "Password required");
            continue;
        }
        if (!strcmp(line, "PASS")) {
            logged = authenticate(user, arg);
            memset(arg, 0, strlen(arg));
            fprintf(stderr, "[ftpd] login %s for %s\n",
                    logged ? "accepted" : "rejected", user);
            if (logged) {
                chdir("/");
                reply(control, 230, "Logged in");
            } else {
                usleep(500000);
                reply(control, 530, "Login incorrect");
                if (++failures >= 3) {
                    break;
                }
            }
            continue;
        }
        if (!strcmp(line, "SYST")) {
            reply(control, 215, "UNIX Type: L8");
            continue;
        }
        if (!strcmp(line, "FEAT")) {
            const char features[] =
                "211-Features\r\n EPSV\r\n PASV\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n UTF8\r\n211 End\r\n";
            send_all(control, features, sizeof(features) - 1);
            continue;
        }
        if (!strcmp(line, "NOOP")) {
            reply(control, 200, "OK");
            continue;
        }
        if (!logged) {
            reply(control, 530, "Log in first");
            continue;
        }
        if (!strcmp(line, "OPTS")) {
            reply(control, !strcasecmp(arg, "UTF8 ON") ? 200 : 501, "UTF8 is enabled");
            continue;
        }
        if (!strcmp(line, "TYPE")) {
            if (!strcmp(arg, "I") || !strcmp(arg, "A")) {
                ascii = *arg == 'A';
                reply(control, 200, "Type set");
            } else {
                reply(control, 504, "Use TYPE I or TYPE A");
            }
        } else if (!strcmp(line, "PWD") || !strcmp(line, "XPWD")) {
            char cwd[4096], quoted[4096];
            if (!getcwd(cwd, sizeof(cwd))) {
                reply(control, 550, "Cannot get directory");
                continue;
            }

            size_t j = 0;
            for (size_t i = 0; cwd[i] && j < sizeof(quoted) - 3; i++) {
                quoted[j++] = cwd[i];
                if (cwd[i] == '"') {
                    quoted[j++] = '"';
                }
            }
            quoted[j] = 0;
            reply(control, 257, "\"%s\"", quoted);
        } else if (!strcmp(line, "CWD") || !strcmp(line, "CDUP")) {
            reply(control, chdir(!strcmp(line, "CDUP") ? ".." : arg) ? 550 : 250,
                  "Directory change");
        } else if (!strcmp(line, "PASV") || !strcmp(line, "EPSV")) {
            if (passive >= 0) {
                close(passive);
            }
            passive = -1;
            int port;
            for (port = first_port; port <= last_port; port++) {
                if ((passive = listener(port)) >= 0) {
                    break;
                }
            }
            if (passive < 0) {
                reply(control, 425, "No passive port available");
                continue;
            }
            if (!strcmp(line, "EPSV")) {
                reply(control, 229, "Entering Extended Passive Mode (|||%d|)", port);
            } else {
                struct sockaddr_in local;
                socklen_t len = sizeof(local);
                if (getsockname(control, (struct sockaddr *)&local, &len)) {
                    close(passive);
                    passive = -1;
                    reply(control, 425, "No local address");
                    continue;
                }

                unsigned char *ip = (unsigned char *)&local.sin_addr.s_addr;
                reply(control, 227, "Entering Passive Mode (%u,%u,%u,%u,%d,%d)", ip[0],
                      ip[1], ip[2], ip[3], port / 256, port % 256);
            }
        } else if (!strcmp(line, "REST")) {
            char *end;
            long long offset = strtoll(arg, &end, 10);
            if (!*arg || *end || offset < 0) {
                reply(control, 501, "Invalid offset");
            } else {
                restart = offset;
                reply(control, 350, "Restart position accepted");
            }
        } else if (!strcmp(line, "SIZE") || !strcmp(line, "MDTM")) {
            struct stat st;
            if (stat(arg, &st) || !S_ISREG(st.st_mode)) {
                reply(control, 550, "Not a regular file");
                continue;
            }
            if (!strcmp(line, "SIZE")) {
                reply(control, 213, "%lld", (long long)st.st_size);
            } else {
                struct tm tm;
                char date[32];
                gmtime_r(&st.st_mtime, &tm);
                strftime(date, sizeof(date), "%Y%m%d%H%M%S", &tm);
                reply(control, 213, "%s", date);
            }
        } else if (!strcmp(line, "MKD")) {
            if (ftp_write_guard(control, arg) == 0) {
                reply(control, mkdir(arg, 0755) ? 550 : 257, "Directory creation");
            }
        } else if (!strcmp(line, "RMD")) {
            if (ftp_write_guard(control, arg) == 0) {
                reply(control, rmdir(arg) ? 550 : 250, "Directory removal");
            }
        } else if (!strcmp(line, "DELE")) {
            struct stat st;
            if (ftp_write_guard(control, arg) != 0) {
                continue;
            }
            reply(control,
                  lstat(arg, &st) || S_ISDIR(st.st_mode) || unlink(arg) ? 550 : 250,
                  "File removal");
        } else if (!strcmp(line, "LIST") || !strcmp(line, "NLST") ||
                   !strcmp(line, "RETR") || !strcmp(line, "STOR") ||
                   !strcmp(line, "APPE")) {
            int list = !strcmp(line, "LIST") || !strcmp(line, "NLST");
            int upload = !strcmp(line, "STOR") || !strcmp(line, "APPE");
            int file = -1;
            off_t offset = restart;
            restart = 0;
            if (passive < 0) {
                reply(control, 425, "Use PASV or EPSV first");
                continue;
            }
            if (!list) {
                if (upload && ftp_write_guard(control, arg) != 0) {
                    close(passive);
                    passive = -1;
                    continue;
                }

                struct stat st;
                if (stat(arg, &st) == 0) {
                    if (!S_ISREG(st.st_mode)) {
                        reply(control, 550, "Not a regular file");
                        close(passive);
                        passive = -1;
                        continue;
                    }
                } else if (errno != ENOENT || !upload) {
                    reply(control, 550, "File unavailable");
                    close(passive);
                    passive = -1;
                    continue;
                }

                int flags = upload ? O_WRONLY | O_CREAT : O_RDONLY;
                if (upload && !offset && strcmp(line, "APPE")) {
                    flags |= O_TRUNC;
                }
                file = open(arg, flags, 0644);
                if (file < 0 || (offset && lseek(file, offset, SEEK_SET) < 0) ||
                    (!strcmp(line, "APPE") && lseek(file, 0, SEEK_END) < 0)) {
                    if (file >= 0) {
                        close(file);
                    }
                    close(passive);
                    passive = -1;
                    reply(control, 550, "Cannot open file");
                    continue;
                }
            }
            reply(control, 150, "Opening data connection");
            int data = data_accept(passive, peer.sin_addr);
            close(passive);
            passive = -1;
            if (data < 0) {
                if (file >= 0) {
                    close(file);
                }
                reply(control, 425, "Data connection failed");
                continue;
            }

            int bad = list ? listing(data, *arg && *arg != '-' ? arg : ".",
                                     !strcmp(line, "NLST"))
                           : transfer(data, file, upload, ascii);
            close(data);
            if (file >= 0 && close(file)) {
                bad = 1;
            }
            reply(control, bad ? 451 : 226,
                  bad ? "Transfer failed" : "Transfer complete");
            fprintf(stderr, "[ftpd] %s %s: %s\n", line, arg,
                    bad ? "failed" : "complete");
        } else if (!strcmp(line, "ABOR")) {
            if (passive >= 0) {
                close(passive);
            }
            passive = -1;
            restart = 0;
            reply(control, 226, "No transfer active");
        } else {
            reply(control, 502, "Command not implemented");
        }
    }
    if (passive >= 0) {
        close(passive);
    }
    close(control);
    _exit(0);
}
// Отследить пару pid и адрес пира для ограничения числа сессий с одного источника
struct ftpd_child {
    pid_t pid;
    uint32_t addr;
};

static size_t ftpd_child_forget(struct ftpd_child *children, size_t count, pid_t pid) {
    for (size_t i = 0; i < count; i++) {
        if (children[i].pid == pid) {
            children[i] = children[count - 1U];
            return count - 1U;
        }
    }
    return count;
}

int main(int argc, char **argv) {
    int port = 21;
    for (int i = 1; i < argc; i++) {
        char *end;
        long value;
        if (i + 1 >= argc) {
            return 2;
        }
        value = strtol(argv[++i], &end, 10);
        if (*end || value < 1 || value > 65535) {
            return 2;
        }
        if (!strcmp(argv[i - 1], "-p")) {
            port = value;
        } else if (!strcmp(argv[i - 1], "--passive-min")) {
            first_port = value;
        } else if (!strcmp(argv[i - 1], "--passive-max")) {
            last_port = value;
        } else {
            return 2;
        }
    }
    if (first_port > last_port) {
        return 2;
    }
    service_log_start("ftp", 0);
    signal(SIGPIPE, SIG_IGN);
    int server = listener(port);
    if (server < 0) {
        perror("[ftpd] listen");
        return 1;
    }
    fprintf(stderr, "[ftpd] listening on port %d, passive %d-%d\n", port, first_port,
            last_port);
    int children = 0;
    struct ftpd_child peers[16];
    size_t peer_count = 0;
    for (;;) {
        pid_t reaped;
        while ((reaped = waitpid(-1, NULL, WNOHANG)) > 0) {
            if (children) {
                children--;
            }
            peer_count = ftpd_child_forget(peers, peer_count, reaped);
        }
        if (!ready(server, POLLIN, 1)) {
            continue;
        }

        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        int control = accept(server, (struct sockaddr *)&peer, &len);
        if (control < 0) {
            continue;
        }
        if (children >= 8) {
            reply(control, 421, "Too many sessions");
            close(control);
            continue;
        }

        unsigned same_ip = 0;
        for (size_t i = 0; i < peer_count; i++) {
            if (peers[i].addr == peer.sin_addr.s_addr) {
                same_ip++;
            }
        }
        if (same_ip >= 3) {
            reply(control, 421, "Too many sessions from this address");
            close(control);
            continue;
        }

        pid_t pid = fork();
        if (pid == 0) {
            close(server);
            session(control, peer);
        }
        if (pid < 0) {
            reply(control, 421, "Cannot start session");
        } else {
            children++;
            if (peer_count < sizeof(peers) / sizeof(peers[0])) {
                peers[peer_count].pid = pid;
                peers[peer_count].addr = peer.sin_addr.s_addr;
                peer_count++;
            }
        }
        close(control);
    }
}
