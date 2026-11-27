// manager.c: Графический вход и смена пользователя SynapseOS
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl и SynAPI
// Описание: Проверить пароль и передать графический ввод сеансу пользователя
// Подсистема: графический сеанс
// Первое изменение: 30.10.2026
// Последнее изменение: 23.11.2026
// Доступ: общедоступный

#define _GNU_SOURCE
#include <errno.h>
#include "../labwc/posix-profile.h"
#include <crypt.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SYNAPSE_SET_SEAT_UID 102005L

struct login_request {
    char user[64];
    char password[256];
};

struct session_account {
    uid_t uid;
    gid_t gid;
    char name[64];
    char home[256];
};

static volatile sig_atomic_t stopping;

// Очистить пароль независимо от оптимизаций компилятора
static void clear_secret(void *memory, size_t length) {
    volatile unsigned char *bytes = memory;
    while (length != 0) {
        *bytes = 0;
        bytes++;
        length--;
    }
}

// Запросить завершение менеджера после выхода из текущего сеанса
static void stop_manager(int signal_number) {
    (void)signal_number;
    stopping = 1;
}

// Дождаться завершения графической оболочки
static bool wait_for_child(pid_t child) {
    int status = 0;
    pid_t result;
    do {
        result = waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR && !stopping);

    return result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Прочитать запрос, прекращая ожидание после выхода графической оболочки входа
static bool read_request(int fd, pid_t child, struct login_request *request,
                         bool *child_reaped) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return false;
    }

    size_t received = 0;
    while (received < sizeof(*request)) {
        struct pollfd channel = {.fd = fd, .events = POLLIN};
        int ready = poll(&channel, 1, 100);
        if (ready > 0 && (channel.revents & POLLIN) != 0) {
            ssize_t count = read(fd, (char *)request + received, sizeof(*request) - received);
            if (count > 0) {
                received += (size_t)count;
                continue;
            }

            if (count == 0 || (errno != EAGAIN && errno != EINTR)) {
                return false;
            }
        } else if (ready > 0 && (channel.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return false;
        } else if (ready < 0 && errno != EINTR) {
            return false;
        }

        int status = 0;
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            *child_reaped = true;
            return false;
        }

        if (result < 0 && errno != EINTR) {
            return false;
        }
    }

    return memchr(request->user, 0, sizeof(request->user)) != NULL &&
        memchr(request->password, 0, sizeof(request->password)) != NULL;
}

// Показать окно входа и принять запрос через закрытый локальный сокет
static bool show_greeter(struct login_request *request, const char *error_text) {
    const char *runtime = "/var/run/synapse-greeter";
    const char *socket_path = "/var/run/synapse-greeter/login.sock";
    struct stat attributes;
    if ((mkdir(runtime, 0700) != 0 && errno != EEXIST) ||
        lstat(runtime, &attributes) != 0 || !S_ISDIR(attributes.st_mode) ||
        attributes.st_uid != 0 || (attributes.st_mode & 0777) != 0700) {
        return false;
    }

    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        return false;
    }

    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path) >= (int)sizeof(address.sun_path)) {
        close(listener);
        return false;
    }

    unlink(socket_path);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(socket_path, 0600) != 0 || listen(listener, 1) != 0) {
        close(listener);
        unlink(socket_path);
        return false;
    }

    pid_t child = fork();
    if (child < 0) {
        close(listener);
        unlink(socket_path);
        return false;
    }

    if (child == 0) {
        close(listener);
        if (setenv("SYNAPSE_GREETER_SOCKET", socket_path, 1) != 0 ||
            setenv("SYNAPSE_LOGIN_ERROR", error_text, 1) != 0 ||
            setenv("XDG_RUNTIME_DIR", runtime, 1) != 0 ||
            setenv("WLR_BACKENDS", "headless", 1) != 0 ||
            setenv("WLR_RENDERER", "pixman", 1) != 0 ||
            setenv("WLR_HEADLESS_OUTPUTS", "1", 1) != 0 ||
            setenv("WLR_XWAYLAND", "/bin/Xwayland", 1) != 0 ||
            setenv("SYNAPSE_FB", "1", 1) != 0 ||
            setenv("LD_LIBRARY_PATH", "/usr/lib/labwc/runtime:/usr/lib/weston/runtime:/usr/lib", 1) != 0) {
            _exit(126);
        }

        char *const arguments[] = {
            "labwc", "-C", "/etc/synapse-greeter", "-S", "/bin/synapse-greeter", NULL,
        };
        execv("/usr/libexec/labwc/labwc", arguments);
        _exit(127);
    }

    struct pollfd waiting = {.fd = listener, .events = POLLIN};
    int ready;
    do {
        ready = poll(&waiting, 1, 10000);
    } while (ready < 0 && errno == EINTR);

    int channel = ready > 0 ? accept4(listener, NULL, NULL, SOCK_CLOEXEC) : -1;
    close(listener);
    unlink(socket_path);
    bool child_reaped = false;
    bool received = channel >= 0 && read_request(channel, child, request, &child_reaped);
    if (channel >= 0) {
        close(channel);
    }
    fprintf(stderr, "synapse-session-manager: login request read %s (%zu bytes expected)\n",
            received ? "ok" : "failed", sizeof(*request));
    if (!child_reaped) {
        kill(child, SIGTERM);
        for (unsigned attempt = 0; attempt < 20; attempt++) {
            if (waitpid(child, NULL, WNOHANG) == child) {
                child_reaped = true;
                break;
            }
            usleep(100000);
        }
        if (!child_reaped) {
            kill(child, SIGKILL);
            wait_for_child(child);
        }
    }

    return received;
}

