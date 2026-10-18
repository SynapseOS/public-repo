// launch.c: подготовить окружение Qt и AyuGram для Weston
// Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
// Подсистема: графические пакеты
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef APPLICATION_PATH
#define APPLICATION_PATH "/usr/libexec/qt6/qt6-smoke"
#endif

// Оставить явно заданные параметры приложения или установить значение по умолчанию
static int default_environment(const char *name, const char *value) {
	if (setenv(name, value, 0) != 0) {
		perror(name);
		return -1;
	}

	return 0;
}

// Запустить приложение с собственными библиотеками без изменения среды Weston
int main(int argc, char **argv) {
	(void)argc;
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime == NULL || runtime[0] == 0) {
		runtime = "/tmp/weston-runtime";
		if (mkdir(runtime, 0700) != 0 && errno != EEXIST) {
			perror(runtime);
			return 1;
		}

		if (setenv("XDG_RUNTIME_DIR", runtime, 1) != 0) {
			return 1;
		}
	}

	struct stat status;
	if (lstat(runtime, &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != getuid( ) ||
	    (status.st_mode & 077) != 0) {
		fputs("Qt runtime directory must be private\n", stderr);
		return 1;
	}

	if (setenv("LD_LIBRARY_PATH",
	           "/usr/lib/ayugram/runtime:/usr/lib/qt6/runtime:"
	           "/usr/lib/qt6/llvm:/usr/lib/weston/runtime:/usr/lib",
	           1) != 0 ||
	    default_environment("QT_PLUGIN_PATH", "/usr/lib/qt6/plugins") != 0 ||
	    default_environment("QT_QPA_PLATFORM", "wayland") != 0 ||
	    default_environment("QT_QUICK_BACKEND", "software") != 0 ||
	    default_environment("QT_OPENGL", "software") != 0 ||
	    default_environment("QT_WAYLAND_CLIENT_BUFFER_INTEGRATION", "shm") != 0 ||
	    default_environment("WAYLAND_DISPLAY", "wayland-1") != 0 ||
	    default_environment("XCURSOR_THEME", "breeze_cursors") != 0 ||
	    default_environment("XCURSOR_SIZE", "32") != 0) {
		return 1;
	}

	execv(APPLICATION_PATH, argv);
	perror(APPLICATION_PATH);
	return 1;
}
