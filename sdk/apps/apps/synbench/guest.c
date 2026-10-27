// guest.c: Измеритель ресурсов процесса для synbench
// Автор: Елчинян Арен Артурович, 2026, x86_64, Linux ABI
// Подсистема: пользовательские измерения Linux и SynapseOS
// Первое изменение: 19.10.2026, последнее изменение: 19.10.2026
// Доступ: общедоступный

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SOURCE_LIMIT 65536U

static volatile sig_atomic_t interrupted = 0;
static char source_buffer[SOURCE_LIMIT + 1U];

// Передать весь вывод без потери короткой записи
static bool write_all(int fd, const char *buffer, size_t length) {
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = write(fd, buffer + offset, length - offset);

        if (written < 0 && errno == EINTR) {
            continue;
        }

        if (written <= 0) {
            return false;
        }

        offset += (size_t)written;
    }

    return true;
}

// Скопировать доступный вывод команды в лог хоста и настоящую гостевую консоль
static size_t copy_output(int fd, int console_fd) {
    char buffer[4096];
    size_t total = 0;

    while (fd >= 0 && total < SOURCE_LIMIT) {
        ssize_t received = read(fd, buffer, sizeof(buffer));

        if (received < 0 && errno == EINTR) {
            continue;
        }

        if (received <= 0) {
            break;
        }

        if (!write_all(STDERR_FILENO, buffer, (size_t)received)
            || !write_all(console_fd, buffer, (size_t)received)) {
            interrupted = SIGTERM;
            break;
        }

        total += (size_t)received;
    }

    return total;
}

// Запомнить запрос остановки до завершения дочернего процесса
static void on_signal(int number) {
    interrupted = number;
}

// Прочитать монотонное время в наносекундах
static uint64_t monotonic_ns(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }

    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

// Записать произвольные байты как безопасную строку JSON
static void json_bytes(const unsigned char *value, size_t length) {
    putchar('"');
    for (size_t index = 0; index < length; index++) {
        unsigned char byte = value[index];

        if (byte == '"' || byte == '\\') {
            putchar('\\');
            putchar((int)byte);
        } else if (byte >= 32U && byte < 127U) {
            putchar((int)byte);
        } else {
            printf("\\u%04x", (unsigned)byte);
        }
    }

    putchar('"');
}

// Записать завершённую нулём строку JSON
static void json_string(const char *value) {
    json_bytes((const unsigned char *)value, strlen(value));
}

// Сохранить исходные данные счётчика и причину ошибки чтения
static void emit_source(const char *name, const char *path, bool *first) {
    if (!*first) {
        putchar(',');
    }

    *first = false;
    json_string(name);
    fputs(":{\"path\":", stdout);
    json_string(path);

    FILE *input = fopen(path, "r");
    if (input == NULL) {
        printf(",\"errno\":%d,\"raw\":null,\"truncated\":false}", errno);
        return;
    }

    size_t length = fread(source_buffer, 1U, sizeof(source_buffer), input);
    int error = ferror(input) != 0 ? EIO : 0;
    bool truncated = length > SOURCE_LIMIT;

    if (truncated) {
        length = SOURCE_LIMIT;
    }

    (void)fclose(input);
    printf(",\"errno\":%d,\"truncated\":%s,\"raw\":", error, truncated ? "true" : "false");
    json_bytes((const unsigned char *)source_buffer, length);
    putchar('}');
}

// Снять состояние ОС и одного непосредственно запущенного процесса
static void sample(pid_t child, const char *phase) {
    static const char *const sources[] = {
        "/proc/meminfo", "/proc/stat", "/proc/performance", "/proc/vmstat",
        "/proc/diskstats", "/proc/net/dev", "/proc/net/snmp", "/proc/net/netstat",
        "/proc/loadavg", "/proc/pressure/cpu", "/proc/pressure/memory", "/proc/pressure/io",
        "/proc/schedstat", "/proc/interrupts", "/proc/softirqs", "/dev/memstat",
        "/proc/thermal", "/proc/power", "/proc/hw_errors", "/proc/net-interfaces"
    };
    static const char *const process_sources[] = {"stat", "status", "io", "smaps_rollup", "schedstat"};
    bool first = true;
    uint64_t start = monotonic_ns();

    printf("{\"event\":\"sample\",\"phase\":");
    json_string(phase);
    printf(",\"monotonic_ns\":%" PRIu64 ",\"pid\":%ld,\"sources\":{", start, (long)child);

    for (size_t index = 0; index < sizeof(sources) / sizeof(sources[0]); index++) {
        emit_source(sources[index], sources[index], &first);
    }

    if (strcmp(phase, "before") == 0) {
        emit_source("/proc/cpuinfo", "/proc/cpuinfo", &first);
        emit_source("/proc/version", "/proc/version", &first);
        emit_source("/proc/cmdline", "/proc/cmdline", &first);
    }

    if (child > 0) {
        for (size_t index = 0; index < sizeof(process_sources) / sizeof(process_sources[0]); index++) {
            char path[96];
            char name[64];

            (void)snprintf(path, sizeof(path), "/proc/%ld/%s", (long)child, process_sources[index]);
            (void)snprintf(name, sizeof(name), "process/%s", process_sources[index]);
            emit_source(name, path, &first);
        }
    }

    printf("},\"collection_ns\":%" PRIu64 "}\n", monotonic_ns() - start);
    (void)fflush(stdout);
}

