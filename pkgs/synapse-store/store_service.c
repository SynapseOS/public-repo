/*
    store_service.c: Root-служба установки пакетов SynapseOS
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI: synapse-x86_64-v1

    Описание: Проверяет клиента и запускает только установку подписанного пакета через setup
    Подсистема: графический магазин программ
    Первое изменение: 01.11.2026
    Последнее изменение: 24.11.2026
    Доступ: общедоступный
*/

#define _GNU_SOURCE
#include "store_protocol.h"

#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Очистить пароль из памяти перед завершением обработки запроса
static void clear_secret(char *data, size_t length) {
    volatile char *secret = (volatile char *)data;
    for (size_t index = 0; index < length; index++) {
        secret[index] = 0;
    }
}

// Дождаться готовности соединения с ограничением времени
static bool wait_socket(int descriptor, short events, int timeout_ms) {
    struct pollfd item = {.fd = descriptor, .events = events, .revents = 0};
    int result;
    do {
        result = poll(&item, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);

    return result > 0 && (item.revents & events) != 0;
}

// Прочитать запрос целиком без бесконечного ожидания клиента
static bool read_request(int descriptor, struct store_request *request) {
    unsigned char *bytes = (unsigned char *)request;
    size_t done = 0;
    struct timespec started;
    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) {
        return false;
    }

    while (done < sizeof(*request)) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return false;
        }

        int64_t elapsed = (int64_t)(now.tv_sec - started.tv_sec) * 1000 +
                          (now.tv_nsec - started.tv_nsec) / 1000000;
        if (elapsed >= 5000 || !wait_socket(descriptor, POLLIN, 5000 - (int)elapsed)) {
            return false;
        }

        ssize_t count = recv(descriptor, bytes + done, sizeof(*request) - done, 0);
        if (count <= 0) {
            return false;
        }

        done += (size_t)count;
    }

    return true;
}

// Передать буфер целиком, учитывая короткие записи
static bool write_bytes(int descriptor, const void *data, size_t length) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t done = 0;
    while (done < length) {
        if (!wait_socket(descriptor, POLLOUT, 5000)) {
            return false;
        }

        ssize_t count = send(descriptor, bytes + done, length - done, 0);
        if (count <= 0) {
            return false;
        }

        done += (size_t)count;
    }

    return true;
}

// Передать один фрагмент вывода программы
static bool write_chunk(int descriptor, const char *data, uint32_t length) {
    return write_bytes(descriptor, &length, sizeof(length)) &&
           (length == 0 || write_bytes(descriptor, data, length));
}

// Завершить ответ кодом результата
static void write_result(int descriptor, int32_t code) {
    uint32_t end = 0;
    if (write_bytes(descriptor, &end, sizeof(end))) {
        (void)write_bytes(descriptor, &code, sizeof(code));
    }
}

// Проверить имя пакета до передачи его в привилегированную команду
static bool valid_name(const struct store_request *request) {
    size_t length = request->name_length;
    if (request->source == STORE_SOURCE_UPDATE || request->source == STORE_SOURCE_DEBIAN_CATALOG ||
        request->source == STORE_SOURCE_UPGRADE || request->source == STORE_SOURCE_NTP) {
        return length == 0 && request->name[0] == 0;
    }

    if (length == 0 || length >= STORE_MAX_NAME || request->name[length] != 0) {
        return false;
    }
    if (request->source >= STORE_SOURCE_SETTINGS && request->source <= STORE_SOURCE_TIME) {
        return true;
    }

    for (size_t index = 0; index < length; index++) {
        unsigned char character = (unsigned char)request->name[index];
        bool letter = (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z');
        bool digit = character >= '0' && character <= '9';
        bool separator = index > 0 &&
                         (character == '.' || character == '_' || character == '+' || character == '-');
        if (!letter && !digit && !separator) {
            return false;
        }
    }

    return true;
}

static bool valid_data(const struct store_request *request, bool required) {
    size_t length = request->data_length;
    if (length >= STORE_MAX_DATA || request->data[length] != 0 || (required && length == 0)) {
        return false;
    }
    for (size_t index = 0; index < length; index++) {
        unsigned char character = (unsigned char)request->data[index];
        if (character < 32 || character == 127) {
            return false;
        }
    }
    return true;
}

static bool valid_account_name(const struct store_request *request) {
    size_t length = request->name_length;
    if (length == 0 || length > 32 || request->name[0] == '-') {
        return false;
    }
    for (size_t index = 0; index < length; index++) {
        unsigned char character = (unsigned char)request->name[index];
        bool letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
        bool digit = character >= '0' && character <= '9';
        if (!letter && !digit && character != '_' && character != '-') {
            return false;
        }
    }
    return true;
}

static bool valid_account_data(const struct store_request *request) {
    return !strcmp(request->data, "/bin/sh") || !strcmp(request->data, "/bin/bash") ||
           !strcmp(request->data, "/bin/zsh");
}

static bool valid_setting(const struct store_request *request) {
    const char *separator = strchr(request->name, '=');
    if (separator == NULL || separator == request->name) {
        return false;
    }
    for (const char *part = request->name; part < separator; part++) {
        unsigned char character = (unsigned char)*part;
        bool letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
        bool digit = character >= '0' && character <= '9';
        if (!letter && !digit && character != '.' && character != '_' && character != '-') {
            return false;
        }
    }
    for (const char *part = separator + 1; *part; part++) {
        unsigned char character = (unsigned char)*part;
        if (character < 32 || character == 127) {
            return false;
        }
    }
    return true;
}

static bool valid_setup(const struct store_request *request) {
    static const char *const commands[] = {"auth", "auth status", "auth logout", "plus enable",
                                           "plus disable", "plus status", "update", "auto"};
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); index++) {
        if (!strcmp(request->name, commands[index])) {
            return valid_data(request, false);
        }
    }
    return false;
}

