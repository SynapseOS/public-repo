/*
    store_control.c: Клиент системной службы установки программ
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI: synapse-x86_64-v1

    Описание: Передаёт имя пакета и пароль root по проверенному локальному сокету
    Подсистема: графический магазин программ
    Первое изменение: 01.11.2026
    Последнее изменение: 23.11.2026
    Доступ: общедоступный
*/

#define _GNU_SOURCE
#include "store_protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

// Очистить пароль после передачи службе
static void clear_secret(char *data, size_t length) {
    volatile char *secret = (volatile char *)data;
    for (size_t index = 0; index < length; index++) {
        secret[index] = 0;
    }
}

// Проверить имя до отправки привилегированной службе
static bool valid_name(const char *name) {
    size_t length = strlen(name);
    if (length == 0 || length >= STORE_MAX_NAME) {
        return false;
    }

    for (size_t index = 0; index < length; index++) {
        unsigned char character = (unsigned char)name[index];
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

// Передать заданное число байтов с учётом короткой записи
static bool send_all(int socket_fd, const void *data, size_t length) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t done = 0;
    while (done < length) {
        ssize_t count = send(socket_fd, bytes + done, length - done, 0);
        if (count <= 0) {
            return false;
        }

        done += (size_t)count;
    }

    return true;
}

// Прочитать заданное число байтов ответа службы
static bool read_all(int socket_fd, void *data, size_t length) {
    unsigned char *bytes = (unsigned char *)data;
    size_t done = 0;
    while (done < length) {
        struct pollfd item = {.fd = socket_fd, .events = POLLIN, .revents = 0};
        int ready;
        do {
            ready = poll(&item, 1, 300000);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0 || (item.revents & POLLIN) == 0) {
            return false;
        }

        ssize_t count = recv(socket_fd, bytes + done, length - done, 0);
        if (count <= 0) {
            return false;
        }

        done += (size_t)count;
    }

    return true;
}

// Подключиться только к сокету root-службы
static int connect_service(void) {
    struct stat attributes;
    if (lstat(STORE_SOCKET_PATH, &attributes) != 0 || !S_ISSOCK(attributes.st_mode) ||
        attributes.st_uid != 0) {
        return -1;
    }

    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return -1;
    }

    struct sockaddr_un address = {.sun_family = AF_UNIX};
    memcpy(address.sun_path, STORE_SOCKET_PATH, sizeof(STORE_SOCKET_PATH));
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(socket_fd);
        return -1;
    }

    struct ucred peer = {0};
    socklen_t peer_length = sizeof(peer);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_length) != 0 ||
        peer_length != sizeof(peer) || peer.uid != 0) {
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

// Прочитать пароль из stdin без передачи в аргументах процесса
static bool read_password(struct store_request *request) {
    if (geteuid() == 0) {
        return true;
    }

    if (fgets(request->password, sizeof(request->password), stdin) == NULL) {
        return false;
    }

    size_t length = strlen(request->password);
    if (length == 0 || request->password[length - 1] != '\n') {
        return false;
    }

    request->password[--length] = 0;
    request->password_length = (uint32_t)length;
    return length > 0;
}

// Прочитать данные операции после пароля администратора
static bool read_data(struct store_request *request, bool required) {
    if (!fgets(request->data, sizeof(request->data), stdin)) {
        return !required;
    }
    size_t length = strlen(request->data);
    if (length == 0 || request->data[length - 1] != '\n') {
        return false;
    }
    request->data[--length] = 0;
    request->data_length = (uint32_t)length;
    return true;
}

// Выполнить запрос установки, настройки или обслуживания системы
int main(int argc, char **argv) {
    uint32_t source = STORE_SOURCE_SETUP;
    const char *name = NULL;
    bool data_stdin = false;
    if (argc == 2 && strcmp(argv[1], "--update") == 0) {
        source = STORE_SOURCE_UPDATE;
        name = "";
    } else if (argc == 2 && strcmp(argv[1], "--upgrade") == 0) {
        source = STORE_SOURCE_UPGRADE;
        name = "";
    } else if (argc == 2 && strcmp(argv[1], "--debian-catalog") == 0) {
        source = STORE_SOURCE_DEBIAN_CATALOG;
        name = "";
    } else if (argc == 3 && strcmp(argv[1], "--remove") == 0) {
        source = STORE_SOURCE_SETUP_REMOVE;
        name = argv[2];
    } else if (argc == 3 && strcmp(argv[1], "--debian-remove") == 0) {
        source = STORE_SOURCE_DEBIAN_REMOVE;
        name = argv[2];
    } else if (argc == 3 && strcmp(argv[1], "--setting") == 0) {
        source = STORE_SOURCE_SETTINGS;
        name = argv[2];
    } else if (argc == 3 && strcmp(argv[1], "--power-profile") == 0) {
        source = STORE_SOURCE_POWER;
        name = argv[2];
    } else if (argc == 4 && strcmp(argv[1], "--useradd") == 0) {
        source = STORE_SOURCE_USERADD;
        name = argv[2];
    } else if (argc == 4 && strcmp(argv[1], "--usermod-shell") == 0) {
        source = STORE_SOURCE_USERMOD;
        name = argv[2];
    } else if (argc == 3 && strcmp(argv[1], "--passwd") == 0) {
        source = STORE_SOURCE_PASSWD;
        name = argv[2];
        data_stdin = true;
    } else if (argc >= 3 && strcmp(argv[1], "--setup") == 0) {
        source = STORE_SOURCE_ACCOUNT_SETUP;
        static char setup_name[STORE_MAX_NAME];
        size_t used = 0;
        for (int index = 2; index < argc; ++index) {
            size_t length = strlen(argv[index]);
            if (used + length + 1 >= sizeof(setup_name)) {
                return 2;
            }
            if (index > 2) {
                setup_name[used++] = ' ';
            }
            memcpy(setup_name + used, argv[index], length);
            used += length;
        }
        setup_name[used] = 0;
        name = setup_name;
        data_stdin = true;
    } else if (argc == 2) {
        name = argv[1];
    } else if (argc == 3 && strcmp(argv[1], "--debian") == 0) {
        source = STORE_SOURCE_DEBIAN;
        name = argv[2];
    }

    bool catalog = source == STORE_SOURCE_DEBIAN_CATALOG || source == STORE_SOURCE_UPDATE ||
                   source == STORE_SOURCE_UPGRADE;
    if (name == NULL || (source <= STORE_SOURCE_UPGRADE && !catalog && !valid_name(name))) {
        fputs("usage: synapse-store-control [--debian] PACKAGE | --remove PACKAGE | --upgrade | --update | --debian-catalog\n", stderr);
        return 2;
    }

    int socket_fd = connect_service();
    if (socket_fd < 0) {
        fputs("Служба установки SynapseOS недоступна\n", stderr);
        return 1;
    }

    struct store_request request = {0};
    request.magic = STORE_REQUEST_MAGIC;
    request.version = STORE_REQUEST_VERSION;
    request.source = source;
    request.name_length = (uint32_t)strlen(name);
    memcpy(request.name, name, request.name_length + 1);
    if (source == STORE_SOURCE_USERADD || source == STORE_SOURCE_USERMOD) {
        if (source == STORE_SOURCE_USERADD) {
            if (argc != 4) {
                close(socket_fd);
                return 2;
            }
            snprintf(request.data, sizeof(request.data), "%s", argv[3]);
            request.data_length = (uint32_t)strlen(request.data);
        } else {
            snprintf(request.data, sizeof(request.data), "%s", argv[3]);
            request.data_length = (uint32_t)strlen(request.data);
        }
    }
    if (!read_password(&request) || (data_stdin && !read_data(&request, source == STORE_SOURCE_PASSWD))) {
        clear_secret(request.password, sizeof(request.password));
        clear_secret(request.data, sizeof(request.data));
        close(socket_fd);
        fputs("Не удалось прочитать пароль администратора или данные операции\n", stderr);
        return 1;
    }

    bool sent = send_all(socket_fd, &request, sizeof(request));
    clear_secret(request.password, sizeof(request.password));
    clear_secret(request.data, sizeof(request.data));
    if (!sent) {
        close(socket_fd);
        fputs("Не удалось передать запрос службе\n", stderr);
        return 1;
    }

    for (;;) {
        uint32_t length = 0;
        if (!read_all(socket_fd, &length, sizeof(length)) || length > STORE_MAX_OUTPUT) {
            break;
        }

        if (length == 0) {
            int32_t result = 1;
            bool complete = read_all(socket_fd, &result, sizeof(result));
            close(socket_fd);
            return complete && result >= 0 && result <= 255 ? result : 1;
        }

        char output[STORE_MAX_OUTPUT];
        if (!read_all(socket_fd, output, length) || fwrite(output, 1, length, stdout) != length) {
            break;
        }

        fflush(stdout);
    }

    close(socket_fd);
    fputs("Соединение со службой установки прервано\n", stderr);
    return 1;
}
