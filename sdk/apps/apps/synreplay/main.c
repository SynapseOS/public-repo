// main.c: Синтетический ввод и сценарии
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: Синтетический ввод и сценарии в пользовательском пространстве
// Первое изменение: до текущей уборки, исходная дата не установлена
// Последнее изменение: 15.10.2026

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>
#include "../../../include/lumine/replay.h"
#include "../../../include/lumine/pointer.h"
#include "../../../include/lumine/process.h"
#define CONTROL "/var/run/synreplay.active"
#define RESULT "/var/run/synreplay.result"
static volatile sig_atomic_t interrupted;
static FILE *errors;
static int diagnostic_errors;
static struct lumine_process_info_v2 procs[4096], previous[4096];
static long previous_count;
static uint64_t previous_time;

static int pid_order(const void *a, const void *b) {
	const struct lumine_process_info_v2 *x = a, *y = b;
	return (x->base.pid > y->base.pid) - (x->base.pid < y->base.pid);
}

static void process_name(FILE *f, const char *name) {
	fputc('"', f);

	for (unsigned i = 0; i < 256 && name[i]; i++) {
		if (name[i] == '"') {
			fputc('"', f);
		}

		fputc(name[i], f);
	}

	fputc('"', f);
}

static void stop_signal(int sig) {
	(void)sig;
	interrupted = 1;
}

static uint64_t now_us(void) {
	struct timespec t;

	if (clock_gettime(CLOCK_MONOTONIC, &t)) {
		return 0;
	}

	return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void problem(const char *path) {
	if (errors) {
		diagnostic_errors++;
		fprintf(errors, "%s: %s\n", path, strerror(errno));
		fflush(errors);
	} else {
		fprintf(stderr, "synrpl: %s: %s\n", path, strerror(errno));
	}
}

static FILE *new_file(const char *path) {
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

	if (fd < 0) {
		return NULL;
	}

	FILE *f = fdopen(fd, "wb");

	if (!f) {
		close(fd);
	}

	return f;
}

// Скопировать доступные данные, для потока EAGAIN означает конец текущей порции
static int copy_stream(int fd, FILE *f) {
	char b[8192];

	for (;;) {
		ssize_t n = read(fd, b, sizeof b);

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}

			return errno == EAGAIN ? 0 : -1;
		}

		if (n == 0) {
			return 0;
		}

		if (fwrite(b, 1, (size_t)n, f) != (size_t)n) {
			return -1;
		}
	}
}

static void copy_file(const char *src, const char *dst) {
	int fd = open(src, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	if (fd < 0) {
		problem(src);
		return;
	}

	struct stat st;

	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		close(fd);
		return;
	}

	FILE *out = new_file(dst);

	if (!out) {
		problem(dst);
	} else {
		if (copy_stream(fd, out)) {
			problem(src);
		}

		if (fclose(out)) {
			problem(dst);
		}
	}

	close(fd);
}

static void copy_logs(const char *src, const char *dst, unsigned depth) {
	if (depth > 16) {
		errno = ELOOP;
		problem(src);
		return;
	}

	DIR *d = opendir(src);

	if (!d) {
		problem(src);
		return;
	}

	if (mkdir(dst, 0700) && errno != EEXIST) {
		problem(dst);
		closedir(d);
		return;
	}

	struct dirent *e;

	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
			continue;
		}

		char a[4096], b[4096];

		if (snprintf(a, sizeof a, "%s/%s", src, e->d_name) >= (int)sizeof a ||
		    snprintf(b, sizeof b, "%s/%s", dst, e->d_name) >= (int)sizeof b) {
			errno = ENAMETOOLONG;
			problem(src);
			continue;
		}

		struct stat st;

		if (lstat(a, &st)) {
			problem(a);
			continue;
		}

		if (S_ISDIR(st.st_mode)) {
			copy_logs(a, b, depth + 1);
		} else if (S_ISREG(st.st_mode)) {
			copy_file(a, b);
		}
	}

	closedir(d);
}

