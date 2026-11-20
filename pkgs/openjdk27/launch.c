// launch.c: запуск выбранного инструмента OpenJDK 27
// Автор: Елчинян Арен Артурович, 2026, x86_64, ABI Linux и musl
// Подсистема: пакет OpenJDK 27
// Первое изменение: 20.11.2026, последнее изменение: 20.11.2026
// Статус доступа: общедоступный
#include <stdio.h>
#include <unistd.h>

// Передать исходные аргументы инструменту по его пути внутри JDK
int main(int argc, char **argv) {
    (void)argc;

    argv[0] = JAVA_TOOL;
    execv(JAVA_TOOL, argv);
    perror(JAVA_TOOL);
    return 127;
}