// Разрешить время только в десятичном формате и в пределах используемых дат
static bool valid_time(const struct store_request *request) {
    const char *value = request->name;
    if (request->name_length < 9 || request->name_length > 10) {
        return false;
    }
    for (const char *part = value; *part; part++) {
        if (*part < '0' || *part > '9') {
            return false;
        }
    }
    unsigned long long seconds = strtoull(value, NULL, 10);
    return seconds >= 946684800ULL && seconds <= 4102444799ULL;
}

// Разрешить чтение только обычных параметров, которые показывает графическая панель
static bool valid_read_setting(const struct store_request *request) {
    static const char *const names[] = {
        "Boot.DefaultShell", "Terminal.Font", "Terminal.SerialConsoles", "System.Timezone",
        "Setup.CheckUpdates", "stats.stats", "stats.statsDelay", "Power.Profile",
        "Display.Resolution", "Display.FPS", "Desktop.Enabled", "Desktop.LoginManager",
        "System.Hostname", "System.Language", "SSH.Enabled", "SSH.Port", "FTP.Enabled",
        "FTP.Port", "GuestAgent.Enabled", "Network.Mode", "Network.IP", "Network.Subnet",
        "Network.Gateway", "DNS.1", "DNS.2", "LanDiscovery.Enabled", "Bluetooth.Role",
        "Bluetooth.Name", "Bluetooth.Enabled", "Bluetooth.Discoverable"
    };
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        if (!strcmp(request->name, names[index])) {
            return valid_data(request, false);
        }
    }
    return false;
}

// Разрешить только известные операции с пакетами и системными настройками
static bool valid_source(const struct store_request *request) {
    if (request->source == STORE_SOURCE_SETUP || request->source == STORE_SOURCE_UPDATE ||
        request->source == STORE_SOURCE_DEBIAN_CATALOG || request->source == STORE_SOURCE_UPGRADE ||
        request->source == STORE_SOURCE_SETUP_REMOVE || request->source == STORE_SOURCE_DEBIAN_REMOVE) {
        return valid_name(request);
    }
    if (request->source == STORE_SOURCE_SETTINGS) {
        return valid_setting(request);
    }
    if (request->source == STORE_SOURCE_POWER) {
        return !strcmp(request->name, "balanced") || !strcmp(request->name, "powersave") ||
               !strcmp(request->name, "performance") || !strcmp(request->name, "firmware");
    }
    if (request->source == STORE_SOURCE_ACCOUNT_SETUP) {
        return valid_setup(request);
    }
    if (request->source == STORE_SOURCE_USERADD || request->source == STORE_SOURCE_USERMOD) {
        return valid_account_name(request) && valid_account_data(request);
    }
    if (request->source == STORE_SOURCE_PASSWD) {
        return valid_account_name(request) && valid_data(request, true);
    }
    if (request->source == STORE_SOURCE_TIME) {
        return valid_time(request) && valid_data(request, false);
    }
    if (request->source == STORE_SOURCE_NTP) {
        return valid_name(request) && valid_data(request, false);
    }
    if (request->source == STORE_SOURCE_GET_SETTING) {
        return valid_read_setting(request);
    }
    return request->source == STORE_SOURCE_DEBIAN && valid_name(request);
}

