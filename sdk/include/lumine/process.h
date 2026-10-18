/*
    process.h: Структуры снимка процессов
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: Linux (ELF64)

    Описание: Структуры снимка процессов, используемые ядром и приложениями
    Подсистема: общий интерфейс ядра и приложений
    Первое изменение: 11.10.2026
    Последнее изменение: 18.10.2026
*/

#ifndef LUMINE_PROCESS_H
#define LUMINE_PROCESS_H

#include <stdint.h>

#define LUMINE_PROCESS_SNAPSHOT 101008
#define LUMINE_PROCESS_SNAPSHOT_V2 101009
#define LUMINE_PROCESS_SNAPSHOT_V3 101010

// Версия 1 сохранена для уже собранных программ
struct lumine_process_info {
	int64_t pid;
	uint32_t state;
	uint32_t threads;
	uint64_t native_calls;
	uint64_t posix_calls;
	char name[256];
};

// Версия 2 запрашивается через LUMINE_PROCESS_SNAPSHOT_V2
// Третий аргумент равен sizeof(struct lumine_process_info_v2)
// Общие страницы учитываются в resident_bytes каждого процесса
struct lumine_process_info_v2 {
	struct lumine_process_info base;
	uint64_t cpu_us;
	uint64_t resident_bytes;
};

// Версия 3 дополняет снимок количеством физических байт контекста потоков
// kernel_bytes учитывает ядерные и PF стеки и состояние FPU каждого потока
// Общая память ядра, драйверов и кэшей не приписывается процессам
struct lumine_process_info_v3 {
	struct lumine_process_info_v2 base;
	uint64_t kernel_bytes;
};

#endif
