// service.c: Управление службами и перезапуск дочерних процессов
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Супервизор хранит состояние в var/run и принимает команды через файлы
// Подсистема: запуск и обслуживание пользовательских служб
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 25.10.2026
// Доступ: общедоступный

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <poll.h>
#include <netinet/in.h>
#include <stdint.h>

// Команды и состояние службы передаются через обычные файлы
static char runbase[512], confbase[512], logbase[512];
static int read_definition;
static unsigned health_port;
static char health_path[128];
static int valid(const char *s) {
    if (!*s || strlen(s) > 63) {
        return 0;
    }
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_') {
            return 0;
        }
    }
    return 1;
}
static void paths(const char *name, const char *leaf, char *out, size_t n) {
    snprintf(out, n, "%s/%.63s/%s", runbase, name, leaf);
}
// Сроки ожидания считаем по монотонным часам, чтобы коррекция NTP их не меняла
static time_t ticks(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return ts.tv_sec;
    }
    return time(NULL);
}
static time_t heartbeat_age(long stamp) {
    // Старые супервизоры записывали календарное время, принимаем его до перезапуска
    return (stamp >= 1000000000L ? time(NULL) : ticks()) - stamp;
}

// Получить монотонное время для общего срока сетевой проверки
static int64_t health_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

// Дождаться готовности сокета в пределах общего срока проверки
static int health_wait(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t now = health_milliseconds();
        if (now < 0 || now >= deadline) {
            return 0;
        }
        struct pollfd wait = {.fd = fd, .events = events};
        int result = poll(&wait, 1, (int)(deadline - now));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return result > 0 && (wait.revents & (events | POLLERR | POLLHUP)) != 0;
    }
}

