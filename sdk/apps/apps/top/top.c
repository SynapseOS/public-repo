// top.c: Таблица процессов в терминале
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Подсистема: Таблица процессов в терминале в пользовательском пространстве
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 27.10.2026
// Доступ: общедоступный

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>
#include <curses.h>
#include "../../../include/lumine/process.h"
// SynAPI сохраняет внутренний ENOSYS 88, POSIX errno использует значение 38
#define SNAPSHOT_NATIVE_ENOSYS 88

static struct lumine_process_info_v3 processes[4096];
static struct lumine_process_info_v2 previous[4096];
static int display_order[4096];
static bool kernel_memory_available;
static double cpu[4096];
static int previous_count;
static uint64_t previous_us;
static volatile sig_atomic_t interrupted;

// Завершить обновление экрана после сигнала
static void stop(int sig) {
    (void)sig;
    interrupted = 1;
}

// Сравнить процессы по идентификатору для стабильного порядка строк
static int order(const void *a, const void *b) {
    const struct lumine_process_info_v3 *x = a, *y = b;
    return (x->base.base.pid > y->base.base.pid) - (x->base.base.pid < y->base.base.pid);
}

// Показать сначала процессы с наибольшим объёмом резидентной памяти
static int memory_order(const void *a, const void *b) {
    int left = *(const int *)a;
    int right = *(const int *)b;
    uint64_t left_bytes = processes[left].base.resident_bytes + processes[left].kernel_bytes;
    uint64_t right_bytes = processes[right].base.resident_bytes + processes[right].kernel_bytes;
    if (left_bytes != right_bytes) {
        return left_bytes < right_bytes ? 1 : -1;
    }

    return (processes[left].base.base.pid > processes[right].base.base.pid) -
        (processes[left].base.base.pid < processes[right].base.base.pid);
}

struct memory_details {
    uint64_t available, shared, heap, tables, drivers, network, modules, anon, other;
};

// Прочитать категории физических страниц, которые не принадлежат отдельному процессу
static bool memory_details_read(struct memory_details *details) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (file == NULL) {
        return false;
    }

    memset(details, 0, sizeof(*details));
    char line[160];
    while (fgets(line, sizeof(line), file) != NULL) {
        char name[32];
        unsigned long long value;
        if (sscanf(line, "%31[^:]: %llu kB", name, &value) != 2) {
            continue;
        }

        if (strcmp(name, "MemAvailable") == 0) {
            details->available = value;
        } else if (strcmp(name, "Shmem") == 0) {
            details->shared = value;
        } else if (strcmp(name, "KernelHeap") == 0) {
            details->heap = value;
        } else if (strcmp(name, "PageTables") == 0) {
            details->tables = value;
        } else if (strcmp(name, "Drivers") == 0) {
            details->drivers = value;
        } else if (strcmp(name, "Network") == 0) {
            details->network = value;
        } else if (strcmp(name, "Modules") == 0) {
            details->modules = value;
        } else if (strcmp(name, "UserAnon") == 0) {
            details->anon = value;
        } else if (strcmp(name, "OtherUsed") == 0) {
            details->other = value;
        }
    }

    fclose(file);
    return true;
}

// Получить снимок процессов и вычислить загрузку CPU между выборками
static int snapshot(void) {
    long n = syscall(LUMINE_PROCESS_SNAPSHOT_V3, processes, 4096, sizeof(*processes));
    kernel_memory_available = n >= 0;

    if (n < 0 && (errno == ENOSYS || errno == SNAPSHOT_NATIVE_ENOSYS)) {
        n = syscall(LUMINE_PROCESS_SNAPSHOT_V2, processes, 4096, sizeof(struct lumine_process_info_v2));
        if (n >= 0 && n <= 4096) {
            // Расширяем записи с конца, чтобы не затереть ещё не прочитанную запись V2
            for (long i = n; i > 0; i--) {
                struct lumine_process_info_v2 legacy;
                memcpy(&legacy, (char *)processes + (i - 1) * sizeof(legacy), sizeof(legacy));
                processes[i - 1].base = legacy;
                processes[i - 1].kernel_bytes = 0;
            }
        }
    }

    if (n < 0) {
        perror("top: process snapshot");
        return -1;
    }

    if (n > 4096) {
        fputs("top: invalid process count\n", stderr);
        return -1;
    }

    qsort(processes, (size_t)n, sizeof(*processes), order);
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("top: monotonic clock");
        return -1;
    }

    uint64_t now = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    for (int i = 0, j = 0; i < n; i++) {
        cpu[i] = 0;
        while (j < previous_count && previous[j].base.pid < processes[i].base.base.pid) {
            j++;
        }
        if (j < previous_count && previous[j].base.pid == processes[i].base.base.pid &&
            now > previous_us && processes[i].base.cpu_us >= previous[j].cpu_us) {
            cpu[i] = 100.0 * (processes[i].base.cpu_us - previous[j].cpu_us) /
                     (now - previous_us);
        }
    }

    for (long i = 0; i < n; i++) {
        previous[i] = processes[i].base;
        display_order[i] = (int)i;
    }
    qsort(display_order, (size_t)n, sizeof(*display_order), memory_order);
    previous_count = (int)n;
    previous_us = now;
    return (int)n;
}

