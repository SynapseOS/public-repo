// synapi.c: Обёртки нативных системных вызовов SynapseOS
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: synlibc, пользовательская библиотека нативного API
// Создан: 09.10.2026
// Последнее изменение: 14.10.2026

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

// Системная информация
int syn_getuptime(uint64_t *usec, uint64_t *sec) {
	return (int)syscall2(SYS_SYN_MISC_GET_GETUPTIME, (uint64_t)usec, (uint64_t)sec);
}