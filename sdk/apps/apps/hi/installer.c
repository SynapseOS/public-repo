// installer.c: Установка системы на диск
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: Linux (ELF64)
//
// Работа с GPT и копирование файлов на выбранный целевой раздел
// Подсистема: установка и начальная настройка системы
// Первое изменение: до текущей уборки, исходная дата не установлена
// Последнее изменение: 22.11.2026
// Доступ: общедоступный

#define _GNU_SOURCE
#include "installer.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SECTOR 512ULL
#define MAX_ENTRIES 256
#define ALIGN_SECTORS 2048ULL

struct gpt_header {
    unsigned char signature[8];
    uint32_t revision, header_size, header_crc32, reserved;
    uint64_t current_lba, backup_lba, first_usable_lba, last_usable_lba;
    unsigned char disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count, entry_size, entries_crc32;
} __attribute__((packed));

struct gpt_entry {
    unsigned char type[16], unique[16];
    uint64_t first_lba, last_lba, attributes;
    uint16_t name[36];
} __attribute__((packed));

struct disk {
    char path[128], name[96];
    int fd;
    uint64_t bytes;
    struct gpt_header primary, backup;
    unsigned char *entries;
    size_t entries_bytes;
};

static int is_ru;
static char source_device[128];
static const char *tt(const char *en, const char *ru) {
    return is_ru ? ru : en;
}

static uint32_t crc32_bytes(const void *data, size_t length) {
    uint32_t crc = 0xffffffffu;
    const unsigned char *p = data;
    while (length--) {
        crc ^= *p++;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & -(int)(crc & 1));
        }
    }
    return ~crc;
}

static int read_full(int fd, void *buffer, size_t length, off_t offset) {
    unsigned char *p = buffer;
    while (length) {
        ssize_t n = pread(fd, p, length, offset);
        if (n <= 0) {
            return -1;
        }
        p += n;
        length -= (size_t)n;
        offset += n;
    }
    return 0;
}

static int write_full(int fd, const void *buffer, size_t length, off_t offset) {
    const unsigned char *p = buffer;
    while (length) {
        ssize_t n = pwrite(fd, p, length, offset);
        if (n <= 0) {
            return -1;
        }
        p += n;
        length -= (size_t)n;
        offset += n;
    }
    return 0;
}

static bool zero_guid(const unsigned char guid[16]) {
    for (int i = 0; i < 16; ++i) {
        if (guid[i]) {
            return false;
        }
    }
    return true;
}

static int valid_header(struct gpt_header *h, uint64_t sectors, uint64_t expected_lba) {
    if (memcmp(h->signature, "EFI PART", 8) || h->revision != 0x10000 ||
        h->header_size != sizeof(struct gpt_header) || h->current_lba != expected_lba ||
        h->backup_lba >= sectors || h->first_usable_lba > h->last_usable_lba ||
        h->last_usable_lba >= sectors || !h->entry_count ||
        h->entry_count > MAX_ENTRIES || h->entry_size < sizeof(struct gpt_entry) ||
        h->entry_size > 4096) {
        return -1;
    }
    uint32_t saved = h->header_crc32;
    h->header_crc32 = 0;
    uint32_t actual = crc32_bytes(h, h->header_size);
    h->header_crc32 = saved;
    return saved == actual ? 0 : -1;
}

static uint64_t device_size(int fd) {
    size_t bytes = 0;
#ifdef BLKGETSIZE64
    if (!ioctl(fd, BLKGETSIZE64, &bytes) && bytes) {
        return bytes;
    }
#endif
    off_t end = lseek(fd, 0, SEEK_END);
    return end > 0 ? (uint64_t)end : 0;
}

