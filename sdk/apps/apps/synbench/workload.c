// workload.c: Нагрузки CPU, памяти, файлов и обмена через pipe для synbench
// Автор: Елчинян Арен Артурович, 2026, x86_64, Linux ABI
// Подсистема: пользовательские измерения Linux и SynapseOS
// Первое изменение: 19.10.2026, последнее изменение: 25.10.2026
// Доступ: общедоступный

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

static volatile uint64_t checksum_sink;

// Вернуть время для измерения самой нагрузки без интервала опроса
static double seconds(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }

    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

// Перенести весь блок с учётом короткого чтения и записи
static bool transfer_all(int fd, unsigned char *buffer, size_t length, bool writing) {
    size_t offset = 0;

    while (offset < length) {
        ssize_t result = writing ? write(fd, buffer + offset, length - offset)
                                 : read(fd, buffer + offset, length - offset);

        if (result < 0 && errno == EINTR) {
            continue;
        }

        if (result <= 0) {
            return false;
        }

        offset += (size_t)result;
    }

    return true;
}

// Проверить ограниченный размер в МиБ либо число операций
static bool number(const char *text, unsigned long maximum, unsigned long *out) {
    char *end = NULL;

    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > maximum) {
        return false;
    }

    *out = value;
    return true;
}

// Выполнить фиксированное число зависимых целочисленных операций
static int cpu_test(unsigned long iterations) {
    uint64_t value = UINT64_C(0x123456789abcdef);
    double start = seconds();

    for (unsigned long index = 0; index < iterations; index++) {
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
    }

    checksum_sink = value;
    double elapsed = seconds() - start;
    printf("SYNBENCH_RESULT {\"cpu_seconds\":%.9f,\"iterations\":%lu,\"checksum\":\"%016" PRIx64 "\"}\n",
           elapsed, iterations, value);
    return 0;
}

// Копировать заранее выделенные и заполненные блоки с проверкой результата
static int memory_test(unsigned long mib, unsigned long repeats) {
    size_t bytes = (size_t)mib * 1024U * 1024U;
    unsigned char *source = malloc(bytes);
    unsigned char *destination = malloc(bytes);

    if (source == NULL || destination == NULL) {
        free(source);
        free(destination);
        return 1;
    }

    memset(source, 0x53, bytes);
    memset(destination, 0, bytes);
    double start = seconds();

    for (unsigned long index = 0; index < repeats; index++) {
        source[0] = (unsigned char)index;
        memcpy(destination, source, bytes);
        // Наблюдаемый результат каждой копии мешает удалить повторения оптимизатором
        checksum_sink += destination[index % bytes];
    }

    double elapsed = seconds() - start;
    bool verified = memcmp(source, destination, bytes) == 0;
    printf("SYNBENCH_RESULT {\"copy_seconds\":%.9f,\"copy_bytes\":%" PRIu64 ","
           "\"copy_mb_s\":%.6f,\"verified\":%s}\n", elapsed, (uint64_t)bytes * repeats,
           (double)bytes * (double)repeats / elapsed / 1000000.0, verified ? "true" : "false");

    free(source);
    free(destination);
    return verified ? 0 : 1;
}