// Сверить пароль root, если клиент не root
static bool authenticate_password(uid_t uid, const struct store_request *request) {
    if (uid == 0) {
        return true;
    }

    size_t length = request->password_length;
    if (length == 0 || length >= STORE_MAX_PASSWORD || request->password[length] != 0 ||
        memchr(request->password, 0, length) != NULL) {
        return false;
    }

    struct passwd *account = getpwnam("root");
    if (account == NULL || account->pw_passwd == NULL) {
        return false;
    }

    const char *expected = account->pw_passwd;
    if (strcmp(expected, "x") == 0) {
        struct spwd *shadow = getspnam("root");
        if (shadow == NULL) {
            return false;
        }

        expected = shadow->sp_pwdp;
    }

    if (expected == NULL || expected[0] == 0 || expected[0] == '!' || expected[0] == '*') {
        return false;
    }

    char *actual = crypt(request->password, expected);
    return actual != NULL && strcmp(actual, expected) == 0;
}

#define STORE_AUTH_SLOTS 64U
#define STORE_AUTH_RESET_MS UINT64_C(300000)

struct auth_failure {
    uid_t uid;
    unsigned failures;
    uint64_t last_failure;
    uint64_t retry_after;
};

static struct auth_failure auth_failures[STORE_AUTH_SLOTS];

// Получить время для задержек, не зависящее от смены системной даты
static bool monotonic_ms(uint64_t *value) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > (UINT64_MAX - 999U) / 1000U) {
        return false;
    }

    *value = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
    return true;
}

// Ограничивать попытки каждого UID отдельно, не задерживая службу целиком
static bool authenticate(uid_t uid, const struct store_request *request) {
    if (uid == 0) {
        return true;
    }

    uint64_t now;
    if (!monotonic_ms(&now)) {
        return false;
    }

    struct auth_failure *selected = NULL;
    struct auth_failure *oldest = &auth_failures[0];
    for (size_t index = 0; index < STORE_AUTH_SLOTS; index++) {
        struct auth_failure *entry = &auth_failures[index];
        if (entry->uid == uid) {
            selected = entry;
            break;
        }

        if (entry->uid == 0 && selected == NULL) {
            selected = entry;
        }

        if (entry->last_failure < oldest->last_failure) {
            oldest = entry;
        }
    }

    if (selected == NULL) {
        selected = oldest;
    }

    if (selected->uid != uid || now < selected->last_failure ||
        now - selected->last_failure >= STORE_AUTH_RESET_MS) {
        memset(selected, 0, sizeof(*selected));
        selected->uid = uid;
    }

    if (now < selected->retry_after) {
        return false;
    }

    if (authenticate_password(uid, request)) {
        memset(selected, 0, sizeof(*selected));
        return true;
    }

    selected->last_failure = now;
    if (selected->failures < 8U) {
        selected->failures++;
    }

    if (selected->failures >= 3U) {
        uint64_t delay = UINT64_C(1000) << (selected->failures - 3U);
        if (delay > 30000U) {
            delay = 30000U;
        }

        selected->retry_after = now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
    }

    return false;
}