static int disk_open(struct disk *d, const char *path) {
    memset(d, 0, sizeof(*d));
    d->fd = -1;
    snprintf(d->path, sizeof(d->path), "%s", path);
    const char *base = strrchr(path, '/');
    snprintf(d->name, sizeof(d->name), "%.95s", base ? base + 1 : path);
    d->fd = open(path, O_RDWR);
    if (d->fd < 0) {
        return -1;
    }

    int logical_sector = 0;
#ifdef BLKSSZGET
    if (!ioctl(d->fd, BLKSSZGET, &logical_sector) && logical_sector != (int)SECTOR) {
        goto bad;
    }
#endif
    d->bytes = device_size(d->fd);
    uint64_t sectors = d->bytes / SECTOR;
    unsigned char sector[SECTOR];
    if (sectors < 4096 || read_full(d->fd, sector, sizeof(sector), SECTOR)) {
        goto bad;
    }
    memcpy(&d->primary, sector, sizeof(d->primary));
    if (valid_header(&d->primary, sectors, 1)) {
        goto bad;
    }
    if (read_full(d->fd, sector, sizeof(sector),
                  (off_t)d->primary.backup_lba * SECTOR)) {
        goto bad;
    }
    memcpy(&d->backup, sector, sizeof(d->backup));
    if (valid_header(&d->backup, sectors, d->primary.backup_lba) ||
        d->backup.backup_lba != 1 ||
        memcmp(d->backup.disk_guid, d->primary.disk_guid, 16) ||
        d->backup.entry_count != d->primary.entry_count ||
        d->backup.entry_size != d->primary.entry_size) {
        goto bad;
    }
    if (d->primary.entry_count > SIZE_MAX / d->primary.entry_size) {
        goto bad;
    }
    d->entries_bytes = (size_t)d->primary.entry_count * d->primary.entry_size;
    d->entries = malloc(d->entries_bytes);
    if (!d->entries) {
        goto bad;
    }
    if (read_full(d->fd, d->entries, d->entries_bytes,
                  (off_t)d->primary.entries_lba * SECTOR) ||
        crc32_bytes(d->entries, d->entries_bytes) != d->primary.entries_crc32) {
        goto bad;
    }

    unsigned char *backup = malloc(d->entries_bytes);
    if (!backup) {
        goto bad;
    }

    int result = read_full(d->fd, backup, d->entries_bytes,
                           (off_t)d->backup.entries_lba * SECTOR) ||
                 crc32_bytes(backup, d->entries_bytes) != d->backup.entries_crc32 ||
                 memcmp(backup, d->entries, d->entries_bytes);
    free(backup);
    if (result) {
        goto bad;
    }
    return 0;
bad:
    free(d->entries);
    d->entries = NULL;
    close(d->fd);
    d->fd = -1;
    errno = EINVAL;
    return -1;
}

static void disk_close(struct disk *d) {
    free(d->entries);
    if (d->fd >= 0) {
        close(d->fd);
    }
}
static struct gpt_entry *entry_at(struct disk *d, unsigned index) {
    return (struct gpt_entry *)(d->entries + (size_t)index * d->primary.entry_size);
}

static void entry_name(const struct gpt_entry *e, char *out, size_t capacity) {
    size_t n = 0;
    for (int i = 0; i < 36 && e->name[i] && n + 1 < capacity; ++i) {
        uint16_t c = e->name[i];
        out[n++] = c < 128 ? (char)c : '?';
    }
    out[n] = 0;
}

static void set_name(struct gpt_entry *e, const char *name) {
    memset(e->name, 0, sizeof(e->name));
    for (size_t i = 0; name[i] && i < 35; ++i) {
        e->name[i] = (unsigned char)name[i];
    }
}

static int commit_gpt(struct disk *d) {
    uint32_t table_crc = crc32_bytes(d->entries, d->entries_bytes);
    d->primary.entries_crc32 = d->backup.entries_crc32 = table_crc;
    d->primary.header_crc32 = 0;
    d->primary.header_crc32 = crc32_bytes(&d->primary, d->primary.header_size);
    d->backup.header_crc32 = 0;
    d->backup.header_crc32 = crc32_bytes(&d->backup, d->backup.header_size);
    unsigned char p[SECTOR] = {0}, b[SECTOR] = {0};
    memcpy(p, &d->primary, sizeof(d->primary));
    memcpy(b, &d->backup, sizeof(d->backup));
    if (write_full(d->fd, d->entries, d->entries_bytes,
                   (off_t)d->backup.entries_lba * SECTOR) ||
        write_full(d->fd, b, sizeof(b), (off_t)d->backup.current_lba * SECTOR) ||
        write_full(d->fd, d->entries, d->entries_bytes,
                   (off_t)d->primary.entries_lba * SECTOR) ||
        write_full(d->fd, p, sizeof(p), SECTOR) || fsync(d->fd)) {
        return -1;
    }
    return 0;
}