// Проверить ответ службы через loopback без бесконечного ожидания
static int health_http_probe(void) {
    int64_t started = health_milliseconds();
    if (started < 0) {
        return 0;
    }
    int64_t deadline = started + 1500;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return 0;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)health_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int connected = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (connected < 0 && errno == EINPROGRESS) {
        int error = 0;
        socklen_t length = sizeof(error);
        connected = health_wait(fd, POLLOUT, deadline) &&
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0 ? 0 : -1;
    }
    if (connected < 0) {
        close(fd);
        return 0;
    }

    char request[256];
    int size = snprintf(request, sizeof(request),
                        "GET %s HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n", health_path);
    if (size <= 0 || (size_t)size >= sizeof(request)) {
        close(fd);
        return 0;
    }

    size_t written = 0;
    while (written < (size_t)size) {
        if (!health_wait(fd, POLLOUT, deadline)) {
            close(fd);
            return 0;
        }
        ssize_t sent = send(fd, request + written, (size_t)size - written, MSG_NOSIGNAL);
        if (sent < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        }
        if (sent <= 0) {
            close(fd);
            return 0;
        }
        written += (size_t)sent;
    }

    char response[32];
    size_t received = 0;
    while (received < 12) {
        if (!health_wait(fd, POLLIN, deadline)) {
            close(fd);
            return 0;
        }
        ssize_t count = recv(fd, response + received, sizeof(response) - received, 0);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        }
        if (count <= 0) {
            close(fd);
            return 0;
        }
        received += (size_t)count;
    }
    close(fd);
    if (memcmp(response, "HTTP/1.", 7) != 0 || response[8] != ' ' ||
        !isdigit((unsigned char)response[9]) || !isdigit((unsigned char)response[10]) ||
        !isdigit((unsigned char)response[11])) {
        return -1;
    }

    return (response[9] - '0') * 100 + (response[10] - '0') * 10 + response[11] - '0';
}
static int heartbeat_alive(const char *name) {
    char path[768];
    long stamp, pid;
    paths(name, "status", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    int ok = fscanf(f, "%ld %ld", &stamp, &pid) == 2;
    fclose(f);
    if (!ok || pid <= 0) {
        return 0;
    }

    if (kill((pid_t)pid, 0) < 0 && errno == ESRCH) {
        return 0;
    }

    time_t age = heartbeat_age(stamp);
    return ok && age >= 0 && age <= 30;
}
static void publish(const char *name, pid_t child, const char *state, int desired,
                    int delay) {
    char path[768], temp[800];
    paths(name, "status", path, sizeof(path));
    snprintf(temp, sizeof(temp), "%s.%ld", path, (long)getpid());
    FILE *f = fopen(temp, "w");
    if (!f) {
        return;
    }
    fprintf(f, "%ld %ld %ld %s %d %d\n", (long)ticks(), (long)getpid(), (long)child,
            state, desired, delay);
    if (fclose(f) == 0) {
        rename(temp, path);
    } else {
        unlink(temp);
    }
}
static int status(const char *name, int display) {
    char path[768], state[32];
    long stamp, pid, child;
    int wanted, delay;
    paths(name, "status", path, sizeof(path));
    FILE *f = fopen(path, "r");
    int ok = f && fscanf(f, "%ld %ld %ld %31s %d %d", &stamp, &pid, &child, state,
                         &wanted, &delay) == 6;
    if (f) {
        fclose(f);
    }
    if (!ok || heartbeat_age(stamp) < 0 || heartbeat_age(stamp) > 30) {
        if (display) {
            printf("%-16s stopped (no supervisor)\n", name);
        }
        return 3;
    }
    if (display) {
        printf("%-16s %-9s pid=%ld enabled=%s retry=%ds\n", name, state, child,
               wanted ? "yes" : "no", delay);
    }
    return child > 0 ? 0 : 3;
}
// Разбираем строку exec сами: иначе sh создаст промежуточный процесс
static int definition(const char *name, char **args, char *buffer, size_t capacity) {
    char path[768];
    health_port = 0;
    health_path[0] = '\0';
    snprintf(path, sizeof(path), "%s/%s", confbase, name);
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    int found = 0;
    while (fgets(buffer, capacity, f)) {
        char *p = buffer;
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (!*p || *p == '#') {
            continue;
        }
        if (strncmp(p, "exec ", 5)) {
            fclose(f);
            errno = EINVAL;
            return -1;
        }
        memmove(buffer, p + 5, strlen(p + 5) + 1);
        found = 1;
        break;
    }
    char extra[4096];
    // Необязательная строка после exec: health http PORT PATH
    while (fgets(extra, sizeof(extra), f)) {
        char *p = extra;
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (!strncmp(p, "health http ", 12) && health_port == 0) {
            char *number = p + 12;
            char *end = NULL;
            errno = 0;
            unsigned long port = strtoul(number, &end, 10);
            if (!isdigit((unsigned char)*number) || errno != 0 || port == 0 || port > 65535 ||
                !isspace((unsigned char)*end)) {
                fclose(f);
                errno = EINVAL;
                return -1;
            }

            while (isspace((unsigned char)*end)) {
                end++;
            }
            size_t length = strcspn(end, "\r\n");
            while (length > 0 && isspace((unsigned char)end[length - 1])) {
                length--;
            }
            if (length == 0 || length >= sizeof(health_path) || end[0] != '/') {
                fclose(f);
                errno = EINVAL;
                return -1;
            }
            for (size_t index = 0; index < length; index++) {
                if ((unsigned char)end[index] <= 32 || (unsigned char)end[index] > 126) {
                    fclose(f);
                    errno = EINVAL;
                    return -1;
                }
            }
            memcpy(health_path, end, length);
            health_path[length] = '\0';
            health_port = (unsigned)port;
        } else if (*p && *p != '#') {
            fclose(f);
            errno = EINVAL;
            return -1;
        }
    }
    fclose(f);
    if (!found) {
        errno = EINVAL;
        return -1;
    }
    char *in = buffer, *out = buffer;
    int n = 0;
    while (*in) {
        while (isspace((unsigned char)*in)) {
            in++;
        }
        if (!*in) {
            break;
        }
        if (n >= 11) {
            errno = E2BIG;
            return -1;
        }
        args[n++] = out;
        int quote = 0;
        while (*in && (quote || !isspace((unsigned char)*in))) {
            char c = *in++;
            if ((c == 39 || c == '"') && (!quote || quote == c)) {
                quote = quote ? 0 : c;
                continue;
            }
            if (c == '\\' && quote != 39 && *in) {
                c = *in++;
            }
            if (!quote && strchr(";|&<>$`", c)) {
                errno = EINVAL;
                return -1;
            }
            *out++ = c;
        }
        if (quote) {
            errno = EINVAL;
            return -1;
        }
        while (isspace((unsigned char)*in)) {
            in++;
        }
        *out++ = 0;
    }
    args[n] = NULL;
    return n ? 0 : -1;
}
static void event_log(const char *name, const char *event, pid_t child, int detail) {
    char path[768];
    snprintf(path, sizeof(path), "%s/%s.log", logbase, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        return;
    }
    dprintf(fd, "[supervisor %ld] %s pid=%ld detail=%d time=%ld\n", (long)getpid(),
            event, (long)child, detail, (long)time(NULL));
    close(fd);
}

// Передать короткое событие в очередь necromant без ожидания диска
static int report_necromant_text(const char *message, size_t length) {
    int fd = open("/dev/necro", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    ssize_t written = write(fd, message, length);
    int write_error = errno;
    close(fd);
    if (written != (ssize_t)length) {
        errno = written < 0 ? write_error : EIO;
        return -1;
    }

    return 0;
}

// Передать отдельный сбой службы в очередь necromant
static void report_necromant(const char *kind, const char *name, int detail) {
    char message[96];
    int length = snprintf(message, sizeof(message), "%s service %s code=%d", kind, name, detail);
    if (length < 3 || (size_t)length >= sizeof(message)) {
        return;
    }

    if (report_necromant_text(message, (size_t)length) < 0) {
        event_log(name, "necro-report-failed", 0, errno);
    }
}

// Перезапускать службу и изолировать её после пяти быстрых отказов
static int supervise(const char *name, char **argv, int once) {
    char dir[768], req[768], disabled[768], lock[800];
    snprintf(dir, sizeof(dir), "%s/%s", runbase, name);
    mkdir(dir, 0700);
    paths(name, "owner", lock, sizeof(lock));
    // Заменяем старый маркер владельца только после истечения heartbeat
    int fd = open(lock, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0 && errno == EEXIST) {
        struct stat st;
        if (heartbeat_alive(name)) {
            return 0;
        }
        // При запуске статус ещё не записан, маркер не даёт создать второго владельца
        char statefile[768];
        paths(name, "status", statefile, sizeof(statefile));
        if (access(statefile, F_OK) && !stat(lock, &st) &&
            time(NULL) - st.st_mtime <= 30) {
            return 0;
        }
        unlink(lock);
        fd = open(lock, O_CREAT | O_EXCL | O_WRONLY, 0600);
    }
    if (fd < 0) {
        return 1;
    }
    close(fd);
    snprintf(disabled, sizeof(disabled), "%s/%s.disabled", confbase, name);
    paths(name, "request", req, sizeof(req));
    int wanted = access(disabled, F_OK) != 0, backoff = 1;
    pid_t child = 0;
    time_t retry = 0, started = 0, last_publish = 0;
    pid_t last_child = -1;
    int last_wanted = -1, last_delay = -1;
    int incident_open = 0;
    int quick_failures = 0;
    int quarantined = 0;
    int health_failures = 0;
    int health_healthy = 0;
    time_t next_health = 0;
    for (;;) {
        FILE *f = fopen(req, "r");
        char action[32] = "";
        if (f) {
            fgets(action, sizeof(action), f);
            fclose(f);
            unlink(req);
            action[strcspn(action, "\r\n")] = 0;
            if (!strcmp(action, "stop") || !strcmp(action, "disable")) {
                wanted = 0;
            } else if (!strcmp(action, "start") || !strcmp(action, "restart") ||
                       !strcmp(action, "enable")) {
                wanted = 1;
                quick_failures = 0;
                quarantined = 0;
            }
            if (child && (!wanted || !strcmp(action, "restart"))) {
                event_log(name, action, child, SIGTERM);
                kill(child, SIGTERM);
                time_t deadline = ticks() + 3;
                int reaped = 0;
                while (ticks() < deadline) {
                    pid_t r = waitpid(child, NULL, WNOHANG);
                    if (r == child || (r < 0 && errno == ECHILD)) {
                        reaped = 1;
                        break;
                    }
                    usleep(100000);
                }
                if (!reaped) {
                    kill(child, SIGKILL);
                    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
                    }
                }
                child = 0;
            }
            retry = 0;
            backoff = 1;
        }
        if (child) {
            int exit_status = 0;
            int result = waitpid(child, &exit_status, WNOHANG);
            if (result == child || (result < 0 && errno == ECHILD)) {
                int completed = result == child && WIFEXITED(exit_status) &&
                                WEXITSTATUS(exit_status) == 0;
                event_log(name, "exited", child,
                          WIFEXITED(exit_status)     ? WEXITSTATUS(exit_status)
                          : WIFSIGNALED(exit_status) ? 128 + WTERMSIG(exit_status)
                                                     : exit_status);
                child = 0;
                if (once && completed) {
                    publish(name, 0, "completed", 0, 0);
                    unlink(lock);
                    return 0;
                }
                if (wanted) {
                    int code = WIFEXITED(exit_status) ? WEXITSTATUS(exit_status)
                        : WIFSIGNALED(exit_status) ? 128 + WTERMSIG(exit_status) : -1;
                    report_necromant("F", name, code);
                    incident_open = 1;
                    if (ticks() - started < 10) {
                        quick_failures++;
                    } else {
                        quick_failures = 0;
                    }
                    if (quick_failures >= 5) {
                        wanted = 0;
                        quarantined = 1;
                        event_log(name, "quarantined", 0, quick_failures);
                        char report[96];
                        int length = snprintf(report, sizeof(report),
                                              "F service %s quarantined after %d failures",
                                              name, quick_failures);
                        if (length > 0 && (size_t)length < sizeof(report)) {
                            report_necromant_text(report, (size_t)length);
                        }
                    }
                }
                if (ticks() - started >= 30) {
                    backoff = 1;
                }
                retry = ticks() + backoff;
                backoff = backoff < 32 ? backoff * 2 : 60;
            }
        }
        if (child && wanted && health_port != 0 && !once && ticks() >= next_health) {
            int response = health_http_probe();
            next_health = ticks() + 5;
            if (response >= 200 && response < 400) {
                health_failures = 0;
                health_healthy = 1;
            } else {
                health_healthy = 0;
                health_failures++;
                if (health_failures >= 3) {
                    report_necromant("F", name, response);
                    event_log(name, "health-failed", child, response);
                    incident_open = 1;
                    kill(child, SIGTERM);
                    time_t deadline = ticks() + 3;
                    int reaped = 0;
                    while (ticks() < deadline) {
                        pid_t result = waitpid(child, NULL, WNOHANG);
                        if (result == child || (result < 0 && errno == ECHILD)) {
                            reaped = 1;
                            break;
                        }
                        usleep(100000);
                    }
                    if (!reaped) {
                        kill(child, SIGKILL);
                        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
                        }
                    }
                    child = 0;
                    health_failures = 0;
                    retry = ticks() + backoff;
                    backoff = backoff < 32 ? backoff * 2 : 60;
                }
            }
        }
        if (wanted && !child && ticks() >= retry) {
            child = fork();
            started = ticks();
            next_health = started + 5;
            health_failures = 0;
            health_healthy = 0;
            if (child == 0) {
                setsid();
                char log[768];
                snprintf(log, sizeof(log), "%s/%s.log", logbase, name);
                int input = open("/dev/null", O_RDONLY),
                    output = open(log, O_WRONLY | O_CREAT | O_APPEND, 0600);
                if (input >= 0) {
                    dup2(input, 0);
                    if (input > 2) {
                        close(input);
                    }
                }
                if (output >= 0) {
                    dup2(output, 1);
                    dup2(output, 2);
                    if (output > 2) {
                        close(output);
                    }
                }
                // Служба уже пишет в открытый журнал, внутренний service_log_start
                // должен пропустить создание второго читателя
                setenv("LUMINE_LOG_SERVICE", name, 1);
                if (read_definition) {
                    char buffer[4096], *fresh[12];
                    if (definition(name, fresh, buffer, sizeof(buffer))) {
                        perror("service definition");
                        _exit(127);
                    }
                    execvp(fresh[0], fresh);
                    perror(fresh[0]);
                } else {
                    execvp(argv[0], argv);
                    perror(argv[0]);
                }
                _exit(127);
            }
            if (child < 0) {
                int fault = errno;
                event_log(name, "fork-failed", 0, fault);
                report_necromant("F", name, fault);
                incident_open = 1;
                child = 0;
                retry = ticks() + backoff;
                backoff = backoff < 32 ? backoff * 2 : 60;
            } else {
                event_log(name, "started", child, 0);
            }
        }
        if (incident_open && child > 0 && ticks() - started >= 5 &&
            (health_port == 0 || health_healthy)) {
            report_necromant("R", name, 0);
            incident_open = 0;
        }
        int delay = retry > ticks() ? (int)(retry - ticks()) : 0;
        if (child != last_child || wanted != last_wanted ||
            (delay > 0 && last_delay == 0) || ticks() - last_publish >= 10) {
            publish(name, child,
                    child    ? "running"
                    : wanted ? "backoff"
                    : quarantined ? "quarantined"
                             : "stopped",
                    wanted, delay);
            last_publish = ticks();
            last_child = child;
            last_wanted = wanted;
            last_delay = delay;
        }
        sleep(1);
    }
}
static void detach_supervisor(void) {
    setsid();
    // MAX_FDS в SynapseOS равен 256, закрываем также запасной диапазон дескрипторов
    for (int fd = 0; fd < 512; fd++) {
        close(fd);
    }
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd >= 0) {
        for (int fd = 0; fd < 3; fd++) {
            if (fd != nullfd) {
                dup2(nullfd, fd);
            }
        }
        if (nullfd > 2) {
            close(nullfd);
        }
    }
}

