// main.c: Сводка сведений о системе
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Подсистема: Сводка сведений о системе в пользовательском пространстве
// Первое изменение в истории Git: 09.10.2026
// Последнее изменение: 27.10.2026
// Доступ: общедоступный

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <pwd.h>
#include <ctype.h>
#include <dirent.h>

// Прочитать память, доступную после освобождения файлового кеша
static unsigned long long available_memory_kib(void) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (file == NULL) {
        return 0;
    }

    char line[160];
    unsigned long long available = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (sscanf(line, "MemAvailable: %llu kB", &available) == 1) {
            break;
        }
    }

    fclose(file);
    return available;
}

// Цвета терминала
#define C_RESET "\033[0m"
#define C_BOLD "\033[1m"
#define C_LOGO "\033[1;36m"
#define C_TITLE "\033[1;37m"
#define C_LABEL "\033[1;32m"
#define C_INFO "\033[0;37m"

#define LOGO_WIDTH 48
#define GAP 2

static const char *logo_lines[] = {"                             .++++              ",
                                   "                            .++ .++             ",
                                   "                            .++ -++             ",
                                   "                              +++.              ",
                                   "                               ++               ",
                                   "                               ++               ",
                                   "                             .+++++             ",
                                   "                                 .+++           ",
                                   "                          ++  +++: =++          ",
                                   "                         .++ .++++  ++          ",
                                   "                          +++     .++.          ",
                                   "                       .+++=++++++++++++        ",
                                   "              .+++++.++++     ....    .++++++++ ",
                                   "             +++   +++                  ++   .++",
                                   "             ++.    ++                  ++.  .++",
                                   "             .++...++-                  .++++++ ",
                                   "               +++++                    ",
                                   NULL};

static void trim(char *str) {
    if (!str) {
        return;
    }

    char *start = str;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    if (*start == '\0') {
        *str = '\0';
        return;
    }

    char *end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) {
        end--;
    }
    *(end + 1) = '\0';
    if (start != str) {
        memmove(str, start, end - start + 2);
    }
}

static bool read_file_str(const char *path, char *buf, size_t len) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    if (fgets(buf, len, f)) {
        trim(buf);
        fclose(f);
        return strlen(buf) > 0;
    }
    fclose(f);
    return false;
}

static bool read_kv(const char *path, const char *key, char *buf, size_t len) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }

    char line[256];
    bool found = false;
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0) {
            char *val = line + klen;
            while (*val == ':' || *val == '=' || *val == ' ') {
                val++;
            }
            trim(val);
            if (val[0] == '"' || val[0] == '\'') {
                val++;
                char *q = strchr(val, val[-1]);
                if (q) {
                    *q = '\0';
                }
            }
            strncpy(buf, val, len - 1);
            buf[len - 1] = '\0';
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static int count_dir_entries(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        return 0;
    }

    struct dirent *dir;
    int count = 0;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] != '.') {
            count++;
        }
    }
    closedir(d);
    return count;
}

static void get_os(char *buf, size_t len) {
    if (read_kv("/etc/os-release", "PRETTY_NAME", buf, len)) {
        return;
    }
    if (read_kv("/etc/os-release", "NAME", buf, len)) {
        return;
    }

    if (read_file_str("/etc/system-release", buf, len)) {
        return;
    }

    FILE *f = fopen("/etc/issue", "r");
    if (f) {
        if (fgets(buf, len, f)) {
            trim(buf);
            char *esc = strchr(buf, '\\');
            if (esc) {
                *esc = '\0';
            }
            trim(buf);
            if (strlen(buf) > 0) {
                fclose(f);
                return;
            }
        }
        fclose(f);
    }

    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(buf, len, "%.63s %.63s", u.sysname, u.release);
    } else {
        strncpy(buf, "Unknown OS", len);
    }
}