// Проверить положительный ограниченный числовой аргумент
static bool parse_number(const char *text, unsigned long maximum, unsigned long *result) {
    char *end = NULL;

    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0UL || value > maximum) {
        return false;
    }

    *result = value;
    return true;
}

// Записать статистику завершённого процесса без подстановки фиктивных значений SynapseOS
static void finish(int status, bool timed_out, bool linux_system, uint64_t elapsed, const struct rusage *usage) {
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int signal_number = WIFSIGNALED(status) ? WTERMSIG(status) : 0;

    printf("{\"event\":\"finish\",\"elapsed_ns\":%" PRIu64 ",\"exit_code\":%d,"
           "\"signal\":%d,\"timed_out\":%s,\"interrupted\":%d,\"rusage\":",
           elapsed, exit_code, signal_number, timed_out ? "true" : "false", (int)interrupted);

    if (linux_system) {
        printf("{\"user_us\":%jd,\"system_us\":%jd,\"max_rss_kib\":%ld,"
               "\"minor_faults\":%ld,\"major_faults\":%ld,\"input_blocks\":%ld,"
               "\"output_blocks\":%ld,\"voluntary_switches\":%ld,\"involuntary_switches\":%ld}",
               (intmax_t)usage->ru_utime.tv_sec * 1000000 + usage->ru_utime.tv_usec,
               (intmax_t)usage->ru_stime.tv_sec * 1000000 + usage->ru_stime.tv_usec,
               usage->ru_maxrss, usage->ru_minflt, usage->ru_majflt, usage->ru_inblock,
               usage->ru_oublock, usage->ru_nvcsw, usage->ru_nivcsw);
    } else {
        fputs("null,\"rusage_reason\":\"not validated for this OS\"", stdout);
    }

    fputs("}\n", stdout);
}

