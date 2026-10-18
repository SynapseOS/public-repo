/*
    launch.c: Запуск сервера и графического клиента Mindustry
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: Linux (ELF64), musl

    Описание: Подготовка каталогов, Java и библиотек клиента SynapseOS
    Подсистема: пакет Mindustry
    Первое изменение: 17.10.2026
    Последнее изменение: 17.10.2026
*/

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#ifndef MINDUSTRY_SERVER
#define MINDUSTRY_SERVER 1
#endif

// Запускать клиент только с библиотеками SynapseOS, серверу графика не требуется
int main(int argc, char **argv) {
	if (!MINDUSTRY_SERVER && access("/usr/share/games/mindustry/synapse-natives.jar", R_OK) != 0) {
		fprintf(stderr, "Mindustry: для графического клиента нужен пакет mindustry-desktop "
		                "в редакции с поддержкой графики\n"
		                "В cloud запустите сервер командой mindustry-server\n");
		return 1;
	}

	const char *dir = getenv("MINDUSTRY_DATA_DIR");

	if (dir == NULL || *dir == '\0') {
		dir = MINDUSTRY_SERVER ? "/var/lib/mindustry/server" : "/var/lib/mindustry/client";
	}

	const char *parents[] = { "/var", "/var/lib", "/var/lib/mindustry", dir };

	for (size_t i = 0; i < sizeof(parents) / sizeof(parents[0]); i++) {
		if (mkdir(parents[i], 0755) != 0 && errno != EEXIST) {
			perror(parents[i]);
			return 1;
		}
	}

	if (setenv("XDG_DATA_HOME", dir, 1) != 0 || chdir(dir) != 0) {
		perror(dir);
		return 1;
	}

	char **args = calloc((size_t)argc + 24, sizeof(*args));

	if (args == NULL) {
		perror("calloc");
		return 1;
	}

	size_t n = 0;
	args[n++] = "java";
	args[n++] = "-Djava.nio.channels.spi.SelectorProvider=sun.nio.ch.PollSelectorProvider";

	if (MINDUSTRY_SERVER) {
		args[n++] = "-Djava.awt.headless=true";
	}

	args[n++] = "-Xms32m";
	args[n++] = "-Xmx512m";

	if (!MINDUSTRY_SERVER) {
		if (getenv("JAVA_TOOL_OPTIONS") == NULL) {
			args[n++] = "-XX:+UseSerialGC";
		}

		if (setenv("SDL_VIDEODRIVER", "synapse", 1) != 0 || setenv("LP_NUM_THREADS", "2", 0) != 0) {
			perror("Mindustry environment");
			free(args);
			return 1;
		}

		// Arc считает неизвестное имя ОС признаком iOS, здесь используются JNI для Linux ABI и musl
		args[n++] = "-Dos.name=Linux";
		args[n++] = "-Dnodiscord=true";
		args[n++] = "-DSDL2_STATIC=true";
		args[n++] = "-cp";
		args[n++] = "/usr/share/games/mindustry/synapse-natives.jar:"
		            "/usr/share/games/mindustry/Mindustry.jar";
		args[n++] = "mindustry.desktop.DesktopLauncher";
		args[n++] = "-width";
		args[n++] = "900";
		args[n++] = "-height";
		args[n++] = "700";
	} else {
		args[n++] = "-jar";
		args[n++] = "/usr/share/games/mindustry/server-release.jar";
	}

	for (int i = 1; i < argc; i++) {
		args[n++] = argv[i];
	}

	execv("/bin/java", args);
	perror("java");
	free(args);
	return 1;
}
