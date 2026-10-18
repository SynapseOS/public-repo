// wayland-timers.inc: таймер Wayland через поток и неблокирующий pipe
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl и POSIX
// Подсистема: цикл событий Wayland
// Первое изменение: 17.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT
#include <pthread.h>

struct synapse_timer {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	pthread_t thread;
	int pipe[2];
	bool stopped;
	bool armed;
	struct timespec deadline;
};

// Ждать ближайший срок и сообщать готовность через pipe
static void *synapse_timer_thread(void *data) {
	struct synapse_timer *timer = data;
	sigset_t mask;
	sigfillset(&mask);
	pthread_sigmask(SIG_BLOCK, &mask, NULL);
	pthread_mutex_lock(&timer->mutex);

	while (!timer->stopped) {
		if (!timer->armed) {
			pthread_cond_wait(&timer->condition, &timer->mutex);
			continue;
		}

		struct timespec deadline = timer->deadline;
		int result = pthread_cond_timedwait(&timer->condition, &timer->mutex, &deadline);
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);

		if (result == ETIMEDOUT && timer->armed &&
		    (now.tv_sec > timer->deadline.tv_sec ||
		     (now.tv_sec == timer->deadline.tv_sec && now.tv_nsec >= timer->deadline.tv_nsec))) {
			timer->armed = false;
			uint64_t expired = 1;
			ssize_t written = write(timer->pipe[1], &expired, sizeof(expired));

			if (written < 0 && errno != EAGAIN && errno != EINTR) {
				timer->stopped = true;
			}
		}
	}

	pthread_mutex_unlock(&timer->mutex);
	return NULL;
}

// Создать один обслуживающий поток на цикл событий
static struct synapse_timer *synapse_timer_create(void) {
	struct synapse_timer *timer = calloc(1, sizeof(*timer));

	if (timer == NULL) {
		return NULL;
	}

	if (pipe2(timer->pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
		free(timer);
		return NULL;
	}

	pthread_condattr_t attributes;
	int result = pthread_mutex_init(&timer->mutex, NULL);

	if (result != 0) {
		goto fail_pipe;
	}

	result = pthread_condattr_init(&attributes);

	if (result != 0) {
		goto fail_mutex;
	}

	result = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);

	if (result == 0) {
		result = pthread_cond_init(&timer->condition, &attributes);
	}

	pthread_condattr_destroy(&attributes);

	if (result != 0) {
		goto fail_mutex;
	}

	// Рабочий поток наследует заблокированные сигналы до первого пользовательского кода
	sigset_t mask;
	sigset_t previous;
	sigfillset(&mask);
	pthread_sigmask(SIG_BLOCK, &mask, &previous);
	result = pthread_create(&timer->thread, NULL, synapse_timer_thread, timer);
	pthread_sigmask(SIG_SETMASK, &previous, NULL);

	if (result == 0) {
		return timer;
	}

	pthread_cond_destroy(&timer->condition);
fail_mutex:
	pthread_mutex_destroy(&timer->mutex);
fail_pipe:
	close(timer->pipe[0]);
	close(timer->pipe[1]);
	free(timer);
	errno = result;
	return NULL;
}

// Изменить срок, сбросив уже доставленное уведомление
static int synapse_timer_update(struct wl_timer_heap *heap, struct timespec deadline) {
	struct synapse_timer *timer = heap->synapse_timer;

	if (timer == NULL) {
		errno = EINVAL;
		return -1;
	}

	pthread_mutex_lock(&timer->mutex);
	uint64_t expired;

	while (read(timer->pipe[0], &expired, sizeof(expired)) > 0) {
	}

	timer->deadline = deadline;
	timer->armed = deadline.tv_sec != 0 || deadline.tv_nsec != 0;
	pthread_cond_signal(&timer->condition);
	pthread_mutex_unlock(&timer->mutex);
	return 0;
}

// Остановить поток до закрытия его дескрипторов
static void synapse_timer_destroy(struct wl_timer_heap *heap) {
	struct synapse_timer *timer = heap->synapse_timer;

	if (timer == NULL) {
		return;
	}

	pthread_mutex_lock(&timer->mutex);
	timer->stopped = true;
	pthread_cond_signal(&timer->condition);
	pthread_mutex_unlock(&timer->mutex);
	pthread_join(timer->thread, NULL);
	close(timer->pipe[0]);
	close(timer->pipe[1]);
	pthread_cond_destroy(&timer->condition);
	pthread_mutex_destroy(&timer->mutex);
	free(timer);
	heap->synapse_timer = NULL;
	heap->base.fd = -1;
}
