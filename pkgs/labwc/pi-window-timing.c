/*
    pi-window-timing.c: Время запуска до публикации окна в Labwc
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: AArch64, Wayland
    Подсистема: диагностика рабочего стола
    Первое изменение: 21.11.2026
    Последнее изменение: 21.11.2026
    Доступ: общедоступный
*/
#define _POSIX_C_SOURCE 200809L
#include "foreign-toplevel.h"
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct zwlr_foreign_toplevel_handle_v1 Window;
typedef struct zwlr_foreign_toplevel_manager_v1 Manager;

static Manager *manager;
static Window *mapped;
static const char *wanted;

// Запомнить первое окно с заголовком запущенной программы
static void title(void *data, Window *window, const char *text) {
    (void)data;
    if (wanted && !strcmp(text, wanted)) mapped = window;
}
static void app_id(void *data, Window *w, const char *id) { (void)data; (void)w; (void)id; }
static void output(void *data, Window *w, struct wl_output *o) { (void)data; (void)w; (void)o; }
static void state(void *data, Window *w, struct wl_array *a) { (void)data; (void)w; (void)a; }
static void done(void *data, Window *w) { (void)data; (void)w; }
static void parent(void *data, Window *w, Window *p) { (void)data; (void)w; (void)p; }
static void closed(void *data, Window *w) {
    (void)data;
    if (mapped == w) mapped = NULL;
    zwlr_foreign_toplevel_handle_v1_destroy(w);
}
static const struct zwlr_foreign_toplevel_handle_v1_listener window_events = {
    title, app_id, output, output, state, done, closed, parent
};
static void window(void *data, Manager *m, Window *w) {
    (void)data; (void)m;
    zwlr_foreign_toplevel_handle_v1_add_listener(w, &window_events, NULL);
}
static void stopped(void *data, Manager *m) { (void)data; (void)m; }
static const struct zwlr_foreign_toplevel_manager_v1_listener events = {window, stopped};
static void global(void *data, struct wl_registry *r, uint32_t name, const char *type, uint32_t version) {
    (void)data; (void)version;
    if (strcmp(type, zwlr_foreign_toplevel_manager_v1_interface.name)) return;
    manager = wl_registry_bind(r, name, &zwlr_foreign_toplevel_manager_v1_interface, 1);
    zwlr_foreign_toplevel_manager_v1_add_listener(manager, &events, NULL);
}
static void removed(void *data, struct wl_registry *r, uint32_t name) { (void)data; (void)r; (void)name; }
static const struct wl_registry_listener registry_events = {global, removed};

// Те же часы используются внутри Qt и в драйвере замера окна
static long long micros(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

struct counters { long blocks, commands, read, write, hz; };

// Одно открытие /proc/storage даёт согласованный снимок накопительных счётчиков
static int snapshot(struct counters *s) {
    FILE *f = fopen("/proc/storage", "r");
    if (!f) return -1;
    char line[128], key[40];
    long n;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%39s %ld", key, &n) != 2) continue;
        if (!strcmp(key, "read_blocks")) s->blocks = n;
        if (!strcmp(key, "read_commands")) s->commands = n;
        if (!strcmp(key, "read_ticks")) s->read = n;
        if (!strcmp(key, "write_ticks")) s->write = n;
        if (!strcmp(key, "timer_hz")) s->hz = n;
    }
    fclose(f);
    return s->hz > 0 ? 0 : -1;
}

struct task_time {
    unsigned pid;
    long ticks;
    char name[40];
};

// Снимок живых процессов не требует чтения stat и таблицы страниц для каждого PID
static size_t tasks(struct task_time *list, size_t limit) {
    FILE *f = fopen("/proc/task-times", "r");
    if (!f) return 0;
    size_t count = 0;
    char line[128];
    while (count < limit && fgets(line, sizeof(line), f)) {
        struct task_time *t = &list[count];
        long user, system;
        if (sscanf(line, "%u %ld %ld %39s", &t->pid, &user, &system, t->name) == 4) {
            t->ticks = user + system;
            ++count;
        }
    }
    fclose(f);
    return count;
}

// Для нового PID отсчёт начинается с нуля
static long since(const struct task_time *task, const struct task_time *before, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (before[i].pid == task->pid) return task->ticks - before[i].ticks;
    }
    return task->ticks;
}

// Остановить измерение сразу при появлении окна или завершении приложения
static int observe(struct wl_display *display, pid_t child, long long deadline) {
    struct pollfd fd = {wl_display_get_fd(display), POLLIN, 0};
    while (!mapped && micros() < deadline) {
        if (wl_display_dispatch_pending(display) < 0) return -1;
        if (mapped) break;
        if (waitpid(child, NULL, WNOHANG) != 0) return -1;
        int rc = poll(&fd, 1, 200);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0 || (rc > 0 && wl_display_dispatch(display) < 0)) return -1;
    }
    return mapped ? 0 : -1;
}

