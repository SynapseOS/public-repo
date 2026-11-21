#include <sched.h>
#include <errno.h>
#include "syscall.h"

int sched_getscheduler(pid_t pid) {
#ifdef __aarch64__
	return syscall(SYS_sched_getscheduler, pid);
#else
	return __syscall_ret(-ENOSYS);
#endif
}