static uint64_t aligned_up(uint64_t value) {
    return (value + ALIGN_SECTORS - 1) & ~(ALIGN_SECTORS - 1);
}
static uint64_t aligned_down(uint64_t value) {
    return value & ~(ALIGN_SECTORS - 1);
}

static int largest_free(struct disk *d, uint64_t *start, uint64_t *end) {
    uint64_t best_start = 0, best_end = 0;
    for (uint64_t cursor = aligned_up(d->primary.first_usable_lba);
         cursor <= d->primary.last_usable_lba;) {
        uint64_t next = d->primary.last_usable_lba + 1;
        for (unsigned i = 0; i < d->primary.entry_count; ++i) {
            struct gpt_entry *e = entry_at(d, i);
            if (!zero_guid(e->type) && e->first_lba >= cursor && e->first_lba < next) {
                next = e->first_lba;
            }
        }
        uint64_t free_end = next ? aligned_down(next - 1) : 0;
        if (free_end >= cursor && free_end - cursor > best_end - best_start) {
            best_start = cursor;
            best_end = free_end;
        }
        if (next > d->primary.last_usable_lba) {
            break;
        }
        uint64_t occupied_end = next;
        for (unsigned i = 0; i < d->primary.entry_count; ++i) {
            struct gpt_entry *e = entry_at(d, i);
            if (!zero_guid(e->type) && e->first_lba <= next &&
                e->last_lba > occupied_end) {
                occupied_end = e->last_lba;
            }
        }
        cursor = aligned_up(occupied_end + 1);
    }
    if (!best_start) {
        return -1;
    }
    *start = best_start;
    *end = best_end;
    return 0;
}

static int find_empty(struct disk *d) {
    int highest = -1;
    for (unsigned i = 0; i < d->primary.entry_count; ++i) {
        if (!zero_guid(entry_at(d, i)->type)) {
            highest = (int)i;
        }
    }

    for (unsigned i = (unsigned)(highest + 1); i < d->primary.entry_count; ++i) {
        if (zero_guid(entry_at(d, i)->type)) {
            return (int)i;
        }
    }

    for (int i = 0; i <= highest; ++i) {
        if (zero_guid(entry_at(d, (unsigned)i)->type)) {
            return i;
        }
    }
    return -1;
}

static bool esp_type(const unsigned char type[16]) {
    static const unsigned char esp[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8,
                                          0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0,
                                          0xc9, 0x3e, 0xc9, 0x3b};
    return !memcmp(type, esp, 16);
}

static void partition_path(const struct disk *d, unsigned index, char *path,
                           size_t capacity) {
    snprintf(path, capacity, "%sp%u", d->path, index);
}

static unsigned device_index(struct disk *d, unsigned entry_index) {
    unsigned result = 0;
    for (unsigned i = 0; i < entry_index; ++i) {
        if (!zero_guid(entry_at(d, i)->type)) {
            result++;
        }
    }
    return result;
}

static bool media_payload(char *path, size_t capacity, const char *directory) {
    const char *names[] = {"delta.xz", "install.xz"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        int n = snprintf(path, capacity, "%s/boot/%s", directory, names[i]);
        if (n > 0 && (size_t)n < capacity && !access(path, R_OK)) {
            return true;
        }
    }
    return false;
}