static int sample(FILE *metrics, FILE *processes, uint64_t step, const struct syn_replay_event *e) {
	uint64_t t = now_us( );
	struct sysinfo si;

	if (sysinfo(&si)) {
		problem("sysinfo");
		memset(&si, 0, sizeof si);
	}

	long n = syscall(LUMINE_PROCESS_SNAPSHOT_V2, procs, 4096, sizeof(procs[0]));

	if (n < 0) {
		problem("process snapshot");
		n = 0;
	}

	if (n > 4096) {
		errno = EOVERFLOW;
		problem("process snapshot");
		return -1;
	}

	if (n == 4096) {
		errno = EOVERFLOW;
		problem("process snapshot capacity reached");
	}

	qsort(procs, n, sizeof procs[0], pid_order);
	uint64_t cpu = 0;
	double total_percent = 0;

	for (long i = 0, j = 0; i < n; i++) {
		double percent = -1;

		while (j < previous_count && previous[j].base.pid < procs[i].base.pid) {
			j++;
		}

		if (previous_time && t > previous_time && j < previous_count &&
		    previous[j].base.pid == procs[i].base.pid && procs[i].cpu_us >= previous[j].cpu_us) {
			percent = 100.0 * (procs[i].cpu_us - previous[j].cpu_us) / (t - previous_time);
		}

		if (percent >= 0) {
			total_percent += percent;
		}

		cpu += procs[i].cpu_us;
		fprintf(processes, "%llu,%llu,%lld,%u,%u,%llu,%llu,%.3f,", (unsigned long long)step,
		        (unsigned long long)t, (long long)procs[i].base.pid, procs[i].base.state,
		        procs[i].base.threads, (unsigned long long)procs[i].cpu_us,
		        (unsigned long long)procs[i].resident_bytes, percent);
		process_name(processes, procs[i].base.name);
		fputc('\n', processes);
	}

	fprintf(metrics, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%ld,%.3f\n", (unsigned long long)step,
	        (unsigned long long)(e ? e->us : 0), (unsigned long long)t,
	        (unsigned long long)(e ? e->total_ram : 0), (unsigned long long)(e ? e->free_ram : 0),
	        (unsigned long long)((uint64_t)si.totalram * si.mem_unit),
	        (unsigned long long)((uint64_t)si.freeram * si.mem_unit), (unsigned long long)cpu, n,
	        previous_time ? total_percent : -1);
	memcpy(previous, procs, n * sizeof procs[0]);
	previous_count = n;
	previous_time = t;
	return fflush(metrics) || fflush(processes) || ferror(metrics) || ferror(processes) ? -1 : 0;
}

static int run_archive(const char *archive, const char *rpl, const char *logs) {
	pid_t p = fork( );

	if (p < 0) {
		return -1;
	}

	if (!p) {
		execl("/bin/tar", "tar", "-c", "-z", "-f", archive, rpl, logs, (char *)NULL);
		_exit(127);
	}

	int status;

	while (waitpid(p, &status, 0) < 0) {
		if (errno != EINTR) {
			return -1;
		}
	}

	return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}

static void finish_control(int status, const char *file) {
	FILE *f = fopen(RESULT, "w");

	if (f) {
		fprintf(f, "%d %s\n", status, file);
		fclose(f);
	}

	unlink(CONTROL "/stop");
	rmdir(CONTROL);
}

