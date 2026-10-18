/*
    launch.c: Подготовка среды OpenJDK
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: Linux (ELF64), musl

    Описание: Пути библиотек и автоматические модули совместимости JAR
    Подсистема: пакет OpenJDK
    Первое изменение: 17.10.2026
    Последнее изменение: 17.10.2026
*/

#define _GNU_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#ifndef JAVA_HOME
#define JAVA_HOME "/usr/lib/jvm/java-21-openjdk"
#endif

#ifndef COMPAT_JAR
#define COMPAT_JAR "/usr/share/java/synapse-compat.jar"
#endif
#define TARGET_PREFIX "META-INF/synapse/targets/"
#define MAX_ZIP_DIRECTORY (16u * 1024u * 1024u)

// Прочитать невыровненное число из заголовка ZIP
static uint32_t zip_u32(const unsigned char *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Прочитать длину поля ZIP, не обращаясь к невыровненному указателю
static uint16_t zip_u16(const unsigned char *p) {
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// Загрузить ограниченный каталог обычного JAR, не распаковывая и не исполняя его содержимое
static unsigned char *zip_directory(const char *path, size_t *length) {
	FILE *file = fopen(path, "rb");

	if (file == NULL) {
		return NULL;
	}

	struct stat info;
	unsigned char *tail = NULL;
	unsigned char *directory = NULL;
	*length = 0;

	if (fstat(fileno(file), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 22) {
		fclose(file);
		return NULL;
	}

	size_t count = info.st_size > 65557 ? 65557 : (size_t)info.st_size;
	tail = malloc(count);

	if (tail == NULL || fseeko(file, info.st_size - (off_t)count, SEEK_SET) != 0 ||
	    fread(tail, 1, count, file) != count) {
		free(tail);
		fclose(file);
		return NULL;
	}

	for (size_t i = count - 22 + 1; i > 0; i--) {
		const unsigned char *end = tail + i - 1;

		if (zip_u32(end) != 0x06054b50 || (size_t)zip_u16(end + 20) + i - 1 + 22 != count) {
			continue;
		}

		uint32_t size = zip_u32(end + 12);
		uint32_t offset = zip_u32(end + 16);
		// ZIP64 и многотомные архивы не участвуют в автоматическом выборе модуля
		if (zip_u32(end + 4) != 0 || zip_u16(end + 8) != zip_u16(end + 10) ||
		    zip_u16(end + 10) == 0xffff || size == 0 || size > MAX_ZIP_DIRECTORY ||
		    (uint64_t)offset + size > (uint64_t)info.st_size - count + i - 1) {
			break;
		}

		directory = malloc(size);

		if (directory != NULL && fseeko(file, offset, SEEK_SET) == 0 &&
		    fread(directory, 1, size, file) == size) {
			*length = size;
		} else {
			free(directory);
			directory = NULL;
		}

		break;
	}

	free(tail);
	fclose(file);
	return directory;
}

// Получить имя очередной записи каталога, проверив границы всех полей
static const unsigned char *zip_next(const unsigned char *directory, size_t length,
                                     size_t *position, size_t *name_length) {
	if (*position > length || length - *position < 46) {
		return NULL;
	}

	const unsigned char *entry = directory + *position;

	if (zip_u32(entry) != 0x02014b50) {
		return NULL;
	}

	size_t name = zip_u16(entry + 28);
	size_t total = 46 + name + zip_u16(entry + 30) + zip_u16(entry + 32);

	if (total > length - *position) {
		return NULL;
	}

	*position += total;
	*name_length = name;
	return entry + 46;
}

// Выбирать модуль по объявленным им классам, а не по имени пользовательского файла
static int needs_compat(const char *jar) {
	if (access(COMPAT_JAR, R_OK) != 0) {
		return 0;
	}

	size_t targets_size = 0, entries_size = 0;
	unsigned char *targets = zip_directory(COMPAT_JAR, &targets_size);
	unsigned char *entries = targets != NULL ? zip_directory(jar, &entries_size) : NULL;
	int matched = 0;

	if (entries != NULL) {
		size_t target_position = 0, target_length = 0;
		const unsigned char *target;

		while ((target = zip_next(targets, targets_size, &target_position, &target_length)) !=
		       NULL) {
			size_t prefix = sizeof(TARGET_PREFIX) - 1;

			if (target_length <= prefix || memcmp(target, TARGET_PREFIX, prefix) != 0) {
				continue;
			}

			size_t position = 0, length = 0;
			const unsigned char *name;

			while ((name = zip_next(entries, entries_size, &position, &length)) != NULL) {
				if (length == target_length - prefix &&
				    memcmp(name, target + prefix, length) == 0) {
					matched = 1;
					break;
				}
			}

			if (matched) {
				break;
			}
		}
	}

	free(entries);
	free(targets);
	return matched;
}

// Найти настоящий аргумент -jar, не принимая параметры пользовательского main за параметры JVM
static const char *jar_argument(int argc, char **argv) {
	static const char *with_value[] = { "-cp",
		                                "-classpath",
		                                "--class-path",
		                                "-p",
		                                "--module-path",
		                                "--upgrade-module-path",
		                                "--add-modules",
		                                "--limit-modules",
		                                "--add-exports",
		                                "--add-opens",
		                                "--add-reads",
		                                "--patch-module",
		                                "--enable-native-access",
		                                "--source" };
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-jar") == 0) {
			return i + 1 < argc ? argv[i + 1] : NULL;
		}

		if (argv[i][0] != '-' || strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--module") == 0 ||
		    strncmp(argv[i], "--module=", 9) == 0) {
			break;
		}

		for (size_t j = 0; j < sizeof(with_value) / sizeof(with_value[0]); j++) {
			if (strcmp(argv[i], with_value[j]) == 0) {
				i++;
				break;
			}
		}
	}

	return NULL;
}

// Передать аргументы OpenJDK и подключить подходящий модуль до загрузки main
int main(int argc, char **argv) {
	const char *base = JAVA_HOME "/lib:" JAVA_HOME "/lib/server";
	const char *old = getenv("LD_LIBRARY_PATH");
	size_t length = strlen(base) + (old != NULL ? strlen(old) : 0) + 2;
	char *path = malloc(length);

	if (path == NULL) {
		perror("malloc");
		return 1;
	}

	snprintf(path, length, "%s%s%s", base, old != NULL && *old != '\0' ? ":" : "",
	         old != NULL ? old : "");
	if (setenv("LD_LIBRARY_PATH", path, 1) != 0) {
		perror("setenv");
		free(path);
		return 1;
	}

	free(path);

	char **args = calloc((size_t)argc + 2, sizeof(*args));

	if (args == NULL) {
		perror("calloc");
		return 1;
	}

	const char *jar = jar_argument(argc, argv);
	char *agent = NULL;
	size_t n = 0;
	args[n++] = JAVA_HOME "/bin/java";

	if (jar != NULL && needs_compat(jar)) {
		length = strlen("-javaagent:" COMPAT_JAR "=") + strlen(jar) + 1;
		agent = malloc(length);

		if (agent == NULL) {
			perror("malloc");
			free(args);
			return 1;
		}

		snprintf(agent, length, "-javaagent:" COMPAT_JAR "=%s", jar);
		args[n++] = agent;
	}

	for (int i = 1; i < argc; i++) {
		args[n++] = argv[i];
	}

	execv(args[0], args);
	perror("java");
	free(agent);
	free(args);
	return 1;
}