static const char *find_payload(void) {
    static char path[256];
    if (media_payload(path, sizeof(path), "/mnt/live")) {
        // Идентификатор исходного диска предоставлен ядром через readlink
        ssize_t n = readlink("/dev/live-source", source_device, sizeof(source_device) - 1);
        if (n > 0 && (size_t)n < sizeof(source_device) - 1) {
            source_device[n] = 0;
            return path;
        }
    }
    for (char c = 'a'; c <= 'z'; ++c) {
        char directory[32];
        snprintf(directory, sizeof(directory), "/mnt/iso%c", c);
        if (media_payload(path, sizeof(path), directory)) {
            return path;
        }
    }
    glob_t devices = {0};
    const char *patterns[] = {"/dev/sd?", "/dev/vd?", "/dev/nvme?n?"};
    int mount_number = 0;
    for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); ++p) {
        if (glob(patterns[p], 0, NULL, &devices)) {
            continue;
        }

        for (size_t i = 0; i < devices.gl_pathc; ++i) {
            char directory[128];
            snprintf(directory, sizeof(directory), "/tmp/hi-source%d", mount_number++);
            mkdir(directory, 0755);
            if (mount(devices.gl_pathv[i], directory, "isofs", MS_RDONLY, NULL)) {
                continue;
            }
            if (media_payload(path, sizeof(path), directory)) {
                snprintf(source_device, sizeof(source_device), "%s",
                         devices.gl_pathv[i]);
                globfree(&devices);
                return path;
            }
        }
        globfree(&devices);
        memset(&devices, 0, sizeof(devices));
    }
    return NULL;
}

static uint64_t payload_size(const char *payload) {
    char meta[320], line[128];
    const char *slash = strrchr(payload, '/');
    snprintf(meta, sizeof(meta), "%.*s/install.inf", (int)(slash ? slash - payload : 0),
             payload);
    FILE *f = fopen(meta, "r");
    if (!f) {
        return 0;
    }
    uint64_t size = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "size=%llu", (unsigned long long *)&size) == 1) {
            break;
        }
    }
    fclose(f);
    return size;
}

// Прочитать исходный блок с ISO, из потока отличий или заполнить его нулями
static int delta_record(FILE *stream, int base, unsigned char *buffer, uint64_t remaining) {
    uint32_t header[3];
    if (fread(header, sizeof(header), 1, stream) != 1) {
        return -1;
    }
    uint32_t source = header[0], length = header[1];
    if (!length || length > 65536 || length % 4096 || length > remaining) {
        return -1;
    }
    if (source == UINT32_MAX) {
        if (fread(buffer, 1, length, stream) != length) {
            return -1;
        }
    } else if (source == UINT32_MAX - 1) {
        memset(buffer, 0, length);
    } else if (read_full(base, buffer, length, (off_t)source * 4096)) {
        return -1;
    }
    if (crc32_bytes(buffer, length) != header[2]) {
        return -1;
    }
    return (int)length;
}

static int copy_payload(FILE *stream, const char *payload, int disk_fd, uint64_t offset, uint64_t expected) {
    unsigned char buffer[65536];
    int base = -1;
    const char *name = strrchr(payload, '/');
    if (name && !strcmp(name, "/delta.xz")) {
        char path[320];
        static const char delta_magic[] __attribute__((used)) = "SYNDEL01";
        char magic[8];
        if (fread(magic, 1, sizeof(magic), stream) != sizeof(magic) || memcmp(magic, delta_magic, sizeof(magic))) {
            return -1;
        }
        // Имена одного файла в Rock Ridge и ISO9660
        const char *names[] = {"live-root.ext4", "live_roo.ext"};
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            snprintf(path, sizeof(path), "%.*s/%s", (int)(name - payload), payload, names[i]);
            base = open(path, O_RDONLY);
            if (base >= 0) {
                break;
            }
        }
        if (base < 0) {
            snprintf(path, sizeof(path), "%.*s/root.lz4", (int)(name - payload), payload);
            if (!access(path, R_OK)) {
                base = open("/dev/livebase", O_RDONLY);
            }
        }
        if (base < 0) {
            return -1;
        }
    }

    uint64_t total = 0;
    while (total < expected) {
        uint64_t left = expected - total;
        size_t want = left < sizeof(buffer) ? (size_t)left : sizeof(buffer);
        int n = base >= 0 ? delta_record(stream, base, buffer, left) : (int)fread(buffer, 1, want, stream);
        if (n <= 0 || write_full(disk_fd, buffer, (size_t)n, (off_t)(offset + total))) {
            break;
        }
        uint64_t previous = total;
        total += (size_t)n;
        if (previous / 1048576 != total / 1048576) {
            printf("\r%s: %llu MiB", tt("Writing system", "Запись системы"), (unsigned long long)(total / 1048576));
            fflush(stdout);
        }
    }
    if (base >= 0) {
        close(base);
    }
    return total == expected && fgetc(stream) == EOF && !ferror(stream) ? 0 : -1;
}