static int record(void) {
	if (mkdir("/var/run", 0755) && errno != EEXIST) {
		problem("/var/run");
		return 1;
	}

	if (mkdir(CONTROL, 0700)) {
		problem("recording active (or stale " CONTROL ")");
		return 1;
	}

	unlink(RESULT);
	int dev = open("/dev/synreplay", O_RDWR | O_NONBLOCK | O_CLOEXEC);

	if (dev < 0) {
		problem("/dev/synreplay (requires a kernel with replay support)");
		rmdir(CONTROL);
		return 1;
	}

	time_t rt = time(NULL);
	struct tm tm;
	localtime_r(&rt, &tm);
	char date[32];
	strftime(date, sizeof date, "%d.%m.%Y", &tm);
	char rpl[128], logs[160], archive[160];
	FILE *out = NULL;

	for (unsigned i = 1; i < 1000000; i++) {
		snprintf(rpl, sizeof rpl, "rpl-%u-%s.rpl", i, date);
		out = new_file(rpl);

		if (out || errno != EEXIST) {
			break;
		}
	}

	if (!out) {
		problem("create replay");
		close(dev);
		rmdir(CONTROL);
		return 1;
	}

	snprintf(logs, sizeof logs, "%s.logs", rpl);
	snprintf(archive, sizeof archive, "%s.tar.gz", rpl);

	if (mkdir(logs, 0700)) {
		problem(logs);
		fclose(out);
		unlink(rpl);
		close(dev);
		rmdir(CONTROL);
		return 1;
	}

	char path[256];
	snprintf(path, sizeof path, "%s/errors.log", logs);
	errors = new_file(path);
	snprintf(path, sizeof path, "%s/metrics.csv", logs);
	FILE *metrics = new_file(path);
	snprintf(path, sizeof path, "%s/processes.csv", logs);
	FILE *processes = new_file(path);
	snprintf(path, sizeof path, "%s/dmesg.log", logs);
	FILE *dmesg = new_file(path);

	if (!errors || !metrics || !processes || !dmesg) {
		problem("create diagnostics");
		finish_control(1, rpl);
		return 1;
	}

	int dfd = open("/dev/dmesg", O_RDONLY | O_CLOEXEC | O_NONBLOCK);

	if (dfd < 0) {
		problem("/dev/dmesg");
	}

	snprintf(path, sizeof path, "%s/info.txt", logs);
	FILE *info = new_file(path);

	if (info) {
		fprintf(
		    info,
		    "synreplay v1\nDate: %s\nInput: keyboard scancodes and pointer events\nRAM in events: at capture; CPU/process snapshots: at consumption\nCPU counters are cumulative microseconds; snapshots may omit exited processes.\nNo SSH/serial/PTTY bytes, clipboard, screenshots or filesystem contents.\n",
		    date);
		fclose(info);
	}

	fprintf(
	    metrics,
	    "step,event_us,sample_us,event_total_ram,event_free_ram,total_ram,free_ram,live_cpu_us,process_count,process_cpu_percent\n");
	fprintf(processes, "step,sample_us,pid,state,threads,cpu_us,rss_bytes,cpu_percent,name\n");
	struct syn_replay_header h = { .magic = "SYNRPL1",
		                           .version = 1,
		                           .event_size = sizeof(struct syn_replay_event),
		                           .realtime = rt,
		                           .monotonic = now_us( ) };
	if (fwrite(&h, 1, sizeof h, out) != sizeof h || fflush(out)) {
		problem(rpl);
		finish_control(1, rpl);
		return 1;
	}

	if (fflush(NULL)) {
		problem("flush recording files");
		finish_control(1, rpl);
		return 1;
	}

	int ready[2];

	if (pipe(ready)) {
		problem("pipe");
		finish_control(1, rpl);
		return 1;
	}

	pid_t child = fork( );

	if (child < 0) {
		problem("fork");
		finish_control(1, rpl);
		return 1;
	}

	if (child) {
		close(ready[1]);
		char ok = 0;
		ssize_t n = read(ready[0], &ok, 1);
		close(ready[0]);
		fclose(out);
		fclose(metrics);
		fclose(processes);
		fclose(dmesg);
		fclose(errors);
		close(dev);

		if (dfd >= 0) {
			close(dfd);
		}

		if (n == 1 && ok == 1) {
			printf("Recording: %s\nStop: synrpl stop\n", rpl);
			return 0;
		}

		fprintf(stderr, "synrpl: recorder failed to start\n");
		return 1;
	}

	close(ready[0]);
	setsid( );
	signal(SIGINT, stop_signal);
	signal(SIGTERM, stop_signal);
	int null = open("/dev/null", O_RDWR);

	if (null >= 0) {
		dup2(null, 0);
		dup2(null, 1);
		dup2(fileno(errors), 2);
		close(null);
	}

	int status = 0;
	char ok = 1;
	snprintf(path, sizeof path, "%s/recorder.pid", logs);
	FILE *pidfile = new_file(path);

	if (pidfile) {
		fprintf(pidfile, "%ld\n", (long)getpid( ));

		if (fclose(pidfile)) {
			problem("recorder.pid");
		}
	} else {
		problem("recorder.pid");
	}

	if (sample(metrics, processes, 0, NULL) || ioctl(dev, SYN_REPLAY_START, 0)) {
		problem("arm recorder");
		ok = 0;
		status = 1;
	}

	write(ready[1], &ok, 1);
	close(ready[1]);
	uint64_t step = 0, tick = now_us( );
	int stopped = !ok;
	struct syn_replay_event e;

	while (ok) {
		if (!stopped && (interrupted || !access(CONTROL "/stop", F_OK))) {
			if (ioctl(dev, SYN_REPLAY_STOP, 0)) {
				problem("stop input");
				status = 1;
				break;
			}

			stopped = 1;
		}

		ssize_t n = read(dev, &e, sizeof e);

		if (n == (ssize_t)sizeof e) {
			if (fwrite(&e, 1, sizeof e, out) != sizeof e || fflush(out)) {
				problem(rpl);
				status = 1;
				break;
			}

			if (e.kind == SYN_REPLAY_END) {
				if (e.total_ram) {
					fprintf(errors, "Lost input events: %llu\n", (unsigned long long)e.total_ram);
					status = 1;
				}

				break;
			}

			if (sample(metrics, processes, ++step, &e)) {
				problem("write metrics");
				status = 1;
				break;
			}
		} else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
			if (now_us( ) - tick >= 1000000) {
				if (sample(metrics, processes, step, NULL)) {
					status = 1;
					break;
				}

				tick = now_us( );
			}

			struct timespec delay = { 0, 10000000 };
			nanosleep(&delay, NULL);
		} else {
			problem("input read");
			status = 1;
			break;
		}

		if (dfd >= 0 && copy_stream(dfd, dmesg)) {
			problem("dmesg");
			close(dfd);
			dfd = -1;
		}
	}

	ioctl(dev, SYN_REPLAY_STOP, 0);
	close(dev);

	if (dfd >= 0) {
		if (copy_stream(dfd, dmesg)) {
			problem("dmesg");
		}

		close(dfd);
	}

	if (fclose(out)) {
		status = 1;
	}

	if (fclose(metrics)) {
		status = 1;
	}

	if (fclose(processes)) {
		status = 1;
	}

	if (fclose(dmesg)) {
		status = 1;
	}

	snprintf(path, sizeof path, "%s/system", logs);
	copy_logs("/var/log", path, 0);
	const char *sources[] = { "/proc/cpuinfo", "/proc/meminfo", "/proc/uptime", "/proc/version",
		                      NULL };
	for (int i = 0; sources[i]; i++) {
		snprintf(path, sizeof path, "%s/%s", logs, strrchr(sources[i], '/') + 1);
		copy_file(sources[i], path);
	}

	if (diagnostic_errors) {
		status = 1;
	}

	fflush(errors);
	char temp[192];
	snprintf(temp, sizeof temp, "%s.partial", archive);

	if (run_archive(temp, rpl, logs) || rename(temp, archive)) {
		problem("compressed archive; raw files retained");
		status = 1;
	}

	fclose(errors);
	finish_control(status, status ? rpl : archive);
	_exit(status);
}

