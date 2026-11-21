#include <sched.h>
#include <errno.h>
#include "syscall.h"

int sched_setscheduler(pid_t pid, int sched, const struct sched_param *param) {
#ifdef __aarch64__
	return syscall(SYS_sched_setscheduler, pid, sched, param);
#else
	return __syscall_ret(-ENOSYS);
#endif
}