static int copy_stream_to_disk(const char *payload, int disk_fd, uint64_t offset,
                               uint64_t limit, uint64_t expected) {
    int pipefd[2];
    if (pipe(pipefd)) {
        return -1;
    }

    pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (!child) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl("/bin/xz", "xz", "-dc", payload, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    FILE *stream = fdopen(pipefd[0], "rb");
    int failed = -1;
    if (stream) {
        if (expected <= limit) {
            failed = copy_payload(stream, payload, disk_fd, offset, expected);
        }
        fclose(stream);
    } else {
        close(pipefd[0]);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    puts("");
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) || failed || fsync(disk_fd)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int copy_file(const char *from, const char *to) {
    int in = open(from, O_RDONLY);
    if (in < 0) {
        return -1;
    }

    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return -1;
    }

    char buffer[65536];
    int failed = 0;
    ssize_t n;
    off_t offset = 0;
    while ((n = read(in, buffer, sizeof(buffer))) > 0) {
        if (write_full(out, buffer, (size_t)n, offset)) {
            failed = 1;
            break;
        }
        offset += n;
    }
    if (n < 0 || fsync(out)) {
        failed = 1;
    }
    close(in);
    close(out);
    return failed ? -1 : 0;
}

static int mkdir_ok(const char *path) {
    return !mkdir(path, 0755) || errno == EEXIST ? 0 : -1;
}

static int install_boot(struct disk *d, unsigned root_index, const char *iso_root, bool configure_grub) {
    int esp_index = -1;
    for (unsigned i = 0; i < d->primary.entry_count; ++i) {
        if (esp_type(entry_at(d, i)->type)) {
            esp_index = (int)i;
            break;
        }
    }
    if (esp_index < 0) {
        errno = ENOENT;
        return -1;
    }

    char dev[160];
    partition_path(d, device_index(d, (unsigned)esp_index), dev, sizeof(dev));
    mkdir_ok("/tmp/hi-efi");
    if (mount(dev, "/tmp/hi-efi", "fat32", 0, NULL)) {
        return -1;
    }
    bool empty_efi = access("/tmp/hi-efi/EFI", F_OK) != 0 && errno == ENOENT;
    if (mkdir_ok("/tmp/hi-efi/EFI") || mkdir_ok("/tmp/hi-efi/EFI/Synapse") ||
        mkdir_ok("/tmp/hi-efi/boot") || mkdir_ok("/tmp/hi-efi/boot/synapse") ||
        mkdir_ok("/tmp/hi-efi/boot/limine")) {
        return -1;
    }

    char source[320];
    snprintf(source, sizeof(source), "%s/efi/boot/bootx64.efi", iso_root);
    if (copy_file(source, "/tmp/hi-efi/EFI/Synapse/BOOTX64.EFI")) {
        return -1;
    }
    if (empty_efi && (mkdir_ok("/tmp/hi-efi/EFI/BOOT") ||
                      copy_file(source, "/tmp/hi-efi/EFI/BOOT/BOOTX64.EFI"))) {
        return -1;
    }
    snprintf(source, sizeof(source), "%s/boot/kernel", iso_root);
    if (copy_file(source, "/tmp/hi-efi/boot/synapse/kernel")) {
        return -1;
    }
    snprintf(source, sizeof(source), "%s/boot/install.cfg", iso_root);
    if (copy_file(source, "/tmp/hi-efi/boot/limine/limine.conf")) {
        return -1;
    }

    FILE *marker = fopen("/tmp/hi-efi/syngitf.txt", "w");
    if (!marker) {
        return -1;
    }
    fprintf(marker, "root=%sp%u\nedition=desktop\n", d->name, root_index);
    if (fclose(marker)) {
        return -1;
    }
    if (!configure_grub) {
        puts(tt("EFI files were installed without changing the existing GRUB configuration.",
                "EFI-файлы установлены без изменения существующей конфигурации GRUB."));
        return 0;
    }
    DIR *efi = opendir("/tmp/hi-efi/EFI");
    if (!efi) {
        return -1;
    }

    struct dirent *de;
    char grub[320] = "";
    while ((de = readdir(efi))) {
        if (de->d_name[0] == '.') {
            continue;
        }

        char candidate[320];
        snprintf(candidate, sizeof(candidate), "/tmp/hi-efi/EFI/%s/grub.cfg",
                 de->d_name);
        if (!access(candidate, R_OK) && strcmp(de->d_name, "Synapse")) {
            snprintf(grub, sizeof(grub), "%s", candidate);
            break;
        }
    }
    closedir(efi);
    if (grub[0]) {
        char backup[384];
        snprintf(backup, sizeof(backup), "%s.hi-original", grub);
        if (access(backup, F_OK) && rename(grub, backup)) {
            return -1;
        }

        FILE *f = fopen(grub, "w");
        if (!f) {
            return -1;
        }
        const char *relative = strstr(backup, "/EFI/");
        fprintf(f,
                "menuentry 'Ubuntu / existing OS' {\n search --file --set=esp %s\n "
                "configfile ($esp)%s\n}\n"
                "menuentry 'SynapseOS' {\n search --file --set=esp "
                "/EFI/Synapse/BOOTX64.EFI\n chainloader "
                "($esp)/EFI/Synapse/BOOTX64.EFI\n boot\n}\n",
                relative, relative);
        if (fclose(f)) {
            return -1;
        }
    } else if (!empty_efi) {
        printf("%s\n", tt("No GRUB configuration found; Synapse EFI files were "
                          "installed, but a firmware entry must be added manually.",
                          "Конфигурация GRUB не найдена; EFI-файлы Synapse "
                          "установлены, но запись прошивки нужно добавить вручную."));
    }
    return 0;
}

static void random_guid(unsigned char guid[16]) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0 && read(fd, guid, 16) == 16) {
        close(fd);
    } else {
        if (fd >= 0) {
            close(fd);
        }
        uint64_t x = (uint64_t)time(NULL) ^ (uint64_t)getpid();
        for (int i = 0; i < 16; i++) {
            x = x * 6364136223846793005ULL + 1;
            guid[i] = (unsigned char)(x >> 32);
        }
    }
    guid[7] = (guid[7] & 0x0f) | 0x40;
    guid[8] = (guid[8] & 0x3f) | 0x80;
}

