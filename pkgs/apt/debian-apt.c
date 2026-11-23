// debian-apt.c: изолированные операции APT с переключением поколений и общим runtime
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: synapse-x86_64-v1
// Подсистема: менеджер Debian-пакетов
// Первое изменение: 01.11.2026, последнее изменение: 23.11.2026
// Статус доступа: общедоступный

#define _GNU_SOURCE

#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef DEBIAN_APT_BASE
#define DEBIAN_APT_BASE "/var/lib/debian-apt"
#endif

#define APT_GET "/bin/apt-get"
#define APT_CACHE "/bin/apt-cache"
#define DPKG_NATIVE "/usr/libexec/debian-apt/dpkg-native"
#define DPKG_QUERY "/bin/dpkg-query"
#define PROVIDER_PATH "/usr/share/debian-apt/synapse-glibc-runtime.deb"
#define PACKAGE_LISTS DEBIAN_APT_BASE "/lists"
#define GENERATIONS DEBIAN_APT_BASE "/generations"
#define CURRENT DEBIAN_APT_BASE "/current"
#define CATALOG_DIRECTORY "/var/cache/debian-apt"
#define CATALOG_SNAPSHOT CATALOG_DIRECTORY "/catalog.jsonl"
#define RUNTIME_DIRECTORY "/usr/libexec/debian-runtime"
#define RUNTIME_CURRENT RUNTIME_DIRECTORY "/current"

#ifndef PROVIDER_SHA256
#define PROVIDER_SHA256 ""
#endif

#ifndef LOADER_SHA256
#define LOADER_SHA256 ""
#endif

#ifndef LIBC_SHA256
#define LIBC_SHA256 ""
#endif