static void get_host(char *buf, size_t len) {
    // Пробуем DMI
    char vendor[64] = "", name[64] = "";
    bool has_vendor = read_file_str("/sys/devices/virtual/dmi/id/board_vendor", vendor,
                                    sizeof(vendor));
    bool has_name =
        read_file_str("/sys/devices/virtual/dmi/id/board_name", name, sizeof(name));

    if (!has_name) {
        has_name = read_file_str("/sys/devices/virtual/dmi/id/product_name", name,
                                 sizeof(name));
    }

    if (has_name) {
        if (has_vendor && strcmp(vendor, "To be filled by O.E.M.") != 0) {
            snprintf(buf, len, "%s %s", vendor, name);
        } else {
            strncpy(buf, name, len);
        }
        return;
    }

    // Пробуем Device Tree
    if (read_file_str("/sys/firmware/devicetree/base/model", buf, len)) {
        return;
    }

    // Если модель не найдена, используем uname
    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(buf, len, "%s (Machine)", u.machine);
    } else {
        strncpy(buf, "Unknown Host", len);
    }
}

static void get_cpu(char *buf, size_t len) {
    if (read_kv("/proc/cpuinfo", "model name", buf, len)) {
        return;
    }
    if (read_kv("/proc/cpuinfo", "Model", buf, len)) {
        return;
    }
    if (read_kv("/proc/cpuinfo", "Hardware", buf, len)) {
        return;
    }
    if (read_kv("/proc/cpuinfo", "cpu model", buf, len)) {
        return;
    }

#if defined(__x86_64__) || defined(__i386__)
    unsigned eax = 0x80000000, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (eax >= 0x80000004) {
        unsigned words[12];
        for (unsigned i = 0; i < 3; i++) {
            eax = 0x80000002 + i;
            __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
            words[i * 4] = eax;
            words[i * 4 + 1] = ebx;
            words[i * 4 + 2] = ecx;
            words[i * 4 + 3] = edx;
        }

        char brand[49];
        memcpy(brand, words, 48);
        brand[48] = 0;
        trim(brand);
        if (brand[0]) {
            snprintf(buf, len, "%s", brand);
            return;
        }
    }
#endif
    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(buf, len, "%s", u.machine);
    } else {
        snprintf(buf, len, "Unknown CPU");
    }
}

static int get_packages(void) {
    int pkgs = 0;

    pkgs += count_dir_entries("/var/lib/dpkg/info");

    FILE *f = fopen("/lib/apk/db/installed", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == 'P' && line[1] == ':') {
                pkgs++;
            }
        }
        fclose(f);
    }

    pkgs += count_dir_entries("/var/lib/pacman/local");

    pkgs += count_dir_entries("/var/lib/rpm/Packages"); // Приблизительный подсчёт

    // Если база пакетов не найдена, пробуем подсчитать записи известного каталога
    if (pkgs == 0) {
    }
    return pkgs;
}

static void get_disk_usage(char *buf, size_t len) {
    // В этой версии не используем поля statfs для вывода ёмкости диска
    struct utsname u;
    if (uname(&u) == 0 && !strcmp(u.sysname, "SynapseOS")) {
        snprintf(buf, len, "N/A (filesystem statistics unavailable)");
        return;
    }

    struct statvfs stat;
    if (statvfs("/", &stat) == 0) {
        unsigned long total = (stat.f_blocks * stat.f_frsize) / (1024 * 1024);
        unsigned long free = (stat.f_bfree * stat.f_frsize) / (1024 * 1024);
        unsigned long used = total - free;
        unsigned long percent = (total > 0) ? (used * 100 / total) : 0;
        snprintf(buf, len, "%lu MiB / %lu MiB (%lu%%)", used, total, percent);
    } else {
        strncpy(buf, "N/A", len);
    }
}

