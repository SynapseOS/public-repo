// gxx-wrapper.c: Запуск нативного компилятора C++ с путями SynapseOS
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: synapse-x86_64-v1
// Файл: gxx-wrapper.c, подсистема: сборка пакетов
// Первое изменение: 22.10.2026, последнее изменение: 22.10.2026
// Статус доступа: общедоступный

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Передать аргументы пользователя компилятору C++ с системными путями
int main(int argc, char **argv) {
    static const char *const defaults[] = {
        "/usr/libexec/gcc/x86_64-linux-musl/14.3.0/g++",
        "-B/usr/libexec/gcc/x86_64-linux-musl/14.3.0/",
        "-B/usr/lib/gcc/x86_64-linux-musl/14.3.0/",
        "-B/usr/lib/",
        "-L/usr/lib/gcc/x86_64-linux-musl/14.3.0",
        "-L/usr/lib",
        "-specs=/usr/lib/gcc/x86_64-linux-musl/14.3.0/specs",
        "-isystem",
        "/usr/include/c++/14.3.0",
        "-isystem",
        "/usr/include/c++/14.3.0/x86_64-linux-musl",
        "-static",
        "-no-pie",
        "-fno-pie",
        "-fno-stack-protector",
        "-fno-use-linker-plugin",
        "-mno-red-zone",
    };
    size_t count = sizeof defaults / sizeof defaults[0];

    if (argc < 1 || (size_t)argc > SIZE_MAX / sizeof(char *) - count) {
        fputs("g++: invalid argument count\n", stderr);
        return 1;
    }

    char **args = calloc(count + (size_t)argc, sizeof *args);
    if (args == NULL) {
        perror("g++");
        return 1;
    }

    for (size_t i = 0; i < count; ++i) {
        args[i] = (char *)defaults[i];
    }

    for (int i = 1; i < argc; ++i) {
        args[count + (size_t)i - 1] = argv[i];
    }

    execv(args[0], args);
    fprintf(stderr, "g++: %s: %s\n", args[0], strerror(errno));
    free(args);
    return 1;
}
