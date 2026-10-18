/*
    replay.h: Формат записи событий ввода
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: Linux (ELF64)

    Описание: Формат записи событий ввода
    Подсистема: общий интерфейс ядра и приложений
    Первое изменение: не установлено по истории Git
    Последнее изменение: 18.10.2026
*/

#ifndef LUMINE_REPLAY_H
#define LUMINE_REPLAY_H

#include <stdint.h>

#define SYN_REPLAY_VERSION 1
#define SYN_REPLAY_START 0x53520001u
#define SYN_REPLAY_STOP 0x53520002u
#define SYN_REPLAY_KEY 1u
#define SYN_REPLAY_POINTER 2u
#define SYN_REPLAY_END 3u

// Формат x86_64 использует little-endian, зарезервированные поля должны быть нулевыми
struct syn_replay_event {
	uint64_t us, total_ram, free_ram;
	uint32_t kind, device, flags, buttons;
	int32_t x, y;
	uint32_t code, extended, pressed, reserved;
};

struct syn_replay_header {
	char magic[8];
	uint32_t version, event_size;
	uint64_t realtime, monotonic;
};
#endif
