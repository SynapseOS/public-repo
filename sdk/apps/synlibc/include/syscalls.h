#ifndef MOD_SYSCALL_H
#define MOD_SYSCALL_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

extern uint64_t syscall0(uint64_t syscall_number);
extern uint64_t syscall1(uint64_t syscall_number, uint64_t args0);
extern uint64_t syscall2(uint64_t syscall_number, uint64_t args0, uint64_t args1);
extern uint64_t syscall3(uint64_t syscall_number, uint64_t args0, uint64_t args1, uint64_t args2);
extern uint64_t syscall4(uint64_t syscall_number, uint64_t args0, uint64_t args1, uint64_t args2,
                         uint64_t args3);
extern uint64_t syscall5(uint64_t syscall_number, uint64_t args0, uint64_t args1, uint64_t args2,
                         uint64_t args3, uint64_t args4);
extern uint64_t syscall6(uint64_t syscall_number, uint64_t args0, uint64_t args1, uint64_t args2,
                         uint64_t args3, uint64_t args4, uint64_t args5);

extern void *memcpy(void *restrict dest, const void *restrict src, size_t n);

#define SYSCALL_MAX 512
#define SYS_SYN_OFFSET 100000

// SynAPI
#define SYS_SYN_YIELD (0 + SYS_SYN_OFFSET)
#define SYS_SYN_LOG (1 + SYS_SYN_OFFSET)
#define SYS_SYN_ALLOC_PAGES (2 + SYS_SYN_OFFSET)
#define SYS_SYN_FREE_PAGES (3 + SYS_SYN_OFFSET)
#define SYS_SYN_MAP (4 + SYS_SYN_OFFSET)
#define SYS_SYN_UNMAP (5 + SYS_SYN_OFFSET)
#define SYS_SYN_GET_TICKS (6 + SYS_SYN_OFFSET)
#define SYS_SYN_SLEEP (7 + SYS_SYN_OFFSET)
#define SYS_SYN_IPC_CREATE (8 + SYS_SYN_OFFSET)
#define SYS_SYN_IPC_SEND (9 + SYS_SYN_OFFSET)
#define SYS_SYN_IPC_RECV (10 + SYS_SYN_OFFSET)
#define SYS_SYN_MUTEX_LOCK (11 + SYS_SYN_OFFSET)
#define SYS_SYN_MUTEX_UNLOCK (12 + SYS_SYN_OFFSET)
#define SYS_SYN_MMIO_MAP (13 + SYS_SYN_OFFSET)
#define SYS_SYN_MMIO_UNMAP (14 + SYS_SYN_OFFSET)

// Process and thread management
#define SYS_SYN_PROC_CREATE (1000 + SYS_SYN_OFFSET)
#define SYS_SYN_PROC_EXIT (1001 + SYS_SYN_OFFSET)
#define SYS_SYN_THREAD_CREATE (1002 + SYS_SYN_OFFSET)
#define SYS_SYN_THREAD_EXIT (1003 + SYS_SYN_OFFSET)
#define SYS_SYN_PROC_GETPID (1004 + SYS_SYN_OFFSET)
#define SYS_SYN_THREAD_GETTID (1005 + SYS_SYN_OFFSET)
#define SYS_SYN_PROC_KILL (1006 + SYS_SYN_OFFSET)
#define SYS_SYN_RUN_APP (1007 + SYS_SYN_OFFSET)

// HID (Human Interface Devices)
#define SYS_SYN_KBD_READ_EVENT (2000 + SYS_SYN_OFFSET)
#define SYS_SYN_KBD_GET_MODS (2001 + SYS_SYN_OFFSET)
#define SYS_SYN_KBD_SET_LEDS (2002 + SYS_SYN_OFFSET)
#define SYS_SYN_KBD_GET_LEDS (2003 + SYS_SYN_OFFSET)
#define SYS_SYN_KBD_HAS_EVENT (2004 + SYS_SYN_OFFSET)

// VFS
#define SYS_SYN_VFS_OPEN (3000 + SYS_SYN_OFFSET)
#define SYS_SYN_VFS_CLOSE (3001 + SYS_SYN_OFFSET)
#define SYS_SYN_VFS_GETCWD (3002 + SYS_SYN_OFFSET)
#define SYS_SYN_VFS_CHDIR (3003 + SYS_SYN_OFFSET)
#define SYS_SYN_VFS_MKDIRAT (3004 + SYS_SYN_OFFSET)
#define SYS_SYN_VFS_UNLINKAT (3005 + SYS_SYN_OFFSET)
#define SYS_SYN_VFS_READDIR (3006 + SYS_SYN_OFFSET)
#define SYS_SYN_VFS_READ (3007 + SYS_SYN_OFFSET)
#define SYS_SYN_VFS_WRITE (3008 + SYS_SYN_OFFSET)
#define SYS_SYN_VFS_SEEK (3009 + SYS_SYN_OFFSET)

