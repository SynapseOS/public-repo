// debian-apt.c: изолированные операции APT с переключением поколений
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: synapse-x86_64-v1
// Подсистема: менеджер Debian-пакетов
// Первое изменение: 01.11.2026, последнее изменение: 01.11.2026
// Статус доступа: общедоступный

#define _GNU_SOURCE

#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
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
#define BUSYBOX_INSTALL "busybox-static=1:1.37.0-6+b9"
#define BUSYBOX_ELF_SHA256 "7ae95c260622412ceba6eb9f45d68f616cdc091a1d71d6c0a08b5875bb2c31dd"
#define M4_INSTALL "m4=1.4.19-8"
#define M4_ARCHIVE "/var/cache/debian-apt/archives/m4_1.4.19-8_amd64.deb"
#define M4_ARCHIVE_SHA256 "221b3e224708c1da2a1a88b6824380593579b2254ae7d914afa14cbd5d343334"
#define M4_ELF_SHA256 "24066e3ebc4b7a0eeb33b3d50e108afcfd384f1f01dd1797fa0423d5494cce16"
#define PROVIDER_PATH "/usr/share/debian-apt/synapse-glibc-runtime.deb"
#define PACKAGE_LISTS DEBIAN_APT_BASE "/lists"
#define GENERATIONS DEBIAN_APT_BASE "/generations"
#define CURRENT DEBIAN_APT_BASE "/current"

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