static void show_partitions(struct disk *d) {
    puts(tt("Partitions:", "Разделы:"));
    for (unsigned i = 0; i < d->primary.entry_count; ++i) {
        struct gpt_entry *e = entry_at(d, i);
        if (zero_guid(e->type)) {
            continue;
        }

        char name[96];
        entry_name(e, name, sizeof(name));
        printf("  %u. %-24s %8llu MiB%s\n", i + 1, name[0] ? name : "(unnamed)",
               (unsigned long long)((e->last_lba - e->first_lba + 1) / 2048),
               esp_type(e->type) ? "  [ESP]" : "");
    }
}

static int read_number(const char *prompt) {
    char line[64];
    fputs(prompt, stdout);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) {
        return -1;
    }
    return atoi(line);
}

static int confirm_word(const char *expected) {
    char token[64];
    size_t length = 0;
    int c;
    do {
        c = fgetc(stdin);
    } while (c == '\r' || c == '\n' || c == ' ' || c == '\t');
    if (c == EOF) {
        return 0;
    }
    do {
        if (length + 1 < sizeof(token)) {
            token[length++] = (char)c;
        }
        c = fgetc(stdin);
    } while (c != EOF && c != '\r' && c != '\n' && c != ' ' && c != '\t');
    token[length] = 0;
    return !strcmp(token, expected);
}