// Guest agent
#define SYS_SYN_GA_CONSOLE_STATUS (4000 + SYS_SYN_OFFSET)
#define SYS_SYN_GA_CONSOLE_READ (4001 + SYS_SYN_OFFSET)
#define SYS_SYN_GA_CONSOLE_WRITE (4002 + SYS_SYN_OFFSET)
#define SYS_SYN_GA_CONSOLE_HAS_DATA (4003 + SYS_SYN_OFFSET)
#define SYS_SYN_GA_CONSOLE_FLUSH (4004 + SYS_SYN_OFFSET)
// vgpu
#define SYS_SYN_GA_VGPU_SET_MAX_FPS (4100 + SYS_SYN_OFFSET)
#define SYS_SYN_GA_VGPU_GET_MAX_FPS (4101 + SYS_SYN_OFFSET)
#define SYS_SYN_GA_VGPU_GET_CUR_FPS (4102 + SYS_SYN_OFFSET)
#define SYS_SYN_GA_VGPU_GET_AVG_FPS (4103 + SYS_SYN_OFFSET)
#define SYS_SYN_GA_VGPU_GET_MAX_RAM_MB (4104 + SYS_SYN_OFFSET)
#define SYS_SYN_VGPU_GET_INFO (4200 + SYS_SYN_OFFSET)
#define SYS_SYN_VGPU_ALLOC_WINDOW (4201 + SYS_SYN_OFFSET)
#define SYS_SYN_VGPU_FREE_WINDOW (4202 + SYS_SYN_OFFSET)
#define SYS_SYN_VGPU_BLIT (4203 + SYS_SYN_OFFSET)
#define SYS_SYN_VGPU_DRAW_RECT (4204 + SYS_SYN_OFFSET)
#define SYS_SYN_VGPU_FLUSH (4205 + SYS_SYN_OFFSET)
#define SYS_SYN_MISC_GET_CPU_FEATURES (5000 + SYS_SYN_OFFSET)
#define SYS_SYN_MISC_GET_MODULE_INFO (5001 + SYS_SYN_OFFSET)
#define SYS_SYN_MISC_GET_GETUPTIME (5007 + SYS_SYN_OFFSET)

// net

#define SYS_SYN_NETDEV_GET_COUNT (6000 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_GET_NAME (6001 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_GET_IPV4 (6002 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_GET_IPV6 (6003 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_GET_LINK_STATUS (6004 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_TX_INC (6005 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_RX_INC (6006 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_TX_GET (6007 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_RX_GET (6008 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_MAC_GET (6009 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_MAC_SET (6010 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_RAW_SEND (6011 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_RAW_RECV (6012 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_TUN_CREATE (6013 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_TUN_DESTROY (6014 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_TUN_SEND (6015 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_TUN_RECV (6016 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_TUN_GET_COUNT (6017 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_TUN_GET_NAME (6018 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_TUN_GET_MAC (6019 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_TUN_SET_MAC (6020 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_TUN_GET_IPV4 (6021 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_TUN_GET_IPV6 (6022 + SYS_SYN_OFFSET)
#define SYS_SYN_NETDEV_TUN_GET_LINK_STATUS (6023 + SYS_SYN_OFFSET)
#define SYS_SYN_TCP_CONNECT (6100 + SYS_SYN_OFFSET)
#define SYS_SYN_TCP_SEND (6101 + SYS_SYN_OFFSET)
#define SYS_SYN_TCP_CLOSE (6102 + SYS_SYN_OFFSET)
#define SYS_SYN_TCP_RECV (6103 + SYS_SYN_OFFSET)

// VFS
#define O_ACCMODE (03 | O_PATH)
#define O_RDONLY 00
#define O_WRONLY 01
#define O_RDWR 02
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define O_CREAT 0100
#define O_EXCL 0200
#define O_NOCTTY 0400
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#define O_DSYNC 010000
#define O_ASYNC 020000
#define O_DIRECT 040000
#define O_DIRECTORY 0200000
#define O_NOFOLLOW 0400000
#define O_CLOEXEC 02000000
#define O_SYNC 04010000
#define O_RSYNC 04010000
#define O_LARGEFILE 0100000
#define O_NOATIME 01000000
#define O_TMPFILE 020000000

struct dirent {
	uint64_t d_ino;
	uint64_t d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[1024];
};

#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

// Максимальная длина лога за один вызов
#define SYN_LOG_MAX_LEN 4096

struct utsname {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
};

// POSIX (x86_64 Linux ABI)
#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_mmap 9
#define SYS_munmap 11
#define SYS_brk 12
#define SYS_getpid 39
#define SYS_fork 57
#define SYS_execve 59
#define SYS_exit 60
#define SYS_wait4 61
#define SYS_kill 62
#define SYS_uname 63
#define SYS_sysinfo 99
#define SYS_posix_spawn 334

struct sysinfo {
	long uptime;
	unsigned long loads[3];
	unsigned long totalram;
	unsigned long freeram;
	unsigned long sharedram;
	unsigned long bufferram;
	unsigned long totalswap;
	unsigned long freeswap;
	unsigned short procs;
	unsigned long totalhigh;
	unsigned long freehigh;
	unsigned int mem_unit;
	char _f[20 - 2 * sizeof(long) - sizeof(int)];
};

#endif