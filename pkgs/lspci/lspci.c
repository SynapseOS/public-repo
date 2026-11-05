// lspci.c: показать устройства PCI из sysfs
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: synapse-x86_64-v1
// Файл: lspci.c, подсистема: пользовательские утилиты
// Первое изменение: 29.10.2026, последнее изменение: 29.10.2026
// Статус доступа: общедоступный

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PCI_ROOT "/sys/bus/pci/devices"
#define DEFAULT_PCI_IDS "/usr/share/hwdata/pci.ids"

struct pci_entry {
    char address[32];
    unsigned vendor;
    unsigned device;
    unsigned class_code;
    unsigned subsystem_vendor;
    unsigned subsystem_device;
    unsigned revision;
    char vendor_name[256];
    char device_name[256];
    char class_name[256];
};

// Прочитать шестнадцатеричный атрибут устройства
static bool read_hex(const char *root, const char *address, const char *name, unsigned *value) {
    char path[PATH_MAX];
    char text[64];
    char *end;

    if (snprintf(path, sizeof(path), "%s/%s/%s", root, address, name) >= (int)sizeof(path)) {
        return false;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }

    bool valid = fgets(text, sizeof(text), file) != NULL;
    fclose(file);
    if (!valid) {
        return false;
    }

    errno = 0;
    unsigned long parsed = strtoul(text, &end, 16);
    if (errno != 0 || end == text || parsed > UINT_MAX) {
        return false;
    }

    *value = (unsigned)parsed;
    return true;
}

// Скопировать имя из базы PCI без завершающего перевода строки
static void copy_name(char *output, size_t length, const char *name) {
    size_t count = strcspn(name, "\r\n");
    if (count >= length) {
        count = length - 1;
    }

    memcpy(output, name, count);
    output[count] = '\0';
}

// Найти имена производителя, устройства и класса в pci.ids
static void lookup_names(const char *database, struct pci_entry *entry) {
    FILE *file = fopen(database, "r");
    if (file == NULL) {
        return;
    }

    char line[1024];
    bool vendor_section = false;
    bool class_section = false;
    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned code;
        int consumed = 0;

        if (line[0] == 'C' && line[1] == ' ' &&
            sscanf(line + 2, "%2x %n", &code, &consumed) >= 1 && consumed > 0) {
            class_section = code == (entry->class_code >> 16);
            vendor_section = false;
            if (class_section) {
                copy_name(entry->class_name, sizeof(entry->class_name), line + 2 + consumed);
            }
            continue;
        }

        if (line[0] != '\t' && sscanf(line, "%4x %n", &code, &consumed) >= 1 && consumed > 0) {
            vendor_section = code == entry->vendor;
            class_section = false;
            if (vendor_section) {
                copy_name(entry->vendor_name, sizeof(entry->vendor_name), line + consumed);
            }
            continue;
        }

        if (line[0] == '\t' && line[1] != '\t' &&
            sscanf(line + 1, "%4x %n", &code, &consumed) >= 1 && consumed > 0) {
            if (vendor_section && code == entry->device) {
                copy_name(entry->device_name, sizeof(entry->device_name), line + 1 + consumed);
            }
            if (class_section && code == ((entry->class_code >> 8) & 0xffU)) {
                copy_name(entry->class_name, sizeof(entry->class_name), line + 1 + consumed);
            }
        }
    }

    fclose(file);
}

// Сравнить адреса PCI для стабильного порядка вывода
static int compare_entry(const void *left, const void *right) {
    const struct pci_entry *a = left;
    const struct pci_entry *b = right;
    return strcmp(a->address, b->address);
}

// Вывести справку по поддерживаемым ключам
static void usage(FILE *stream) {
    fprintf(stream, "Usage: lspci [-n|-nn] [-D] [-v] [-s address]\n");
}

// Прочитать PCI sysfs и вывести найденные устройства
int main(int argc, char **argv) {
    bool numeric = false;
    bool numeric_names = false;
    bool domain = false;
    bool verbose = false;
    const char *selected = NULL;

    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "-n") == 0) {
            numeric = true;
        } else if (strcmp(argv[index], "-nn") == 0) {
            numeric_names = true;
        } else if (strcmp(argv[index], "-D") == 0) {
            domain = true;
        } else if (strcmp(argv[index], "-v") == 0 || strcmp(argv[index], "-vv") == 0) {
            verbose = true;
        } else if (strcmp(argv[index], "-s") == 0 && index + 1 < argc) {
            selected = argv[++index];
        } else if (strcmp(argv[index], "--help") == 0) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }

    const char *root = getenv("SYNAPSE_PCI_ROOT");
    const char *database = getenv("SYNAPSE_PCI_IDS");
    if (root == NULL || root[0] == '\0') {
        root = DEFAULT_PCI_ROOT;
    }
    if (database == NULL || database[0] == '\0') {
        database = DEFAULT_PCI_IDS;
    }

    DIR *directory = opendir(root);
    if (directory == NULL) {
        fprintf(stderr, "lspci: cannot open %s: %s\n", root, strerror(errno));
        return 1;
    }

    struct pci_entry entries[256];
    size_t count = 0;
    struct dirent *item;
    while ((item = readdir(directory)) != NULL) {
        unsigned segment, bus, slot, function;
        if (sscanf(item->d_name, "%4x:%2x:%2x.%1x", &segment, &bus, &slot, &function) != 4) {
            continue;
        }
        if (count >= sizeof(entries) / sizeof(entries[0])) {
            break;
        }

        struct pci_entry *entry = &entries[count];
        memset(entry, 0, sizeof(*entry));
        copy_name(entry->address, sizeof(entry->address), item->d_name);
        if (!read_hex(root, entry->address, "vendor", &entry->vendor) ||
            !read_hex(root, entry->address, "device", &entry->device) ||
            !read_hex(root, entry->address, "class", &entry->class_code)) {
            continue;
        }

        (void)read_hex(root, entry->address, "subsystem_vendor", &entry->subsystem_vendor);
        (void)read_hex(root, entry->address, "subsystem_device", &entry->subsystem_device);
        (void)read_hex(root, entry->address, "revision", &entry->revision);
        lookup_names(database, entry);
        count++;
    }
    closedir(directory);
    qsort(entries, count, sizeof(entries[0]), compare_entry);

    for (size_t index = 0; index < count; index++) {
        const struct pci_entry *entry = &entries[index];
        const char *address = domain ? entry->address : entry->address + 5;
        if (selected != NULL && strstr(entry->address, selected) == NULL) {
            continue;
        }

        if (numeric) {
            printf("%s %04x: %04x:%04x (rev %02x)\n", address, entry->class_code >> 8,
                   entry->vendor, entry->device, entry->revision);
        } else {
            printf("%s %s", address, entry->class_name[0] ? entry->class_name : "PCI device");
            if (numeric_names) {
                printf(" [%04x]", entry->class_code >> 8);
            }
            printf(": %s%s%s", entry->vendor_name[0] ? entry->vendor_name : "Unknown vendor",
                   entry->device_name[0] ? " " : "",
                   entry->device_name[0] ? entry->device_name : "");
            if (numeric_names) {
                printf(" [%04x:%04x]", entry->vendor, entry->device);
            }
            printf(" (rev %02x)\n", entry->revision);
        }

        if (verbose) {
            printf("\tSubsystem: %04x:%04x\n\tClass: %06x\n", entry->subsystem_vendor,
                   entry->subsystem_device, entry->class_code);
        }
    }

    return 0;
}
