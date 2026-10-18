// service_log.h: Запись и ротация журналов служб
// Автор: Елчинян Арен Артурович, 2026
// Общий фильтр терминальных последовательностей и перенаправление вывода
// Подсистема: запуск и обслуживание пользовательских служб
// Создан: 11.10.2026
// Последнее изменение: 14.10.2026

#ifndef LUMINE_SERVICE_LOG_H
#define LUMINE_SERVICE_LOG_H
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef SERVICE_LOG_DIR
#define SERVICE_LOG_DIR "/var/log"
#endif
#define SERVICE_LOG_LIMIT (1024 * 1024)
#define SERVICE_LOG_KEEP 3

static inline int log_write_all(int fd, const char *buf, size_t size) {
	while (size) {
		ssize_t n = write(fd, buf, size);

		if (n < 0 && errno == EINTR) {
			continue;
		}

		if (n <= 0) {
			return -1;
		}

		buf += n;
		size -= n;
	}

	return 0;
}

static inline int log_copy(const char *from, const char *to) {
	int in = open(from, O_RDONLY);

	if (in < 0) {
		return errno == ENOENT ? 0 : -1;
	}

	int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (out < 0) {
		close(in);
		return -1;
	}

	char buf[4096];
	ssize_t n;
	int bad = 0;

	while ((n = read(in, buf, sizeof(buf))) > 0) {
		if (log_write_all(out, buf, n)) {
			bad = 1;
			break;
		}
	}

	if (n < 0) {
		bad = 1;
	}

	close(in);

	if (close(out)) {
		bad = 1;
	}

	return bad ? -1 : 0;
}

// Сначала копируем журнал, затем очищаем исходный файл, сохраняя открытые дескрипторы
static inline int service_log_rotate(const char *path, size_t limit, int keep) {
	struct stat st;

	if (stat(path, &st)) {
		return errno == ENOENT ? 0 : -1;
	}

	if (st.st_size < (off_t)limit) {
		return 0;
	}

	char from[512], to[512];

	for (int i = keep; i > 1; i--) {
		snprintf(from, sizeof(from), "%s.%d", path, i - 1);
		snprintf(to, sizeof(to), "%s.%d", path, i);

		if (log_copy(from, to)) {
			return -1;
		}
	}

	snprintf(to, sizeof(to), "%s.1", path);

	if (keep && log_copy(path, to)) {
		return -1;
	}

	int fd = open(path, O_WRONLY | O_TRUNC);

	if (fd < 0) {
		return -1;
	}

	return close(fd) ? -1 : 1;
}

// Состояние фильтра ECMA-48 сохраняется между чтениями, включая CSI, OSC и DCS/APC
// Удаляем управляющие последовательности, сохраняя UTF-8, табуляцию и переводы строк
struct service_log_filter {
	unsigned state;
};

static inline size_t service_log_clean(struct service_log_filter *filter, char *data,
                                       size_t count) {
	size_t out = 0;

	for (size_t i = 0; i < count; i++) {
		unsigned char c = (unsigned char)data[i];

		switch (filter->state) {
			case 0:
				if (c == 27) {
					filter->state = 1;
				} else if (c == '\n' || c == '\t' || (c >= 32 && c != 127)) {
					data[out++] = c;
				}

				break;
			case 1:
				if (c == '[') {
					filter->state = 2;
				} else if (c == ']') {
					filter->state = 3;
				} else if (c == 'P' || c == '^' || c == '_') {
					filter->state = 4;
				} else if (c >= 32 && c <= 47) {
					filter->state = 5;
				} else {
					filter->state = 0;
				}

				break;
			case 2:
				if (c >= 64 && c <= 126) {
					filter->state = 0;
				} else if (c == 27) {
					filter->state = 1;
				}

				break;
			case 3:
				if (c == 7) {
					filter->state = 0;
				} else if (c == 27) {
					filter->state = 6;
				}

				break;
			case 4:
				if (c == 27) {
					filter->state = 7;
				}

				break;
			case 5:
				if (c >= 48 && c <= 126) {
					filter->state = 0;
				} else if (c == 27) {
					filter->state = 1;
				}

				break;
			case 6:
				filter->state = c == '\\' ? 0 : (c == 7 ? 0 : 3);
				break;
			case 7:
				filter->state = c == '\\' ? 0 : 4;
				break;
		}
	}

	return out;
}