// Сверить пароль с системной записью, не разрешая пустые и закрытые учётные записи
static bool authenticate(const struct login_request *request, struct session_account *account) {
    struct passwd *entry = getpwnam(request->user);
    if (entry == NULL || entry->pw_dir == NULL || entry->pw_dir[0] != '/' ||
        entry->pw_name == NULL || strlen(entry->pw_name) >= sizeof(account->name) ||
        strlen(entry->pw_dir) >= sizeof(account->home)) {
        return false;
    }

    account->uid = entry->pw_uid;
    account->gid = entry->pw_gid;
    strcpy(account->name, entry->pw_name);
    strcpy(account->home, entry->pw_dir);

    const char *hash = entry->pw_passwd;
    if (hash != NULL && strcmp(hash, "x") == 0) {
        struct spwd *shadow = getspnam(account->name);
        if (shadow == NULL) {
            return false;
        }

        hash = shadow->sp_pwdp;
    }

    if (hash == NULL || hash[0] == 0 || hash[0] == '!' || hash[0] == '*') {
        return false;
    }

    char *result = crypt(request->password, hash);
    if (result == NULL || strcmp(result, hash) != 0) {
        return false;
    }

    return true;
}

// Подготовить общий каталог сокетов X11 до запуска экранов входа и пользователей
static bool prepare_x11_socket_directory(void) {
    const char *directory = "/tmp/.X11-unix";
    struct stat before;

    if (lstat(directory, &before) != 0) {
        if (errno != ENOENT || (mkdir(directory, 01777) != 0 && errno != EEXIST) ||
            lstat(directory, &before) != 0) {
            perror("synapse-session-manager: X11 socket directory");
            return false;
        }
    }

    if (!S_ISDIR(before.st_mode) || before.st_uid != 0) {
        fputs("synapse-session-manager: unsafe X11 socket directory\n", stderr);
        return false;
    }

    int descriptor = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        perror("synapse-session-manager: open X11 socket directory");
        return false;
    }

    struct stat opened;
    bool safe = fstat(descriptor, &opened) == 0 && S_ISDIR(opened.st_mode) &&
                opened.st_uid == 0 && opened.st_ino == before.st_ino && opened.st_dev == before.st_dev;
    if (safe && (opened.st_mode & 07777) != 01777) {
        safe = fchmod(descriptor, 01777) == 0;
    }

    int saved_error = errno;
    if (close(descriptor) != 0) {
        perror("synapse-session-manager: close X11 socket directory");
        return false;
    }

    if (!safe) {
        errno = saved_error;
        fputs("synapse-session-manager: cannot secure X11 socket directory\n", stderr);
        return false;
    }

    return true;
}

// Назначить framebuffer и указатели владельцу активного графического сеанса
static bool set_device_owner(uid_t uid, gid_t gid) {
    const char *paths[] = {"/dev/fb0", "/dev/pointers", "/dev/mouse"};
    for (size_t index = 0; index < sizeof(paths) / sizeof(paths[0]); index++) {
        struct stat attributes;
        if (lstat(paths[index], &attributes) != 0 || !S_ISCHR(attributes.st_mode) ||
            chown(paths[index], uid, gid) != 0 || chmod(paths[index], 0600) != 0) {
            return false;
        }
    }

    return true;
}

// Передать события ввода пользователю через привилегированный вызов ядра
static bool set_seat_owner(uid_t uid) {
    return syscall(SYNAPSE_SET_SEAT_UID, (unsigned long)uid) == 0;
}

// Узнать родителя процесса по данным procfs
static bool process_has_parent(pid_t process, pid_t parent) {
    char path[64];
    if (snprintf(path, sizeof(path), "/proc/%ld/status", (long)process) >= (int)sizeof(path)) {
        return false;
    }

    FILE *status = fopen(path, "r");
    if (status == NULL) {
        return false;
    }

    char line[256];
    bool matches = false;
    while (fgets(line, sizeof(line), status) != NULL) {
        if (strncmp(line, "PPid:", 5) == 0) {
            matches = strtol(line + 5, NULL, 10) == (long)parent;
            break;
        }
    }

    fclose(status);
    return matches;
}

