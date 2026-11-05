// launch.c: окружение Weston и его клиентов
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl и Linux
// Подсистема: графические пакеты
// Первое изменение: 17.10.2026, последнее изменение: 28.10.2026
// Статус доступа: общедоступный, MIT
#include <errno.h>
#include <ctype.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Дождаться завершения подготовки сеансовой шины
static bool run_bus_tool(const char *path, char *const arguments[]) {
	pid_t child = fork();
	if (child < 0) {
		return false;
	}

	if (child == 0) {
		execv(path, arguments);
		_exit(127);
	}

	int status = 0;
	if (waitpid(child, &status, 0) != child) {
		return false;
	}

	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Подготовить D-Bus до запуска Weston и передать адрес приложениям
static void prepare_session_bus(const char *runtime) {
	if (access("/bin/dbus-daemon", X_OK) != 0 || access("/bin/dbus-uuidgen", X_OK) != 0) {
		return;
	}

	if (access("/etc/machine-id", R_OK) != 0) {
		char *const generate[] = {"dbus-uuidgen", "--ensure=/etc/machine-id", NULL};
		if (!run_bus_tool("/bin/dbus-uuidgen", generate)) {
			fputs("Не удалось создать идентификатор D-Bus\n", stderr);
			return;
		}
	}

	char address[256];
	char socket_path[256];
	if (snprintf(address, sizeof(address), "unix:path=%s/bus", runtime) >= (int)sizeof(address) ||
	    snprintf(socket_path, sizeof(socket_path), "%s/bus", runtime) >= (int)sizeof(socket_path)) {
		return;
	}

	if (access(socket_path, F_OK) != 0) {
		char option[272];
		if (snprintf(option, sizeof(option), "--address=%s", address) >= (int)sizeof(option)) {
			return;
		}

		char *const daemon[] = {"dbus-daemon", "--session", "--fork", option, NULL};
		if (!run_bus_tool("/bin/dbus-daemon", daemon)) {
			fputs("Не удалось запустить сеансовую шину D-Bus\n", stderr);
			return;
		}
	}

	if (setenv("DBUS_SESSION_BUS_ADDRESS", address, 1) != 0) {
		perror("DBUS_SESSION_BUS_ADDRESS");
	}
}

// Запустить звук для приложений рабочего стола, если установлен аудиопакет
static void prepare_audio(const char *runtime) {
	if (access("/usr/bin/pulseaudio", X_OK) != 0 || access("/dev/dsp", F_OK) != 0) {
		return;
	}

	char socket_path[256];
	char server[272];
	char module[320];
	if (snprintf(socket_path, sizeof(socket_path), "%s/pulse/native", runtime) >=
			(int)sizeof(socket_path) ||
		snprintf(server, sizeof(server), "unix:%s", socket_path) >= (int)sizeof(server) ||
		snprintf(module, sizeof(module),
			 "module-native-protocol-unix socket=%s auth-anonymous=1", socket_path) >=
			(int)sizeof(module)) {
		return;
	}

	if (setenv("PULSE_SERVER", server, 1) != 0 || access(socket_path, F_OK) == 0) {
		return;
	}

	pid_t child = fork();
	if (child != 0) {
		return;
	}

	setsid();
	int null_fd = open("/dev/null", O_RDWR);
	if (null_fd >= 0) {
		dup2(null_fd, STDIN_FILENO);
		dup2(null_fd, STDOUT_FILENO);
		dup2(null_fd, STDERR_FILENO);
		if (null_fd > STDERR_FILENO) {
			close(null_fd);
		}
	}

	char *const arguments[] = {
		"pulseaudio", "-n", "--daemonize=no", "--exit-idle-time=-1", "--log-level=warn",
		"-L", module, "-L", "module-oss device=/dev/dsp mmap=0", NULL,
	};
	execv("/usr/bin/pulseaudio", arguments);
	_exit(127);
}

// Прочитать выбранную оболочку для каждого нового окна терминала
static void select_terminal_shell(void) {
	FILE *file = fopen("/etc/settings.ini", "r");

	if (file == NULL) {
		return;
	}

	char line[512];
	bool boot = false;

	while (fgets(line, sizeof(line), file) != NULL) {
		char *value = line;

		while (isspace((unsigned char)*value)) {
			value++;
		}

		if (*value == '[') {
			boot = strncmp(value, "[Boot]", 6) == 0;
			continue;
		}

		if (!boot || strncmp(value, "DefaultShell", 12) != 0) {
			continue;
		}

		value += 12;

		while (isspace((unsigned char)*value)) {
			value++;
		}

		if (*value != '=') {
			continue;
		}

		value++;

		while (isspace((unsigned char)*value)) {
			value++;
		}

		char *end = value;

		while (*end != 0 && !isspace((unsigned char)*end) && *end != '#' && *end != ';') {
			end++;
		}

		*end = 0;

		if ((!strcmp(value, "/bin/sh") || !strcmp(value, "/bin/bash") ||
		     !strcmp(value, "/bin/zsh")) && access(value, X_OK) == 0) {
			setenv("SHELL", value, 1);
		}
	}

	fclose(file);
}

// Добавить каталоги ярлыков Flatpak к путям данных рабочего стола
static bool prepare_data_dirs(void) {
	const char *current = getenv("XDG_DATA_DIRS");
	if (current != NULL && strstr(current, "/var/lib/flatpak/exports/share") != NULL) {
		return true;
	}

	const char *home = getenv("HOME");
	if (home == NULL || home[0] == 0) {
		home = "/root";
	}

	if (current == NULL || current[0] == 0) {
		current = "/usr/local/share:/usr/share";
	}

	const char *format = "%s/.local/share/flatpak/exports/share:/var/lib/flatpak/exports/share:%s";
	size_t home_length = strlen(home);
	size_t current_length = strlen(current);
	size_t overhead = strlen(format) + 1;
	if (home_length > SIZE_MAX - overhead ||
	    current_length > SIZE_MAX - overhead - home_length) {
		return false;
	}

	size_t capacity = home_length + current_length + overhead;
	char *paths = malloc(capacity);
	if (paths == NULL) {
		return false;
	}

	int length = snprintf(paths, capacity, format, home, current);
	bool valid = length >= 0 && (size_t)length < capacity &&
	             setenv("XDG_DATA_DIRS", paths, 1) == 0;
	free(paths);
	return valid;
}

// Подготовить закрытый каталог сокетов и запустить выбранную программу
int main(int argc, char **argv) {
	(void)argc;
	const char *name = strrchr(argv[0], '/');
	name = name != NULL ? name + 1 : argv[0];

	if (strcmp(name, "weston-terminal") == 0) {
		select_terminal_shell();
	}
	const char *runtime = getenv("XDG_RUNTIME_DIR");

	if (runtime == NULL || runtime[0] == 0) {
		runtime = "/var/run/weston-runtime";

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

	if (strcmp(name, "weston") == 0 || strcmp(name, "weston-extended") == 0) {
		if (!prepare_data_dirs()) {
			fputs("Не удалось настроить каталоги приложений Flatpak\n", stderr);
			return 1;
		}

		prepare_session_bus(runtime);
		prepare_audio(runtime);
	}

	if (setenv("LD_LIBRARY_PATH", "/usr/lib/weston/runtime:/usr/lib/weston:/usr/lib", 1) != 0) {
		return 1;
	}

	if (setenv("LIBDECOR_FORCE_CSD", "1", 0) != 0) {
		return 1;
	}

	if (getenv("WAYLAND_DISPLAY") == NULL && setenv("WAYLAND_DISPLAY", "wayland-1", 0) != 0) {
		return 1;
	}

	if (setenv("XCURSOR_THEME", "breeze_cursors", 0) != 0 || setenv("XCURSOR_SIZE", "32", 0) != 0) {
		return 1;
	}

	if (strcmp(name, "weston-screenshooter") == 0) {
		umask(0077);

		if (getenv("XDG_PICTURES_DIR") == NULL) {
			const char *home = getenv("HOME");

			if (home == NULL || home[0] == 0) {
				home = "/root";
			}

			if (setenv("XDG_PICTURES_DIR", home, 1) != 0) {
				return 1;
			}
		}
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