// Соединить два компонента пути без усечения
static int join_path(char *out, size_t size, const char *left, const char *right) {
    int written = snprintf(out, size, "%s/%s", left, right);

    if (written < 0 || (size_t)written >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

// Создать каталог и все недостающие родительские каталоги
static int make_directories(const char *path) {
    char copy[PATH_MAX];
    size_t length = strlen(path);

    if (length == 0U || length >= sizeof copy) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(copy, path, length + 1U);

    for (size_t index = 1U; index <= length; index++) {
        if (copy[index] != '/' && copy[index] != '\0') {
            continue;
        }

        char saved = copy[index];
        copy[index] = '\0';

        if (mkdir(copy, 0755) < 0 && errno != EEXIST) {
            return -1;
        }

        copy[index] = saved;
    }

    return 0;
}

// Рекурсивно удалить временный каталог staging или старое поколение
static int remove_tree(const char *path, unsigned int depth) {
    struct stat status;

    if (depth > 64U || lstat(path, &status) < 0) {
        return errno == ENOENT ? 0 : -1;
    }

    if (!S_ISDIR(status.st_mode)) {
        return unlink(path);
    }

    DIR *directory = opendir(path);
    if (directory == NULL) {
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        char child[PATH_MAX];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (join_path(child, sizeof child, path, entry->d_name) < 0 ||
            remove_tree(child, depth + 1U) < 0) {
            closedir(directory);
            return -1;
        }
    }

    closedir(directory);
    return rmdir(path);
}

// Запустить дочерний процесс и вернуть его код завершения
static int run_program(const char *program, char *const arguments[]) {
    pid_t child = fork();

    if (child < 0) {
        return -1;
    }

    if (child == 0) {
        execv(program, arguments);
        _exit(127);
    }

    int status = 0;

    for (;;) {
        if (waitpid(child, &status, 0) >= 0) {
            break;
        }

        if (errno != EINTR) {
            return -1;
        }
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    if (!WIFEXITED(status)) {
        return -1;
    }

    return WEXITSTATUS(status);
}

// Прочитать короткий вывод служебной команды без shell
static int capture_program(const char *program, char *const arguments[], char *output,
                           size_t size) {
    int channel[2];

    if (size == 0U || pipe(channel) < 0) {
        return -1;
    }

    pid_t child = fork();

    if (child < 0) {
        close(channel[0]);
        close(channel[1]);
        return -1;
    }

    if (child == 0) {
        close(channel[0]);

        if (dup2(channel[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }

        close(channel[1]);
        execv(program, arguments);
        _exit(127);
    }

    close(channel[1]);

    size_t used = 0U;
    int overflow = 0;
    char block[256];

    for (;;) {
        ssize_t count = read(channel[0], block, sizeof block);

        if (count == 0) {
            break;
        }

        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }

            close(channel[0]);
            waitpid(child, NULL, 0);
            return -1;
        }

        if ((size_t)count > size - 1U - used) {
            overflow = 1;
        } else {
            memcpy(output + used, block, (size_t)count);
            used += (size_t)count;
        }
    }

    close(channel[0]);
    output[used] = '\0';

    int status = 0;

    if (waitpid(child, &status, 0) < 0 || overflow || !WIFEXITED(status)) {
        return -1;
    }

    return WEXITSTATUS(status);
}

// Сверить файл с контрольной суммой проверенного пакета
static int verify_sha256(const char *path, const char *expected) {
    char output[PATH_MAX + 80U] = {0};
    char *const arguments[] = {"sha256sum", (char *)path, NULL};

    if (strlen(expected) != 64U ||
        capture_program("/bin/sha256sum", arguments, output, sizeof output) != 0 ||
        strlen(output) < 65U ||
        strncmp(output, expected, 64U) != 0 || output[64] != ' ') {
        fprintf(stderr, "Package digest mismatch: %s\n", path);
        return -1;
    }

    return 0;
}

// Сравнить конец пути с именем проверенного файла
static int ends_with(const char *path, const char *suffix) {
    size_t path_length = strlen(path);
    size_t suffix_length = strlen(suffix);

    return path_length >= suffix_length &&
           strcmp(path + path_length - suffix_length, suffix) == 0;
}

// Скопировать один файл или дерево, сохранив права и ссылки
static int copy_entry(const char *source, const char *destination) {
    char *const arguments[] = {"cp", "-a", (char *)source, (char *)destination, NULL};

    return run_program("/bin/cp", arguments);
}

static int valid_generation(const char *name);

// Перенести базу dpkg поколений старого формата внутрь root
static int migrate_generations(void) {
    DIR *directory = opendir(GENERATIONS);

    if (directory == NULL) {
        return errno == ENOENT ? 0 : -1;
    }

    int result = 0;
    struct dirent *entry;

    while (result == 0 && (entry = readdir(directory)) != NULL) {
        char generation[PATH_MAX];
        char admin[PATH_MAX];
        char database[PATH_MAX];
        struct stat status;

        if (!valid_generation(entry->d_name) ||
            join_path(generation, sizeof generation, GENERATIONS, entry->d_name) < 0 ||
            join_path(admin, sizeof admin, generation, "admin") < 0 ||
            join_path(database, sizeof database, generation, "root/var/lib/dpkg") < 0) {
            result = -1;
            break;
        }

        if (lstat(admin, &status) < 0) {
            continue;
        }

        if (!S_ISDIR(status.st_mode)) {
            result = -1;
            break;
        }

        if (lstat(database, &status) == 0) {
            if (remove_tree(admin, 0U) < 0) {
                result = -1;
                break;
            }
            continue;
        }

        if (errno != ENOENT) {
            result = -1;
            break;
        }

        char parent[PATH_MAX];

        if (join_path(parent, sizeof parent, generation, "root/var/lib") < 0 ||
            make_directories(parent) < 0 || rename(admin, database) < 0) {
            result = -1;
            break;
        }
    }

    closedir(directory);
    return result;
}

// Создать исходное поколение при первом запуске
static int initialize_store(void) {
    char initial[PATH_MAX];
    char admin[PATH_MAX];
    char root[PATH_MAX];
    char destination[PATH_MAX];
    struct stat status;

    if (make_directories(GENERATIONS) < 0 || migrate_generations() < 0) {
        return -1;
    }

    if (lstat(CURRENT, &status) == 0) {
        return S_ISLNK(status.st_mode) ? 0 : -1;
    }

    if (errno != ENOENT || join_path(initial, sizeof initial, GENERATIONS, "0") < 0 ||
        join_path(root, sizeof root, initial, "root") < 0 ||
        join_path(admin, sizeof admin, root, "var/lib/dpkg") < 0 ||
        make_directories(admin) < 0) {
        return -1;
    }

    if (join_path(destination, sizeof destination, admin, "status") < 0 ||
        copy_entry("/etc/debian-apt/status", destination) != 0 ||
        join_path(destination, sizeof destination, admin, "arch") < 0 ||
        copy_entry("/etc/debian-apt/arch", destination) != 0 ||
        join_path(destination, sizeof destination, admin, "arch-native") < 0 ||
        copy_entry("/etc/debian-apt/arch", destination) != 0) {
        return -1;
    }

    if (symlink("generations/0", CURRENT) < 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

// Получить имя текущего поколения без перехода по ссылке
static int current_generation(char *name, size_t size) {
    char target[PATH_MAX];
    ssize_t length = readlink(CURRENT, target, sizeof target - 1U);

    if (length < 0 || (size_t)length >= sizeof target - 1U) {
        return -1;
    }

    target[length] = '\0';

    if (strncmp(target, "generations/", 12U) != 0 || target[12] == '\0') {
        return -1;
    }

    for (size_t index = 12U; target[index] != '\0'; index++) {
        if ((target[index] < '0' || target[index] > '9') && target[index] != '-') {
            return -1;
        }
    }

    int written = snprintf(name, size, "%s", target + 12);

    if (written < 0 || (size_t)written >= size) {
        return -1;
    }

    return 0;
}

// Проверить, что символическая ссылка не выходит за пределы корня поколения
static int validate_symlink_target(const char *path, unsigned int depth) {
    char target[PATH_MAX];
    ssize_t length = readlink(path, target, sizeof target - 1U);

    if (length <= 0 || (size_t)length >= sizeof target - 1U) {
        return -1;
    }

    target[length] = '\0';
    if (target[0] == '/') {
        return 0;
    }

    int level = (int)depth;
    char *cursor = target;

    while (*cursor != '\0') {
        char *slash = strchr(cursor, '/');
        size_t part_len = slash != NULL ? (size_t)(slash - cursor) : strlen(cursor);

        if (part_len == 2U && memcmp(cursor, "..", 2U) == 0) {
            level--;
            if (level < 0) {
                fprintf(stderr, "Unsafe symlink traversal in package: %s\n", path);
                return -1;
            }
        } else if (part_len > 0U && !(part_len == 1U && cursor[0] == '.')) {
            level++;
        }

        if (slash == NULL) {
            break;
        }
        cursor = slash + 1;
    }

    return 0;
}

// Проверить исполняемый формат и архитектуру каждого ELF-файла поколения
static int check_elf_file(const char *path, int *is_dynamic_out) {
    if (is_dynamic_out != NULL) {
        *is_dynamic_out = 0;
    }

    if (ends_with(path, "/lib/ld-linux-x86-64.so.2") && strlen(LOADER_SHA256) == 64U) {
        return verify_sha256(path, LOADER_SHA256);
    }

    if (ends_with(path, "/lib/libc.so.6") && strlen(LIBC_SHA256) == 64U) {
        return verify_sha256(path, LIBC_SHA256);
    }

    int descriptor = open(path, O_RDONLY | O_NOFOLLOW);

    if (descriptor < 0) {
        return -1;
    }

    Elf64_Ehdr header;
    ssize_t count = pread(descriptor, &header, sizeof header, 0);

    if (count < 4 || memcmp(header.e_ident, ELFMAG, SELFMAG) != 0) {
        close(descriptor);
        return 0;
    }

    if (count != (ssize_t)sizeof header || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != EM_X86_64 ||
        (header.e_type != ET_EXEC && header.e_type != ET_DYN) ||
        header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum > 128U) {
        fprintf(stderr, "Unsupported ELF ABI: %s\n", path);
        close(descriptor);
        return -1;
    }

    struct stat file_status;

    if (fstat(descriptor, &file_status) < 0 ||
        header.e_phoff > (uint64_t)file_status.st_size ||
        (uint64_t)header.e_phnum >
            ((uint64_t)file_status.st_size - header.e_phoff) / sizeof(Elf64_Phdr)) {
        close(descriptor);
        return -1;
    }

    for (uint16_t index = 0U; index < header.e_phnum; index++) {
        Elf64_Phdr segment;
        off_t offset = (off_t)(header.e_phoff + ((uint64_t)index * sizeof segment));

        if (pread(descriptor, &segment, sizeof segment, offset) != (ssize_t)sizeof segment) {
            close(descriptor);
            return -1;
        }

        if (segment.p_type == PT_INTERP) {
            char interp[256];
            if (segment.p_filesz == 0U || segment.p_filesz >= sizeof interp ||
                segment.p_offset > (uint64_t)file_status.st_size ||
                segment.p_filesz > (uint64_t)file_status.st_size - segment.p_offset ||
                pread(descriptor, interp, (size_t)segment.p_filesz, (off_t)segment.p_offset) !=
                    (ssize_t)segment.p_filesz) {
                close(descriptor);
                return -1;
            }
            interp[segment.p_filesz - 1U] = '\0';
            if (strcmp(interp, "/lib64/ld-linux-x86-64.so.2") != 0 &&
                strcmp(interp, "/lib/ld-linux-x86-64.so.2") != 0) {
                fprintf(stderr, "Unsupported ELF interpreter %s in %s\n", interp, path);
                close(descriptor);
                return -1;
            }
            if (is_dynamic_out != NULL) {
                *is_dynamic_out = 1;
            }
        }
    }

    close(descriptor);
    return 0;
}

// Просмотреть дерево без перехода по символическим ссылкам
static int check_tree(const char *path, unsigned int depth) {
    DIR *directory = opendir(path);

    if (directory == NULL || depth > 64U) {
        if (directory != NULL) {
            closedir(directory);
        }

        return -1;
    }

    struct dirent *entry;

    while ((entry = readdir(directory)) != NULL) {
        char child[PATH_MAX];
        struct stat status;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (join_path(child, sizeof child, path, entry->d_name) < 0 ||
            lstat(child, &status) < 0) {
            closedir(directory);
            return -1;
        }

        if (S_ISDIR(status.st_mode)) {
            if (check_tree(child, depth + 1U) < 0) {
                closedir(directory);
                return -1;
            }
        } else if (S_ISREG(status.st_mode)) {
            if (check_elf_file(child, NULL) < 0) {
                closedir(directory);
                return -1;
            }
        } else if (S_ISLNK(status.st_mode)) {
            if (validate_symlink_target(child, depth) < 0) {
                closedir(directory);
                return -1;
            }
        } else if (!S_ISCHR(status.st_mode) && !S_ISBLK(status.st_mode)) {
            // Устройства создаёт сам backend для chroot, остальное специальное запрещено
            fprintf(stderr, "Unsupported package special file: %s\n", child);
            closedir(directory);
            return -1;
        }
    }

    closedir(directory);
    return 0;
}

// Записать файлы и каталоги поколения до смены указателя
static int sync_tree(const char *path, unsigned int depth) {
    DIR *directory = opendir(path);

    if (directory == NULL || depth > 64U) {
        if (directory != NULL) {
            closedir(directory);
        }

        return -1;
    }

    struct dirent *entry;

    while ((entry = readdir(directory)) != NULL) {
        char child[PATH_MAX];
        struct stat status;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (join_path(child, sizeof child, path, entry->d_name) < 0 ||
            lstat(child, &status) < 0) {
            closedir(directory);
            return -1;
        }

        if (S_ISDIR(status.st_mode)) {
            if (sync_tree(child, depth + 1U) < 0) {
                closedir(directory);
                return -1;
            }
        } else if (S_ISREG(status.st_mode)) {
            int descriptor = open(child, O_RDONLY | O_NOFOLLOW);

            if (descriptor < 0 || fsync(descriptor) < 0) {
                if (descriptor >= 0) {
                    close(descriptor);
                }

                closedir(directory);
                return -1;
            }

            close(descriptor);
        }
    }

    int descriptor = dirfd(directory);

    if (descriptor < 0 || fsync(descriptor) < 0) {
        closedir(directory);
        return -1;
    }

    closedir(directory);
    return 0;
}

// Сохранить запись каталога после создания поколения
static int sync_directory(const char *path) {
    int descriptor = open(path, O_RDONLY | O_DIRECTORY);

    if (descriptor < 0) {
        return -1;
    }

    int result = fsync(descriptor);
    close(descriptor);
    return result;
}

static int invalidate_catalog(void);
static int publish_catalog(const char *generation);
static int invalidate_runtime(void);
static int publish_runtime(const char *generation);

// Переключить текущий указатель только после проверки поколения
static int commit_generation(const char *name) {
    char target[PATH_MAX];
    char path[PATH_MAX];
    int written = snprintf(target, sizeof target, "generations/%s", name);

    if (written < 0 || (size_t)written >= sizeof target ||
        snprintf(path, sizeof path, "%s/current.%ld", DEBIAN_APT_BASE, (long)getpid()) >= (int)sizeof path) {
        return -1;
    }

    if (invalidate_runtime() < 0 || invalidate_catalog() < 0 ||
        symlink(target, path) < 0 || rename(path, CURRENT) < 0) {
        return -1;
    }

    int directory = open(DEBIAN_APT_BASE, O_RDONLY | O_DIRECTORY);

    if (directory < 0 || fsync(directory) < 0) {
        if (directory >= 0) {
            close(directory);
        }

        return -1;
    }

    close(directory);
    return publish_catalog(name);
}

// Сформировать параметры APT для выбранного каталога состояния
static int execute_apt(const char *program, const char *command, const char *item,
                       const char *generation, int mutation, int download_only) {
    char directory[PATH_MAX];
    char status_option[PATH_MAX];
    char admin_option[PATH_MAX];
    char root_option[PATH_MAX];

    if (join_path(directory, sizeof directory, GENERATIONS, generation) < 0) {
        return -1;
    }

    int written = snprintf(status_option, sizeof status_option,
                           "Dir::State::status=%s/root/var/lib/dpkg/status", directory);

    if (written < 0 || (size_t)written >= sizeof status_option) {
        return -1;
    }

    written = snprintf(admin_option, sizeof admin_option,
                       "DPkg::Options::=--admindir=%s/root/var/lib/dpkg", directory);

    if (written < 0 || (size_t)written >= sizeof admin_option) {
        return -1;
    }

    written = snprintf(root_option, sizeof root_option,
                       "DPkg::Options::=--root=%s/root", directory);

    if (written < 0 || (size_t)written >= sizeof root_option) {
        return -1;
    }

    char *const arguments[] = {
        (char *)program,
        "-o", status_option,
        "-o", "Dir::Bin::dpkg=" DPKG_NATIVE,
        "-o", admin_option,
        "-o", root_option,
        (char *)command,
        mutation ? "-y" : NULL,
        mutation ? "--no-install-recommends" : NULL,
        download_only ? "-d" : NULL,
        (char *)item,
        NULL
    };
    char *compact[20];
    size_t count = 0U;

    for (size_t index = 0U; index < sizeof arguments / sizeof arguments[0]; index++) {
        if (arguments[index] != NULL) {
            compact[count] = arguments[index];
            count++;
        }
    }

    compact[count] = NULL;
    return run_program(program, compact);
}

// Проверить, получен ли индекс пакетов из подписанного репозитория
static int package_index_ready(void) {
    DIR *directory = opendir(PACKAGE_LISTS);

    if (directory == NULL) {
        return errno == ENOENT ? 0 : -1;
    }

    int ready = 0;
    struct dirent *entry;

    for (;;) {
        errno = 0;
        entry = readdir(directory);

        if (entry == NULL) {
            if (errno != 0) {
                ready = -1;
            }

            break;
        }

        if (strstr(entry->d_name, "_Packages") == NULL) {
            continue;
        }

        char path[PATH_MAX];
        struct stat status;

        if (join_path(path, sizeof path, PACKAGE_LISTS, entry->d_name) < 0 ||
            lstat(path, &status) < 0) {
            ready = -1;
            break;
        }

        if (S_ISREG(status.st_mode) && status.st_size > 0) {
            ready = 1;
            break;
        }
    }

    if (closedir(directory) < 0) {
        return -1;
    }

    return ready;
}

// Создать копию текущего поколения для пакетной операции
static int stage_generation(char *name, size_t size, char *directory, size_t directory_size) {
    char previous[64];
    char source[PATH_MAX];
    char destination[PATH_MAX];
    int written = snprintf(name, size, "%ld-%ld", (long)time(NULL), (long)getpid());

    if (written < 0 || (size_t)written >= size ||
        current_generation(previous, sizeof previous) < 0 ||
        join_path(directory, directory_size, GENERATIONS, name) < 0 ||
        mkdir(directory, 0755) < 0) {
        return -1;
    }

    char contents[PATH_MAX];

    if (join_path(destination, sizeof destination, directory, "root") < 0 ||
        make_directories(destination) < 0 ||
        join_path(source, sizeof source, CURRENT, "root") < 0 ||
        join_path(contents, sizeof contents, source, ".") < 0 ||
        copy_entry(contents, destination) != 0) {
        return -1;
    }

    // dpkg берёт native-архитектуру chroot из arch-native, без него подставляет build-триплет musl
    if (join_path(source, sizeof source, destination, "var/lib/dpkg/arch-native") < 0 ||
        copy_entry("/etc/debian-apt/arch", source) != 0) {
        return -1;
    }

    return 0;
}

// Проверить имя пакета перед передачей в APT
static int valid_package(const char *name) {
    size_t length = strlen(name);

    if (length == 0U || length > 100U || name[0] == '-') {
        return 0;
    }

    for (size_t index = 0U; index < length; index++) {
        char value = name[index];

        if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '+' || value == '-' ||
            value == '.' || value == ':' || value == '_') {
            continue;
        }

        return 0;
    }

    return 1;
}

// Принять только номер поколения, созданный этим backend
static int valid_generation(const char *name) {
    size_t length = strlen(name);

    if (length == 0U || length > 63U) {
        return 0;
    }

    if (strcmp(name, "0") == 0) {
        return 1;
    }

    int separator = 0;

    for (size_t index = 0U; index < length; index++) {
        if (name[index] == '-') {
            if (separator != 0 || index == 0U || index + 1U == length) {
                return 0;
            }

            separator = 1;
        } else if (name[index] < '0' || name[index] > '9') {
            return 0;
        }
    }

    return separator;
}

// Описать допустимые команды Debian backend
static void usage(void) {
    fprintf(stderr, "debian-apt update|search TEXT|show NAME|policy NAME|"
                    "install NAME|remove NAME|upgrade|run PROG [ARGS]|catalog [TEXT]|"
                    "current|status|rollback ID|gc\n");
}

// Установить проверенный Synapse glibc runtime как обычный dpkg provider
static int install_provider(const char *directory) {
    char root[PATH_MAX];
    char database[PATH_MAX];

    if ((strlen(PROVIDER_SHA256) == 64U && verify_sha256(PROVIDER_PATH, PROVIDER_SHA256) < 0) ||
        join_path(root, sizeof root, directory, "root") < 0 ||
        join_path(database, sizeof database, root, "var/lib/dpkg") < 0) {
        return -1;
    }

    // Provider делит библиотеки с одноимёнными пакетами Debian и вправе их заменить
    char *const arguments[] = {"dpkg-native", "--root", root, "--admindir", database,
                               "--force-overwrite", "-i", PROVIDER_PATH, NULL};

    return run_program(DPKG_NATIVE, arguments);
}

// Создать базовые каталоги, нужные maintainer scripts в chroot
// dev остаётся пустой точкой монтирования: ext4 не хранит файлы устройств,
// поэтому перед транзакцией на неё подключается хостовый devtmpfs
static int ensure_base_layout(const char *root) {
    static const char *const dirs[] = {
        "dev", "proc", "sys", "run", "var", "usr/local", "usr/local/bin",
        "usr/local/sbin", "usr/local/lib", "usr/local/share",
    };

    char path[PATH_MAX];

    for (size_t index = 0U; index < sizeof dirs / sizeof dirs[0]; index++) {
        if (join_path(path, sizeof path, root, dirs[index]) < 0 ||
            make_directories(path) < 0) {
            return -1;
        }
    }

    if (join_path(path, sizeof path, root, "tmp") < 0 ||
        (mkdir(path, 01777) < 0 && errno != EEXIST)) {
        return -1;
    }

    if (join_path(path, sizeof path, root, "root") < 0 ||
        (mkdir(path, 0700) < 0 && errno != EEXIST)) {
        return -1;
    }

    // sh не входит в provider: пакет dash перезаписывает только неуправляемый файл
    if (join_path(path, sizeof path, root, "usr/bin/sh") < 0) {
        return -1;
    }

    struct stat present;

    if (lstat(path, &present) < 0 &&
        (errno != ENOENT || symlink("../lib/synapse-runtime/bin/dash", path) < 0)) {
        return -1;
    }

    return 0;
}

// Подключить хостовый devtmpfs внутрь chroot на время транзакции
static int mount_chroot_devices(const char *root) {
    char devdir[PATH_MAX];

    if (join_path(devdir, sizeof devdir, root, "dev") < 0) {
        return -1;
    }

    if (mount("/dev", devdir, NULL, MS_BIND | MS_REC, NULL) < 0) {
        fprintf(stderr, "Cannot bind /dev into the staged root: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

// Отключить devtmpfs от chroot, точка остаётся пустым каталогом поколения
static void unmount_chroot_devices(const char *root) {
    char devdir[PATH_MAX];

    if (join_path(devdir, sizeof devdir, root, "dev") == 0) {
        (void)umount2(devdir, MNT_DETACH);
    }
}

// Запустить APT в отдельном пространстве монтирований с хостовым devtmpfs
// Пространство исчезает вместе с процессом, включая аварийное завершение
static int execute_apt_isolated(const char *root, const char *command,
                                const char *package, const char *name) {
    pid_t child = (pid_t)syscall(SYS_clone, (long)(CLONE_NEWNS | SIGCHLD),
                                 0L, 0L, 0L, 0L);

    if (child < 0) {
        fprintf(stderr, "Cannot create a mount namespace for APT: %s\n", strerror(errno));
        return -1;
    }

    if (child == 0) {
        int status = mount_chroot_devices(root);

        if (status == 0) {
            status = execute_apt(APT_GET, command, package, name, 1, 0);
            unmount_chroot_devices(root);
        }

        _exit(status == 0 ? 0 : 1);
    }

    int wait_status = 0;

    while (waitpid(child, &wait_status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }

    return WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
}

// Обновить кэш загрузчика внутри поколения после транзакции
static int refresh_loader_cache(const char *directory) {
    char root[PATH_MAX];
    char binary[PATH_MAX];
    struct stat status;

    if (join_path(root, sizeof root, directory, "root") < 0 ||
        join_path(binary, sizeof binary, root, "usr/lib/synapse-runtime/bin/ldconfig") < 0) {
        return -1;
    }

    // Поколение без provider просто не имеет chroot-инструментов
    if (lstat(binary, &status) < 0) {
        return errno == ENOENT ? 0 : -1;
    }

    char *const arguments[] = {"chroot", root,
                               "/usr/lib/synapse-runtime/bin/ldconfig", NULL};

    return run_program("/bin/chroot", arguments);
}

// Провести пакетную операцию и переключить поколение при успехе
static int mutate_package(const char *command, const char *package) {
    char name[64];
    char directory[PATH_MAX];
    char root[PATH_MAX];

    int installing = strcmp(command, "install") == 0 || strcmp(command, "upgrade") == 0;

    if (installing) {
        int index_ready = package_index_ready();
        char current[64];

        if (index_ready < 0 ||
            (index_ready == 0 &&
             (current_generation(current, sizeof current) < 0 ||
              execute_apt(APT_GET, "update", NULL, current, 0, 0) != 0))) {
            fprintf(stderr, "Cannot refresh the signed Debian package index\n");
            return 1;
        }
    }

    if (stage_generation(name, sizeof name, directory, sizeof directory) < 0) {
        fprintf(stderr, "Cannot stage Debian generation\n");
        return 1;
    }

    if (installing && install_provider(directory) != 0) {
        fprintf(stderr, "Cannot install Synapse glibc runtime provider\n");
        (void)remove_tree(directory, 0U);
        return 1;
    }

    if (join_path(root, sizeof root, directory, "root") < 0 ||
        ensure_base_layout(root) < 0) {
        fprintf(stderr, "Cannot prepare the base layout in the staged root\n");
        (void)remove_tree(directory, 0U);
        return 1;
    }

    // Загрузчик Synapse glibc ищет библиотеки только в /lib и /usr/lib
    // Кэш нужен до maintainer scripts: их программы тянут библиотеки из x86_64-linux-gnu
    if (refresh_loader_cache(directory) != 0) {
        fprintf(stderr, "Cannot refresh the loader cache in the staged root\n");
        (void)remove_tree(directory, 0U);
        return 1;
    }

    // Maintainer scripts работают в chroot и ждут настоящие устройства
    if (execute_apt_isolated(root, command, package, name) != 0 ||
        refresh_loader_cache(directory) != 0 ||
        join_path(root, sizeof root, directory, "root") < 0 ||
        check_tree(root, 0U) < 0 || sync_tree(directory, 0U) < 0 ||
        sync_directory(GENERATIONS) < 0) {
        fprintf(stderr, "Debian transaction failed, current generation is unchanged\n");
        (void)remove_tree(directory, 0U);
        return 1;
    }

#ifdef DEBIAN_APT_TEST_KILL_BEFORE_COMMIT
    raise(SIGKILL);
#endif

    if (commit_generation(name) < 0) {
        fprintf(stderr, "Debian transaction failed during generation switch\n");
        (void)remove_tree(directory, 0U);
        return 1;
    }

    printf("Debian generation %s is current\n", name);
    return 0;
}

// Открыть публичный каталог только в настоящем каталоге root
static int catalog_directory(int create) {
    if (create && make_directories(CATALOG_DIRECTORY) < 0) {
        return -1;
    }

    int descriptor = open(CATALOG_DIRECTORY, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    struct stat status;
    if (descriptor < 0 || fstat(descriptor, &status) < 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != 0 || (status.st_mode & 0022U) != 0U ||
        (create && fchmod(descriptor, 0755) < 0)) {
        if (descriptor >= 0) {
            close(descriptor);
        }

        return -1;
    }

    return descriptor;
}

// Удалить старую сводку до переключения приватного поколения
static int invalidate_catalog(void) {
    int directory = catalog_directory(1);
    if (directory < 0) {
        return -1;
    }

    int result = unlink(CATALOG_SNAPSHOT);
    if (result < 0 && errno == ENOENT) {
        result = 0;
    }

    if (result == 0) {
        result = fsync(directory);
    }

    close(directory);
    return result;
}

// Атомарно опубликовать динамический каталог пакетов текущего поколения
static int publish_catalog(const char *generation) {
    char root[PATH_MAX];
    char admin[PATH_MAX];
    char admin_opt[PATH_MAX];
    char temporary[PATH_MAX];
    char private_root[PATH_MAX];
    struct stat status;

    if (!valid_generation(generation) ||
        join_path(root, sizeof root, GENERATIONS, generation) < 0 ||
        lstat(root, &status) < 0 || !S_ISDIR(status.st_mode) || status.st_uid != 0 ||
        (status.st_mode & 0022U) != 0U ||
        join_path(private_root, sizeof private_root, root, "root") < 0 ||
        lstat(private_root, &status) < 0 || !S_ISDIR(status.st_mode) || status.st_uid != 0 ||
        (status.st_mode & 0022U) != 0U ||
        join_path(admin, sizeof admin, private_root, "var/lib/dpkg") < 0) {
        return -1;
    }

    if (publish_runtime(generation) < 0) {
        return -1;
    }

    int written = snprintf(admin_opt, sizeof admin_opt, "--admindir=%s", admin);
    if (written < 0 || (size_t)written >= sizeof admin_opt) {
        return -1;
    }

    char query_out[65536] = {0};
    char *const query_args[] = {
        "dpkg-query",
        admin_opt,
        "-W",
        "-f=${Package}\t${Version}\t${Status}\t${binary:Summary}\n",
        NULL
    };
    if (capture_program(DPKG_QUERY, query_args, query_out, sizeof query_out) < 0) {
        query_out[0] = '\0';
    }

    written = snprintf(temporary, sizeof temporary, CATALOG_DIRECTORY "/.catalog.%ld", (long)getpid());
    int directory = catalog_directory(1);
    if (written < 0 || (size_t)written >= sizeof temporary || directory < 0) {
        if (directory >= 0) {
            close(directory);
        }

        return -1;
    }

    int descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (descriptor < 0) {
        close(directory);
        return -1;
    }

    FILE *stream = fdopen(descriptor, "w");
    if (stream == NULL) {
        close(descriptor);
        unlink(temporary);
        close(directory);
        return -1;
    }

    char *line = query_out;
    while (*line != '\0') {
        char *nl = strchr(line, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }

        char *t1 = strchr(line, '\t');
        char *t2 = t1 != NULL ? strchr(t1 + 1, '\t') : NULL;
        char *t3 = t2 != NULL ? strchr(t2 + 1, '\t') : NULL;
        if (t1 != NULL && t2 != NULL && t3 != NULL) {
            *t1 = '\0';
            *t2 = '\0';
            *t3 = '\0';
            const char *pkg = line;
            const char *ver = t1 + 1;
            const char *st = t2 + 1;
            const char *desc = t3 + 1;
            int installed = strstr(st, "installed") != NULL;
            if (strcmp(pkg, "synapse-glibc-runtime") != 0) {
                fprintf(stream,
                        "{\"schema\":1,\"kind\":\"package\",\"id\":\"%s\","
                        "\"version\":\"%s\",\"description\":\"%s\","
                        "\"installed\":%s,\"installable\":true}\n",
                        pkg, ver, desc, installed ? "true" : "false");
            }
        }

        if (nl == NULL) {
            break;
        }
        line = nl + 1;
    }

    fflush(stream);
    int fd = fileno(stream);
    int result = (fchmod(fd, 0644) < 0 || fsync(fd) < 0) ? -1 : 0;
    if (fclose(stream) != 0) {
        result = -1;
    }

    if (result == 0 && (rename(temporary, CATALOG_SNAPSHOT) < 0 || fsync(directory) < 0)) {
        result = -1;
    }

    if (result != 0) {
        unlink(temporary);
    }

    close(directory);
    return result;
}

// Прочитать ограниченную сводку без доступа к приватному состоянию и без записи
static int catalog_snapshot(const char *text) {
    struct stat status;
    int directory = catalog_directory(0);
    if (directory < 0) {
        fprintf(stderr, "Debian catalog is unavailable, run debian-apt update as root\n");
        return 1;
    }

    int descriptor = open(CATALOG_SNAPSHOT, O_RDONLY | O_NOFOLLOW);
    close(directory);
    if (descriptor < 0 || fstat(descriptor, &status) < 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != 0 || (status.st_mode & 0777U) != 0644U ||
        status.st_size < 0 || status.st_size > 1048576) {
        if (descriptor >= 0) {
            close(descriptor);
        }

        fprintf(stderr, "Debian catalog is missing or unsafe, run debian-apt update as root\n");
        return 1;
    }

    FILE *stream = fdopen(descriptor, "r");
    if (stream == NULL) {
        close(descriptor);
        return 1;
    }

    char line[2048];
    while (fgets(line, sizeof line, stream) != NULL) {
        if (text[0] == '\0' || strstr(line, text) != NULL) {
            fputs(line, stdout);
        }
    }

    fclose(stream);
    return 0;
}

// Проверить каждого родителя до создания или чтения публичного пути
static int runtime_directory(const char *path, int create) {
    char copy[PATH_MAX];
    size_t length = strlen(path);

    if (length == 0U || path[0] != '/' || length >= sizeof copy) {
        return -1;
    }

    memcpy(copy, path, length + 1U);

    for (size_t index = 1U; index <= length; index++) {
        if (copy[index] != '/' && copy[index] != '\0') {
            continue;
        }

        char saved = copy[index];
        struct stat status;
        copy[index] = '\0';

        if (lstat(copy, &status) < 0) {
            if (!create || errno != ENOENT || mkdir(copy, 0755) < 0 ||
                chmod(copy, 0755) < 0 || lstat(copy, &status) < 0) {
                return -1;
            }
        }

        if (!S_ISDIR(status.st_mode)) {
            return -1;
        }

        if (strcmp(copy, RUNTIME_DIRECTORY) == 0 || strncmp(copy, RUNTIME_DIRECTORY "/", sizeof(RUNTIME_DIRECTORY)) == 0) {
            if (create && geteuid() == 0 && (status.st_uid != 0U || (status.st_mode & (S_IWGRP | S_IWOTH)) != 0U)) {
                (void)chown(copy, 0, 0);
                (void)chmod(copy, 0755);
                if (lstat(copy, &status) < 0) {
                    return -1;
                }
            }
            if (status.st_uid != 0U || (status.st_mode & (S_IWGRP | S_IWOTH)) != 0U) {
                return -1;
            }
        }

        copy[index] = saved;
    }

    return 0;
}

// Отключить новые запуски до изменения состава установленного поколения
static int invalidate_runtime(void) {
    if (runtime_directory(RUNTIME_DIRECTORY, 1) < 0) {
        return -1;
    }

    if (unlink(RUNTIME_CURRENT) < 0 && errno != ENOENT) {
        return -1;
    }

    return sync_directory(RUNTIME_DIRECTORY);
}

// Снять биты setuid/setgid с публикуемого публичного дерева runtime
static int sanitize_runtime_tree(const char *path, unsigned int depth) {
    DIR *directory = opendir(path);

    if (directory == NULL || depth > 64U) {
        if (directory != NULL) {
            closedir(directory);
        }
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        char child[PATH_MAX];
        struct stat status;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (join_path(child, sizeof child, path, entry->d_name) < 0 ||
            lstat(child, &status) < 0) {
            closedir(directory);
            return -1;
        }

        if (S_ISDIR(status.st_mode)) {
            if ((status.st_mode & (S_ISUID | S_ISGID | S_IWGRP | S_IWOTH)) != 0U) {
                (void)chmod(child, (status.st_mode & 0755U) | 0755U);
            }
            if (sanitize_runtime_tree(child, depth + 1U) < 0) {
                closedir(directory);
                return -1;
            }
        } else if (S_ISREG(status.st_mode)) {
            if ((status.st_mode & (S_ISUID | S_ISGID | S_IWGRP | S_IWOTH)) != 0U) {
                (void)chmod(child, status.st_mode & 0755U);
            }
        }
    }

    closedir(directory);
    return 0;
}

// Перевести абсолютные ссылки опубликованного дерева в относительные
// Ссылки, ведущие внутрь дерева, остаются рабочими без chroot
// Ссылки на ресурсы вне дерева, например устройства, сохраняются как есть
static int rewrite_absolute_links(const char *root, const char *path, unsigned int depth) {
    DIR *directory = opendir(path);

    if (directory == NULL || depth > 64U) {
        if (directory != NULL) {
            closedir(directory);
        }
        return -1;
    }

    int result = 0;
    struct dirent *entry;

    while (result == 0 && (entry = readdir(directory)) != NULL) {
        char child[PATH_MAX];
        char target[PATH_MAX];
        char resolved[PATH_MAX];
        struct stat status;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (join_path(child, sizeof child, path, entry->d_name) < 0 ||
            lstat(child, &status) < 0) {
            result = -1;
            break;
        }

        if (S_ISDIR(status.st_mode)) {
            if (rewrite_absolute_links(root, child, depth + 1U) < 0) {
                result = -1;
            }
            continue;
        }

        if (!S_ISLNK(status.st_mode)) {
            continue;
        }

        ssize_t length = readlink(child, target, sizeof target - 1U);

        if (length <= 0 || (size_t)length >= sizeof target - 1U) {
            result = -1;
            break;
        }

        target[length] = '\0';

        if (target[0] != '/' || strstr(target, "..") != NULL) {
            continue;
        }

        int written = snprintf(resolved, sizeof resolved, "%s%s", root, target);

        if (written < 0 || (size_t)written >= sizeof resolved ||
            lstat(resolved, &status) < 0) {
            continue;
        }

        size_t ups = 0;
        const char *relative = path + strlen(root);

        for (const char *cursor = relative; *cursor != '\0'; cursor++) {
            if (*cursor == '/') {
                ups++;
            }
        }

        char link[PATH_MAX];
        size_t used = 0U;

        for (size_t index = 0U; index < ups; index++) {
            if (used + 3U >= sizeof link) {
                result = -1;
                break;
            }
            memcpy(link + used, "../", 3U);
            used += 3U;
        }

        if (result != 0) {
            break;
        }

        written = snprintf(link + used, sizeof link - used, "%s", target + 1);

        if (written < 0 || used + (size_t)written >= sizeof link ||
            unlink(child) < 0 || symlink(link, child) < 0) {
            result = -1;
            break;
        }
    }

    closedir(directory);
    return result;
}

// Опубликовать полное дерево поколения для непривилегированного запуска
static int publish_runtime(const char *generation) {
    char private_directory[PATH_MAX];
    char private_root[PATH_MAX];
    char directory[PATH_MAX];
    char staging[PATH_MAX];
    char pointer[PATH_MAX];
    char contents[PATH_MAX];
    struct stat status;

    if (!valid_generation(generation) || invalidate_runtime() < 0 ||
        join_path(private_directory, sizeof private_directory, GENERATIONS, generation) < 0 ||
        runtime_directory(private_directory, 0) < 0 ||
        join_path(private_root, sizeof private_root, private_directory, "root") < 0 ||
        join_path(directory, sizeof directory, RUNTIME_DIRECTORY, generation) < 0) {
        return -1;
    }

#ifdef DEBIAN_APT_TEST_FAIL_RUNTIME_PUBLISH
    return -1;
#endif

    if (lstat(directory, &status) != 0) {
        if (errno != ENOENT) {
            return -1;
        }

        int written = snprintf(staging, sizeof staging, RUNTIME_DIRECTORY "/.stage.%ld", (long)getpid());
        if (written < 0 || (size_t)written >= sizeof staging || mkdir(staging, 0755) < 0) {
            return -1;
        }

        int stage = 1;
        if (join_path(contents, sizeof contents, private_root, ".") < 0) {
            stage = 0;
        } else if (copy_entry(contents, staging) != 0) {
            stage = 2;
        } else if (rewrite_absolute_links(staging, staging, 0U) < 0) {
            stage = 3;
        } else if (sanitize_runtime_tree(staging, 0U) < 0) {
            stage = 4;
        } else if (check_tree(staging, 0U) < 0) {
            stage = 5;
        } else if (sync_tree(staging, 0U) < 0) {
            stage = 6;
        } else if (rename(staging, directory) < 0) {
            stage = 7;
        } else if (sync_directory(RUNTIME_DIRECTORY) < 0) {
            stage = 8;
        } else {
            stage = 0;
        }
        if (stage != 0) {
            fprintf(stderr, "Debian runtime publish stage %d failed: %s\n", stage, strerror(errno));
            (void)remove_tree(staging, 0U);
            return -1;
        }
    }

    int written = snprintf(pointer, sizeof pointer, RUNTIME_DIRECTORY "/.current.%ld", (long)getpid());

    if (written < 0 || (size_t)written >= sizeof pointer ||
        symlink(generation, pointer) < 0 || rename(pointer, RUNTIME_CURRENT) < 0) {
        return -1;
    }

    return sync_directory(RUNTIME_DIRECTORY);
}

// Найти исполняемый файл в опубликованном дереве Debian
static int resolve_runtime_binary(const char *root, const char *name, char *out, size_t size) {
    const char *prefixes[] = {"usr/bin", "bin", "usr/sbin", "sbin", ""};

    if (strstr(name, "..") != NULL) {
        return -1;
    }

    for (size_t i = 0U; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        char candidate[PATH_MAX];
        if (prefixes[i][0] != '\0') {
            if (snprintf(candidate, sizeof candidate, "%s/%s/%s", root, prefixes[i], name) >= (int)sizeof candidate) {
                continue;
            }
        } else {
            const char *rel = name[0] == '/' ? name + 1 : name;
            if (snprintf(candidate, sizeof candidate, "%s/%s", root, rel) >= (int)sizeof candidate) {
                continue;
            }
        }

        struct stat st;
        if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode) && access(candidate, X_OK) == 0) {
            snprintf(out, size, "%s", candidate);
            return 0;
        }
    }

    return -1;
}

// Запустить программу из установленного дерева Debian с сохранением UID/GID
static int run_package(int argc, char **argv) {
    char generation[64];
    char directory[PATH_MAX];
    char binary[PATH_MAX];
    char loader[PATH_MAX];
    char libpath[PATH_MAX * 5];
    struct stat status;

    if (argc < 3 || argc > 64 ||
        runtime_directory(RUNTIME_DIRECTORY, 0) < 0 ||
        lstat(RUNTIME_CURRENT, &status) < 0 || !S_ISLNK(status.st_mode) || status.st_uid != 0U) {
        fprintf(stderr, "Verified Debian runtime is unavailable\n");
        return 1;
    }

    ssize_t length = readlink(RUNTIME_CURRENT, generation, sizeof generation - 1U);

    if (length <= 0 || (size_t)length >= sizeof generation - 1U) {
        return 1;
    }

    generation[length] = '\0';

    if (!valid_generation(generation) ||
        join_path(directory, sizeof directory, RUNTIME_DIRECTORY, generation) < 0 ||
        runtime_directory(directory, 0) < 0) {
        return 1;
    }

    if (resolve_runtime_binary(directory, argv[2], binary, sizeof binary) < 0) {
        fprintf(stderr, "Debian program is not installed: %s\n", argv[2]);
        return 1;
    }

    int is_dynamic = 0;
    if (check_elf_file(binary, &is_dynamic) < 0) {
        return 1;
    }

    char path_env[PATH_MAX * 4];
    char gconv_env[PATH_MAX + 32U];
    char loc_env[PATH_MAX + 32U];
    char xdg_env[PATH_MAX + 32U];
    if (snprintf(path_env, sizeof path_env,
                 "%s/usr/local/bin:%s/usr/bin:%s/bin:/usr/bin:/bin",
                 directory, directory, directory) >= (int)sizeof path_env ||
        snprintf(gconv_env, sizeof gconv_env, "%s/usr/lib/gconv", directory) >= (int)sizeof gconv_env ||
        snprintf(loc_env, sizeof loc_env, "%s/usr/lib/locale", directory) >= (int)sizeof loc_env ||
        snprintf(xdg_env, sizeof xdg_env, "%s/usr/share:/usr/share", directory) >= (int)sizeof xdg_env) {
        return 1;
    }
    setenv("PATH", path_env, 1);
    setenv("GCONV_PATH", gconv_env, 0);
    setenv("LOCPATH", loc_env, 0);
    setenv("XDG_DATA_DIRS", xdg_env, 0);
    setenv("LANG", "C.UTF-8", 0);

    char *arguments[70];
    size_t count = 0U;

    if (is_dynamic) {
        if (join_path(loader, sizeof loader, directory, "lib/ld-linux-x86-64.so.2") < 0) {
            return 1;
        }
        if (strlen(LOADER_SHA256) == 64U && verify_sha256(loader, LOADER_SHA256) < 0) {
            return 1;
        }

        if (snprintf(libpath, sizeof libpath,
                     "%s/lib:%s/lib/x86_64-linux-gnu:%s/usr/lib:%s/usr/lib/x86_64-linux-gnu",
                     directory, directory, directory, directory) >= (int)sizeof libpath) {
            return 1;
        }
        arguments[count++] = loader;
        arguments[count++] = "--library-path";
        arguments[count++] = libpath;
    }

    arguments[count++] = binary;

    for (int index = 3; index < argc; index++) {
        arguments[count++] = argv[index];
    }

    arguments[count] = NULL;
    return run_program(is_dynamic ? loader : binary, arguments);
}

// Удалить старые поколения, не являющиеся текущими
static int gc_generations(const char *current) {
    DIR *dir = opendir(GENERATIONS);
    if (dir == NULL) {
        return 1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, current) == 0 || strcmp(entry->d_name, "0") == 0) {
            continue;
        }
        if (!valid_generation(entry->d_name)) {
            continue;
        }

        char gen_path[PATH_MAX];
        char rt_path[PATH_MAX];
        if (join_path(gen_path, sizeof gen_path, GENERATIONS, entry->d_name) == 0) {
            (void)remove_tree(gen_path, 0U);
        }
        if (join_path(rt_path, sizeof rt_path, RUNTIME_DIRECTORY, entry->d_name) == 0) {
            (void)remove_tree(rt_path, 0U);
        }
    }

    closedir(dir);
    return 0;
}

// Выбрать операцию, сохраняя setup вне Debian-root
int main(int argc, char **argv) {
    const char *program_name = strrchr(argv[0], '/');

    if (program_name == NULL) {
        program_name = argv[0];
    } else {
        program_name++;
    }

    if (strcmp(program_name, "dpkg-blocked") == 0) {
        fprintf(stderr, "Use debian-apt install/remove for checked Debian generations\n");
        return 1;
    }

    if (argc < 2) {
        usage();
        return 2;
    }

    if (strcmp(argv[1], "catalog") == 0 && (argc == 2 || argc == 3)) {
        const char *query = argc == 3 ? argv[2] : "";

        if (strlen(query) > 100U) {
            fprintf(stderr, "Catalog query is too long\n");
            return 2;
        }

        if (geteuid() != 0) {
            return catalog_snapshot(query);
        }
    }

    if (strcmp(argv[1], "run") == 0) {
        return run_package(argc, argv);
    }

    umask(0077);

    // Путь к приватным помощникам backend и bootstrap-утилитам внутри chroot
    const char *base_path = getenv("PATH");
    char backend_path[PATH_MAX];

    if (snprintf(backend_path, sizeof backend_path,
                 "/usr/lib/synapse-runtime/bin:/usr/libexec/debian-apt:%s",
                 base_path == NULL ? "/bin:/usr/bin" : base_path) < (int)sizeof backend_path) {
        setenv("PATH", backend_path, 1);
    }

    // Интерактивные debconf-фронтенды в госте не поддерживаются
    setenv("DEBIAN_FRONTEND", "noninteractive", 0);

    int lock = open(DEBIAN_APT_BASE "/lock", O_CREAT | O_RDWR, 0600);

    if (lock < 0 && make_directories(DEBIAN_APT_BASE) == 0) {
        lock = open(DEBIAN_APT_BASE "/lock", O_CREAT | O_RDWR, 0600);
    }

    if (lock < 0 || flock(lock, LOCK_EX) < 0 || initialize_store() < 0 ||
        make_directories("/var/cache/debian-apt/archives/partial") < 0) {
        perror("debian-apt state");
        return 1;
    }

    char current[64];

    if (current_generation(current, sizeof current) < 0) {
        fprintf(stderr, "Invalid Debian generation pointer\n");
        return 1;
    }

    if ((strcmp(argv[1], "catalog") == 0 && (argc == 2 || argc == 3)) ||
        (strcmp(argv[1], "update") == 0 && argc == 2)) {
        if (strcmp(argv[1], "update") == 0) {
            int upd = execute_apt(APT_GET, "update", NULL, current, 0, 0);
            if (upd != 0) {
                return upd;
            }
        }

        if (publish_catalog(current) < 0) {
            fprintf(stderr, "Cannot publish the Debian catalog\n");
            return 1;
        }

        if (strcmp(argv[1], "catalog") == 0) {
            return catalog_snapshot(argc == 3 ? argv[2] : "");
        }

        return 0;
    }

    if (strcmp(argv[1], "current") == 0 && argc == 2) {
        printf("%s\n", current);
        return 0;
    }

    if (strcmp(argv[1], "status") == 0 && argc == 2) {
        printf("{\"schema\":1,\"kind\":\"state\",\"generation\":\"%s\","
               "\"mode\":\"isolated\"}\n", current);
        return 0;
    }

    if (strcmp(argv[1], "gc") == 0 && argc == 2) {
        return gc_generations(current);
    }

    if (strcmp(argv[1], "upgrade") == 0 && argc == 2) {
        return mutate_package("upgrade", NULL);
    }

    if (strcmp(argv[1], "rollback") == 0 && argc == 3) {
        char directory[PATH_MAX];
        char root[PATH_MAX];
        struct stat status;

        if (!valid_generation(argv[2]) ||
            join_path(directory, sizeof directory, GENERATIONS, argv[2]) < 0 ||
            join_path(root, sizeof root, directory, "root") < 0 ||
            stat(root, &status) < 0 || !S_ISDIR(status.st_mode) ||
            check_tree(root, 0U) < 0 || commit_generation(argv[2]) < 0) {
            fprintf(stderr, "Cannot select Debian generation\n");
            return 1;
        }

        printf("Debian generation %s is current\n", argv[2]);
        return 0;
    }

    if (argc != 3) {
        usage();
        return 2;
    }

    if (strcmp(argv[1], "search") == 0 || strcmp(argv[1], "show") == 0 ||
        strcmp(argv[1], "policy") == 0) {
        return execute_apt(APT_CACHE, argv[1], argv[2], current, 0, 0);
    }

    if (strcmp(argv[1], "install") == 0 || strcmp(argv[1], "remove") == 0) {
        if (!valid_package(argv[2])) {
            fprintf(stderr, "Invalid Debian package name\n");
            return 2;
        }

        return mutate_package(argv[1], argv[2]);
    }

    usage();
    return 2;
}