static int request(const char *name, const char *action) {
    char dir[768], path[768], temp[800];
    snprintf(dir, sizeof(dir), "%s/%s", runbase, name);
    mkdir(dir, 0700);
    paths(name, "request", path, sizeof(path));
    snprintf(temp, sizeof(temp), "%s.%ld", path, (long)getpid());
    FILE *f = fopen(temp, "w");
    if (!f) {
        return 1;
    }
    fprintf(f, "%s\n", action);
    if (fclose(f) || rename(temp, path)) {
        unlink(temp);
        return 1;
    }
    return 0;
}
// Выполнить команду службы или начать наблюдение за её процессом
int main(int argc, char **argv) {
    const char *prefix = getenv("SERVICE_ROOT");
    if (!prefix) {
        prefix = "";
    }
    snprintf(runbase, sizeof(runbase), "%s/var/run/services", prefix);
    snprintf(confbase, sizeof(confbase), "%s/etc/services", prefix);
    snprintf(logbase, sizeof(logbase), "%s/var/log", prefix);
    char parent[512];
    snprintf(parent, sizeof(parent), "%s/var/run", prefix);
    mkdir(parent, 0755);
    mkdir(runbase, 0700);
    mkdir(confbase, 0755);
    mkdir(logbase, 0700);
    if (argc == 2 && !strcmp(argv[1], "list")) {
        DIR *d = opendir(runbase);
        struct dirent *e;
        if (!d) {
            return 0;
        }
        while ((e = readdir(d))) {
            if (valid(e->d_name)) {
                status(e->d_name, 1);
            }
        }
        closedir(d);
        d = opendir(confbase);
        if (!d) {
            return 0;
        }
        while ((e = readdir(d))) {
            if (valid(e->d_name)) {
                char path[768];
                paths(e->d_name, "status", path, sizeof(path));
                if (access(path, F_OK)) {
                    status(e->d_name, 1);
                }
            }
        }
        closedir(d);
        return 0;
    }
    if (argc < 3 || !valid(argv[2])) {
        fprintf(stderr, "Usage: service list | service "
                        "{start|stop|restart|status|enable|disable} NAME\n");
        return 2;
    }
    const char *action = argv[1], *name = argv[2];
    if (!strcmp(action, "supervise") || !strcmp(action, "supervise-once")) {
        int once = !strcmp(action, "supervise-once");
        if (argc >= 5 && !strcmp(argv[3], "--")) {
            return supervise(name, &argv[4], once);
        }
        char buffer[4096], *args[12];
        if (definition(name, args, buffer, sizeof(buffer))) {
            perror("service definition");
            return 1;
        }
        read_definition = 1;
        return supervise(name, args, once);
    }
    if (!strcmp(action, "status")) {
        return status(name, 1);
    }
    if (strcmp(action, "start") && strcmp(action, "stop") &&
        strcmp(action, "restart") && strcmp(action, "enable") &&
        strcmp(action, "disable")) {
        return 2;
    }
    if (!strcmp(action, "enable") || !strcmp(action, "disable")) {
        char path[768];
        snprintf(path, sizeof(path), "%s/%s.disabled", confbase, name);
        if (!strcmp(action, "disable")) {
            int fd = open(path, O_CREAT | O_WRONLY, 0600);
            if (fd < 0) {
                return 1;
            }
            close(fd);
        } else if (unlink(path) < 0 && errno != ENOENT) {
            return 1;
        }
    }
    // Для перезапуска нужен сохранённый исполняемый файл службы
    int alive = heartbeat_alive(name);
    if (!alive && strcmp(action, "stop") && strcmp(action, "disable")) {
        char buffer[4096], *args[12];
        if (definition(name, args, buffer, sizeof(buffer))) {
            perror("service definition");
            return 1;
        }
        pid_t pid = fork();
        if (pid < 0) {
            return 1;
        }
        if (pid == 0) {
            detach_supervisor();
            read_definition = 1;
            _exit(supervise(name, args, 0));
        }
    }
    if (request(name, action)) {
        return 1;
    }
    printf("%s: %s requested\n", name, action);
    return 0;
}