// Завершить усыновлённых потомков вышедшего сеанса, включая процессы с новым setsid
static bool clear_session_children(void) {
    for (unsigned attempt = 0; attempt < 12; attempt++) {
        DIR *directory = opendir("/proc");
        if (directory == NULL) {
            return false;
        }

        unsigned remaining = 0;
        struct dirent *item;
        while ((item = readdir(directory)) != NULL) {
            char *end = NULL;
            long number = strtol(item->d_name, &end, 10);
            if (end == item->d_name || *end != 0 || number <= 1 || number > INT32_MAX ||
                number == (long)getpid()) {
                continue;
            }

            pid_t process = (pid_t)number;
            if (!process_has_parent(process, getpid())) {
                continue;
            }

            remaining++;
            kill(process, attempt < 2 ? SIGTERM : SIGKILL);
        }

        closedir(directory);
        while (waitpid(-1, NULL, WNOHANG) > 0) {
        }

        if (remaining == 0) {
            return true;
        }

        usleep(100000);
    }

    return false;
}

// Запустить графическую оболочку после полного снижения привилегий
static bool run_user_session(const struct session_account *account) {
    if (prepare_posix_profile(account->uid, account->gid)) perror("POSIX session profile");
    // Сохранить выбранный init часовой пояс до очистки окружения пользователя
    char timezone[128] = "MSK-3";
    const char *configured_timezone = getenv("TZ");
    if (configured_timezone != NULL) {
        size_t length = strnlen(configured_timezone, sizeof(timezone));
        if (length > 0 && length < sizeof(timezone)) {
            memcpy(timezone, configured_timezone, length + 1U);
        }
    }

    pid_t child = fork();
    if (child < 0) {
        return false;
    }

    if (child == 0) {
        if (setsid() < 0 || initgroups(account->name, account->gid) != 0 ||
            setgid(account->gid) != 0 || setuid(account->uid) != 0 ||
            chdir(account->home) != 0 || clearenv() != 0 ||
            setenv("HOME", account->home, 1) != 0 ||
            setenv("USER", account->name, 1) != 0 ||
            setenv("LOGNAME", account->name, 1) != 0 ||
            setenv("PATH", "/bin:/usr/bin:/usr/local/bin", 1) != 0 ||
            setenv("LANG", "ru_RU.UTF-8", 1) != 0 ||
            setenv("TZ", timezone, 1) != 0) {
            _exit(126);
        }

        execl("/bin/labwc", "labwc", (char *)NULL);
        _exit(127);
    }

    fprintf(stderr, "synapse-session-manager: user session child %ld started\n", (long)child);
    bool exited = wait_for_child(child);
    fprintf(stderr, "synapse-session-manager: user session child exit %s\n",
            exited ? "clean" : "nonzero");
    bool cleaned = clear_session_children();
    fprintf(stderr, "synapse-session-manager: session descendants cleanup %s\n",
            cleaned ? "ok" : "failed");
    return cleaned;
}

// Сохранить последнего пользователя для следующего окна входа
static void save_last_user(const char *name) {
    if (mkdir("/var/lib/synapse-session", 0700) != 0 && errno != EEXIST) {
        return;
    }

    int file = open("/var/lib/synapse-session/last-user", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (file < 0) {
        return;
    }

    size_t length = strlen(name);
    if (write(file, name, length) == (ssize_t)length) {
        fsync(file);
    }
    close(file);
}

// Повторять вход после выхода из рабочего стола, сохраняя root только у менеджера
int main(void) {
    if (geteuid() != 0) {
        fputs("synapse-session-manager: root required\n", stderr);
        return 1;
    }

    int log = open("/var/log/synapse-session.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (log >= 0) {
        if (dup2(log, STDERR_FILENO) < 0) {
            close(log);
            return 1;
        }
        close(log);
    }

    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0) {
        fputs("synapse-session-manager: subreaper unavailable\n", stderr);
        return 1;
    }

    signal(SIGTERM, stop_manager);
    signal(SIGINT, stop_manager);
    if (!prepare_x11_socket_directory()) {
        return 1;
    }

    if (!set_device_owner(0, 0) || !set_seat_owner(0)) {
        fputs("synapse-session-manager: input setup failed\n", stderr);
        return 1;
    }

    const char *error_text = "";
    while (!stopping) {
        struct login_request request = {0};
        if (!show_greeter(&request, error_text)) {
            clear_secret(&request, sizeof(request));
            error_text = "Не удалось открыть окно входа";
            sleep(2);
            continue;
        }

        struct session_account account = {0};
        bool valid = authenticate(&request, &account);
        fprintf(stderr, "synapse-session-manager: login authentication %s\n",
                valid ? "ok" : "failed");
        clear_secret(&request, sizeof(request));
        if (!valid) {
            error_text = "Неверное имя или пароль";
            sleep(1);
            continue;
        }

        save_last_user(account.name);
        if (!set_device_owner(account.uid, account.gid) || !set_seat_owner(account.uid)) {
            set_seat_owner(0);
            set_device_owner(0, 0);
            error_text = "Не удалось открыть устройства ввода";
            continue;
        }

        fputs("synapse-session-manager: session devices prepared\n", stderr);
        bool stopped_cleanly = run_user_session(&account);
        set_seat_owner(0);
        set_device_owner(0, 0);
        if (!stopped_cleanly) {
            fputs("synapse-session-manager: old session still running\n", stderr);
            break;
        }

        error_text = "";
    }

    set_seat_owner(UINT32_MAX);
    set_device_owner(0, 0);
    return 0;
}
