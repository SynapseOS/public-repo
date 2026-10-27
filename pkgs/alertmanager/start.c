/*
    start.c: Запуск Alertmanager с локальными настройками
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, ABI: Linux amd64 ABI

    Описание: Передаёт серверу параметры без ограничений системной оболочки
    Подсистема: мониторинг
    Первое изменение: 23.10.2026
    Последнее изменение: 23.10.2026
    Доступ: общедоступный
*/

#include <stdio.h>
#include <unistd.h>

// Запустить одиночный Alertmanager только на локальном интерфейсе
int main(void) {
    char *arguments[] = {
        "/bin/alertmanager",
        "--config.file=/etc/alertmanager/alertmanager.yml",
        "--storage.path=/var/lib/alertmanager",
        "--web.listen-address=127.0.0.1:9093",
        "--cluster.listen-address=",
        NULL
    };

    execv(arguments[0], arguments);
    perror("alertmanager-start: execv");
    return 1;
}
