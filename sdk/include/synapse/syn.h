/*
    syn.h: Пользовательский интерфейс Orbit
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: Linux (ELF64)

    Описание: Пользовательский интерфейс Orbit
    Подсистема: асинхронные запросы Orbit
    Первое изменение: 13.10.2026
    Последнее изменение: 18.10.2026
*/

#ifndef SYNAPSE_SYN_H
#define SYNAPSE_SYN_H

#include <synapse/orbit.h>

#ifdef __cplusplus
extern "C" {
#endif

// Функции возвращают неотрицательный результат либо отрицательный код SYN_E
// Значение errno не меняется
struct syn_orbit {
	int fd;
	uint32_t entries, outstanding;
	size_t bytes;
	uint64_t next_id;
	struct syn_ring *ring;
	struct syn_request *sq;
	struct syn_completion *cq;
};

struct syn_buffer {
	syn_cap_t cap;
	void *data;
	size_t bytes;
};

// Создать контекст и очереди указанной вместимости
int syn_orbit_create(struct syn_orbit *, unsigned entries);
// Освободить контекст и его очереди
int syn_orbit_destroy(struct syn_orbit *);
// Поместить запрос в очередь отправки
int syn_orbit_submit(struct syn_orbit *, const struct syn_request *);
// Передать запросы ядру и ждать минимального числа результатов
int syn_orbit_enter(struct syn_orbit *, unsigned minimum, uint64_t deadline);
// Получить следующий результат из очереди завершений
int syn_orbit_next(struct syn_orbit *, struct syn_completion *);
// Синхронный вызов требует контекст без других незавершённых запросов
// Отправить запрос и дождаться его результата
int syn_orbit_call(struct syn_orbit *, struct syn_request *, struct syn_completion *);
// Выполнить управляющий запрос контекста
int syn_orbit_control(struct syn_orbit *, struct syn_control *);
// Импортировать файловый дескриптор с указанными правами
int syn_cap_import(struct syn_orbit *, int fd, unsigned rights, syn_cap_t *);
// Создать ограниченную копию ссылки на объект
int syn_cap_restrict(struct syn_orbit *, syn_cap_t, unsigned rights, syn_cap_t *);
// Освободить ссылку на объект
int syn_cap_drop(struct syn_orbit *, syn_cap_t);
// Открыть путь и получить ссылку с указанными правами
int syn_open(struct syn_orbit *, const char *path, unsigned flags, unsigned mode, unsigned rights,
             syn_cap_t *);
// Создать буфер запросов указанного размера
int syn_buffer_create(struct syn_orbit *, size_t bytes, struct syn_buffer *);
// Освободить буфер запросов
int syn_buffer_destroy(struct syn_orbit *, struct syn_buffer *);
// Создать группу отмены запросов
int syn_scope_create(struct syn_orbit *, syn_cap_t *);
// Отменить запросы группы
int syn_scope_cancel(struct syn_orbit *, syn_cap_t);
// Отменить запрос по идентификатору
int syn_request_cancel(struct syn_orbit *, uint64_t id);
// Получить ссылку на процесс для ожидания завершения
int syn_process_import(struct syn_orbit *, int64_t pid, syn_cap_t *);
// Возвращённый fd можно передать через fork и импортировать
// Для закрытия используется syn_close
// Создать канал и вернуть ссылку и файловый дескриптор
int syn_channel_create(struct syn_orbit *, syn_cap_t *, int *fd);
// Получить счётчики работы контекста
int syn_orbit_stats(struct syn_orbit *, struct syn_orbit_stats *);
// Отобразить объект или анонимную память в адресное пространство
void *syn_memory_map(struct syn_orbit *, syn_cap_t object, size_t bytes, uint64_t offset,
                     unsigned prot, unsigned flags);
// Удалить отображение памяти
int syn_memory_unmap(void *, size_t);
// Закрыть файловый дескриптор
int syn_close(int fd);
// Прочитать монотонное время в микросекундах
uint64_t syn_clock_us(void);

#ifdef __cplusplus
}
#endif

#endif
