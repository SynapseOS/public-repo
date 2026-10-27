// main.c: Проверка дочерних процессов
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Подсистема: Проверка дочерних процессов в пользовательском пространстве
// Первое изменение в истории Git: 09.10.2026
// Последнее изменение: 14.10.2026
// Доступ: общедоступный

#include <unistd.h>
#include <sys/wait.h>

static void out(const char *s) {
    size_t len = 0;
    const char *p = s;
    while (*p++) {
        len++;
    }
    write(1, s, len);
}

static void out_num(int n) {
    char buf[16];
    int i = 0;
    if (n == 0) {
        write(1, "0", 1);
        return;
    }
    if (n < 0) {
        write(1, "-", 1);
        n = -n;
    }

    while (n > 0 && i < 15) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }

    while (i > 0) {
        write(1, &buf[--i], 1);
    }
}

int main(void) {
    out("=== FORK TEST START ===\n");
    out("[PARENT] pid=");
    out_num(getpid());
    out(" calling fork()\n");

    pid_t child = fork();

    if (child < 0) {
        out("[ERROR] fork() failed!\n");
        return 1;
    }

    if (child == 0) {
        // Дочерний процесс
        out("[CHILD] fork() returned 0 (correct!)\n");
        out("[CHILD] my pid=");
        out_num(getpid());
        out("\n");
        out("[CHILD] exiting with code 42\n");
        _exit(42);
    }

    // Родительский процесс
    out("[PARENT] fork() returned ");
    out_num(child);
    out("\n");
    out("[PARENT] calling wait4(-1)...\n");

    int status = 0;
    pid_t waited = wait4(-1, &status, 0, 0);

    if (waited < 0) {
        out("[ERROR] wait4() failed!\n");
        return 1;
    }

    out("[PARENT] wait4() returned pid=");
    out_num(waited);
    out(" status=");
    out_num(status);
    out("\n");

    if (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 42) {
        out("=== FORK TEST PASSED ===\n");
    } else {
        out("=== FORK TEST FAILED ===\n");
        out("  expected: waited=");
        out_num(child);
        out(" status=42\n");
        out("  got:      waited=");
        out_num(waited);
        out(" status=");
        out_num(WEXITSTATUS(status));
        out("\n");
    }

    // Несколько последовательных fork
    out("\n=== MULTI-FORK TEST ===\n");
    for (int i = 0; i < 3; i++) {
        pid_t c = fork();
        if (c == 0) {
            out("[CHILD ");
            out_num(i);
            out("] pid=");
            out_num(getpid());
            out(" exiting\n");
            _exit(i);
        } else if (c > 0) {
            out("[PARENT] spawned child ");
            out_num(i);
            out(" pid=");
            out_num(c);
            out("\n");
        }
    }

    // Ожидаем завершения дочерних процессов
    for (int i = 0; i < 3; i++) {
        int st;
        pid_t w = wait4(-1, &st, 0, 0);
        if (w > 0) {
            out("[PARENT] reaped pid=");
            out_num(w);
            out(" exit=");
            out_num(WEXITSTATUS(st));
            out("\n");
        } else {
            out("[ERROR] wait4 failed at iteration ");
            out_num(i);
            out("\n");
            break;
        }
    }

    out("\n=== ALL TESTS DONE ===\n");
    return 0;
}
