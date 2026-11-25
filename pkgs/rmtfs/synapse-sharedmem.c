// synapse-sharedmem.c: Узкая граница BSD RMTFS и общего буфера SynapseOS
// Автор: Елчинян Арен Артурович 2026
// Архитектура: AArch64 и x86_64, ABI: фиксированный RMTFS ioctl
// Описание: Только подготовленный двухмегабайтный буфер, без физических устройств и sysfs
// Подсистема: публичные пакеты телефона
// Первое изменение: 25.11.2026
// Последнее изменение: 25.11.2026
// Доступ: общедоступный
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "rmtfs.h"

// Публичные значения fixed ABI, не копия внутреннего кода ядра
#define SYN_RMTFS_PREPARE 0x5180u
#define SYN_RMTFS_GETINFO 0x5181u
#define SYN_RMTFS_SIZE (2u * 1024u * 1024u)
struct syn_rmtfs_info {
    uint32_t abi, flags;
    uint64_t physical, size;
    uint32_t guard_size, reserved;
};
_Static_assert(sizeof(struct syn_rmtfs_info) == 32, "RMTFS ioctl ABI");
enum address_mode { MODE_UNSET, MODE_RELATIVE, MODE_PHYSICAL };
struct rmtfs_mem {
    uint64_t address, size;
    void *base;
    int fd;
    enum address_mode mode;
};

// Fail closed если kernel не подтвердил ownership, размер или guard
static bool valid_info(const struct syn_rmtfs_info *info) {
    return info->abi == 1u && info->flags == 7u && !info->reserved &&
        info->size == SYN_RMTFS_SIZE && info->guard_size == 4096u &&
        info->physical >= 4096u && !(info->physical & 4095u) &&
        info->physical <= UINT64_C(0x100000000) - SYN_RMTFS_SIZE;
}

// Никакого fallback, адрес mmap всегда device offset0
struct rmtfs_mem *rmtfs_mem_open(void) {
    struct rmtfs_mem *memory = calloc(1, sizeof(*memory));
    if (!memory) return NULL;
    memory->fd = open("/dev/qcom-rmtfs", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (memory->fd < 0) goto failed;
    struct syn_rmtfs_info info = {0};
    if (ioctl(memory->fd, SYN_RMTFS_PREPARE, 0) ||
        ioctl(memory->fd, SYN_RMTFS_GETINFO, &info) || !valid_info(&info)) goto close_failed;
    memory->base = mmap(NULL, SYN_RMTFS_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, memory->fd, 0);
    if (memory->base == MAP_FAILED) goto close_failed;
    memory->address = info.physical;
    memory->size = info.size;
    return memory;
close_failed:
    close(memory->fd);
failed:
    free(memory);
    return NULL;
}

// ALLOC0 выбирает глобальный relative namespace, ответ сохраняет реальный physical как native55
int64_t rmtfs_mem_alloc(struct rmtfs_mem *memory, size_t size) {
    if (!memory || size > memory->size) return -EINVAL;
    enum address_mode mode = size ? MODE_PHYSICAL : MODE_RELATIVE;
    if (memory->mode != MODE_UNSET && memory->mode != mode) return -EINVAL;
    memory->mode = mode;
    return (int64_t)memory->address;
}

// Free из протокола не освобождает kernel reservation и не меняет namespace работающего MPSS
void rmtfs_mem_free(struct rmtfs_mem *memory) { (void)memory; }

// Overflow-free единый namespace для всех caller_id в одном запуске модема
static void *memory_ptr(struct rmtfs_mem *memory, unsigned long address, ssize_t length) {
    if (!memory || length < 0 || memory->mode == MODE_UNSET) return NULL;
    uint64_t offset = (uint64_t)address;
    if (memory->mode == MODE_PHYSICAL) {
        if (offset < memory->address) return NULL;
        offset -= memory->address;
    }
    if (offset > memory->size || (uint64_t)length > memory->size - offset) return NULL;
    return (char *)memory->base + offset;
}

ssize_t rmtfs_mem_read(struct rmtfs_mem *memory, unsigned long address, void *out, ssize_t length) {
    void *pointer = memory_ptr(memory, address, length);
    if (!pointer || (!out && length)) return -EINVAL;
    if (length) memcpy(out, pointer, (size_t)length);
    return length;
}

ssize_t rmtfs_mem_write(struct rmtfs_mem *memory, unsigned long address, const void *in, ssize_t length) {
    void *pointer = memory_ptr(memory, address, length);
    if (!pointer || (!in && length)) return -EINVAL;
    if (length) memcpy(pointer, in, (size_t)length);
    return length;
}

// Release userspace mapping only, ownership remains pinned by the kernel until phone reboot
void rmtfs_mem_close(struct rmtfs_mem *memory) {
    if (!memory) return;
    munmap(memory->base, (size_t)memory->size);
    close(memory->fd);
    free(memory);
}