// Запустить одну разрешённую операцию и вернуть её вывод клиенту
static void install_package(int client, int listener, uint32_t source, const char *name, const char *data) {
    int channel[2], secret[2] = {-1, -1};
    if (pipe(channel) != 0) {
        write_result(client, 1);
        return;
    }
    if (data[0] && (source == STORE_SOURCE_ACCOUNT_SETUP || source == STORE_SOURCE_PASSWD) &&
        pipe(secret) != 0) {
        close(channel[0]);
        close(channel[1]);
        write_result(client, 1);
        return;
    }

    pid_t child = fork();
    if (child < 0) {
        close(channel[0]);
        close(channel[1]);
        if (secret[0] >= 0) {
            close(secret[0]);
            close(secret[1]);
        }
        write_result(client, 1);
        return;
    }

    if (child == 0) {
        close(channel[0]);
        close(listener);
        close(client);
        if (secret[0] >= 0) {
            if (dup2(secret[0], STDIN_FILENO) < 0) {
                _exit(126);
            }
            close(secret[0]);
            close(secret[1]);
        }
        if (dup2(channel[1], STDOUT_FILENO) < 0 || dup2(channel[1], STDERR_FILENO) < 0) {
            _exit(126);
        }

        close(channel[1]);
        if (source == STORE_SOURCE_SETUP || source == STORE_SOURCE_UPDATE) {
            umask(0022);
        }

        if (source == STORE_SOURCE_POWER) {
            int fd = open("/proc/power-profile", O_WRONLY | O_CLOEXEC);
            size_t length = strlen(name);
            bool okay = fd >= 0 && write(fd, name, length) == (ssize_t)length;
            if (fd >= 0) {
                close(fd);
            }
            _exit(okay ? 0 : 1);
        }
        if (source == STORE_SOURCE_TIME) {
            struct timespec requested = {.tv_sec = (time_t)strtoull(name, NULL, 10), .tv_nsec = 0};
            if (clock_settime(CLOCK_REALTIME, &requested) != 0) {
                dprintf(STDERR_FILENO, "Не удалось установить время: %s\n", strerror(errno));
                _exit(1);
            }
            if (access("/dev/rtc", F_OK) != 0) {
                dprintf(STDOUT_FILENO, "Время установлено, аппаратные часы недоступны\n");
                _exit(0);
            }
            execl("/bin/hwclock", "hwclock", "-w", (char *)NULL);
            dprintf(STDERR_FILENO, "Не удалось сохранить время: %s\n", strerror(errno));
            _exit(1);
        }
        if (source == STORE_SOURCE_NTP) {
            pid_t sync = fork();
            if (sync == 0) {
                execl("/bin/ntpdate", "ntpdate", "-s", "pool.ntp.org", (char *)NULL);
                dprintf(STDERR_FILENO, "Не удалось запустить NTP: %s\n", strerror(errno));
                _exit(127);
            }
            if (sync < 0) {
                dprintf(STDERR_FILENO, "Не удалось запустить NTP: %s\n", strerror(errno));
                _exit(1);
            }
            int sync_status = 0;
            if (waitpid(sync, &sync_status, 0) != sync || !WIFEXITED(sync_status) ||
                WEXITSTATUS(sync_status) != 0) {
                dprintf(STDERR_FILENO, "Синхронизация времени не завершилась\n");
                _exit(1);
            }
            if (access("/dev/rtc", F_OK) != 0) {
                dprintf(STDOUT_FILENO, "Время синхронизировано, аппаратные часы недоступны\n");
                _exit(0);
            }
            execl("/bin/hwclock", "hwclock", "-w", (char *)NULL);
            dprintf(STDERR_FILENO, "Не удалось сохранить время: %s\n", strerror(errno));
            _exit(1);
        }
        if (source == STORE_SOURCE_SETTINGS) {
            char setting[STORE_MAX_NAME];
            snprintf(setting, sizeof(setting), "%s", name);
            char *separator = strchr(setting, '=');
            if (separator != NULL) {
                *separator = 0;
                execl("/bin/settings", "settings", "--set", setting, separator + 1, (char *)NULL);
            }
            _exit(127);
        }
        if (source == STORE_SOURCE_GET_SETTING) {
            execl("/bin/settings", "settings", "--get", name, (char *)NULL);
            _exit(127);
        }
        if (source == STORE_SOURCE_USERADD) {
            execl("/bin/useradd", "useradd", "-m", "-s", data, name, (char *)NULL);
        }
        if (source == STORE_SOURCE_USERMOD) {
            execl("/bin/usermod", "usermod", "-s", data, name, (char *)NULL);
        }
        if (source == STORE_SOURCE_PASSWD) {
            execl("/bin/passwd", "passwd", "--stdin", name, (char *)NULL);
        }
        if (source == STORE_SOURCE_ACCOUNT_SETUP) {
            if (!strcmp(name, "auth")) {
                execl("/bin/setup", "setup", "auth", (char *)NULL);
            } else if (!strcmp(name, "auth status")) {
                execl("/bin/setup", "setup", "auth", "status", (char *)NULL);
            } else if (!strcmp(name, "auth logout")) {
                execl("/bin/setup", "setup", "auth", "logout", (char *)NULL);
            } else if (!strcmp(name, "update")) {
                execl("/bin/setup", "setup", "update", (char *)NULL);
            } else if (!strcmp(name, "auto")) {
                execl("/bin/setup", "setup", "auto", (char *)NULL);
            } else {
                execl("/bin/setup", "setup", "plus", name + 5, (char *)NULL);
            }
        }
        if (source == STORE_SOURCE_UPDATE) {
            execl("/bin/setup", "setup", "update", (char *)NULL);
            dprintf(STDERR_FILENO, "Не удалось обновить каталог setup: %s\n", strerror(errno));
        } else if (source == STORE_SOURCE_UPGRADE) {
            execl("/bin/setup", "setup", "upgrade", "--yes", (char *)NULL);
            dprintf(STDERR_FILENO, "Не удалось обновить пакеты: %s\n", strerror(errno));
        } else if (source == STORE_SOURCE_SETUP_REMOVE) {
            execl("/bin/setup", "setup", "remove", "--yes", name, (char *)NULL);
            dprintf(STDERR_FILENO, "Не удалось удалить пакет: %s\n", strerror(errno));
        } else if (source == STORE_SOURCE_DEBIAN_REMOVE) {
            execl("/bin/debian-apt", "debian-apt", "remove", name, (char *)NULL);
            dprintf(STDERR_FILENO, "Не удалось удалить Debian пакет: %s\n", strerror(errno));
        } else if (source == STORE_SOURCE_DEBIAN_CATALOG) {
            execl("/bin/debian-apt", "debian-apt", "catalog", (char *)NULL);
            dprintf(STDERR_FILENO, "Не удалось подготовить каталог Debian: %s\n", strerror(errno));
        } else if (source == STORE_SOURCE_DEBIAN) {
            execl("/bin/debian-apt", "debian-apt", "install", name, (char *)NULL);
            dprintf(STDERR_FILENO, "Не удалось запустить Debian backend: %s\n", strerror(errno));
        } else {
            execl("/bin/setup", "setup", "install", "--yes", name, (char *)NULL);
            dprintf(STDERR_FILENO, "Не удалось запустить setup: %s\n", strerror(errno));
        }

        _exit(127);
    }

    close(channel[1]);
    if (secret[0] >= 0) {
        close(secret[0]);
        size_t length = strlen(data);
        size_t done = 0;
        while (done < length) {
            ssize_t count = write(secret[1], data + done, length - done);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                break;
            }
            done += (size_t)count;
        }
        close(secret[1]);
    }
    bool connected = true;
    char output[STORE_MAX_OUTPUT];
    for (;;) {
        ssize_t length = read(channel[0], output, sizeof(output));
        if (length < 0 && errno == EINTR) {
            continue;
        }

        if (length <= 0) {
            break;
        }

        if (!write_chunk(client, output, (uint32_t)length)) {
            connected = false;
            kill(child, SIGTERM);
            break;
        }
    }

    close(channel[0]);
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);

    if (connected) {
        int32_t code = waited == child && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        write_result(client, code);
    }
}

