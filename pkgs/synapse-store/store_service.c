/*
    store_service.c: Root-служба установки пакетов SynapseOS
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI: synapse-x86_64-v1

    Описание: Проверяет клиента и запускает только установку подписанного пакета через setup
    Подсистема: графический магазин программ
    Первое изменение: 01.11.2026
    Последнее изменение: 01.11.2026
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
    if (request->source == STORE_SOURCE_UPDATE) {
        return length == 0 && request->name[0] == 0;
    }

    if (length == 0 || length >= STORE_MAX_NAME || request->name[length] != 0) {
        return false;
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

// Ограничить установку Debian проверенными пакетами
static bool valid_source(const struct store_request *request) {
    if (request->source == STORE_SOURCE_SETUP || request->source == STORE_SOURCE_UPDATE) {
        return true;
    }

    return request->source == STORE_SOURCE_DEBIAN &&
           (strcmp(request->name, "m4") == 0 || strcmp(request->name, "busybox-static") == 0);
}

// Сверить пароль root, если клиент не root
static bool authenticate(uid_t uid, const struct store_request *request) {
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

// Запустить одну разрешённую команду менеджера и вернуть её вывод клиенту
static void install_package(int client, int listener, uint32_t source, const char *name) {
    int channel[2];
    if (pipe(channel) != 0) {
        write_result(client, 1);
        return;
    }

    pid_t child = fork();
    if (child < 0) {
        close(channel[0]);
        close(channel[1]);
        write_result(client, 1);
        return;
    }

    if (child == 0) {
        close(channel[0]);
        close(listener);
        close(client);
        if (dup2(channel[1], STDOUT_FILENO) < 0 || dup2(channel[1], STDERR_FILENO) < 0) {
            _exit(126);
        }

        close(channel[1]);
        if (source == STORE_SOURCE_UPDATE) {
            execl("/bin/setup", "setup", "update", (char *)NULL);
            dprintf(STDERR_FILENO, "Не удалось обновить каталог setup: %s\n", strerror(errno));
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
        return;
    }

    bool valid = request.magic == STORE_REQUEST_MAGIC &&
                 request.version == STORE_REQUEST_VERSION && valid_name(&request) &&
                 valid_source(&request) &&
                 authenticate(peer.uid, &request);
    clear_secret(request.password, sizeof(request.password));
    if (!valid) {
        static const char message[] = "Неверный пароль или имя пакета\n";
        (void)write_chunk(client, message, sizeof(message) - 1);
        write_result(client, 1);
        return;
    }

    install_package(client, listener, request.source, request.name);
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
    umask(0177);
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