static inline void service_log_reader(int input, int console, const char *path) {
	char data[4096];
	ssize_t n;
	struct service_log_filter filter = { 0 };

	while ((n = read(input, data, sizeof(data))) != 0) {
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}

			break;
		}
		// Параметр console сохранён для совместимости API, журнал не дублируется в
		// терминал
		n = (ssize_t)service_log_clean(&filter, data, (size_t)n);
		int consumed = 0;

		while (consumed < n) {
			if (service_log_rotate(path, SERVICE_LOG_LIMIT, SERVICE_LOG_KEEP) < 0) {
				break;
			}

			int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);

			if (fd < 0) {
				break;
			}

			off_t offset = lseek(fd, 0, SEEK_END);

			if (offset < 0) {
				close(fd);
				break;
			}

			size_t size = n - consumed;
			size_t room = SERVICE_LOG_LIMIT - (size_t)offset;

			if (size > room) {
				size = room;
			}

			if (!size) {
				close(fd);
				break;
			}

			int bad = log_write_all(fd, data + consumed, size);
			close(fd);

			if (bad) {
				break;
			}

			consumed += size;
		}
	}

	close(input);

	if (console >= 0) {
		close(console);
	}

	_exit(0);
}

// При ошибке журнала вывод фоновой службы не должен возвращаться в терминал
static inline int service_log_fail_closed(int stdout_too) {
	int fd = open("/dev/null", O_WRONLY);

	if (fd >= 0) {
		if (stdout_too && dup2(fd, STDOUT_FILENO) < 0) {
			close(STDOUT_FILENO);
		}

		if (dup2(fd, STDERR_FILENO) < 0) {
			close(STDERR_FILENO);
		}

		if (fd > 2) {
			close(fd);
		}
	} else {
		if (stdout_too) {
			close(STDOUT_FILENO);
		}

		close(STDERR_FILENO);
	}

	return -1;
}

static inline int service_log_start(const char *name, int stdout_too) {
	const char *active = getenv("LUMINE_LOG_SERVICE");

	if (active && !strcmp(active, name)) {
		return 0;
	}

	mkdir("/var", 0755);
	mkdir(SERVICE_LOG_DIR, 0700);
	char path[256];
	snprintf(path, sizeof(path), SERVICE_LOG_DIR "/%s.log", name);
	int check = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);

	if (check < 0) {
		return service_log_fail_closed(stdout_too);
	}

	close(check);
	int console = -1;
	int pipefd[2];

	if (pipe(pipefd)) {
		return service_log_fail_closed(stdout_too);
	}

	fflush(NULL);
	pid_t child = fork( );

	if (child < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return service_log_fail_closed(stdout_too);
	}

	if (child == 0) {
		for (int fd = 0; fd < 256; fd++) {
			if (fd != pipefd[0] && fd != console) {
				close(fd);
			}
		}

		service_log_reader(pipefd[0], console, path);
	}

	close(pipefd[0]);

	if ((stdout_too && dup2(pipefd[1], STDOUT_FILENO) < 0) || dup2(pipefd[1], STDERR_FILENO) < 0) {
		close(pipefd[1]);
		return service_log_fail_closed(stdout_too);
	}

	close(pipefd[1]);
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	setenv("LUMINE_LOG_SERVICE", name, 1);
	time_t now = time(NULL);
	char stamp[64];
	struct tm tm;
	gmtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S UTC", &tm);
	dprintf(STDERR_FILENO, "[%s] %s started\n", stamp, name);
	return 0;
}
#endif
