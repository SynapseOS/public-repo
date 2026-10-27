/*
    synlibc.h - Пользовательский интерфейс нативной библиотеки
    Автор: Елчинян Арен Артурович, 2026
    Архитектура: x86_64, SynAPI ABI
    Подсистема: synlibc
    Первое изменение: 09.10.2026
    Последнее изменение: 25.10.2026
    Статус доступа: общедоступный
*/
#ifndef SYNLIBC_H
#define SYNLIBC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <syscalls.h>
#include <synapse/syn.h>
#include <synapse/inference.h>
#include <dev/hid.h>

// POSIX
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif
typedef int pid_t;
typedef unsigned int mode_t;
typedef long off_t;
typedef long intptr_t;

#define printf syn_printf

void syn_yield(void);
int syn_log(const char *message, size_t length);
void *syn_alloc_pages(size_t count);
int syn_free_pages(void *ptr, size_t count);
uint64_t syn_get_ticks(void);
void syn_sleep(uint64_t milliseconds);
int syn_abi_get_info(struct syn_abi_info *info);
int syn_syscall_get_info(uint32_t number, struct syn_syscall_info *info);
int syn_feature_available(enum syn_feature feature);
int syn_memory_get_info(struct syn_memory_info *info);
int syn_cpu_get_info(struct syn_cpu_info *info);
int syn_futex_wait(uint32_t *address, uint32_t expected, unsigned flags);
int syn_futex_wake(uint32_t *address, uint32_t count, unsigned flags);
int64_t syn_proc_create(const char *name, uintptr_t entry, uint64_t arg);
void syn_proc_exit(int code) __attribute__((noreturn));
int64_t syn_thread_create(uintptr_t entry, void *arg);
void syn_thread_exit(void) __attribute__((noreturn));
int64_t syn_proc_getpid(void);
int64_t syn_proc_getppid(void);
uint64_t syn_proc_get_count(void);
int64_t syn_thread_gettid(void);
int syn_proc_kill(int64_t pid);
int syn_run_app(const char *path);

// HID
int syn_kbd_read_event(kbd_event_t *event);
bool syn_kbd_has_event(void);
uint8_t syn_kbd_get_mods(void);
uint8_t syn_kbd_get_leds(void);
void syn_kbd_set_leds(uint8_t leds);

// Guest Agent
ssize_t syn_ga_console_read(void *buf, size_t count);
ssize_t syn_ga_console_write(const void *buf, size_t count);
bool syn_ga_console_has_data(void);
void syn_ga_console_flush(void);
bool syn_ga_console_is_ready(void);

// VFS
int syn_vfs_open(const char *pathname, int flags, mode_t mode);
int syn_vfs_close(int fd);
int syn_vfs_getcwd(char *buf, size_t size);
int syn_vfs_chdir(const char *path);
int syn_vfs_mkdirat(int dirfd, const char *pathname, mode_t mode);
int syn_vfs_unlinkat(int dirfd, const char *pathname, int flags);
int syn_vfs_readdir(int fd, void *dirp, size_t *count);
ssize_t syn_vfs_read(int fd, void *buf, size_t count);
ssize_t syn_vfs_write(int fd, const void *buf, size_t count);
off_t syn_vfs_seek(int fd, off_t offset, int whence);
int syn_vfs_openat(int dirfd, const char *pathname, int flags, mode_t mode);
int syn_vfs_dup_to(int fd, int target, int flags);
int syn_vfs_sync(void);
ssize_t syn_vfs_readlinkat(int dirfd, const char *pathname, void *buffer, size_t size);
int syn_vfs_mknodat(int dirfd, const char *pathname, mode_t mode);

// misc
int syn_getuptime(uint64_t *usec, uint64_t *sec);
uint64_t syn_clock_monotonic(void);

size_t strlen(const char *str);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

// В crt0.asm реализация
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);

int putchar(int c);
int puts(const char *s);
int syn_printf(const char *format, ...);
int atoi(const char *str);
long atol(const char *str);
void *malloc(size_t size);
void free(void *ptr);

int sprintf(char *buf, const char *format, ...);
int snprintf(char *buf, size_t size, const char *format, ...);
int vsprintf(char *buf, const char *format, va_list args);
int vsnprintf(char *buf, size_t size, const char *format, va_list args);
int syn_printf(const char *format, ...);
int atoi(const char *str);
long atol(const char *str);
long long atoll(const char *str);

// POSIX System Call Wrappers
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int open(const char *pathname, int flags, mode_t mode);
int close(int fd);
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);
void *brk(void *addr);
pid_t getpid(void);
pid_t fork(void);
int execve(const char *pathname, char *const argv[], char *const envp[]);
void _exit(int status) __attribute__((noreturn));
pid_t wait4(pid_t pid, int *wstatus, int options, void *rusage);
int kill(pid_t pid, int sig);
int uname(struct utsname *buf);
int sysinfo(struct sysinfo *s);
pid_t posix_spawn(const char *path, char *const argv[], char *const envp[]);
#endif