// Выполнить команду с периодическими снимками и общим ограничением времени
int main(int argc, char **argv) {
    unsigned long interval_ms = 100UL;
    unsigned long timeout_seconds = 300UL;
    int command_index = 1;
    const char *console_path = NULL;
    const char *label = NULL;
    struct utsname system;

    while (command_index < argc && strcmp(argv[command_index], "--") != 0) {
        if (command_index + 1 >= argc) {
            break;
        }

        bool valid = false;
        if (strcmp(argv[command_index], "--interval-ms") == 0) {
            valid = parse_number(argv[command_index + 1], 60000UL, &interval_ms);
        } else if (strcmp(argv[command_index], "--timeout") == 0) {
            valid = parse_number(argv[command_index + 1], 86400UL, &timeout_seconds);
        } else if (strcmp(argv[command_index], "--console") == 0) {
            console_path = argv[command_index + 1];
            valid = true;
        } else if (strcmp(argv[command_index], "--label") == 0) {
            label = argv[command_index + 1];
            valid = true;
        }

        if (!valid) {
            fprintf(stderr, "Invalid argument: %s\n", argv[command_index]);
            return 2;
        }

        command_index += 2;
    }

    if (command_index >= argc || strcmp(argv[command_index], "--") != 0 || command_index + 1 >= argc) {
        fputs("Usage: synbench [--interval-ms 100] [--timeout 300] "
              "[--console /dev/tty1] [--label test] -- command [args...]\n",
              stderr);
        return 2;
    }

    if (uname(&system) != 0) {
        perror("uname");
        return 1;
    }

    bool linux_system = strcmp(system.sysname, "Linux") == 0;
    int console_fd = -1;
    int output_pipe[2] = {-1, -1};

    if (console_path != NULL) {
        console_fd = open(console_path, O_WRONLY | O_NOCTTY | O_CLOEXEC);
        struct stat info;

        if (console_fd < 0 || fstat(console_fd, &info) != 0 || !S_ISCHR(info.st_mode)) {
            fputs("Console must be an existing writable character device\n", stderr);
            return 2;
        }

        if (pipe(output_pipe) != 0 || fcntl(output_pipe[0], F_SETFL, O_NONBLOCK) < 0) {
            perror("console pipe");
            return 1;
        }

        if (label != NULL) {
            (void)dprintf(console_fd, "\033[2J\033[H\033[1;36mSYNBENCH 2.0\033[0m\n\n");
            (void)dprintf(console_fd, "System: %s\nNode: %s\nRelease: %s\nVersion: %s\nMachine: %s\n\n",
                          system.sysname, system.nodename, system.release, system.version, system.machine);
            (void)dprintf(console_fd, "\033[1;32mTest: %s\033[0m\n\n", label);
        } else {
            (void)dprintf(console_fd, "\nSYNBENCH | %s %s\ncommand:", system.sysname, system.release);
            for (int index = command_index + 1; index < argc; index++) {
                (void)dprintf(console_fd, " %s", argv[index]);
            }

            (void)dprintf(console_fd, "\n");
        }
    }

    printf("{\"event\":\"metadata\",\"schema\":1,\"os\":");
    json_string(system.sysname);
    fputs(",\"release\":", stdout);
    json_string(system.release);
    printf(",\"clock_ticks\":%ld,\"page_size\":%ld,\"interval_ms\":%lu,"
           "\"scope\":\"direct child\",\"process_user_system_split_valid\":%s}\n",
           linux_system ? sysconf(_SC_CLK_TCK) : 100L, sysconf(_SC_PAGESIZE), interval_ms,
           linux_system ? "true" : "false");
    sample(0, "before");

    struct sigaction action = {0};
    action.sa_handler = on_signal;
    (void)sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0
        || sigaction(SIGHUP, &action, NULL) != 0 || sigaction(SIGPIPE, &action, NULL) != 0) {
        perror("sigaction");
        return 1;
    }

    (void)fflush(NULL);
    uint64_t start = monotonic_ns();
    pid_t child = fork();

    if (child < 0) {
        perror("fork");
        return 1;
    }

    if (child == 0) {
        int output = console_fd >= 0 ? output_pipe[1] : STDERR_FILENO;

        if (setpgid(0, 0) != 0 || dup2(output, STDOUT_FILENO) < 0 || dup2(output, STDERR_FILENO) < 0) {
            _exit(126);
        }

        if (console_fd >= 0) {
            (void)close(output_pipe[0]);
            (void)close(output_pipe[1]);
            (void)close(console_fd);
        }

        execvp(argv[command_index + 1], &argv[command_index + 1]);
        perror("execvp");
        _exit(127);
    }

    (void)setpgid(child, child);
    if (output_pipe[1] >= 0) {
        (void)close(output_pipe[1]);
    }

    bool timed_out = false;
    int status = 0;
    struct rusage usage = {0};
    uint64_t next_sample = start;
    uint64_t next_progress = start + UINT64_C(1000000000);

    for (;;) {
        (void)copy_output(output_pipe[0], console_fd);
        uint64_t now = monotonic_ns();

        if (now >= next_sample) {
            sample(child, "running");
            next_sample = monotonic_ns() + (uint64_t)interval_ms * UINT64_C(1000000);
        }

        if (console_fd >= 0 && now >= next_progress) {
            (void)dprintf(console_fd, "\rRunning: %" PRIu64 " s   ", (now - start) / UINT64_C(1000000000));
            next_progress = now + UINT64_C(1000000000);
        }

        pid_t result = linux_system ? wait4(child, &status, WNOHANG, &usage) : waitpid(child, &status, WNOHANG);
        if (result == child) {
            break;
        }

        if (result < 0 && errno != EINTR) {
            perror("wait");
            (void)kill(-child, SIGKILL);
            (void)waitpid(child, NULL, 0);
            return 1;
        }

        uint64_t elapsed = monotonic_ns() - start;
        if (interrupted != 0 || elapsed >= (uint64_t)timeout_seconds * UINT64_C(1000000000)) {
            timed_out = interrupted == 0;
            (void)kill(-child, SIGKILL);
            (void)kill(child, SIGKILL);
        }

        now = monotonic_ns();
        int delay_ms = now >= next_sample ? 0 : (int)((next_sample - now) / UINT64_C(1000000)) + 1;
        struct pollfd output_event = {.fd = output_pipe[0], .events = POLLIN};

        (void)poll(&output_event, 1, delay_ms);
        if ((output_event.revents & POLLHUP) != 0 && copy_output(output_pipe[0], console_fd) == 0) {
            (void)close(output_pipe[0]);
            output_pipe[0] = -1;
        }
    }

    uint64_t elapsed = monotonic_ns() - start;
    if (console_fd >= 0) {
        for (unsigned index = 0; index < 16U; index++) {
            if (copy_output(output_pipe[0], console_fd) == 0) {
                break;
            }
        }

        (void)dprintf(console_fd, "\nSYNBENCH finished | exit=%d signal=%d elapsed=%" PRIu64 " ms\n",
                      WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                      WIFSIGNALED(status) ? WTERMSIG(status) : 0, elapsed / UINT64_C(1000000));
        (void)close(output_pipe[0]);
        (void)close(console_fd);
    }

    sample(0, "after");
    finish(status, timed_out, linux_system, elapsed, &usage);

    if (fflush(stdout) != 0 || ferror(stdout) != 0) {
        return 1;
    }

    return WIFEXITED(status) && !timed_out && interrupted == 0 ? WEXITSTATUS(status) : 1;
}
