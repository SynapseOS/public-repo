// menu-toggle.c: Быстрое переключение меню рабочего стола
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: synapse-x86_64-v1
// Описание: Передать команду уже работающему меню без загрузки Qt
// Подсистема: рабочий стол
// Первое изменение: 30.10.2026
// Последнее изменение: 30.10.2026
// Доступ: общедоступный

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// Переключить меню через сокет текущего пользовательского сеанса
static int toggle_menu(void) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime == NULL || runtime[0] == '\0') {
        return -1;
    }

    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    int length = snprintf(address.sun_path, sizeof(address.sun_path),
                          "%s/synapse-desktop-menu", runtime);
    if (length < 0 || (size_t)length >= sizeof(address.sun_path)) {
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int result = -1;
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0) {
        const char command = 'T';
        if (send(fd, &command, sizeof(command), MSG_NOSIGNAL) == 1) {
            result = 0;
        }
    }

    close(fd);
    return result;
}

// Запустить меню отдельно, если служба ещё не успела открыть сокет
int main(void) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime != NULL && runtime[0] != '\0') {
        for (int attempt = 0; attempt < 10; attempt++) {
            if (toggle_menu() == 0) {
                return 0;
            }

            if (attempt < 9) {
                const struct timespec delay = {0, 10000000};
                nanosleep(&delay, NULL);
            }
        }
    }

    execl("/bin/synapse-desktop", "synapse-desktop", (char *)NULL);
    return 1;
}
