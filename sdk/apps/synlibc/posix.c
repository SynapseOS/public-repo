// posix.c: Обёртки системных вызовов пользовательского пространства
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: synlibc, пользовательская библиотека нативного API
// Создан: 09.10.2026
// Последнее изменение: 14.10.2026

#include <synlibc.h>

ssize_t read(int fd, void *buf, size_t count) {
	return (ssize_t)syscall3(SYS_read, (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}

ssize_t write(int fd, const void *buf, size_t count) {
	return (ssize_t)syscall3(SYS_write, (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}

int open(const char *pathname, int flags, mode_t mode) {
	return (int)syscall3(SYS_open, (uint64_t)pathname, (uint64_t)flags, (uint64_t)mode);
}

int close(int fd) {
	return (int)syscall1(SYS_close, (uint64_t)fd);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
	return (void *)syscall6(SYS_mmap, (uint64_t)addr, (uint64_t)length, (uint64_t)prot,
	                        (uint64_t)flags, (uint64_t)fd, (uint64_t)offset);
}

int munmap(void *addr, size_t length) {
	return (int)syscall2(SYS_munmap, (uint64_t)addr, (uint64_t)length);
}

void *brk(void *addr) {
	return (void *)syscall1(SYS_brk, (uint64_t)addr);
}

pid_t getpid(void) {
	return (pid_t)syscall0(SYS_getpid);
}

pid_t fork(void) {
	return (pid_t)syscall0(SYS_fork);
}

int execve(const char *pathname, char *const argv[], char *const envp[]) {
	return (int)syscall3(SYS_execve, (uint64_t)pathname, (uint64_t)argv, (uint64_t)envp);
}

void _exit(int status) {
	syscall1(SYS_exit, (uint64_t)status);

	while (1) {
		syn_yield( );
	}
}

pid_t wait4(pid_t pid, int *wstatus, int options, void *rusage) {
	return (pid_t)syscall4(SYS_wait4, (uint64_t)pid, (uint64_t)wstatus, (uint64_t)options,
	                       (uint64_t)rusage);
}

int kill(pid_t pid, int sig) {
	return (int)syscall2(SYS_kill, (uint64_t)pid, (uint64_t)sig);
}

int uname(struct utsname *buf) {
	return (int)syscall1(SYS_uname, (uint64_t)buf);
}

int sysinfo(struct sysinfo *s) {
	return (int)syscall1(SYS_sysinfo, (uint64_t)s);
}

pid_t posix_spawn(const char *path, char *const argv[], char *const envp[]) {
	return (pid_t)syscall3(SYS_posix_spawn, (uint64_t)path, (uint64_t)argv, (uint64_t)envp);
}