// Счётчики и вывод журнала после публикации окна не входят во время запуска
static int launch(struct wl_display *display, int round, int settings) {
    struct counters before = {0}, after = {0};
    const char *name = settings ? "settings" : "files";
    wanted = settings ? "Настройки SynapseOS" : "Files - SynapseOS";
    mapped = NULL;
    if (snapshot(&before) < 0) return -1;
    struct task_time previous[32], current[32];
    size_t nprevious = tasks(previous, 32);
    if (!nprevious) return -1;
    long long begin = micros();
    pid_t child = fork();
    if (child == 0) {
        if (settings) execl("/bin/synapse-desktop", "synapse-desktop", "--settings", (char *)NULL);
        else execl("/bin/synapse-files", "synapse-files", (char *)NULL);
        _exit(127);
    }
    if (child < 0) return -1;
    if (observe(display, child, begin + 120000000LL) < 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        return -1;
    }

    long long end = micros();
    int result = snapshot(&after);
    size_t ncurrent = tasks(current, 32);
    long ticks = -1;
    for (size_t i = 0; i < ncurrent; ++i) {
        long delta = since(&current[i], previous, nprevious);
        if (current[i].pid == (unsigned)child) ticks = delta;
        else if (delta >= 2) {
            printf("WINDOW_TASK round=%d app=%s pid=%u name=%s cpu_ticks=%ld\n",
                   round, name, current[i].pid, current[i].name, delta);
        }
    }
    if (ticks < 0) result = -1;
    if (result == 0) {
        printf("MAPPED %d %s %.3f seconds %ld sectors\n", round, name, (end - begin) / 1e6, after.blocks - before.blocks);
        printf("WINDOW_IO round=%d app=%s pid=%ld start_us=%lld mapped_us=%lld commands=%ld "
               "sd_read_ms=%.3f sd_write_ms=%.3f app_cpu_ticks=%ld cpu_tick_hz=%ld\n",
               round, name, (long)child, begin, end, after.commands - before.commands,
               1000.0 * (after.read - before.read) / after.hz, 1000.0 * (after.write - before.write) / after.hz,
               ticks, sysconf(_SC_CLK_TCK));
    }
    wanted = NULL;
    zwlr_foreign_toplevel_handle_v1_close(mapped);
    wl_display_flush(display);
    int status = 0;
    if (waitpid(child, &status, 0) != child || status != 0 || wl_display_roundtrip(display) < 0) return -1;
    return result;
}

// Начинать следующий шаг только после успешной проверки предыдущего
static int check_clocks(void) {
    const unsigned targets[] = {62500000u, 83333334u};
    int fd = open("/proc/storage", O_RDONLY);
    if (fd < 0) return 1;
    int result = 0;
    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        int hz = ioctl(fd, 0x534401u, (unsigned long)targets[i]);
        int err = hz < 0 ? errno : 0;
        const char *status = "passed";
        if (err == EOPNOTSUPP) status = "skipped-mode";
        else if (err == EILSEQ) status = "data-mismatch";
        else if (err == ENXIO) status = "transfer-error";
        else if (err == EAGAIN) status = "baseline-read-error";
        else if (err) status = "error";
        printf("SD_CLOCK_TEST target_hz=%u passed_hz=%d result=%s errno=%d\n", targets[i],
               hz > 0 ? hz : 0, status, err);
        FILE *stats = fopen("/proc/storage", "r");
        if (!stats) { result = 1; break; }
        char line[128];
        while (fgets(line, sizeof(line), stats)) {
            if (!strncmp(line, "probe_", 6) || !strncmp(line, "timer_hz ", 9))
                printf("SD_CLOCK_SAMPLE target_hz=%u %s", targets[i], line);
        }
        fclose(stats);
        if (err) {
            result = err != EOPNOTSUPP && err != EILSEQ && err != ENXIO;
            break;
        }
    }
    close(fd);
    return result;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--sd-clocks")) return check_clocks();
    if (argc != 1) return 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (setenv("SYNAPSE_PI_WINDOW_TIMING", "1", 1) != 0) return 1;
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 1;
    wl_registry_add_listener(wl_display_get_registry(display), &registry_events, NULL);
    if (wl_display_roundtrip(display) < 0 || !manager || wl_display_roundtrip(display) < 0) return 1;
    for (int i = 0; i < 6; ++i) {
        if (launch(display, i / 2 + 1, i % 2) < 0) return 1;
        sleep(1);
    }
    wl_display_disconnect(display);
    return 0;
}
