// launch.c: Запуск компилятора Go с настройками SynapseOS
// Автор: Елчинян Арен Артурович, 2026, x86_64, Linux amd64 ABI
// Подсистема: Go, первое изменение: 22.10.2026, последнее изменение: 22.10.2026
// Статус доступа: общедоступный
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Сохранить явные настройки пользователя и запустить установленный Go
int main(int argc, char **argv) {
    (void)argc;

    if (setenv("GOROOT", "/usr/lib/go", 0) != 0 ||
        setenv("CGO_ENABLED", "0", 0) != 0 ||
        setenv("GOTOOLCHAIN", "local", 0) != 0) {
        perror("go: environment");
        return 1;
    }

    execv("/usr/lib/go/bin/go", argv);
    perror("go: exec");
    return 127;
}