static int edit_partition(struct disk *d, bool remove) {
    show_partitions(d);
    int number = read_number(
        tt("Partition number (0 cancels): ", "Номер раздела (0 : отмена): "));
    if (number <= 0 || (unsigned)number > d->primary.entry_count) {
        return 0;
    }

    struct gpt_entry *e = entry_at(d, (unsigned)number - 1);
    if (zero_guid(e->type)) {
        return 0;
    }
    if (!remove) {
        char line[80];
        fputs(tt("New ASCII name: ", "Новое имя (ASCII): "), stdout);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            return -1;
        }
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) {
            return 0;
        }
        set_name(e, line);
    } else {
        fputs(tt("Type DELETE to remove this GPT entry: ",
                 "Введите DELETE, чтобы удалить запись GPT: "),
              stdout);
        fflush(stdout);
        if (!confirm_word("DELETE")) {
            return 0;
        }
        memset(e, 0, d->primary.entry_size);
    }
    return commit_gpt(d);
}

static int install_system(struct disk *d, const char *payload) {
    uint64_t image_bytes = payload_size(payload), start, end;
    int slot = find_empty(d);
    if (!image_bytes || slot < 0 || largest_free(d, &start, &end)) {
        errno = ENOSPC;
        return -1;
    }

    const uint64_t image_sectors = (image_bytes + SECTOR - 1) / SECTOR;
    const uint64_t minimum_mib = (image_bytes + 1048575) / 1048576;
    const uint64_t available_mib = (end - start + 1) / 2048;
    if (end - start + 1 < image_sectors || available_mib < minimum_mib) {
        errno = ENOSPC;
        return -1;
    }

    int requested = read_number(tt("Root partition size in MiB (0 = minimum): ",
                                   "Размер корневого раздела в МиБ (0 = минимум): "));
    if (requested < 0 || (requested && (uint64_t)requested < minimum_mib) ||
        (requested && (uint64_t)requested > available_mib)) {
        errno = ENOSPC;
        return -1;
    }
    const uint64_t partition_mib = requested ? (uint64_t)requested : minimum_mib;
    const uint64_t needed = partition_mib * 2048;

    char name[80] = "SYNAPSE_ROOT";
    fputs(tt("Partition name, ASCII (empty = SYNAPSE_ROOT): ",
             "Имя раздела ASCII (пусто = SYNAPSE_ROOT): "), stdout);
    fflush(stdout);
    if (!fgets(name, sizeof(name), stdin)) {
        return -1;
    }
    name[strcspn(name, "\r\n")] = 0;
    if (!name[0]) {
        snprintf(name, sizeof(name), "SYNAPSE_ROOT");
    }

    puts(tt("1. Install EFI files and update an existing GRUB entry\n"
            "2. Install EFI files without changing GRUB\n"
            "0. Cancel",
            "1. Установить EFI-файлы и обновить существующую запись GRUB\n"
            "2. Установить EFI-файлы без изменения GRUB\n"
            "0. Отмена"));
    int boot = read_number("> ");
    if (boot != 1 && boot != 2) {
        return 0;
    }

    printf("%s: %llu MiB\n%s: %s\n%s: %llu MiB\n%s: %s\n",
           tt("System image", "Образ системы"), (unsigned long long)minimum_mib,
           tt("Target disk", "Целевой диск"), d->path,
           tt("New partition", "Новый раздел"), (unsigned long long)partition_mib,
           tt("Partition name", "Имя раздела"), name);
    fputs(tt("Type INSTALL to continue: ", "Введите INSTALL для продолжения: "),
          stdout);
    fflush(stdout);
    if (!confirm_word("INSTALL")) {
        return 0;
    }
    if (copy_stream_to_disk(payload, d->fd, start * SECTOR, needed * SECTOR,
                            image_bytes)) {
        return -1;
    }

    struct gpt_entry *e = entry_at(d, (unsigned)slot);
    memset(e, 0, d->primary.entry_size);
    static const unsigned char linux_type[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84,
                                                 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69,
                                                 0xd8, 0x47, 0x7d, 0xe4};
    memcpy(e->type, linux_type, 16);
    random_guid(e->unique);
    e->first_lba = start;
    e->last_lba = start + needed - 1;
    set_name(e, name);
    char iso_root[256];
    snprintf(iso_root, sizeof(iso_root), "%.*s",
             (int)(strstr(payload, "/boot/") - payload), payload);
    if (install_boot(d, device_index(d, (unsigned)slot), iso_root, boot == 1)) {
        return -1;
    }
    if (commit_gpt(d)) {
        return -1;
    }
    puts(tt("SynapseOS was installed. Reboot and choose it in GRUB.",
            "SynapseOS установлена. Перезагрузитесь и выберите её в GRUB."));
    return 0;
}

