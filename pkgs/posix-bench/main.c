#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
	do {                                                                                           \
		if (!(x))                                                                                  \
			return errno ? errno : EIO;                                                            \
	} while (0)
#define EXPECT(x)                                                                                  \
	do {                                                                                           \
		if (!(x))                                                                                  \
			return EPROTO;                                                                         \
	} while (0)
static char directory[128], file[160], other[160];
static const char *self;
static volatile sig_atomic_t caught;

static void on_signal(int n) {
	caught = n;
}

static uint64_t now(void) {
	struct timespec t;

	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return 0;
	return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static int file_io(void) {
	int fd = open(file, O_CREAT | O_TRUNC | O_RDWR, 0600);
	CHECK(fd >= 0);
	char data[16] = "posix-data", readback[16] = { 0 };
	int ok = write(fd, data, sizeof data) == sizeof data && lseek(fd, 0, SEEK_SET) == 0 &&
	         read(fd, readback, sizeof readback) == sizeof readback &&
	         !memcmp(data, readback, sizeof data);
	int saved = errno;
	close(fd);
	errno = saved;
	CHECK(ok);
	return 0;
}

static int positioned_io(void) {
	int fd = open(file, O_RDWR);
	CHECK(fd >= 0);
	char c = 0;
	int ok = pwrite(fd, "P", 1, 3) == 1 && pread(fd, &c, 1, 3) == 1 && c == 'P' &&
	         lseek(fd, 0, SEEK_CUR) == 0;
	int saved = errno;
	close(fd);
	errno = saved;
	CHECK(ok);
	return 0;
}

static int metadata(void) {
	struct stat st;
	CHECK(chmod(file, 0640) == 0);
	CHECK(stat(file, &st) == 0);
	EXPECT(S_ISREG(st.st_mode) && st.st_size == 16 && (st.st_mode & 0777) == 0640);
	CHECK(rename(file, other) == 0);
	CHECK(rename(other, file) == 0);
	return 0;
}

static int directories(void) {
	DIR *d = opendir(directory);
	CHECK(d);
	int found = 0;
	struct dirent *e;

	while ((e = readdir(d)))
		if (!strcmp(e->d_name, "data"))
			found = 1;
	CHECK(closedir(d) == 0);
	EXPECT(found);
	return 0;
}

static int links(void) {
	unlink(other);
	CHECK(symlink("data", other) == 0);
	char buf[32] = { 0 };
	ssize_t n = readlink(other, buf, sizeof buf);
	struct stat st;
	int ok = n == 4 && !memcmp(buf, "data", 4) && lstat(other, &st) == 0 && S_ISLNK(st.st_mode);
	unlink(other);
	EXPECT(ok);
	return 0;
}

static int descriptor_dup(void) {
	int fd = open(file, O_RDONLY);
	CHECK(fd >= 0);
	int second = dup(fd);

	if (second < 0) {
		int e = errno;
		close(fd);
		return e;
	}

	char a, b;
	int ok = read(fd, &a, 1) == 1 && read(second, &b, 1) == 1 && lseek(fd, 0, SEEK_CUR) == 2;
	close(second);
	close(fd);
	EXPECT(ok);
	return 0;
}

static int descriptor_flags(void) {
	int fd = open(file, O_RDONLY);
	CHECK(fd >= 0);
	int ok = fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC);
	int saved = errno;
	close(fd);
	errno = saved;
	CHECK(ok);
	return 0;
}

static int pipe_poll(void) {
	int p[2];
	CHECK(pipe(p) == 0);
	struct pollfd pollfd = { p[0], POLLIN, 0 };
	char c;
	int ok = write(p[1], "X", 1) == 1 && poll(&pollfd, 1, 1000) == 1 && (pollfd.revents & POLLIN) &&
	         read(p[0], &c, 1) == 1 && c == 'X';
	int saved = errno;
	close(p[0]);
	close(p[1]);
	errno = saved;
	CHECK(ok);
	return 0;
}

static int pipe_select(void) {
	int p[2];
	CHECK(pipe(p) == 0);
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(p[0], &fds);
	struct timeval tv = { 1, 0 };
	int ok = write(p[1], "X", 1) == 1 && select(p[0] + 1, &fds, NULL, NULL, &tv) == 1 &&
	         FD_ISSET(p[0], &fds);
	int saved = errno;
	close(p[0]);
	close(p[1]);
	errno = saved;
	CHECK(ok);
	return 0;
}