static int stop_recording(void) {
	if (access(CONTROL, F_OK)) {
		problem("no active recording");
		return 1;
	}

	int fd = open(CONTROL "/stop", O_WRONLY | O_CREAT, 0600);

	if (fd < 0) {
		problem("request stop");
		return 1;
	}

	close(fd);
	puts("Finishing recording and archive...");

	while (!access(CONTROL, F_OK)) {
		if (interrupted) {
			return 1;
		}

		struct timespec t = { 0, 100000000 };
		nanosleep(&t, NULL);
	}

	FILE *f = fopen(RESULT, "r");
	int status = 1;
	char result[256] = "unknown";

	if (f) {
		if (fscanf(f, "%d %255s", &status, result) != 2) {
			status = 1;
		}

		fclose(f);
	}

	printf("%s: %s\n", status ? "Recording incomplete; inspect errors.log" : "Saved", result);
	return status;
}

static int valid_event(const struct syn_replay_event *e) {
	if (e->reserved) {
		return 0;
	}

	if (e->kind == SYN_REPLAY_KEY) {
		return e->device < 32 && e->code < 128 && e->extended <= 1 && e->pressed <= 1;
	}

	if (e->kind == SYN_REPLAY_POINTER) {
		return e->device >= 1 && e->device <= SYN_POINTER_MAX &&
		       !(e->flags & ~SYN_POINTER_FLAGS_MASK) && !(e->buttons & ~SYN_POINTER_BUTTONS_MASK);
	}

	return e->kind == SYN_REPLAY_END;
}

