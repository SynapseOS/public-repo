// shm-probe.c: проверить создание общего файла для Qt Wayland
// Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
// Подсистема: проверки графических пакетов
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

// Показать тип открытого файла и результат изменения его размера
static int check_file(const char *name, int fd, int optional) {
	int error = errno;
	struct stat status = { 0 };
	if (fd < 0) {
		printf("%s fd=%d errno=%d\n", name, fd, error);
		return optional == 0 || (error != ENOSYS && error != EOPNOTSUPP);
	}

	if (fstat(fd, &status) != 0) {
		perror("fstat");
		close(fd);
		return 1;
	}

	errno = 0;
	int result = ftruncate(fd, 16384);
	printf("%s fd=%d mode=%o truncate=%d errno=%d\n", name, fd, (unsigned int)status.st_mode,
	       result, errno);
	close(fd);
	return result != 0 || !S_ISREG(status.st_mode);
}

// Сравнить способы создания файла без изменения чужих файлов
int main(void) {
	if (mkdir("/tmp/weston-runtime", 0700) != 0 && errno != EEXIST) {
		perror("runtime directory");
		return 1;
	}

	errno = 0;
	int failures =
	    check_file("memfd", (int)syscall(SYS_memfd_create, "qt-probe", 3UL, 0UL, 0UL, 0UL, 0UL), 1);
	errno = 0;
	failures += check_file("O_TMPFILE",
	                       open("/tmp/weston-runtime", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600), 1);
	char path[] = "/tmp/weston-runtime/qt-probe-XXXXXX";
	errno = 0;
	int fd = mkstemp(path);
	if (fd >= 0) {
		unlink(path);
	}

	failures += check_file("mkstemp", fd, 0);
	if (failures != 0) {
		puts("QT_SHM_PROBE_FAIL");
		return 1;
	}

	puts("QT_SHM_PROBE_PASS");
	return 0;
}