// Обработать соединение только после проверки учётных данных ядра
static void handle_client(int client, int listener) {
    struct ucred peer = {0};
    socklen_t peer_length = sizeof(peer);
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peer_length) != 0 ||
        peer_length != sizeof(peer)) {
        write_result(client, 1);
        return;
    }

    struct store_request request = {0};
    if (!read_request(client, &request)) {
        clear_secret(request.password, sizeof(request.password));
        clear_secret(request.data, sizeof(request.data));
        return;
    }

    bool valid = request.magic == STORE_REQUEST_MAGIC &&
                 request.version == STORE_REQUEST_VERSION && valid_name(&request) &&
                 valid_source(&request) &&
                 (request.source == STORE_SOURCE_GET_SETTING || authenticate(peer.uid, &request));
    clear_secret(request.password, sizeof(request.password));
    if (!valid) {
        static const char message[] = "Неверный пароль или имя пакета\n";
        (void)write_chunk(client, message, sizeof(message) - 1);
        write_result(client, 1);
        return;
    }

    install_package(client, listener, request.source, request.name, request.data);
    clear_secret(request.data, sizeof(request.data));
}

// Проверить запрет доступа к системному файлу после понижения прав
static bool check_private_file(const char *path) {
    if (setgroups(0, NULL) != 0 || setgid(1001) != 0 || setuid(1001) != 0) {
        return false;
    }

    int descriptor = open(path, O_RDONLY);
    if (descriptor >= 0) {
        close(descriptor);
        return false;
    }

    if (errno != EACCES) {
        return false;
    }

    descriptor = open(path, O_WRONLY | O_TRUNC);
    if (descriptor >= 0) {
        close(descriptor);
        return false;
    }

    if (errno != EACCES) {
        return false;
    }

    return chmod(path, 0666) != 0 && errno == EPERM;
}