// Форматировать память контекста, старое ядро не предоставляет этот счётчик
static void format_kernel_memory(char *text, size_t capacity, uint64_t bytes) {
    if (kernel_memory_available) {
        snprintf(text, capacity, "%.2f", bytes / (1024.0 * 1024));
    } else {
        snprintf(text, capacity, "-");
    }
}

// Сложить пользовательские страницы и память контекста потока, если ядро её сообщает
static void format_process_memory(char *text, size_t capacity, int index) {
    if (kernel_memory_available) {
        uint64_t total = processes[index].base.resident_bytes + processes[index].kernel_bytes;
        snprintf(text, capacity, "%.2f", total / (1024.0 * 1024));
    } else {
        snprintf(text, capacity, "-");
    }
}

// Вывести снимки процессов в терминал либо в пакетном режиме
int main(int argc, char **argv) {
    int batch = 0, iterations = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b")) {
            batch = 1;
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            iterations = atoi(argv[++i]);
            if (iterations < 1) {
                return 2;
            }
        } else {
            puts("Usage: top [-b] [-n iterations]\n"
                 "SynapseOS process monitor. Up/Down selects, k sends SIGTERM, q quits.\n"
                 "CPU: 100% is one core, first sample is 0. Processes are sorted by resident memory.\n"
                 "RAM: RSS plus kernel stacks/FPU. Shared pages appear in each mapping process.\n"
                 "KRAM requires snapshot V3, kernels with V2 show -.");
            return strcmp(argv[i], "--help") ? 2 : 0;
        }
    }
    if (!batch && !isatty(0)) {
        fputs("top: use -b without a terminal\n", stderr);
        return 1;
    }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    if (!batch) {
        if (!getenv("TERM")) {
            setenv("TERM", "linux", 1);
        }
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, 1);
        timeout(1000);
        curs_set(0);
    }

    int selected = 0, offset = 0, frames = 0, confirm = 0, result = 0;
    pid_t confirm_pid = 0;
    char notice[128] = "";
    while (!interrupted) {
        int count = snapshot();
        if (count < 0) {
            result = 1;
            break;
        }

        struct sysinfo si;
        if (sysinfo(&si)) {
            memset(&si, 0, sizeof si);
        }
        double total = (double)si.totalram * si.mem_unit / (1024 * 1024);
        struct memory_details details;
        bool have_details = memory_details_read(&details);
        double available = have_details ? details.available / 1024.0 :
            (double)si.freeram * si.mem_unit / (1024 * 1024);
        if (available > total) {
            available = total;
        }

        double used = total - available;
        if (batch) {
            printf(
                "SynapseOS top | uptime %lds | active %.0f/%.0f MiB | available %.0f MiB | %d processes\n",
                si.uptime, used, total, available, count);
            if (have_details) {
                printf("Kernel: heap %.1f | tables %.1f | drivers %.1f | net %.1f | "
                       "modules %.1f | other %.1f MiB\n",
                       details.heap / 1024.0, details.tables / 1024.0,
                       details.drivers / 1024.0, details.network / 1024.0,
                       details.modules / 1024.0, details.other / 1024.0);
                printf("Pages: anon %.1f | shared %.1f MiB\n",
                       details.anon / 1024.0, details.shared / 1024.0);
            }
            puts("PID    THR STATE    CPU%  RSS MiB KRAM MiB RAM MiB NAME");
            for (int row = 0; row < count; row++) {
                int i = display_order[row];
                char kernel_memory[32];
                char process_memory[32];
                format_kernel_memory(kernel_memory, sizeof(kernel_memory), processes[i].kernel_bytes);
                format_process_memory(process_memory, sizeof(process_memory), i);
                printf("%-6lld %-3u %-7s %5.1f %8.2f %8s %7s %s\n",
                       (long long)processes[i].base.base.pid, processes[i].base.base.threads,
                       processes[i].base.base.state == 2 ? "exited" : "alive", cpu[i],
                       processes[i].base.resident_bytes / (1024.0 * 1024),
                       kernel_memory, process_memory,
                       processes[i].base.base.name);
            }
        } else {
            if (selected >= count) {
                selected = count ? count - 1 : 0;
            }

            int rows, cols;
            getmaxyx(stdscr, rows, cols);
            int visible = rows > 7 ? rows - 7 : 1;
            if (selected < offset) {
                offset = selected;
            }
            if (selected >= offset + visible) {
                offset = selected - visible + 1;
            }
            erase();
            mvprintw(0, 0,
                     "SynapseOS top | uptime %lds | RAM %.0f/%.0f MiB | available %.0f | processes %d",
                     si.uptime, used, total, available, count);
            if (have_details) {
                mvprintw(1, 0, "Kernel: heap %.0f | tables %.0f | driver %.0f | net %.0f | "
                         "mods %.0f | other %.0f MiB", details.heap / 1024.0,
                         details.tables / 1024.0, details.drivers / 1024.0,
                         details.network / 1024.0, details.modules / 1024.0,
                         details.other / 1024.0);
                mvprintw(2, 0, "Pages: anon %.0f | shared %.0f MiB",
                         details.anon / 1024.0, details.shared / 1024.0);
            }
            mvaddnstr(3, 0, "RAM=RSS+KRAM | shared pages may repeat | k: kill | q: quit",
                      cols - 1);
            mvaddnstr(4, 0, "PID    THR STATE    CPU%  RSS MiB KRAM MiB RAM MiB NAME", cols - 1);
            for (int row = offset; row < count && row < offset + visible; row++) {
                int i = display_order[row];
                if (row == selected) {
                    attron(A_REVERSE);
                }

                char line[400];
                char kernel_memory[32];
                char process_memory[32];
                format_kernel_memory(kernel_memory, sizeof(kernel_memory), processes[i].kernel_bytes);
                format_process_memory(process_memory, sizeof(process_memory), i);
                snprintf(line, sizeof line, "%-6lld %-3u %-7s %5.1f %8.2f %8s %7s %s",
                         (long long)processes[i].base.base.pid, processes[i].base.base.threads,
                         processes[i].base.base.state == 2 ? "exited" : "alive", cpu[i],
                         processes[i].base.resident_bytes / (1024.0 * 1024),
                         kernel_memory, process_memory,
                         processes[i].base.base.name);
                mvaddnstr(5 + row - offset, 0, line, cols - 1);
                if (row == selected) {
                    attroff(A_REVERSE);
                }
            }
            mvaddnstr(
                rows - 1, 0,
                confirm
                    ? "Send SIGTERM to selected process? y confirms, any other key cancels"
                    : notice,
                cols - 1);
            refresh();
        }
        if (iterations && ++frames >= iterations) {
            break;
        }
        if (batch) {
            sleep(1);
            continue;
        }

        int key = getch();
        if (confirm && key != ERR) {
            if (key == 'y') {
                if (kill(confirm_pid, SIGTERM)) {
                    snprintf(notice, sizeof notice, "Cannot terminate PID %lld",
                             (long long)confirm_pid);
                } else {
                    snprintf(notice, sizeof notice, "SIGTERM sent to PID %lld",
                             (long long)confirm_pid);
                }
            }
            confirm = 0;
            continue;
        }
        if (key == 'q' || key == 27) {
            break;
        }
        if (key == KEY_UP && selected > 0) {
            selected--;
        }
        if (key == KEY_DOWN && selected + 1 < count) {
            selected++;
        }
        int selected_index = count ? display_order[selected] : 0;
        if (key == 'k' && count && processes[selected_index].base.base.pid > 2 &&
            processes[selected_index].base.base.pid != getpid()) {
            confirm = 1;
            confirm_pid = processes[selected_index].base.base.pid;
        }
    }
    if (!batch) {
        endwin();
    }
    return result;
}