// Записать и прочитать новый временный файл с отдельным временем fsync
// Данные могут остаться в кэше либо читаться с устройства, это зависит от ОС
static int file_test(const char *directory, unsigned long mib) {
    char path[4096];
    unsigned char *buffer = malloc(1024U * 1024U);
    int length = snprintf(path, sizeof(path), "%s/synbench-XXXXXX", directory);

    if (buffer == NULL || length < 0 || (size_t)length >= sizeof(path)) {
        free(buffer);
        return 1;
    }

    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        free(buffer);
        return 1;
    }

    bool valid = true;
    struct statfs filesystem = {0};
    bool have_filesystem = fstatfs(fd, &filesystem) == 0;
    memset(buffer, 0x53, 1024U * 1024U);
    double start = seconds();

    for (unsigned long index = 0; index < mib; index++) {
        if (!transfer_all(fd, buffer, 1024U * 1024U, true)) {
            valid = false;
            break;
        }
    }

    double write_seconds = seconds() - start;
    start = seconds();
    if (fsync(fd) != 0) {
        valid = false;
    }

    double sync_seconds = seconds() - start;
    if (lseek(fd, 0, SEEK_SET) < 0) {
        valid = false;
    }

    start = seconds();
    for (unsigned long index = 0; index < mib && valid; index++) {
        if (!transfer_all(fd, buffer, 1024U * 1024U, false)) {
            valid = false;
            break;
        }

        for (size_t offset = 0; offset < 1024U * 1024U; offset++) {
            if (buffer[offset] != 0x53U) {
                valid = false;
                break;
            }
        }
    }

    double read_seconds = seconds() - start;
    if (close(fd) != 0) {
        valid = false;
    }

    if (unlink(path) != 0) {
        valid = false;
    }

    free(buffer);
    printf("SYNBENCH_RESULT {\"file_write_seconds\":%.9f,\"fsync_seconds\":%.9f,"
           "\"file_read_after_write_verify_seconds\":%.9f,\"file_bytes\":%" PRIu64 ",\"verified\":%s}\n",
           write_seconds, sync_seconds, read_seconds, (uint64_t)mib * 1024U * 1024U, valid ? "true" : "false");
    printf("SYNBENCH_RESULT {\"filesystem_type\":");
    if (have_filesystem) {
        printf("\"%jx\"", (uintmax_t)filesystem.f_type);
    } else {
        fputs("null", stdout);
    }

    fputs("}\n", stdout);
    return valid ? 0 : 1;
}

// Подготовить файл для чтения после перезагрузки гостевой системы
static int prepare_cold_file(const char *path, unsigned long mib) {
    unsigned char *buffer = malloc(1024U * 1024U);
    if (buffer == NULL) {
        return 1;
    }

    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) {
        perror("prepare-cold open");
        free(buffer);
        return 1;
    }

    memset(buffer, 0x53, 1024U * 1024U);
    bool valid = true;
    for (unsigned long index = 0; index < mib; index++) {
        if (!transfer_all(fd, buffer, 1024U * 1024U, true)) {
            valid = false;
            break;
        }
    }

    if (valid && fsync(fd) != 0) {
        valid = false;
    }

    if (close(fd) != 0) {
        valid = false;
    }

    if (!valid) {
        perror("prepare-cold write");
        (void)unlink(path);
    }

    free(buffer);
    return valid ? 0 : 1;
}

// Прочитать и проверить файл, подготовленный до перезагрузки
static int cold_read_test(const char *path, unsigned long mib) {
    unsigned char *buffer = malloc(1024U * 1024U);
    if (buffer == NULL) {
        return 1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("cold-read open");
        free(buffer);
        return 1;
    }

    bool valid = true;
    double start = seconds();
    for (unsigned long index = 0; index < mib && valid; index++) {
        if (!transfer_all(fd, buffer, 1024U * 1024U, false)) {
            valid = false;
            break;
        }

        for (size_t offset = 0; offset < 1024U * 1024U; offset++) {
            if (buffer[offset] != 0x53U) {
                valid = false;
                break;
            }
        }
    }

    double elapsed = seconds() - start;
    if (close(fd) != 0) {
        valid = false;
    }

    printf("SYNBENCH_RESULT {\"cold_read_seconds\":%.9f,\"cold_read_bytes\":%" PRIu64 ","
           "\"verified\":%s}\n", elapsed, (uint64_t)mib * 1024U * 1024U,
           valid ? "true" : "false");
    free(buffer);
    return valid ? 0 : 1;
}

// Составить путь отдельного файла для проверки операций каталога
static bool metadata_path(char *path, size_t capacity, const char *directory,
                          unsigned long index, bool renamed) {
    int length = snprintf(path, capacity, "%s/%s-%06lu", directory,
                          renamed ? "renamed" : "created", index);

    return length >= 0 && (size_t)length < capacity;
}

