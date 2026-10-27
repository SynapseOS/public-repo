// netstat.c: Просмотр сетевых интерфейсов
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Получение адресов и состояния устройств через SynAPI
// Подсистема: пользовательские системные утилиты
// Первое изменение в истории Git: 11.10.2026
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    int listeners = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) {
            listeners = 1;
        } else if (strcmp(argv[i], "-a") && strcmp(argv[i], "-n")) {
            fprintf(stderr, "usage: netstat [-l] [-n]\n");
            return 1;
        }
    }

    FILE *file = fopen("/dev/netstat", "r");
    if (!file) {
        perror("netstat: /dev/netstat");
        return 1;
    }

    char *line = NULL;
    size_t capacity = 0;
    int first = 1;
    while (getline(&line, &capacity, file) >= 0) {
        if (first || !listeners || strstr(line, "\tLISTEN\n") ||
            strstr(line, "\tUNCONN\n")) {
            fputs(line, stdout);
        }
        first = 0;
    }

    int error = ferror(file);
    free(line);
    fclose(file);
    if (error) {
        perror("netstat: read");
        return 1;
    }
    return 0;
}
