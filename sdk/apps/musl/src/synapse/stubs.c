#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <syscall.h>

int __synapse_clock_gettime(clockid_t clk, struct timespec *ts) {
	long r = __syscall2(SYS_clock_gettime, clk, (long)ts);

	if (r == -ENOSYS) {
		uint64_t ticks = __syscall0(SYS_syn_get_ticks);
		ts->tv_sec = ticks / 1000;
		ts->tv_nsec = (ticks % 1000) * 1000000L;
		return 0;
	}

	return __syscall_ret(r);
}

int __synapse_nanosleep(const struct timespec *req, struct timespec *rem) {
	long r = __syscall2(SYS_nanosleep, (long)req, (long)rem);

	if (r == -ENOSYS) {
		uint64_t ms = req->tv_sec * 1000 + req->tv_nsec / 1000000;
		__syscall1(SYS_syn_sleep, ms);

		if (rem) {
			rem->tv_sec = 0;
			rem->tv_nsec = 0;
		}

		return 0;
	}

	return __syscall_ret(r);
}

int __synapse_sigaction(int sig, const void *act, void *oldact) {
	long r = __syscall4(SYS_rt_sigaction, sig, (long)act, (long)oldact, _NSIG / 8);

	if (r == -ENOSYS) {
		return 0;
	}

	return __syscall_ret(r);
}

int __synapse_sigprocmask(int how, const void *set, void *oldset) {
	long r = __syscall4(SYS_rt_sigprocmask, how, (long)set, (long)oldset, _NSIG / 8);

	if (r == -ENOSYS) {
		return 0;
	}

	return __syscall_ret(r);
}

long __synapse_futex(int *addr, int op, int val, const void *timeout, int *uaddr2, int val3) {
	long r = __syscall6(SYS_futex, (long)addr, op, val, (long)timeout, (long)uaddr2, val3);

	if (r == -ENOSYS) {
		return -ENOSYS;
	}

	return r;
}

off_t __synapse_lseek(int fd, off_t offset, int whence) {
	long r = __syscall3(SYS_lseek, fd, offset, whence);

	if (r == -ENOSYS) {
		return __syscall_ret(__syscall3(SYS_syn_vfs_seek, fd, offset, whence));
	}

	return __syscall_ret(r);
}

int __synapse_fstat(int fd, struct stat *st) {
	long r = __syscall2(SYS_fstat, fd, (long)st);

	if (r == -ENOSYS) {
		memset(st, 0, sizeof(*st));
		st->st_mode = 0100644; // S_IFREG | 0644
		st->st_blksize = 512;
		return 0;
	}

	return __syscall_ret(r);
}

int __synapse_mprotect(void *addr, size_t len, int prot) {
	long r = __syscall3(SYS_mprotect, (long)addr, len, prot);

	if (r == -ENOSYS) {
		return 0;
	}

	return __syscall_ret(r);
}