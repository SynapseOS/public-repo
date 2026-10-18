/*
    orbit.h: Формат очередей и запросов Orbit
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: Linux (ELF64)

    Описание: Формат очередей и запросов Orbit
    Подсистема: асинхронные запросы Orbit
    Первое изменение: 13.10.2026
    Последнее изменение: 18.10.2026
*/

#ifndef SYNAPSE_ORBIT_H
#define SYNAPSE_ORBIT_H

#include <stdint.h>
#include <stddef.h>

// Формат очереди использует поля фиксированной ширины без указателей
// Время задаётся в микросекундах монотонных часов
#define SYN_ORBIT_VERSION 1u
#define SYN_ORBIT_MAX_ENTRIES 256u
#define SYN_ORBIT_MAX_CAPS 128u
#define SYN_ORBIT_MAX_BUFFER (16u * 1024u * 1024u)
#define SYS_SYN_ORBIT_CREATE 107000u
#define SYS_SYN_ORBIT_ENTER 107001u
#define SYS_SYN_ORBIT_CONTROL 107002u
#define SYS_SYN_ORBIT_MAP 107003u
#define SYS_SYN_ORBIT_UNMAP 107004u
#define SYS_SYN_ORBIT_CLOCK 107005u
#define SYS_SYN_ORBIT_CLOSE 107006u

typedef uint64_t syn_cap_t;

enum syn_rights {
	SYN_RIGHT_READ = 1u,
	SYN_RIGHT_WRITE = 2u,
	SYN_RIGHT_MAP = 4u,
	SYN_RIGHT_STAT = 8u,
	SYN_RIGHT_WAIT = 16u,
	SYN_RIGHT_ALL = 31u
};

enum syn_opcode {
	SYN_OP_NOP,
	SYN_OP_READ,
	SYN_OP_WRITE,
	SYN_OP_TIMER,
	SYN_OP_POLL,
	SYN_OP_PROCESS_WAIT,
	SYN_OP_STAT,
	SYN_OP_CLOCK
};

// Коды состояния Orbit не зависят от значений errno библиотеки libc
enum syn_status {
	SYN_OK = 0,
	SYN_EPERM = 1,
	SYN_ENOENT = 2,
	SYN_ESRCH = 3,
	SYN_EINTR = 4,
	SYN_EIO = 5,
	SYN_EBADF = 9,
	SYN_EAGAIN = 11,
	SYN_ENOMEM = 12,
	SYN_EACCES = 13,
	SYN_EFAULT = 14,
	SYN_EBUSY = 16,
	SYN_EEXIST = 17,
	SYN_EINVAL = 22,
	SYN_EMFILE = 24,
	SYN_ENOSPC = 28,
	SYN_ESPIPE = 29,
	SYN_ENOSYS = 38,
	SYN_EOVERFLOW = 75,
	SYN_EOPNOTSUPP = 95,
	SYN_ETIMEDOUT = 110,
	SYN_ECANCELED = 125
};

enum syn_control_cmd {
	SYN_CTL_IMPORT,
	SYN_CTL_RESTRICT,
	SYN_CTL_DROP,
	SYN_CTL_BUFFER,
	SYN_CTL_SCOPE,
	SYN_CTL_CANCEL_SCOPE,
	SYN_CTL_CANCEL_ID,
	SYN_CTL_PROCESS,
	SYN_CTL_CHANNEL,
	SYN_CTL_OPEN,
	SYN_CTL_STATS
};

#define SYN_POLL_READ 1u
#define SYN_POLL_WRITE 4u
#define SYN_CQE_TERMINAL 1u
#define SYN_CQE_BUFFER_RELEASED 2u

struct syn_request {
	uint64_t id;
	syn_cap_t object;
	uint32_t opcode, flags; // В версии 1 flags должно быть нулевым
	syn_cap_t buffer;
	uint64_t buffer_offset, offset, length, deadline;
	syn_cap_t scope;
};

struct syn_completion {
	uint64_t id;
	int64_t result;
	uint32_t status, flags;
	uint64_t extra;
};

// Счётчики переходят через ноль по модулю 2^32
// Приложение меняет sq_tail и cq_head, ядро меняет остальные счётчики
// Записи публикуются с release, читаются с acquire
// Для контекста допустимы один отправитель и один получатель в приложении
// Общий доступ нескольких потоков требует внешней синхронизации
// Ядро проверяет сведения очереди, полученные от приложения
struct syn_ring {
	uint32_t version, entries, sq_offset, cq_offset;
	uint64_t features, bytes;
	uint8_t reserved0[32];
	uint32_t sq_head;
	uint8_t reserved1[60];
	uint32_t sq_tail;
	uint8_t reserved2[60];
	uint32_t cq_head, cq_tail;
	uint8_t reserved3[56];
};

struct syn_setup {
	uint32_t version, entries;
	uint64_t ring_address, ring_bytes, features;
};

// Входные поля: command, rights, object, value, address, length
// Результат записывается в object, address, length и fd
// Неиспользуемые входные поля должны быть нулевыми
// OPEN принимает в address путь с завершающим нулём
// Для OPEN value задаёт флаги открытия, length задаёт режим создания
struct syn_control {
	uint32_t command, rights;
	syn_cap_t object;
	uint64_t value, address, length;
	int64_t fd;
};

struct syn_mapping {
	syn_cap_t object; // Ноль задаёт анонимную память
	uint64_t length, offset, address;
	uint32_t prot, flags;
};

#define SYN_PROT_READ 1u
#define SYN_PROT_WRITE 2u
#define SYN_MAP_SHARED 1u
#define SYN_MAP_PRIVATE 2u

struct syn_orbit_stats {
	uint64_t accepted, completed, enter_calls, bytes_read, bytes_written;
	uint64_t pending, cq_backpressure, worker_rounds;
};

#ifdef __cplusplus
#define SYN_ABI_ASSERT static_assert
#else
#define SYN_ABI_ASSERT _Static_assert
#endif

SYN_ABI_ASSERT(sizeof(struct syn_request) == 72, "Orbit request ABI");
SYN_ABI_ASSERT(sizeof(struct syn_completion) == 32, "Orbit completion ABI");
SYN_ABI_ASSERT(sizeof(struct syn_ring) == 256, "Orbit ring ABI");
SYN_ABI_ASSERT(sizeof(struct syn_control) == 48, "Orbit control ABI");

#undef SYN_ABI_ASSERT

#endif
