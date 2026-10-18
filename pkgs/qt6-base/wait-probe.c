// wait-probe.c: проверить чтение статуса ребёнка без его удаления
// Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
// Подсистема: проверки графических пакетов
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

// Завершить проверку при неверном статусе или номере процесса
static void require(int condition, const char *operation) {
	if (condition == 0) {
		fprintf(stderr, "QT_WAIT_PROBE_FAIL %s errno=%d\n", operation, errno);
		exit(1);
	}
}

// Дважды прочитать один статус, затем забрать его обычным waitpid
int main(void) {
	alarm(8);
	pid_t child = fork( );
	require(child >= 0, "fork");
	if (child == 0) {
		_exit(37);
	}

	for (unsigned int i = 0; i < 2; i++) {
		siginfo_t info = { 0 };
		require(waitid(P_PID, (id_t)child, &info, WEXITED | WNOWAIT) == 0, "waitid peek");
		require(info.si_pid == child && info.si_code == CLD_EXITED && info.si_status == 37,
		        "peek status");
	}

	int status = 0;
	require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 37,
	        "waitpid after peek");
	siginfo_t info = { 0 };
	errno = 0;
	require(waitid(P_PID, (id_t)child, &info, WEXITED | WNOHANG) == -1 && errno == ECHILD,
	        "already reaped");
	alarm(0);
	puts("QT_WAIT_PROBE_PASS");
	return 0;
}
