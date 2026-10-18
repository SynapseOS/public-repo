// progress.c: Вывод состояния загрузки пакета
// Автор: Елчинян Арен Артурович, 2026
// Подсчёт скорости и показ объёма загруженных данных
// Подсистема: пакетный менеджер setup
// Первое изменение: до текущей уборки, исходная дата не установлена
// Последнее изменение: 14.10.2026

#include "setup.h"

static long long progress_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void progress_start(struct download_progress *p, uint64_t total) {
	p->start = progress_ms( );
	p->last = p->start;
	p->total = total;
	p->tty = isatty(STDERR_FILENO);
}

void progress_update(struct download_progress *p, uint64_t done, int final) {
	long long now = progress_ms( ), elapsed = now - p->start;

	if (!final && now - p->last < (p->tty ? 250 : 2000)) {
		return;
	}

	p->last = now;
	double seconds = (elapsed > 0 ? elapsed : 1) / 1000.0, speed = done / seconds;

	if (p->tty) {
		fputc('\r', stderr);
	}

	if (p->total) {
		unsigned percent = done >= p->total ? 100 : (unsigned)(done * 100 / p->total);

		if (p->tty) {
			fputc('[', stderr);

			for (unsigned i = 0; i < 20; i++) {
				fputc(i < percent / 5 ? '=' : ' ', stderr);
			}

			fprintf(stderr, "] %3u%% ", percent);
		} else {
			fprintf(stderr, "Download %u%% ", percent);
		}

		fprintf(stderr, "%.2f/%.2f MiB", done / 1048576.0, p->total / 1048576.0);
	} else {
		fprintf(stderr, "Download %.2f MiB", done / 1048576.0);
	}

	fprintf(stderr, " | %.1f KiB/s | %.1fs", speed / 1024.0, seconds);

	if (p->total > done && speed > 0) {
		fprintf(stderr, " | ETA %.0fs", (p->total - done) / speed);
	}

	if (p->tty) {
		fputs("\033[K", stderr);
	}

	if (final || !p->tty) {
		fputc('\n', stderr);
	}

	fflush(stderr);
}