static int processes(void) {
	pid_t p = fork( );
	CHECK(p >= 0);

	if (!p)
		_exit(37);
	int status;
	CHECK(waitpid(p, &status, 0) == p);
	EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 37);
	return 0;
}

static int execute(void) {
	pid_t p = fork( );
	CHECK(p >= 0);

	if (!p) {
		execlp(self, self, "--child", (char *)NULL);
		_exit(127);
	}

	int status;
	CHECK(waitpid(p, &status, 0) == p);
	EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 37);
	return 0;
}

static int spawn_process(void) {
	extern char **environ;
	char *args[] = { (char *)self, "--child", NULL };
	pid_t p;
	int e = posix_spawnp(&p, self, NULL, NULL, args, environ);

	if (e)
		return e;
	int status;
	CHECK(waitpid(p, &status, 0) == p);
	EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 37);
	return 0;
}

static int anonymous_memory(void) {
	long page = sysconf(_SC_PAGESIZE);
	CHECK(page > 0);
	void *p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(p != MAP_FAILED);
	memset(p, 0xa5, page);
	int ok = ((unsigned char *)p)[page - 1] == 0xa5;
	CHECK(munmap(p, page) == 0);
	EXPECT(ok);
	return 0;
}

static int protection(void) {
	long page = sysconf(_SC_PAGESIZE);
	CHECK(page > 0);
	void *p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(p != MAP_FAILED);
	*(int *)p = 42;
	int ok = mprotect(p, page, PROT_READ) == 0 && *(int *)p == 42 &&
	         mprotect(p, page, PROT_READ | PROT_WRITE) == 0;
	int saved = errno;
	munmap(p, page);
	errno = saved;
	CHECK(ok);
	return 0;
}

static int file_memory(void) {
	int fd = open(file, O_RDWR);
	CHECK(fd >= 0);
	long page = sysconf(_SC_PAGESIZE);

	if (page <= 0 || ftruncate(fd, page)) {
		int e = errno;
		close(fd);
		return e ? e : EIO;
	}

	char *p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (p == MAP_FAILED) {
		int e = errno;
		close(fd);
		return e;
	}

	p[0] = 'M';
	char c = 0;
	int ok = msync(p, page, MS_SYNC) == 0 && pread(fd, &c, 1, 0) == 1 && c == 'M';
	int saved = errno;
	munmap(p, page);
	close(fd);
	errno = saved;
	CHECK(ok);
	return 0;
}

static void *thread_worker(void *p) {
	return p;
}

static int threads(void) {
	pthread_t t;
	int e = pthread_create(&t, NULL, thread_worker, (void *)(uintptr_t)42);

	if (e)
		return e;
	void *v;
	e = pthread_join(t, &v);

	if (e)
		return e;
	EXPECT((uintptr_t)v == 42);
	return 0;
}

struct sync_state {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int ready;
};

static void *sync_worker(void *arg) {
	struct sync_state *s = arg;
	pthread_mutex_lock(&s->mutex);
	s->ready = 1;
	pthread_cond_signal(&s->cond);
	pthread_mutex_unlock(&s->mutex);
	return NULL;
}

static int synchronization(void) {
	struct sync_state s = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0 };
	pthread_t t;
	int e = pthread_create(&t, NULL, sync_worker, &s);

	if (e)
		return e;
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 2;
	pthread_mutex_lock(&s.mutex);

	while (!s.ready && !e)
		e = pthread_cond_timedwait(&s.cond, &s.mutex, &deadline);
	pthread_mutex_unlock(&s.mutex);
	pthread_join(t, NULL);
	pthread_cond_destroy(&s.cond);
	pthread_mutex_destroy(&s.mutex);
	return e;
}

static int signals(void) {
	struct sigaction sa = { 0 }, old;
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	CHECK(sigaction(SIGUSR1, &sa, &old) == 0);
	caught = 0;
	int e = raise(SIGUSR1);
	int ok = caught == SIGUSR1;
	sigaction(SIGUSR1, &old, NULL);
	CHECK(e == 0);
	EXPECT(ok);
	return 0;
}

static int local_sockets(void) {
	int p[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, p) == 0);
	char c;
	int ok = send(p[0], "S", 1, 0) == 1 && recv(p[1], &c, 1, 0) == 1 && c == 'S';
	int saved = errno;
	close(p[0]);
	close(p[1]);
	errno = saved;
	CHECK(ok);
	return 0;
}