int hi_installer(int russian) {
    is_ru = russian;
    if (!getenv("SYNAPSE_LIVE") || strcmp(getenv("SYNAPSE_LIVE"), "1")) {
        fprintf(stderr, "%s\n",
                tt("Installation is available only in a Live session.",
                   "Установка доступна только в Live-сессии."));
        return 1;
    }
    const char *payload = find_payload();
    if (!payload) {
        fprintf(stderr, "%s\n",
                tt("Installation payload was not found on the Live media.",
                   "Установочный образ не найден на Live-носителе."));
        return 1;
    }

    struct disk disks[16];
    int count = 0;
    const char *prefixes[] = {"vda", "vdb", "sda", "sdb", "nvme0n1", "nvme1n1"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]) && count < 16; ++i) {
        char path[128];
        snprintf(path, sizeof(path), "/dev/%s", prefixes[i]);
        if (source_device[0] && !strcmp(source_device, path)) {
            continue;
        }
        if (!disk_open(&disks[count], path)) {
            count++;
        }
    }
    if (!count) {
        fprintf(stderr, "%s\n",
                tt("No writable GPT disk with valid primary and backup tables found.",
                   "Не найден доступный для записи GPT-диск с исправными основной и "
                   "резервной таблицами."));
        return 1;
    }
    puts("\033[2J\033[HHI : SynapseOS installer\n");
    for (int i = 0; i < count; ++i) {
        printf("  %d. %s  %llu MiB\n", i + 1, disks[i].path,
               (unsigned long long)(disks[i].bytes / 1048576));
    }

    int selected =
        read_number(tt("Disk number (0 cancels): ", "Номер диска (0 : отмена): ")) - 1;
    if (selected < 0 || selected >= count) {
        for (int i = 0; i < count; i++) {
            disk_close(&disks[i]);
        }
        return 0;
    }

    struct disk *d = &disks[selected];
    int result = 0;
    for (;;) {
        show_partitions(d);
        uint64_t a = 0, b = 0;
        if (!largest_free(d, &a, &b)) {
            printf("%s: %llu MiB\n",
                   tt("Largest free region", "Самая большая свободная область"),
                   (unsigned long long)((b - a + 1) / 2048));
        }
        puts(tt("\n1. Install into free space\n2. Rename a partition\n3. Delete a "
                "partition entry\n0. Back",
                "\n1. Установить в свободное место\n2. Переименовать раздел\n3. "
                "Удалить запись раздела\n0. Назад"));
        int action = read_number("> ");
        if (!action) {
            break;
        }
        if (action == 1) {
            result = install_system(d, payload);
            break;
        }
        if (action == 2 || action == 3) {
            result = edit_partition(d, action == 3);
            if (result) {
                break;
            }
            puts(tt("GPT updated.", "GPT обновлена."));
            if (action == 3) {
                puts(tt("Reboot the Live system before further partition operations.",
                        "Перезагрузите Live-систему перед следующими операциями с "
                        "разделами."));
                break;
            }
        }
    }
    if (result) {
        fprintf(stderr, "%s: %s\n", tt("Installer failed", "Ошибка установщика"),
                strerror(errno));
    }

    for (int i = 0; i < count; ++i) {
        disk_close(&disks[i]);
    }
    return result ? 1 : 0;
}
