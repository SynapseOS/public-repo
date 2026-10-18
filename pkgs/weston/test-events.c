// test-events.c: проверка POSIX-адаптации событий Wayland на хосте
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: POSIX
// Подсистема: проверки Wayland
// Первое изменение: 17.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

struct synapse_timer;

struct wl_timer_heap {
	struct synapse_timer *synapse_timer;

	struct {
		int fd;
	} base;
};

struct wl_event_source_signal {
	struct {
		int fd;
	} base;

	int signal_number;
	int write_fd;
	unsigned synapse_slot;
};

#include "wayland-timers.inc"
#include "wayland-signals.inc"

// Получить срок в будущем по монотонным часам
static struct timespec deadline(int milliseconds) {
	struct timespec value;
	assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
	value.tv_nsec += (long)milliseconds * 1000000L;
	value.tv_sec += value.tv_nsec / 1000000000L;
	value.tv_nsec %= 1000000000L;
	return value;
}

// Проверить готовность таймера, перевзвод и отсутствие уведомления после отмены
static void check_timer(void) {
	for (unsigned i = 0; i < 32; i++) {
		struct wl_timer_heap heap = { .synapse_timer = synapse_timer_create( ) };
		assert(heap.synapse_timer != NULL);
		heap.base.fd = heap.synapse_timer->pipe[0];
		struct pollfd event = { .fd = heap.base.fd, .events = POLLIN };
		assert(synapse_timer_update(&heap, deadline(1000)) == 0);
		assert(synapse_timer_update(&heap, deadline(5)) == 0);
		assert(poll(&event, 1, 2000) == 1 && (event.revents & POLLIN) != 0);
		assert(synapse_timer_update(&heap, (struct timespec){ 0, 0 }) == 0);
		assert(poll(&event, 1, 10) == 0);
		assert(synapse_timer_update(&heap, deadline(1000)) == 0);
		synapse_timer_destroy(&heap);
		assert(heap.synapse_timer == NULL && heap.base.fd == -1);
	}

	puts("WAYLAND_TIMER_REARM_CANCEL_DESTROY_PASS");
}

// Проверить доставку сигнала и восстановление прежнего обработчика
static void check_signal(void) {
	struct sigaction ignored = { .sa_handler = SIG_IGN };
	struct sigaction previous;
	sigemptyset(&ignored.sa_mask);
	assert(sigaction(SIGUSR1, &ignored, &previous) == 0);
	struct wl_event_source_signal source = { .signal_number = SIGUSR1 };
	assert(synapse_signal_create(&source) == 0);
	struct wl_event_source_signal duplicate = { .signal_number = SIGUSR1 };
	assert(synapse_signal_create(&duplicate) == 0);
	assert(raise(SIGUSR1) == 0);
	struct pollfd event = { .fd = source.base.fd, .events = POLLIN };
	assert(poll(&event, 1, 1000) == 1);
	struct signalfd_siginfo info;
	assert(read(source.base.fd, &info, sizeof(info)) == sizeof(info));
	assert(info.ssi_signo == SIGUSR1);
	synapse_signal_release(&source);
	synapse_signal_release(&duplicate);
	close(duplicate.base.fd);
	close(source.base.fd);
	struct sigaction restored;
	assert(sigaction(SIGUSR1, NULL, &restored) == 0 && restored.sa_handler == SIG_IGN);
	assert(sigaction(SIGUSR1, &previous, NULL) == 0);
	puts("WAYLAND_SIGNAL_DELIVERY_RESTORE_PASS");
}

// Запустить проверки тех же функций, которые входят в libwayland-server
int main(void) {
	check_timer( );
	check_signal( );
	return 0;
}
