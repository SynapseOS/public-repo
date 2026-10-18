// client-timers.inc: таймеры вспомогательных клиентов Weston
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl и POSIX
// Подсистема: графические клиенты Weston
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT

struct synapse_toy_heap {
	struct synapse_timer *synapse_timer;

	struct {
		int fd;
	} base;
};

#define wl_timer_heap synapse_toy_heap
#include "wayland-timers.inc"
#undef wl_timer_heap

// Перевести относительный срок клиента в монотонное абсолютное время
static void synapse_toy_arm(struct toytimer *timer, const struct itimerspec *value) {
	struct timespec deadline = { 0, 0 };
	timer->synapse_interval = value->it_interval;

	if (value->it_value.tv_sec != 0 || value->it_value.tv_nsec != 0) {
		clock_gettime(CLOCK_MONOTONIC, &deadline);
		deadline.tv_sec += value->it_value.tv_sec;
		deadline.tv_nsec += value->it_value.tv_nsec;
		deadline.tv_sec += deadline.tv_nsec / 1000000000L;
		deadline.tv_nsec %= 1000000000L;
	}

	if (synapse_timer_update(timer->synapse, deadline) != 0) {
		fprintf(stderr, "client timer setup failed: %s\n", strerror(errno));
		abort( );
	}
}