static int clocks(void) {
	struct timespec a, b;
	CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
	EXPECT(b.tv_sec > a.tv_sec || (b.tv_sec == a.tv_sec && b.tv_nsec >= a.tv_nsec));
	return 0;
}

static int sleeping(void) {
	struct timespec pause = { 0, 100000 };
	uint64_t start = now( );
	CHECK(nanosleep(&pause, NULL) == 0);
	EXPECT(now( ) >= start + 100000);
	return 0;
}

static int identity(void) {
	struct utsname info;
	CHECK(uname(&info) == 0);
	EXPECT(info.sysname[0] && getpid( ) > 0);
	return 0;
}

struct test {
	const char *name;
	int (*fn)(void);
	int slow;
};
static const struct test tests[] = { { "open/read/write/lseek", file_io, 0 },
	                                 { "pread/pwrite", positioned_io, 0 },
	                                 { "stat/chmod/rename", metadata, 0 },
	                                 { "opendir/readdir", directories, 0 },
	                                 { "symlink/readlink/lstat", links, 0 },
	                                 { "dup shared offset", descriptor_dup, 0 },
	                                 { "fcntl CLOEXEC", descriptor_flags, 0 },
	                                 { "pipe/poll", pipe_poll, 0 },
	                                 { "pipe/select", pipe_select, 0 },
	                                 { "fork/waitpid", processes, 1 },
	                                 { "exec/waitpid", execute, 1 },
	                                 { "posix_spawn/waitpid", spawn_process, 1 },
	                                 { "mmap/munmap anonymous", anonymous_memory, 0 },
	                                 { "mprotect", protection, 0 },
	                                 { "mmap file/msync", file_memory, 0 },
	                                 { "pthread create/join", threads, 1 },
	                                 { "mutex/condvar", synchronization, 1 },
	                                 { "sigaction/raise", signals, 0 },
	                                 { "socketpair/send/recv", local_sockets, 0 },
	                                 { "clock monotonic", clocks, 0 },
	                                 { "nanosleep", sleeping, 1 },
	                                 { "uname/getpid", identity, 0 } };

int main(int argc, char **argv) {
	unsigned iterations = 100;
	self = argv[0];

	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--child"))
			return 37;
		if (!strcmp(argv[i], "--quick"))
			iterations = 5;
		else if (!strcmp(argv[i], "--iterations") && i + 1 < argc) {
			char *end;
			unsigned long n = strtoul(argv[++i], &end, 10);

			if (*end || !n || n > 100000) {
				fputs("iterations: 1..100000\n", stderr);
				return 2;
			}

			iterations = n;
		} else {
			puts("Usage: posix-bench [--quick | --iterations N]");
			return !strcmp(argv[i], "--help") ? 0 : 2;
		}
	}

	strcpy(directory, "/tmp/posix-bench-XXXXXX");

	if (!mkdtemp(directory)) {
		perror("mkdtemp");
		return 1;
	}

	snprintf(file, sizeof file, "%s/data", directory);
	snprintf(other, sizeof other, "%s/other", directory);
	puts("Synapse POSIX compatibility benchmark v1.0");
	printf("Iterations: %u (process/thread/sleep tests capped at 10)\n", iterations);
	puts("RESULT  API group                  runs     mean us");
	unsigned pass = 0, fail = 0, skip = 0;

	for (unsigned i = 0; i < sizeof tests / sizeof *tests; ++i) {
		unsigned runs = tests[i].slow && iterations > 10 ? 10 : iterations;
		int result = 0;
		unsigned completed = 0;
		uint64_t start = now( );

		for (; completed < runs; ++completed) {
			errno = 0;
			result = tests[i].fn( );

			if (result)
				break;
		}

		uint64_t elapsed = now( ) - start;
		const char *status = "PASS";

		if (result == ENOSYS || result == ENOTSUP) {
			status = "SKIP";
			++skip;
		} else if (result) {
			status = "FAIL";
			++fail;
		} else
			++pass;
		printf("%-6s  %-26s %4u   %9.2f", status, tests[i].name, completed,
		       completed ? elapsed / 1000.0 / completed : 0);
		if (result)
			printf("  errno=%d (%s)", result, strerror(result));
		putchar('\n');
		fflush(stdout);
	}

	unlink(file);
	unlink(other);
	rmdir(directory);
	printf("POSIX_BENCH_RESULT pass=%u fail=%u skip=%u total=%zu\n", pass, fail, skip,
	       sizeof tests / sizeof *tests);
	puts("Measured API groups only; this is not POSIX certification.");
	return fail ? 1 : 0;
}