// Создать исходное поколение при первом запуске
static int initialize_store(void) {
    char initial[PATH_MAX];
    char admin[PATH_MAX];
    char root[PATH_MAX];
    char destination[PATH_MAX];
    struct stat status;

    if (make_directories(GENERATIONS) < 0) {
        return -1;
    }

    if (lstat(CURRENT, &status) == 0) {
        return S_ISLNK(status.st_mode) ? 0 : -1;
    }

    if (errno != ENOENT || join_path(initial, sizeof initial, GENERATIONS, "0") < 0 ||
        join_path(admin, sizeof admin, initial, "admin") < 0 ||
        join_path(root, sizeof root, initial, "root") < 0 ||
        make_directories(admin) < 0 || make_directories(root) < 0) {
        return -1;
    }

    if (join_path(destination, sizeof destination, admin, "status") < 0 ||
        copy_entry("/etc/debian-apt/status", destination) != 0 ||
        join_path(destination, sizeof destination, admin, "arch") < 0 ||
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

// Проверить исполняемый формат каждого файла нового поколения
static int check_elf_file(const char *path) {
    if (ends_with(path, "/usr/bin/busybox")) {
        return verify_sha256(path, BUSYBOX_ELF_SHA256);
    }

    if (ends_with(path, "/lib/ld-linux-x86-64.so.2")) {
        return verify_sha256(path, LOADER_SHA256);
    }

    if (ends_with(path, "/lib/libc.so.6")) {
        return verify_sha256(path, LIBC_SHA256);
    }

    if (ends_with(path, "/usr/bin/m4")) {
        return verify_sha256(path, M4_ELF_SHA256);
    }

    int descriptor = open(path, O_RDONLY);

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
        header.e_type != ET_EXEC || header.e_phentsize != sizeof(Elf64_Phdr) ||
        header.e_phnum > 128U) {
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

        if (segment.p_type == PT_INTERP || segment.p_type == PT_DYNAMIC) {
            fprintf(stderr, "Dynamic ELF needs a verified runtime: %s\n", path);
            close(descriptor);
            return -1;
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
            if (check_elf_file(child) < 0) {
                closedir(directory);
                return -1;
            }
        } else if (!S_ISLNK(status.st_mode)) {
            fprintf(stderr, "Unsupported package file: %s\n", child);
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
            int descriptor = open(child, O_RDONLY);

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

// Переключить текущий указатель только после проверки поколения
static int commit_generation(const char *name) {
    char target[PATH_MAX];
    char path[PATH_MAX];
    int written = snprintf(target, sizeof target, "generations/%s", name);

    if (written < 0 || (size_t)written >= sizeof target ||
        snprintf(path, sizeof path, "%s/current.%ld", DEBIAN_APT_BASE, (long)getpid()) >= (int)sizeof path) {
        return -1;
    }

    if (symlink(target, path) < 0 || rename(path, CURRENT) < 0) {
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
    return 0;
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
                           "Dir::State::status=%s/admin/status", directory);

    if (written < 0 || (size_t)written >= sizeof status_option) {
        return -1;
    }

    written = snprintf(admin_option, sizeof admin_option,
                       "DPkg::Options::=--admindir=%s/admin", directory);

    if (written < 0 || (size_t)written >= sizeof admin_option) {
        return -1;
    }

    written = snprintf(root_option, sizeof root_option,
                       "DPkg::Options::=--instdir=%s/root", directory);

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

    for (size_t index = 0U; index < 2U; index++) {
        const char *part = index == 0U ? "admin" : "root";

        if (join_path(destination, sizeof destination, directory, part) < 0 ||
            make_directories(destination) < 0 ||
            join_path(source, sizeof source, CURRENT, part) < 0) {
            return -1;
        }

        char contents[PATH_MAX];

        if (join_path(contents, sizeof contents, source, ".") < 0 ||
            copy_entry(contents, destination) != 0) {
            return -1;
        }
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
                    "install NAME|remove NAME|run NAME [ARGS]|catalog [TEXT]|current|status|rollback ID\n");
}

// Установить проверенный Synapse glibc runtime как обычный dpkg provider
static int install_provider(const char *directory) {
    char admin[PATH_MAX];
    char root[PATH_MAX];
    char admin_option[PATH_MAX];
    char root_option[PATH_MAX];

    if (verify_sha256(PROVIDER_PATH, PROVIDER_SHA256) < 0 ||
        join_path(admin, sizeof admin, directory, "admin") < 0 ||
        join_path(root, sizeof root, directory, "root") < 0) {
        return -1;
    }

    int written = snprintf(admin_option, sizeof admin_option, "--admindir=%s", admin);

    if (written < 0 || (size_t)written >= sizeof admin_option) {
        return -1;
    }

    written = snprintf(root_option, sizeof root_option, "--instdir=%s", root);

    if (written < 0 || (size_t)written >= sizeof root_option) {
        return -1;
    }

    char *const arguments[] = {"dpkg-native", admin_option, root_option, "-i",
                               PROVIDER_PATH, NULL};

    return run_program(DPKG_NATIVE, arguments);
}

// Проверить архив m4 и отсутствие скриптов до запуска dpkg
static int preflight_m4(const char *directory) {
    char control[PATH_MAX];
    char files[PATH_MAX];
    char binary[PATH_MAX];

    if (verify_sha256(M4_ARCHIVE, M4_ARCHIVE_SHA256) < 0 ||
        join_path(control, sizeof control, directory, "preflight-control") < 0 ||
        join_path(files, sizeof files, directory, "preflight-files") < 0 ||
        make_directories(control) < 0 || make_directories(files) < 0) {
        return -1;
    }

    char *const control_args[] = {"dpkg-deb", "--control", M4_ARCHIVE, control, NULL};

    if (run_program("/bin/dpkg-deb", control_args) != 0) {
        return -1;
    }

    DIR *stream = opendir(control);

    if (stream == NULL) {
        return -1;
    }

    struct dirent *entry;
    int seen_control = 0;

    while ((entry = readdir(stream)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, "md5sums") == 0) {
            continue;
        }

        if (strcmp(entry->d_name, "control") == 0) {
            seen_control = 1;
            continue;
        }

        fprintf(stderr, "Unapproved Debian maintainer file: %s\n", entry->d_name);
        closedir(stream);
        return -1;
    }

    closedir(stream);

    if (!seen_control) {
        return -1;
    }

    char *const extract_args[] = {"dpkg-deb", "--extract", M4_ARCHIVE, files, NULL};

    if (run_program("/bin/dpkg-deb", extract_args) != 0 ||
        join_path(binary, sizeof binary, files, "usr/bin/m4") < 0 ||
        verify_sha256(binary, M4_ELF_SHA256) < 0 || check_tree(files, 0U) < 0) {
        return -1;
    }

    return 0;
}

// Провести пакетную операцию и переключить поколение при успехе
static int mutate_package(const char *command, const char *package) {
    char name[64];
    char directory[PATH_MAX];
    char root[PATH_MAX];

    int installing = strcmp(command, "install") == 0;
    int m4 = strcmp(package, "m4") == 0;
    const char *request = package;

    if (installing) {
        request = m4 ? M4_INSTALL : BUSYBOX_INSTALL;

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

    if (installing && m4 &&
        (install_provider(directory) != 0 ||
         execute_apt(APT_GET, "install", request, name, 1, 1) != 0 ||
         preflight_m4(directory) < 0)) {
        fprintf(stderr, "Debian m4 preflight failed, current generation is unchanged\n");
        return 1;
    }

    if (execute_apt(APT_GET, command, request, name, 1, 0) != 0 ||
        join_path(root, sizeof root, directory, "root") < 0 ||
        check_tree(root, 0U) < 0 || sync_tree(directory, 0U) < 0 ||
        sync_directory(GENERATIONS) < 0) {
        fprintf(stderr, "Debian transaction failed, current generation is unchanged\n");
        return 1;
    }

#ifdef DEBIAN_APT_TEST_KILL_BEFORE_COMMIT
    raise(SIGKILL);
#endif

    if (commit_generation(name) < 0) {
        fprintf(stderr, "Debian transaction failed during generation switch\n");
        return 1;
    }

    printf("Debian generation %s is current\n", name);
    return 0;
}

// Вывести поддерживаемые пакеты строками JSON для магазина
static int catalog(const char *generation, const char *text) {
    char root[PATH_MAX];
    char busybox[PATH_MAX];
    char m4[PATH_MAX];

    if (join_path(root, sizeof root, GENERATIONS, generation) < 0 ||
        join_path(busybox, sizeof busybox, root, "root/usr/bin/busybox") < 0 ||
        join_path(m4, sizeof m4, root, "root/usr/bin/m4") < 0) {
        return 1;
    }

    if (strstr("busybox-static", text) != NULL || strstr("Static Unix utilities", text) != NULL) {
        printf("{\"schema\":1,\"kind\":\"package\",\"id\":\"busybox-static\","
               "\"version\":\"1:1.37.0-6+b9\",\"description\":\"Static Unix utilities\","
               "\"installed\":%s,\"installable\":true,\"reason\":\"signed-index-required\"}\n",
               access(busybox, F_OK) == 0 ? "true" : "false");
    }

    if (strstr("m4", text) != NULL || strstr("GNU macro processor", text) != NULL) {
        printf("{\"schema\":1,\"kind\":\"package\",\"id\":\"m4\","
               "\"version\":\"1.4.19-8\",\"description\":\"GNU macro processor\","
               "\"installed\":%s,\"installable\":true,\"reason\":\"signed-index-required\"}\n",
               access(m4, F_OK) == 0 ? "true" : "false");
    }

    return 0;
}

// Запустить разрешённую программу из выбранного поколения
static int run_package(const char *generation, int argc, char **argv) {
    char root[PATH_MAX];
    char binary[PATH_MAX];
    char loader[PATH_MAX];
    char libraries[PATH_MAX];
    char libc[PATH_MAX];

    if (argc < 3 || argc > 64 ||
        (strcmp(argv[2], "m4") != 0 && strcmp(argv[2], "busybox-static") != 0) ||
        join_path(root, sizeof root, GENERATIONS, generation) < 0) {
        fprintf(stderr, "Debian program is outside the verified ABI allowlist\n");
        return 1;
    }

    if (strcmp(argv[2], "busybox-static") == 0) {
        if (join_path(binary, sizeof binary, root, "root/usr/bin/busybox") < 0) {
            fprintf(stderr, "Verified Debian busybox-static is not installed\n");
            return 1;
        }

        int descriptor = open(binary, O_RDONLY);
        struct stat status;
        char magic[SELFMAG];

        if (descriptor < 0 || fstat(descriptor, &status) < 0 ||
            !S_ISREG(status.st_mode) || status.st_uid != 0U ||
            (status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
            pread(descriptor, magic, sizeof magic, 0) != (ssize_t)sizeof magic ||
            memcmp(magic, ELFMAG, SELFMAG) != 0) {
            if (descriptor >= 0) {
                close(descriptor);
            }

            fprintf(stderr, "Verified Debian busybox-static is not installed\n");
            return 1;
        }

        close(descriptor);

        if (check_elf_file(binary) < 0 || access(binary, X_OK) < 0) {
            fprintf(stderr, "Verified Debian busybox-static is not installed\n");
            return 1;
        }

        char *arguments[65];
        arguments[0] = binary;

        for (int index = 3; index < argc; index++) {
            arguments[index - 2] = argv[index];
        }

        arguments[argc - 2] = NULL;
        return run_program(binary, arguments);
    }

    if (join_path(binary, sizeof binary, root, "root/usr/bin/m4") < 0 ||
        join_path(loader, sizeof loader, root, "root/lib/ld-linux-x86-64.so.2") < 0 ||
        join_path(libraries, sizeof libraries, root, "root/lib") < 0 ||
        join_path(libc, sizeof libc, libraries, "libc.so.6") < 0 ||
        check_elf_file(binary) < 0 || verify_sha256(loader, LOADER_SHA256) < 0 ||
        verify_sha256(libc, LIBC_SHA256) < 0) {
        fprintf(stderr, "Verified Debian m4 is not installed\n");
        return 1;
    }

    char *arguments[68];
    arguments[0] = loader;
    arguments[1] = "--library-path";
    arguments[2] = libraries;
    arguments[3] = binary;

    for (int index = 3; index < argc; index++) {
        arguments[index + 1] = argv[index];
    }

    arguments[argc + 1] = NULL;
    return run_program(loader, arguments);
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

    if (strcmp(argv[1], "current") == 0 && argc == 2) {
        printf("%s\n", current);
        return 0;
    }

    if (strcmp(argv[1], "status") == 0 && argc == 2) {
        printf("{\"schema\":1,\"kind\":\"state\",\"generation\":\"%s\","
               "\"mode\":\"isolated\"}\n", current);
        return 0;
    }

    if (strcmp(argv[1], "catalog") == 0 && (argc == 2 || argc == 3)) {
        const char *query = argc == 3 ? argv[2] : "";

        if (strlen(query) > 100U) {
            fprintf(stderr, "Catalog query is too long\n");
            return 2;
        }

        return catalog(current, query);
    }

    if (strcmp(argv[1], "run") == 0) {
        return run_package(current, argc, argv);
    }

    if (strcmp(argv[1], "update") == 0 && argc == 2) {
        return execute_apt(APT_GET, "update", NULL, current, 0, 0);
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

        if (strcmp(argv[2], "busybox-static") != 0 && strcmp(argv[2], "m4") != 0) {
            fprintf(stderr, "Debian package is outside the verified ABI allowlist\n");
            return 2;
        }

        return mutate_package(argv[1], argv[2]);
    }

    usage();
    return 2;
}