// Проверить ограничения ядра до открытия сокета службы
static bool verify_kernel_guarantees(void) {
    char path[sizeof("/var/run/.synapse-store-check-") + 24];
    int length = snprintf(path, sizeof(path), "/var/run/.synapse-store-check-%ld", (long)getpid());
    if (length < 0 || (size_t)length >= sizeof(path)) {
        return false;
    }

    int private_file = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (private_file < 0) {
        return false;
    }

    struct stat attributes;
    bool safe = fstat(private_file, &attributes) == 0 && attributes.st_uid == 0 &&
                (attributes.st_mode & 0777) == 0600;
    close(private_file);
    if (!safe) {
        unlink(path);
        return false;
    }

    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        unlink(path);
        return false;
    }

    struct sockaddr_un address = {.sun_family = AF_UNIX};
    length = snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1,
                      "synapse-store-check-%ld", (long)getpid());
    if (length < 0 || (size_t)length >= sizeof(address.sun_path) - 1) {
        close(listener);
        unlink(path);
        return false;
    }

    socklen_t address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + length);
    if (bind(listener, (struct sockaddr *)&address, address_length) != 0 || listen(listener, 1) != 0) {
        close(listener);
        unlink(path);
        return false;
    }

    pid_t child = fork();
    if (child == 0) {
        int client = socket(AF_UNIX, SOCK_STREAM, 0);
        if (client < 0 || !check_private_file(path) ||
            connect(client, (struct sockaddr *)&address, address_length) != 0) {
            _exit(1);
        }

        close(client);
        _exit(0);
    }

    if (child < 0) {
        close(listener);
        unlink(path);
        return false;
    }

    bool connected = wait_socket(listener, POLLIN, 5000);
    if (connected) {
        int peer_socket = accept(listener, NULL, NULL);
        struct ucred peer = {0};
        socklen_t peer_length = sizeof(peer);
        connected = peer_socket >= 0 &&
                    getsockopt(peer_socket, SOL_SOCKET, SO_PEERCRED, &peer, &peer_length) == 0 &&
                    peer_length == sizeof(peer) && peer.pid == child &&
                    peer.uid == 1001 && peer.gid == 1001;
        if (peer_socket >= 0) {
            close(peer_socket);
        }
    }

    if (!connected) {
        kill(child, SIGKILL);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);

    close(listener);
    unlink(path);
    return connected && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Открыть сокет службы только в системном каталоге
static int open_listener(void) {
    struct stat attributes;
    if (lstat("/var/run", &attributes) != 0 || !S_ISDIR(attributes.st_mode) ||
        attributes.st_uid != 0 || (attributes.st_mode & 0022) != 0) {
        return -1;
    }

    if (!verify_kernel_guarantees()) {
        errno = EPERM;
        return -1;
    }

    if (lstat(STORE_SOCKET_PATH, &attributes) == 0) {
        if (!S_ISSOCK(attributes.st_mode) || attributes.st_uid != 0 ||
            unlink(STORE_SOCKET_PATH) != 0) {
            return -1;
        }
    } else if (errno != ENOENT) {
        return -1;
    }

    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        return -1;
    }

    struct sockaddr_un address = {.sun_family = AF_UNIX};
    memcpy(address.sun_path, STORE_SOCKET_PATH, sizeof(STORE_SOCKET_PATH));
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(STORE_SOCKET_PATH, 0666) != 0 || listen(listener, 4) != 0) {
        close(listener);
        unlink(STORE_SOCKET_PATH);
        return -1;
    }

    return listener;
}

// Слушать локальные запросы до остановки службы
int main(void) {
    if (geteuid() != 0) {
        fputs("synapse-store-helper: root required\n", stderr);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    umask(0077);
    int listener = open_listener();
    if (listener < 0) {
        perror("synapse-store-helper: socket");
        return 1;
    }

    for (;;) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }

            break;
        }

        handle_client(client, listener);
        close(client);
    }

    close(listener);
    unlink(STORE_SOCKET_PATH);
    return 1;
}
