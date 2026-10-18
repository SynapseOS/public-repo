// launch.c: окружение Weston и его клиентов
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl и Linux
// Подсистема: графические пакеты
// Первое изменение: 17.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Подготовить закрытый каталог сокетов и запустить выбранную программу
int main(int argc, char **argv) {
	(void)argc;
	const char *name = strrchr(argv[0], '/');
	name = name != NULL ? name + 1 : argv[0];
	const char *runtime = getenv("XDG_RUNTIME_DIR");

	if (runtime == NULL || runtime[0] == 0) {
		runtime = "/tmp/weston-runtime";

		if (mkdir(runtime, 0700) != 0 && errno != EEXIST) {
			perror("Weston runtime directory");
			return 1;
		}

		struct stat status;

		if (lstat(runtime, &status) != 0 || !S_ISDIR(status.st_mode) ||
		    status.st_uid != getuid( ) || (status.st_mode & 077) != 0) {
			fputs("Weston runtime directory must be private\n", stderr);
			return 1;
		}

		if (setenv("XDG_RUNTIME_DIR", runtime, 1) != 0) {
			return 1;
		}
	}

	if (setenv("LD_LIBRARY_PATH", "/usr/lib/weston/runtime:/usr/lib/weston:/usr/lib", 1) != 0) {
		return 1;
	}

	if (getenv("WAYLAND_DISPLAY") == NULL && setenv("WAYLAND_DISPLAY", "wayland-1", 0) != 0) {
		return 1;
	}

	if (setenv("XCURSOR_THEME", "breeze_cursors", 0) != 0 || setenv("XCURSOR_SIZE", "32", 0) != 0) {
		return 1;
	}

	char path[256];
	int length = snprintf(path, sizeof(path), "/usr/libexec/weston/%s", name);

	if (length < 0 || (size_t)length >= sizeof(path)) {
		return 1;
	}

	execv(path, argv);
	perror(path);
	return 1;
}
