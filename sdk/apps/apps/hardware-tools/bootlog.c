// bootlog.c: Просмотр журнала загрузки
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Чтение и сохранение журнала ядра через нативный API
// Подсистема: пользовательские системные утилиты
// Первое изменение: до текущей уборки, исходная дата не установлена
// Последнее изменение: 23.10.2026
// Доступ: общедоступный

#include <sys/utsname.h>
#include <sys/stat.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../common/service_log.h"
#define BOOTLOG "/var/log/boot.log"

// Прочитать целое число из проверенной записи журнала necromant
static uint32_t necro_u32(const unsigned char *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

// Прочитать время или номер записи necromant
static uint64_t necro_u64(const unsigned char *data) {
    return (uint64_t)necro_u32(data) | ((uint64_t)necro_u32(data + 4U) << 32U);
}

// Показать сохранённые на boot-разделе отчёты восстановления
static int show_necro(void) {
    int fd = open("/dev/necro", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("bootlog: necromant journal");
        return 1;
    }

    unsigned char record[512];
    int result = 0;
    for (;;) {
        ssize_t length = read(fd, record, sizeof(record));
        if (length < 0 && errno == EINTR) {
            continue;
        }
        if (length < 0) {
            perror("bootlog: necromant read");
            result = 1;
            break;
        }
        if (length == 0) {
            break;
        }
        if (length != (ssize_t)sizeof(record) || memcmp(record, "NECROREC", 8U) != 0) {
            fprintf(stderr, "bootlog: incomplete necromant snapshot\n");
            result = 1;
            break;
        }

        char message[477];
        size_t position = 0U;
        while (position < sizeof(message) - 1U && record[32U + position] != 0U) {
            unsigned char value = record[32U + position];
            message[position] = value >= 32U && value <= 126U ? (char)value : '?';
            position++;
        }
        message[position] = '\0';

        if (printf("#%llu kind=%u code=%u uptime_us=%llu %s\n",
                (unsigned long long)necro_u64(record + 8U),
                necro_u32(record + 16U), necro_u32(record + 20U),
                (unsigned long long)necro_u64(record + 24U), message) < 0) {
            result = 1;
            break;
        }
    }

    if (close(fd) < 0 || ferror(stdout)) {
        result = 1;
    }
    return result;
}

// Сохранить три предыдущих журнала загрузки
static int previous(void) {
    char from[128], to[128];
    for (int i = 3; i > 1; i--) {
        snprintf(from, sizeof(from), BOOTLOG ".%d", i - 1);
        snprintf(to, sizeof(to), BOOTLOG ".%d", i);
        if (log_copy(from, to)) {
            return -1;
        }
    }
    return log_copy(BOOTLOG, BOOTLOG ".1");
}
// Дождаться памяти для записи, сохраняя смещение после частичной записи
static int write_capture(int fd, const char *data, size_t size) {
    while (size > 0) {
        ssize_t n = write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ENOMEM) {
                sleep(5);
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        data += (size_t)n;
        size -= (size_t)n;
    }
    return 0;
}

/*
    O_RDONLY фиксирует конец снимка при открытии /dev/dmesg
    O_NONBLOCK оставляет поток открытым для следующих сообщений
    Позиции независимы у разных open и не затрагивают klogctl(2)
    Снимок при вытеснении или очистке возвращает ошибку вместо незаметного пропуска
*/
static int capture(int input, int fd, int follow) {
    char data[16 * 1024];
    for (;;) {
        size_t total = 0;
        bool done = false;
        bool dirty = false;
        int result = 0;
        while (total < 256 * 1024) {
            ssize_t n = read(input, data, sizeof(data));
            const char *chunk = data;
            size_t size = 0;
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (follow && (errno == ENOMEM || errno == EAGAIN)) {
                    break;
                }
                if (errno == EOVERFLOW || errno == EPIPE) {
                    if (!follow) {
                        int error = errno;
                        fprintf(stderr, "bootlog: snapshot incomplete, kernel log changed before it was saved\n");
                        errno = error;
                        result = -1;
                        break;
                    }
                    chunk = errno == EOVERFLOW ? "\n[bootlog: unread kernel log data overwritten]\n" :
                                                 "\n[bootlog: kernel log cleared]\n";
                    size = strlen(chunk);
                } else {
                    result = -1;
                    break;
                }
            } else if (n == 0) {
                done = !follow;
                break;
            } else {
                size = (size_t)n;
            }

            if (follow) {
                int rotated;
                while ((rotated = service_log_rotate(BOOTLOG, 4 * 1024 * 1024, 3)) < 0 && errno == ENOMEM) {
                    sleep(5);
                }
                if (rotated < 0) {
                    result = -1;
                    break;
                }
            }
            dirty = true;
            if (write_capture(fd, chunk, size) < 0) {
                result = -1;
                break;
            }
            total += size;
        }

        // Сохраняем уже записанные данные и перед выходом с ошибкой чтения
        int saved_errno = errno;
        if ((dirty || done) && fsync(fd) < 0 && result == 0) {
            result = -1;
            saved_errno = errno;
        }
        if (result < 0) {
            errno = saved_errno;
            return -1;
        }
        if (done) {
            return 0;
        }
        if (total >= 256 * 1024) {
            sched_yield();
        } else {
            sleep(5);
        }
    }
}

// Сохранить снимок или запустить фоновую запись с постоянным буфером 16 КиБ
static int save(int follow, int archive) {
    // Открываем источник до изменения файлов, память нужна только для малого состояния чтения
    int input = open("/dev/dmesg", O_RDONLY | O_CLOEXEC | (follow ? O_NONBLOCK : 0));
    if (input < 0) {
        perror("bootlog: kernel log");
        return 1;
    }

    mkdir("/var", 0755);
    mkdir("/var/log", 0700);
    if (archive && previous()) {
        perror("bootlog: previous log");
        close(input);
        return 1;
    }

    const char *path = follow ? BOOTLOG : "/var/log/boot-snapshot.log";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) {
        perror(path);
        close(input);
        return 1;
    }

    struct utsname u;
    int result = 0;
    if (!uname(&u)) {
        if (dprintf(fd, "Kernel %s %s; live logs persist only on the current root\n",
                    u.sysname, u.release) < 0) {
            result = -1;
        }
    }
    if (result == 0) {
        result = capture(input, fd, follow);
    }
    int saved_errno = errno;
    if (close(input) < 0 && result == 0) {
        result = -1;
        saved_errno = errno;
    }
    if (close(fd) < 0 && result == 0) {
        result = -1;
        saved_errno = errno;
    }
    if (result != 0) {
        errno = saved_errno;
        perror("bootlog: capture stopped");
    }
    return result != 0;
}

// Выбрать просмотр, снимок или фоновую запись
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: bootlog save|show|start|necro\n");
        return 2;
    }
    if (!strcmp(argv[1], "save")) {
        return save(0, 0);
    }
    if (!strcmp(argv[1], "start")) {
        return save(1, 1);
    }
    if (!strcmp(argv[1], "show")) {
        FILE *f = fopen(BOOTLOG, "r");
        if (!f) {
            perror(BOOTLOG);
            return 1;
        }

        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f))) {
            fwrite(buf, 1, n, stdout);
        }

        int bad = ferror(f) || ferror(stdout);
        fclose(f);
        return bad;
    }
    if (!strcmp(argv[1], "necro")) {
        return show_necro();
    }
    fprintf(stderr, "bootlog: unknown command %s\n", argv[1]);
    return 2;
}
