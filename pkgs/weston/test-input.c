// test-input.c: запись байтов клавиатуры из терминала Weston
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl и POSIX TTY
// Подсистема: гостевые проверки ввода
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT
#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

// Сохранить доставленные терминалом байты без разбора команд оболочкой
int main(void) {
	struct termios saved;

	if (tcgetattr(STDIN_FILENO, &saved) != 0) {
		perror("tcgetattr");
		return 1;
	}

	struct termios raw = saved;
	cfmakeraw(&raw);

	if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
		return 1;
	}

	FILE *file = fopen("/tmp/weston-keyboard.bytes", "wb");

	if (file == NULL) {
		tcsetattr(STDIN_FILENO, TCSANOW, &saved);
		return 1;
	}

	puts("SynapseOS Weston keyboard test");
	puts("Input is recorded in /tmp/weston-keyboard.bytes, Enter finishes");
	fflush(stdout);
	unsigned count = 0;
	int result = 0;

	while (count < 1024) {
		unsigned char byte;
		ssize_t size = read(STDIN_FILENO, &byte, 1);

		if (size < 0 && errno == EINTR) {
			continue;
		}

		if (size != 1) {
			result = 1;
			break;
		}

		if (byte == '\r' || byte == '\n') {
			break;
		}

		if (fwrite(&byte, 1, 1, file) != 1) {
			result = 1;
			break;
		}

		count++;
		fflush(file);
		ssize_t echoed = write(STDOUT_FILENO, &byte, 1);

		if (echoed != 1) {
			result = 1;
			break;
		}
	}

	fclose(file);
	tcsetattr(STDIN_FILENO, TCSANOW, &saved);
	printf("\nCaptured %u bytes\n", count);
	fflush(stdout);
	sleep(3);
	return result;
}
