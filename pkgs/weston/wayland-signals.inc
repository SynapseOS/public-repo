// wayland-signals.inc: доставка сигналов в цикл Wayland через pipe
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl и POSIX
// Подсистема: цикл событий Wayland
// Первое изменение: 17.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT

#define SYNAPSE_SIGNAL_WATCHERS 16
static volatile sig_atomic_t synapse_signal_fds[_NSIG][SYNAPSE_SIGNAL_WATCHERS];
static struct sigaction synapse_signal_actions[_NSIG];
static bool synapse_signal_blocked[_NSIG];

// Передать уведомление каждому наблюдателю без выделения памяти и блокировок
static void synapse_signal_handler(int signal_number) {
	int previous_errno = errno;

	for (unsigned i = 0; i < SYNAPSE_SIGNAL_WATCHERS; i++) {
		int fd = synapse_signal_fds[signal_number][i] - 1;

		if (fd >= 0) {
			struct signalfd_siginfo info = { 0 };
			info.ssi_signo = (uint32_t)signal_number;
			ssize_t result = write(fd, &info, sizeof(info));
			(void)result;
		}
	}

	errno = previous_errno;
}

// Восстановить обработчик после удаления последнего наблюдателя
static void synapse_signal_release(struct wl_event_source_signal *source) {
	int number = source->signal_number;
	sigset_t mask;
	sigset_t previous;
	sigemptyset(&mask);
	sigaddset(&mask, number);
	sigprocmask(SIG_BLOCK, &mask, &previous);
	synapse_signal_fds[number][source->synapse_slot] = 0;
	bool remaining = false;

	for (unsigned i = 0; i < SYNAPSE_SIGNAL_WATCHERS; i++) {
		if (synapse_signal_fds[number][i] != 0) {
			remaining = true;
		}
	}

	if (!remaining) {
		sigaction(number, &synapse_signal_actions[number], NULL);

		if (synapse_signal_blocked[number]) {
			sigaddset(&previous, number);
		}
	}

	close(source->write_fd);
	sigprocmask(SIG_SETMASK, &previous, NULL);
}

// Создать отдельный pipe для каждого наблюдателя одного сигнала
static int synapse_signal_create(struct wl_event_source_signal *source) {
	int number = source->signal_number;

	if (number <= 0 || number >= _NSIG) {
		errno = EINVAL;
		return -1;
	}

	int descriptors[2];

	if (pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) != 0) {
		return -1;
	}

	sigset_t mask;
	sigset_t previous;
	sigemptyset(&mask);
	sigaddset(&mask, number);
	sigprocmask(SIG_BLOCK, &mask, &previous);
	unsigned slot = SYNAPSE_SIGNAL_WATCHERS;
	bool first = true;

	for (unsigned i = 0; i < SYNAPSE_SIGNAL_WATCHERS; i++) {
		if (synapse_signal_fds[number][i] != 0) {
			first = false;
		} else if (slot == SYNAPSE_SIGNAL_WATCHERS) {
			slot = i;
		}
	}

	int failure = ENOSPC;

	if (slot < SYNAPSE_SIGNAL_WATCHERS) {
		failure = 0;

		if (first) {
			struct sigaction action = { 0 };
			sigemptyset(&action.sa_mask);
			action.sa_handler = synapse_signal_handler;
			action.sa_flags = SA_RESTART;

			if (sigaction(number, &action, &synapse_signal_actions[number]) != 0) {
				failure = errno;
			} else {
				synapse_signal_blocked[number] = sigismember(&previous, number) == 1;
			}
		}
	}

	if (failure != 0) {
		close(descriptors[0]);
		close(descriptors[1]);
		sigprocmask(SIG_SETMASK, &previous, NULL);
		errno = failure;
		return -1;
	}

	source->base.fd = descriptors[0];
	source->write_fd = descriptors[1];
	source->synapse_slot = slot;
	synapse_signal_fds[number][slot] = descriptors[1] + 1;
	sigdelset(&previous, number);
	sigprocmask(SIG_SETMASK, &previous, NULL);
	return 0;
}