// Измерить создание, поиск, переименование и удаление отдельных файлов
static int metadata_test(const char *directory, unsigned long count) {
    char source[4096];
    char destination[4096];
    struct stat status;
    double elapsed[4] = {0.0, 0.0, 0.0, 0.0};
    double start = seconds();

    for (unsigned long index = 0; index < count; index++) {
        if (!metadata_path(source, sizeof(source), directory, index, false)) {
            return 1;
        }

        int fd = open(source, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd < 0) {
            perror("metadata create");
            return 1;
        }

        unsigned char value = (unsigned char)index;
        bool written = write(fd, &value, 1) == 1;
        if (close(fd) != 0) {
            written = false;
        }

        if (!written) {
            perror("metadata write");
            return 1;
        }
    }

    elapsed[0] = seconds() - start;
    start = seconds();
    for (unsigned long index = 0; index < count; index++) {
        if (!metadata_path(source, sizeof(source), directory, index, false) ||
            stat(source, &status) != 0 || status.st_size != 1) {
            perror("metadata stat");
            return 1;
        }
    }

    elapsed[1] = seconds() - start;
    start = seconds();
    for (unsigned long index = 0; index < count; index++) {
        if (!metadata_path(source, sizeof(source), directory, index, false) ||
            !metadata_path(destination, sizeof(destination), directory, index, true) ||
            rename(source, destination) != 0) {
            perror("metadata rename");
            return 1;
        }
    }

    elapsed[2] = seconds() - start;
    start = seconds();
    for (unsigned long index = 0; index < count; index++) {
        if (!metadata_path(destination, sizeof(destination), directory, index, true) ||
            unlink(destination) != 0) {
            perror("metadata unlink");
            return 1;
        }
    }

    elapsed[3] = seconds() - start;
    printf("SYNBENCH_RESULT {\"files\":%lu,\"create_seconds\":%.9f,"
           "\"stat_seconds\":%.9f,\"rename_seconds\":%.9f,\"unlink_seconds\":%.9f,"
           "\"verified\":true}\n", count, elapsed[0], elapsed[1], elapsed[2], elapsed[3]);
    return 0;
}

// Сравнить две длительности операций синхронной записи
static int compare_durations(const void *left, const void *right) {
    double first = *(const double *)left;
    double second = *(const double *)right;

    return (first > second) - (first < second);
}

// Измерить задержки отдельных записей 4 КиБ с обязательным fsync
static int sync_write_test(const char *directory, unsigned long count) {
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/sync-write-XXXXXX", directory);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        return 1;
    }

    int fd = mkstemp(path);
    if (fd < 0) {
        perror("sync-write create");
        return 1;
    }

    double *durations = malloc(count * sizeof(*durations));
    if (durations == NULL) {
        (void)close(fd);
        (void)unlink(path);
        return 1;
    }

    unsigned char buffer[4096];
    memset(buffer, 0x53, sizeof(buffer));
    bool valid = true;
    double start = seconds();
    for (unsigned long index = 0; index < count && valid; index++) {
        off_t offset = (off_t)((index * 73UL) % 1024UL) * (off_t)sizeof(buffer);
        double operation_start = seconds();
        size_t written = 0;
        while (written < sizeof(buffer)) {
            ssize_t result = pwrite(fd, buffer + written, sizeof(buffer) - written,
                                    offset + (off_t)written);
            if (result < 0 && errno == EINTR) {
                continue;
            }

            if (result <= 0) {
                valid = false;
                break;
            }

            written += (size_t)result;
        }

        if (valid && fsync(fd) != 0) {
            valid = false;
        }

        durations[index] = seconds() - operation_start;
    }

    double total = seconds() - start;
    unsigned char readback[4096];
    for (unsigned long index = 0; index < count && valid; index++) {
        off_t offset = (off_t)((index * 73UL) % 1024UL) * (off_t)sizeof(buffer);
        if (pread(fd, readback, sizeof(readback), offset) != (ssize_t)sizeof(readback) ||
            memcmp(buffer, readback, sizeof(buffer)) != 0) {
            valid = false;
        }
    }

    if (close(fd) != 0) {
        valid = false;
    }

    if (unlink(path) != 0) {
        valid = false;
    }

    if (valid) {
        qsort(durations, count, sizeof(*durations), compare_durations);
        printf("SYNBENCH_RESULT {\"sync_writes\":%lu,\"sync_bytes\":%lu,"
               "\"sync_total_seconds\":%.9f,\"sync_p50_ms\":%.6f,"
               "\"sync_p95_ms\":%.6f,\"sync_max_ms\":%.6f,\"verified\":true}\n",
               count, count * sizeof(buffer), total, durations[(count - 1UL) / 2UL] * 1000.0,
               durations[((count * 95UL + 99UL) / 100UL) - 1UL] * 1000.0,
               durations[count - 1UL] * 1000.0);
    } else {
        perror("sync-write operation");
    }

    free(durations);
    return valid ? 0 : 1;
}