void synfetch(void) {
    struct utsname u;
    struct sysinfo si;
    char os_buf[128] = "Unknown";
    char host_buf[128] = "Unknown";
    char cpu_buf[128] = "Unknown";
    char user_host[128] = "user@host";
    char disk_buf[128] = "N/A";

    bool has_uname = (uname(&u) == 0);
    bool has_si = (sysinfo(&si) == 0);

    struct passwd *pw = getpwuid(getuid());
    char host[64] = "unknown";
    if (has_uname) {
        snprintf(host, sizeof(host), "%.63s", u.nodename);
    }

    if (pw && pw->pw_name) {
        snprintf(user_host, sizeof(user_host), "%s@%s", pw->pw_name, host);
    } else {
        snprintf(user_host, sizeof(user_host), "user@%s", host);
    }

    get_os(os_buf, sizeof(os_buf));
    get_host(host_buf, sizeof(host_buf));
    get_cpu(cpu_buf, sizeof(cpu_buf));
    get_disk_usage(disk_buf, sizeof(disk_buf));

    int pkgs = get_packages();

    // Добавляем архитектуру, если её нет в названии системы
    if (has_uname && !strstr(os_buf, u.machine)) {
        strncat(os_buf, " ", sizeof(os_buf) - strlen(os_buf) - 1);
        strncat(os_buf, u.machine, sizeof(os_buf) - strlen(os_buf) - 1);
    }

    printf("\n");

    printf("%-*s" C_BOLD C_TITLE "%s" C_RESET "\n", LOGO_WIDTH + GAP, "", user_host);
    printf("%-*s" C_LABEL, LOGO_WIDTH + GAP, "");
    for (size_t i = 0; i < strlen(user_host); i++) {
        printf("-");
    }
    printf(C_RESET "\n");

    int lidx = 0;

#define OUT(label, fmt, ...)                                                           \
    do {                                                                               \
        const char *ll = logo_lines[lidx] ? logo_lines[lidx] : "";                     \
        printf(C_LOGO "%-*s" C_RESET C_LABEL "%-*s" C_INFO fmt "\n", LOGO_WIDTH + GAP, \
               ll, 10, label ":", ##__VA_ARGS__);                                      \
        lidx++;                                                                        \
    } while (0)

    OUT("OS", "%s", os_buf);
    OUT("Host", "%s", host_buf);
    OUT("Kernel", "%s", has_uname ? u.release : "N/A");

    if (has_si) {
        long s = si.uptime;
        long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
        char up[64];
        if (d > 0) {
            snprintf(up, sizeof(up), "%ldd %ldh %ldm", d, h, m);
        } else if (h > 0) {
            snprintf(up, sizeof(up), "%ldh %ldm", h, m);
        } else {
            snprintf(up, sizeof(up), "%ldm", m);
        }
        OUT("Uptime", "%s", up);

        unsigned long ps = si.mem_unit ? si.mem_unit : 1;
        unsigned long tot_mb = (si.totalram * ps) / (1024 * 1024);
        unsigned long long available_kib = available_memory_kib();
        unsigned long free_mb = available_kib != 0 ? available_kib / 1024 :
            (si.freeram * ps) / (1024 * 1024);
        if (free_mb > tot_mb) {
            free_mb = tot_mb;
        }

        unsigned long used_mb = tot_mb - free_mb;
        unsigned long mem_perc = (tot_mb > 0) ? (used_mb * 100 / tot_mb) : 0;

        OUT("Memory", "%lu / %lu MiB (%lu%%)", used_mb, tot_mb, mem_perc);
    } else {
        OUT("Uptime", "N/A");
        OUT("Memory", "N/A");
    }

    if (pkgs > 0) {
        OUT("Packages", "%d", pkgs);
    }

    const char *sh = getenv("SHELL");
    if (sh) {
        const char *sh_name = strrchr(sh, '/');
        sh_name = sh_name ? sh_name + 1 : sh;
        OUT("Shell", "%s", sh_name);
    } else {
        OUT("Shell", "sh");
    }

    OUT("Disk (/)", "%s", disk_buf);
    OUT("CPU", "%s", cpu_buf);

    // Выводим оставшиеся строки логотипа
    while (logo_lines[lidx]) {
        printf(C_LOGO "%s" C_RESET "\n", logo_lines[lidx]);
        lidx++;
    }
    printf("\n");
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    synfetch();
    return 0;
}
