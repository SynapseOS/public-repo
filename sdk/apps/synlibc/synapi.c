/*
    synapi.c - Обёртки нативных системных вызовов SynapseOS
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, SynAPI ABI
    Подсистема: synlibc
    Первое изменение: 09.10.2026
    Последнее изменение: 25.10.2026
    Статус доступа: общедоступный
*/

#include <synlibc.h>

void syn_yield(void) {
	syscall0(SYS_SYN_YIELD);
}

int syn_log(const char *message, size_t length) {
	if (length > SYN_LOG_MAX_LEN) {
		length = SYN_LOG_MAX_LEN;
	}

	return (int)syscall2(SYS_SYN_LOG, length, (uint64_t)message);
}

void *syn_alloc_pages(size_t count) {
	return (void *)syscall1(SYS_SYN_ALLOC_PAGES, count);
}

int syn_free_pages(void *ptr, size_t count) {
	return (int)syscall2(SYS_SYN_FREE_PAGES, (uint64_t)ptr, count);
}

uint64_t syn_get_ticks(void) {
	return syscall0(SYS_SYN_GET_TICKS);
}

void syn_sleep(uint64_t milliseconds) {
	syscall1(SYS_SYN_SLEEP, milliseconds);
}

// Прочитать версию и возможности текущего SynAPI
int syn_abi_get_info(struct syn_abi_info *info) {
	return (int)syscall2(SYS_SYN_ABI_INFO, (uint64_t)info, sizeof(*info));
}

// Проверить доступность одного абсолютного номера
int syn_syscall_get_info(uint32_t number, struct syn_syscall_info *info) {
	return (int)syscall3(SYS_SYN_SYSCALL_INFO, number, (uint64_t)info, sizeof(*info));
}

// Проверить доступность группы вызовов
int syn_feature_available(enum syn_feature feature) {
	return (int)syscall1(SYS_SYN_FEATURE_QUERY, (uint64_t)feature);
}

// Прочитать объём физической памяти
int syn_memory_get_info(struct syn_memory_info *info) {
	return (int)syscall2(SYS_SYN_MEMORY_INFO, (uint64_t)info, sizeof(*info));
}

// Прочитать сведения о текущем CPU
int syn_cpu_get_info(struct syn_cpu_info *info) {
	return (int)syscall2(SYS_SYN_CPU_INFO, (uint64_t)info, sizeof(*info));
}

// Ждать изменения 32-битного слова
int syn_futex_wait(uint32_t *address, uint32_t expected, unsigned flags) {
	return (int)syscall3(SYS_SYN_FUTEX_WAIT, (uint64_t)address, expected, flags);
}

// Разбудить потоки, ожидающие изменения слова
int syn_futex_wake(uint32_t *address, uint32_t count, unsigned flags) {
	return (int)syscall3(SYS_SYN_FUTEX_WAKE, (uint64_t)address, count, flags);
}

int64_t syn_proc_create(const char *name, uintptr_t entry, uint64_t arg) {
	return (int64_t)syscall3(SYS_SYN_PROC_CREATE, (uint64_t)name, entry, arg);
}

void syn_proc_exit(int code) {
	syscall1(SYS_SYN_PROC_EXIT, (uint64_t)code);

	while (1) {
		syn_yield( );
	}
}

void syn_proc_exit_zero( ) {
	syscall1(SYS_SYN_PROC_EXIT, (uint64_t)0);

	while (1) {
		syn_yield( );
	}
}

int64_t syn_thread_create(uintptr_t entry, void *arg) {
	return (int64_t)syscall2(SYS_SYN_THREAD_CREATE, entry, (uint64_t)arg);
}

void syn_thread_exit(void) {
	syscall0(SYS_SYN_THREAD_EXIT);

	while (1) {
		syn_yield( );
	}
}

int64_t syn_proc_getpid(void) {
	return (int64_t)syscall0(SYS_SYN_PROC_GETPID);
}

// Вернуть номер родительского процесса
int64_t syn_proc_getppid(void) {
	return (int64_t)syscall0(SYS_SYN_PROC_GETPPID);
}

// Вернуть число процессов в снимке ядра
uint64_t syn_proc_get_count(void) {
	return syscall0(SYS_SYN_PROC_GET_COUNT);
}

int64_t syn_thread_gettid(void) {
	return (int64_t)syscall0(SYS_SYN_THREAD_GETTID);
}

int syn_proc_kill(int64_t pid) {
	return (int)syscall1(SYS_SYN_PROC_KILL, (uint64_t)pid);
}

int syn_run_app(const char *path) {
	return (int)syscall1(SYS_SYN_RUN_APP, (uint64_t)path);
}

int syn_kbd_read_event(kbd_event_t *event) {
	if (!event) {
		return KBD_ERR_INVAL;
	}

	return (int)syscall1(SYS_SYN_KBD_READ_EVENT, (uint64_t)event);
}

bool syn_kbd_has_event(void) {
	return syscall0(SYS_SYN_KBD_HAS_EVENT) == 1;
}