struct cpu_worker {
    uint64_t iterations;
    uint64_t seed;
    uint64_t checksum;
};

// Выполнить независимую порцию вычислений в одном рабочем потоке
static void *cpu_worker_run(void *argument) {
    struct cpu_worker *worker = argument;
    uint64_t value = worker->seed;

    for (uint64_t index = 0; index < worker->iterations; index++) {
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
    }

    worker->checksum = value;
    return NULL;
}

// Измерить выполнение одинакового общего объёма работы несколькими потоками
static int threadscale_test(unsigned long workers, unsigned long iterations) {
    pthread_t threads[64];
    struct cpu_worker tasks[64] = {0};
    unsigned long started = 0;
    bool valid = true;
    double start = seconds();

    for (unsigned long index = 0; index < workers; index++) {
        tasks[index].iterations = iterations / workers;
        if (index < iterations % workers) {
            tasks[index].iterations++;
        }

        tasks[index].seed = UINT64_C(0x123456789abcdef) + (uint64_t)index;
        if (pthread_create(&threads[index], NULL, cpu_worker_run, &tasks[index]) != 0) {
            valid = false;
            break;
        }

        started++;
    }

    for (unsigned long index = 0; index < started; index++) {
        if (pthread_join(threads[index], NULL) != 0) {
            valid = false;
        }
    }

    double elapsed = seconds() - start;
    if (!valid) {
        return 1;
    }

    uint64_t checksum = 0;
    for (unsigned long index = 0; index < workers; index++) {
        checksum ^= tasks[index].checksum;
    }

    checksum_sink = checksum;
    printf("SYNBENCH_RESULT {\"thread_seconds\":%.9f,\"workers\":%lu,"
           "\"iterations\":%lu,\"checksum\":\"%016" PRIx64 "\"}\n",
           elapsed, workers, iterations, checksum);
    return 0;
}

// Выбрать ограниченную нагрузку без доступа к существующим файлам на запись
int main(int argc, char **argv) {
    unsigned long value = 0;
    unsigned long repeats = 0;

    if (argc == 3 && strcmp(argv[1], "cpu") == 0 && number(argv[2], 1000000000UL, &value)) {
        return cpu_test(value);
    }

    if (argc == 4 && strcmp(argv[1], "memory") == 0 && number(argv[2], 4096UL, &value)
        && number(argv[3], 100000UL, &repeats)) {
        return memory_test(value, repeats);
    }

    if (argc == 4 && strcmp(argv[1], "file") == 0 && number(argv[3], 4096UL, &value)) {
        return file_test(argv[2], value);
    }

    if (argc == 4 && strcmp(argv[1], "prepare-cold") == 0 && number(argv[3], 4096UL, &value)) {
        return prepare_cold_file(argv[2], value);
    }

    if (argc == 4 && strcmp(argv[1], "cold-read") == 0 && number(argv[3], 4096UL, &value)) {
        return cold_read_test(argv[2], value);
    }

    if (argc == 4 && strcmp(argv[1], "metadata") == 0 && number(argv[3], 10000UL, &value)) {
        return metadata_test(argv[2], value);
    }

    if (argc == 4 && strcmp(argv[1], "sync-write") == 0 && number(argv[3], 1024UL, &value)) {
        return sync_write_test(argv[2], value);
    }

    if (argc == 4 && strcmp(argv[1], "threadscale") == 0 && number(argv[2], 64UL, &value)
        && number(argv[3], 1000000000UL, &repeats)) {
        return threadscale_test(value, repeats);
    }

    fputs("Usage: synbench-workload cpu ITERATIONS | memory MIB REPEATS | "
          "file DIRECTORY MIB | prepare-cold PATH MIB | cold-read PATH MIB | "
          "metadata DIRECTORY FILES | sync-write DIRECTORY WRITES | "
          "threadscale WORKERS ITERATIONS\n", stderr);
    return 2;
}