static int playback(const char *path) {
	FILE *f = fopen(path, "rb");

	if (!f) {
		problem(path);
		return 1;
	}

	struct syn_replay_header h;
	struct syn_replay_event e;
	uint64_t prev;
	int end = 0;

	if (fread(&h, 1, sizeof h, f) != sizeof h || memcmp(h.magic, "SYNRPL1", 8) || h.version != 1 ||
	    h.event_size != sizeof e) {
		goto invalid;
	}

	prev = h.monotonic;

	while (fread(&e, 1, sizeof e, f) == sizeof e) {
		if (!valid_event(&e) || e.us < prev || end) {
			goto invalid;
		}

		prev = e.us;

		if (e.kind == SYN_REPLAY_END) {
			end = 1;

			if (e.total_ram) {
				goto invalid;
			}

			break;
		}
	}

	if (!end || fgetc(f) != EOF || ferror(f)) {
		goto invalid;
	}

	int dev = open("/dev/synreplay", O_RDWR | O_NONBLOCK | O_CLOEXEC);

	if (dev < 0) {
		problem("/dev/synreplay");
		fclose(f);
		return 1;
	}

	puts("Replay starts in 3 seconds. Restore the original application state. Ctrl+C cancels.");
	fflush(stdout);
	uint64_t start = now_us( ) + 3000000;
	unsigned char held[32][256] = { { 0 } };
	unsigned buttons[17] = { 0 };
	int status = 0;

	if (fseek(f, sizeof h, SEEK_SET)) {
		problem("seek replay");
		close(dev);
		fclose(f);
		return 1;
	}

	end = 0;
	prev = h.monotonic;

	while (!interrupted && fread(&e, 1, sizeof e, f) == sizeof e) {
		if (!valid_event(&e) || e.us < prev) {
			status = 1;
			break;
		}

		prev = e.us;

		if (e.kind == SYN_REPLAY_END) {
			end = 1;

			if (e.total_ram) {
				status = 1;
			}

			break;
		}

		uint64_t delay = e.us - h.monotonic;

		if (UINT64_MAX - start < delay) {
			status = 1;
			break;
		}

		while (!interrupted && now_us( ) < start + delay) {
			struct timespec t = { 0, 1000000 };
			nanosleep(&t, NULL);
		}

		if (interrupted) {
			break;
		}

		if (write(dev, &e, sizeof e) != sizeof e) {
			problem("inject event");
			status = 1;
			break;
		}

		if (e.kind == SYN_REPLAY_KEY) {
			held[e.device][e.code + 128 * e.extended] = e.pressed;
		} else {
			buttons[e.device] = e.buttons;
		}
	}

	if (!interrupted && (!end || ferror(f))) {
		status = 1;
	}
	// Отпускаем синтетические клавиши при отмене, ошибке и незавершённой комбинации
	for (unsigned d = 0; d < 32; d++) {
		for (unsigned k = 0; k < 256; k++) {
			if (held[d][k]) {
				struct syn_replay_event release = {
					.kind = SYN_REPLAY_KEY, .device = d, .code = k % 128, .extended = k / 128
				};

				if (write(dev, &release, sizeof release) != sizeof release) {
					status = 1;
				}
			}
		}
	}

	for (unsigned d = 1; d <= 16; d++) {
		if (buttons[d]) {
			struct syn_replay_event release = { .kind = SYN_REPLAY_POINTER, .device = d };

			if (write(dev, &release, sizeof release) != sizeof release) {
				status = 1;
			}
		}
	}

	close(dev);
	fclose(f);
	return interrupted ? 130 : status;
invalid:
	fprintf(stderr, "synrpl: invalid, incomplete or lossy replay: %s\n", path);
	fclose(f);
	return 1;
}

int main(int argc, char **argv) {
	umask(077);
	signal(SIGINT, stop_signal);
	signal(SIGTERM, stop_signal);

	if (argc == 2 && !strcmp(argv[1], "start")) {
		return record( );
	}

	if (argc == 2 && !strcmp(argv[1], "stop")) {
		return stop_recording( );
	}

	if (argc == 3 && !strcmp(argv[1], "try")) {
		return playback(argv[2]);
	}

	puts(
	    "Usage: synrpl start | stop | try FILE.rpl\nRecords local keyboard/pointers and diagnostics in the current directory.");
	return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
}