uint8_t syn_kbd_get_mods(void) {
	return (uint8_t)syscall0(SYS_SYN_KBD_GET_MODS);
}

uint8_t syn_kbd_get_leds(void) {
	return (uint8_t)syscall0(SYS_SYN_KBD_GET_LEDS);
}

void syn_kbd_set_leds(uint8_t leds) {
	syscall1(SYS_SYN_KBD_SET_LEDS, (uint64_t)leds);
}

ssize_t syn_ga_console_read(void *buf, size_t count) {
	return (ssize_t)syscall2(SYS_SYN_GA_CONSOLE_READ, (uint64_t)buf, count);
}

ssize_t syn_ga_console_write(const void *buf, size_t count) {
	return (ssize_t)syscall2(SYS_SYN_GA_CONSOLE_WRITE, (uint64_t)buf, count);
}

bool syn_ga_console_has_data(void) {
	return syscall0(SYS_SYN_GA_CONSOLE_HAS_DATA) == 1;
}

void syn_ga_console_flush(void) {
	syscall0(SYS_SYN_GA_CONSOLE_FLUSH);
}

bool syn_ga_console_is_ready(void) {
	return syscall0(SYS_SYN_GA_CONSOLE_STATUS) == 1;
}

// Доступ к VFS
int syn_vfs_open(const char *pathname, int flags, mode_t mode) {
	return (int)syscall3(SYS_SYN_VFS_OPEN, (uint64_t)pathname, (uint64_t)flags, (uint64_t)mode);
}

int syn_vfs_close(int fd) {
	return (int)syscall1(SYS_SYN_VFS_CLOSE, (uint64_t)fd);
}

int syn_vfs_getcwd(char *buf, size_t size) {
	return (int)syscall2(SYS_SYN_VFS_GETCWD, (uint64_t)buf, (uint64_t)size);
}

int syn_vfs_chdir(const char *path) {
	return (int)syscall1(SYS_SYN_VFS_CHDIR, (uint64_t)path);
}

int syn_vfs_mkdirat(int dirfd, const char *pathname, mode_t mode) {
	return (int)syscall3(SYS_SYN_VFS_MKDIRAT, (uint64_t)dirfd, (uint64_t)pathname, (uint64_t)mode);
}

int syn_vfs_unlinkat(int dirfd, const char *pathname, int flags) {
	return (int)syscall3(SYS_SYN_VFS_UNLINKAT, (uint64_t)dirfd, (uint64_t)pathname,
	                     (uint64_t)flags);
}

int syn_vfs_readdir(int fd, void *dirp, size_t *count) {
	return (int)syscall3(SYS_SYN_VFS_READDIR, (uint64_t)fd, (uint64_t)dirp, (uint64_t)count);
}

ssize_t syn_vfs_read(int fd, void *buf, size_t count) {
	return (ssize_t)syscall3(SYS_SYN_VFS_READ, (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}

ssize_t syn_vfs_write(int fd, const void *buf, size_t count) {
	return (ssize_t)syscall3(SYS_SYN_VFS_WRITE, (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}

off_t syn_vfs_seek(int fd, off_t offset, int whence) {
	return (off_t)syscall3(SYS_SYN_VFS_SEEK, (uint64_t)fd, (uint64_t)offset, (uint64_t)whence);
}

// Открыть путь относительно каталога
int syn_vfs_openat(int dirfd, const char *pathname, int flags, mode_t mode) {
	return (int)syscall4(SYS_SYN_VFS_OPENAT, (uint64_t)dirfd, (uint64_t)pathname,
	                     (uint64_t)flags, (uint64_t)mode);
}

// Скопировать дескриптор в заданный номер
int syn_vfs_dup_to(int fd, int target, int flags) {
	return (int)syscall3(SYS_SYN_VFS_DUP_TO, (uint64_t)fd, (uint64_t)target, (uint64_t)flags);
}

// Сохранить данные всех встроенных файловых систем
int syn_vfs_sync(void) {
	return (int)syscall0(SYS_SYN_VFS_SYNC);
}

// Прочитать значение символьной ссылки
ssize_t syn_vfs_readlinkat(int dirfd, const char *pathname, void *buffer, size_t size) {
	return (ssize_t)syscall4(SYS_SYN_VFS_READLINKAT, (uint64_t)dirfd, (uint64_t)pathname,
	                         (uint64_t)buffer, size);
}

// Создать узел относительно каталога
int syn_vfs_mknodat(int dirfd, const char *pathname, mode_t mode) {
	return (int)syscall3(SYS_SYN_VFS_MKNODAT, (uint64_t)dirfd, (uint64_t)pathname, mode);
}

// Системная информация
int syn_getuptime(uint64_t *usec, uint64_t *sec) {
	return (int)syscall2(SYS_SYN_MISC_GET_GETUPTIME, (uint64_t)usec, (uint64_t)sec);
}

// Вернуть монотонное время в микросекундах
uint64_t syn_clock_monotonic(void) {
	return syscall0(SYS_SYN_CLOCK_MONOTONIC);
}
