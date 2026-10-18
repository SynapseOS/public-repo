#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <pwd.h>
#include <shadow.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <netinet/in.h>
#include "../../apps/apps/common/service_log.h"
#include "../../include/lumine/process.h"
#ifndef WEBPANEL_STATE_DIR
#define WEBPANEL_STATE_DIR "/var/lib/webpanel"
#endif
#ifndef WEBPANEL_LOG_DIR
#define WEBPANEL_LOG_DIR "/var/log"
#endif
#ifndef WEBPANEL_SERVICE_CONF
#define WEBPANEL_SERVICE_CONF "/etc/services"
#endif
#ifndef WEBPANEL_SERVICE_RUN
#define WEBPANEL_SERVICE_RUN "/var/run/services"
#endif
#ifndef WEBPANEL_ASSETS
#define WEBPANEL_ASSETS "/usr/share/webpanel"
#endif
#ifndef WEBPANEL_SERVICE_BIN
#define WEBPANEL_SERVICE_BIN "/bin/service"
#endif
#ifndef WEBPANEL_SETUP_BIN
#define WEBPANEL_SETUP_BIN "/bin/setup"
#endif
#ifndef WEBPANEL_IP_BIN
#define WEBPANEL_IP_BIN "/bin/ip"
#endif
#define LIMIT 65536
#define NJOBS 32
static int listener;
static char csrf[65];
static unsigned sequence;

struct job {
	unsigned id;
	pid_t pid;
	int done, code, restored;
	time_t started, created, finished;
	char path[512], operation[32], action[32], name[64];
};
static struct job jobs[NJOBS];

static time_t monotime(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec;
}

static int safe_name(const char *s, int dots) {
	if (!*s || strlen(s) > 63 || s[0] == '.')
		return 0;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_' && !(dots && *s == '.'))
			return 0;
	return 1;
}

static int safe_identifier(const char *s) {
	return safe_name(s, 0) && s[0] != '-';
}

static int equal(const char *a, const char *b) {
	size_t n = strlen(a);

	if (n != strlen(b))
		return 0;
	unsigned diff = 0;

	for (size_t i = 0; i < n; i++)
		diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
	return !diff;
}

static void json_string(FILE *f, const char *s) {
	fputc('"', f);

	for (; *s; s++) {
		unsigned char c = *s;

		if (c == '"' || c == '\\') {
			fputc('\\', f);
			fputc(c, f);
		} else if (c < 32)
			fprintf(f, "\\u%04x", c);
		else
			fputc(c, f);
	}

	fputc('"', f);
}

static void respond(int fd, int code, const char *type, const char *body, size_t n) {
	char h[1024];
	int z = snprintf(
	    h, sizeof h,
	    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'\r\n\r\n",
	    code, code == 200 ? "OK" : "Error", type, n);
	log_write_all(fd, h, z);
	log_write_all(fd, body, n);
}

static void message(int fd, int code, const char *text) {
	respond(fd, code, "text/plain; charset=utf-8", text, strlen(text));
}

static int header_value(const char *headers, const char *name, char *out, size_t cap) {
	size_t n = strlen(name);

	for (const char *p = headers; *p;) {
		const char *end = strstr(p, "\r\n");

		if (!end)
			end = p + strlen(p);
		if (!strncasecmp(p, name, n) && p[n] == ':') {
			p += n + 1;

			while (*p == ' ' || *p == '\t')
				p++;
			size_t len = end - p;

			if (len >= cap)
				return 0;
			memcpy(out, p, len);
			out[len] = 0;
			return 1;
		}

		if (!*end)
			break;
		p = end + 2;
	}

	return 0;
}

static int authenticated(const char *headers) {
	char authorization[1024], decoded[512];

	if (!header_value(headers, "Authorization", authorization, sizeof authorization) ||
	    strncmp(authorization, "Basic ", 6))
		return 0;
	unsigned acc = 0, bits = 0;
	size_t n = 0;
	const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	for (char *p = authorization + 6; *p && *p != '='; p++) {
		const char *a = strchr(alphabet, *p);

		if (!a)
			return 0;
		acc = (acc << 6) | (a - alphabet);
		bits += 6;

		if (bits >= 8) {
			bits -= 8;

			if (n >= sizeof decoded - 1)
				return 0;
			decoded[n++] = (acc >> bits) & 255;
		}
	}

	decoded[n] = 0;

	if (memchr(decoded, 0, n))
		return 0;
	char *password = strchr(decoded, ':');

	if (!password)
		return 0;
	*password++ = 0;

	if (strcmp(decoded, "root"))
		return 0;
	struct spwd *sp = getspnam("root");

	if (!sp || !sp->sp_pwdp || !*sp->sp_pwdp || sp->sp_pwdp[0] == '!' || sp->sp_pwdp[0] == '*')
		return 0;
	char *hashed = crypt(password, sp->sp_pwdp);
	int ok = hashed && equal(hashed, sp->sp_pwdp);
	memset(decoded, 0, sizeof decoded);
	return ok;
}

static void persist_history(void) {
	int fd =
	    open(WEBPANEL_STATE_DIR "/history.tmp", O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
	if (fd < 0) {
		perror("webpanel history");
		return;
	}

	FILE *f = fdopen(fd, "w");

	if (!f) {
		close(fd);
		return;
	}

	for (int i = 0; i < NJOBS; i++) {
		struct job *j = &jobs[i];

		if (!j->id)
			continue;
		fprintf(f, "%u %ld %ld %d %d %s %s %s\n", j->id, (long)j->created, (long)j->finished,
		        j->done, j->code, j->operation, *j->action ? j->action : "-",
		        *j->name ? j->name : "-");
	}

	int bad = fflush(f);

	if (!bad)
		bad = fsync(fd);
	if (fclose(f))
		bad = 1;
	if (bad || rename(WEBPANEL_STATE_DIR "/history.tmp", WEBPANEL_STATE_DIR "/history")) {
		perror("webpanel history save");
		unlink(WEBPANEL_STATE_DIR "/history.tmp");
	}
}

static void restore_history(void) {
	int fd = open(WEBPANEL_STATE_DIR "/history", O_RDONLY | O_NOFOLLOW);

	if (fd < 0)
		return;
	FILE *f = fdopen(fd, "r");

	if (!f) {
		close(fd);
		return;
	}

	char line[512];

	while (fgets(line, sizeof line, f)) {
		struct job j = { 0 };
		long created, finished;

		if (sscanf(line, "%u %ld %ld %d %d %31s %31s %63s", &j.id, &created, &finished, &j.done,
		           &j.code, j.operation, j.action, j.name) != 8 ||
		    !j.id || !safe_name(j.operation, 0) || !safe_name(j.action, 0) || !safe_name(j.name, 0))
			continue;
		j.created = created;
		j.finished = finished;

		if (!strcmp(j.action, "-"))
			j.action[0] = 0;
		if (!strcmp(j.name, "-"))
			j.name[0] = 0;
		// A command may have survived the old backend. Never invent its exit status.
		if (!j.done) {
			j.done = 1;
			j.code = -1;
			j.restored = 1;
		}

		if (j.code == -1)
			j.restored = 1;
		snprintf(j.path, sizeof j.path, WEBPANEL_STATE_DIR "/job-%u.log", j.id);
		jobs[(j.id - 1) % NJOBS] = j;

		if (j.id > sequence)
			sequence = j.id;
	}

	fclose(f);
}

static void reap(void) {
	int changed = 0;

	for (int i = 0; i < NJOBS; i++)
		if (jobs[i].id && !jobs[i].done) {
			int status;
			pid_t p = waitpid(jobs[i].pid, &status, WNOHANG);

			if (p == jobs[i].pid) {
				jobs[i].done = 1;
				jobs[i].finished = time(NULL);
				jobs[i].code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
				changed = 1;
			} else if (monotime( ) - jobs[i].started > 180)
				kill(jobs[i].pid, SIGKILL);
		}

	if (changed)
		persist_history( );
}

static void clear_job(struct job *j) {
	unlink(j->path);
	char path[512];
	snprintf(path, sizeof path, WEBPANEL_STATE_DIR "/backups/config-%u.bak", j->id);
	unlink(path);
	snprintf(path, sizeof path, WEBPANEL_STATE_DIR "/backups/config-%u.path", j->id);
	unlink(path);
}

static unsigned launch_impl(char *const argv[], int (*worker)(void *), void *context,
                            const char *operation, const char *action, const char *name) {
	reap( );

	for (int i = 0; i < NJOBS; i++)
		if (jobs[i].id && !jobs[i].done)
			return 0;
	struct job *j = &jobs[sequence % NJOBS];
	unsigned id = sequence + 1;
	char path[512];
	snprintf(path, sizeof path, WEBPANEL_STATE_DIR "/job-%u.log", id);
	int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);

	if (fd < 0)
		return 0;
	pid_t p = fork( );

	if (p < 0) {
		close(fd);
		unlink(path);
		return 0;
	}

	if (!p) {
		int null = open("/dev/null", O_RDONLY);
		dup2(null, 0);
		dup2(fd, 1);
		dup2(fd, 2);

		for (int x = 3; x < 256; x++)
			close(x);
		if (worker) {
			int code = worker(context);
			fflush(stdout);
			_exit(code);
		}

		execv(argv[0], argv);
		perror("webpanel exec");
		_exit(127);
	}

	close(fd);

	if (j->id)
		clear_job(j);
	memset(j, 0, sizeof *j);
	sequence = id;
	j->id = id;
	j->pid = p;
	j->code = -1;
	j->started = monotime( );
	j->created = time(NULL);
	snprintf(j->path, sizeof j->path, "%s", path);
	snprintf(j->operation, sizeof j->operation, "%s", operation);
	snprintf(j->action, sizeof j->action, "%s", action);
	snprintf(j->name, sizeof j->name, "%s", name);
	persist_history( );
	return id;
}

static unsigned launch(char *const argv[], const char *operation, const char *action,
                       const char *name) {
	return launch_impl(argv, NULL, NULL, operation, action, name);
}

static void history(int fd) {
	reap( );
	char *out = 0;
	size_t size = 0;
	FILE *f = open_memstream(&out, &size);
	int first = 1;
	fputc('[', f);

	for (unsigned offset = 0; offset < NJOBS && offset < sequence; offset++) {
		unsigned id = sequence - offset;
		struct job *j = &jobs[(id - 1) % NJOBS];

		if (j->id != id)
			continue;
		fprintf(
		    f,
		    "%s{\"id\":%u,\"user\":\"root\",\"created\":%ld,\"finished\":%ld,\"done\":%s,\"exit\":%d,\"unknown\":%s,\"operation\":",
		    first ? "" : ",", id, (long)j->created, (long)j->finished, j->done ? "true" : "false",
		    j->code, j->restored ? "true" : "false");
		json_string(f, j->operation);
		fputs(",\"action\":", f);
		json_string(f, j->action);
		fputs(",\"name\":", f);
		json_string(f, j->name);
		fputc('}', f);
		first = 0;
	}

	fputc(']', f);
	fclose(f);
	respond(fd, 200, "application/json", out, size);
	free(out);
}

static int form(const char *body, const char *key, char *out, size_t cap) {
	out[0] = 0;
	size_t k = strlen(key);

	for (const char *p = body; *p;) {
		const char *end = strchr(p, '&');

		if (!end)
			end = p + strlen(p);
		if ((size_t)(end - p) > k && p[k] == '=' && !strncmp(p, key, k)) {
			size_t n = end - p - k - 1;

			if (n >= cap)
				return 0;
			memcpy(out, p + k + 1, n);
			out[n] =
			    0; /* All values are identifiers; percent encoding is deliberately unnecessary. */
			return 1;
		}

		if (!*end)
			break;
		p = end + 1;
	}

	out[0] = 0;
	return 0;
}

static int protected_package(const char *name) {
	const char *names[] = { "kernel",  "init",  "sh",       "sbase",    "ubase", "setup",
		                    "service", "nginx", "webpanel", "dropbear", 0 };
	for (int i = 0; names[i]; i++)
		if (!strcmp(name, names[i]))
			return 1;
	return 0;
}

static void command(int fd, const char *body) {
	char op[32], action[32], name[64];

	if (!form(body, "operation", op, sizeof op)) {
		message(fd, 400, "Invalid operation\n");
		return;
	}

	form(body, "action", action, sizeof action);
	form(body, "name", name, sizeof name);
	char *argv[7] = { 0 };

	if (!strcmp(op, "services")) {
		argv[0] = WEBPANEL_SERVICE_BIN;
		argv[1] = "list";
	} else if (!strcmp(op, "packages")) {
		argv[0] = WEBPANEL_SETUP_BIN;
		argv[1] = "ls";

		if (!strcmp(action, "installed"))
			argv[2] = "-i";
		else if (!strcmp(action, "catalog"))
			argv[1] = "search";
		else if (*action) {
			message(fd, 400, "Invalid package view\n");
			return;
		}
	} else if (!strcmp(op, "package-info") && safe_identifier(name)) {
		argv[0] = WEBPANEL_SETUP_BIN;
		argv[1] = "info";
		argv[2] = name;
	} else if (!strcmp(op, "package-updates")) {
		argv[0] = WEBPANEL_SETUP_BIN;
		argv[1] = "--dry-run";
		argv[2] = "upgrade";
	} else if (!strcmp(op, "network")) {
		argv[0] = WEBPANEL_IP_BIN;
	} else if (!strcmp(op, "service") && safe_identifier(name) &&
	           (!strcmp(action, "start") || !strcmp(action, "stop") || !strcmp(action, "restart") ||
	            !strcmp(action, "enable") || !strcmp(action, "disable"))) {
		if (!strcmp(name, "webpanel") || !strcmp(name, "nginx")) {
			message(fd, 400, "Manage panel services from SSH\n");
			return;
		}

		argv[0] = WEBPANEL_SERVICE_BIN;
		argv[1] = action;
		argv[2] = name;
	} else if ((!strcmp(op, "package") || !strcmp(op, "package-plan")) &&
	           (!strcmp(action, "update") || !strcmp(action, "install") ||
	            !strcmp(action, "upgrade") || !strcmp(action, "remove"))) {
		if (!strcmp(op, "package-plan") && !strcmp(action, "update")) {
			message(fd, 400, "Catalog update has no transaction plan\n");
			return;
		}

		if (strcmp(action, "update") && !safe_identifier(name)) {
			message(fd, 400, "Package name required\n");
			return;
		}

		if (!strcmp(action, "remove") && protected_package(name)) {
			message(fd, 400, "Protected package\n");
			return;
		}

		argv[0] = WEBPANEL_SETUP_BIN;
		argv[1] = !strcmp(op, "package-plan") ? "--dry-run" : "--yes";
		argv[2] = action;

		if (strcmp(action, "update"))
			argv[3] = name;
	} else {
		message(fd, 400, "Invalid operation\n");
		return;
	}

	unsigned id = launch(argv, op, action, name);

	if (!id) {
		message(fd, 409, "Another task is running\n");
		return;
	}

	char out[64];
	snprintf(out, sizeof out, "{\"job\":%u}", id);
	respond(fd, 200, "application/json", out, strlen(out));
}

static void job_result(int fd, const char *idtext) {
	char *end;
	unsigned long id = strtoul(idtext, &end, 10);

	if (!*idtext || *end) {
		message(fd, 400, "Invalid job\n");
		return;
	}

	reap( );
	struct job *j = 0;

	for (int i = 0; i < NJOBS; i++)
		if (jobs[i].id == id)
			j = &jobs[i];
	if (!j) {
		message(fd, 404, "Job expired\n");
		return;
	}

	char text[LIMIT + 1];
	int in = open(j->path, O_RDONLY | O_NOFOLLOW);
	ssize_t n = 0;

	if (in >= 0) {
		struct stat st;

		if (!fstat(in, &st) && st.st_size > LIMIT)
			lseek(in, st.st_size - LIMIT, SEEK_SET);
		n = read(in, text, LIMIT);
		close(in);
	}

	if (n < 0)
		n = 0;
	text[n] = 0;
	struct service_log_filter filter = { 0 };
	n = service_log_clean(&filter, text, n);
	text[n] = 0;
	char *out = 0;
	size_t size = 0;
	FILE *f = open_memstream(&out, &size);
	fprintf(f, "{\"done\":%s,\"exit\":%d,\"output\":", j->done ? "true" : "false", j->code);
	json_string(f, text);
	fputs("}", f);
	fclose(f);
	respond(fd, 200, "application/json", out, size);
	free(out);
}

static double cpu_load(void) {
	static struct lumine_process_info_v2 previous[4096], current[4096];
	static int old_count;
	static uint64_t old_time;
	long count = syscall(LUMINE_PROCESS_SNAPSHOT_V2, current, 4096, sizeof(*current));

	if (count < 0 || count > 4096)
		return -1;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000, delta = 0;

	for (long i = 0; i < count; i++)
		for (int j = 0; j < old_count; j++)
			if (current[i].base.pid == previous[j].base.pid) {
				if (current[i].cpu_us >= previous[j].cpu_us)
					delta += current[i].cpu_us - previous[j].cpu_us;
				break;
			}

	double load = old_time && now > old_time ? 100.0 * delta / (now - old_time) : 0;
	memcpy(previous, current, count * sizeof(*current));
	old_count = count;
	old_time = now;
	return load;
}

static void system_info(int fd) {
	struct sysinfo si;
	struct utsname un;
	memset(&si, 0, sizeof si);
	memset(&un, 0, sizeof un);
	sysinfo(&si);
	uname(&un);
	char *out = 0;
	size_t size = 0;
	FILE *f = open_memstream(&out, &size);
	fputs("{\"hostname\":", f);
	json_string(f, un.nodename);
	fputs(",\"release\":", f);
	json_string(f, un.release);
	fprintf(f, ",\"cpu_core_percent\":%.1f", cpu_load( ));
	fprintf(f, ",\"uptime\":%ld,\"memory_total\":%llu,\"memory_used\":%llu,\"csrf\":", si.uptime,
	        (unsigned long long)si.totalram * si.mem_unit,
	        (unsigned long long)(si.totalram - si.freeram) * si.mem_unit);
	json_string(f, csrf);
	fputs("}", f);
	fclose(f);
	respond(fd, 200, "application/json", out, size);
	free(out);
}

static void logs(int fd, const char *name) {
	if (!name) {
		DIR *d = opendir(WEBPANEL_LOG_DIR);
		char *out = 0;
		size_t size = 0;
		FILE *f = open_memstream(&out, &size);
		fputc('[', f);
		int first = 1;

		if (d) {
			struct dirent *e;

			while ((e = readdir(d)))
				if (safe_name(e->d_name, 1) && strstr(e->d_name, ".log")) {
					if (!first)
						fputc(',', f);
					first = 0;
					json_string(f, e->d_name);
				}

			closedir(d);
		}

		fputc(']', f);
		fclose(f);
		respond(fd, 200, "application/json", out, size);
		free(out);
		return;
	}

	if (!safe_name(name, 1) || !strstr(name, ".log")) {
		message(fd, 400, "Invalid log\n");
		return;
	}

	char path[512];
	snprintf(path, sizeof path, WEBPANEL_LOG_DIR "/%.63s", name);
	int in = open(path, O_RDONLY | O_NOFOLLOW);
	struct stat st;

	if (in < 0 || fstat(in, &st) || !S_ISREG(st.st_mode)) {
		if (in >= 0)
			close(in);
		message(fd, 404, "Log not found\n");
		return;
	}

	if (st.st_size > LIMIT)
		lseek(in, st.st_size - LIMIT, SEEK_SET);
	char text[LIMIT];
	ssize_t n = read(in, text, sizeof text);
	close(in);

	if (n < 0) {
		message(fd, 500, "Log read failed\n");
		return;
	}

	struct service_log_filter filter = { 0 };
	n = service_log_clean(&filter, text, n);
	respond(fd, 200, "text/plain; charset=utf-8", text, n);
}

static ssize_t read_regular(const char *path, char *text, size_t capacity, int tail) {
	int fd = open(path, O_RDONLY | O_NOFOLLOW);

	if (fd < 0)
		return -1;
	struct stat st;

	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		close(fd);
		return -1;
	}

	if (tail && st.st_size > (off_t)(capacity - 1))
		lseek(fd, st.st_size - (capacity - 1), SEEK_SET);
	ssize_t n = read(fd, text, capacity - 1);
	close(fd);

	if (n >= 0)
		text[n] = 0;
	return n;
}

static void service_info(int fd, const char *name) {
	if (!safe_identifier(name)) {
		message(fd, 400, "Invalid service\n");
		return;
	}

	char path[512], config[4097], status[256], events[LIMIT + 1];
	snprintf(path, sizeof path, WEBPANEL_SERVICE_CONF "/%.63s", name);
	ssize_t config_size = read_regular(path, config, sizeof config, 0);
	snprintf(path, sizeof path, WEBPANEL_SERVICE_RUN "/%.63s/status", name);
	ssize_t status_size = read_regular(path, status, sizeof status, 0);

	if (config_size < 0 && status_size < 0) {
		message(fd, 404, "Service not found\n");
		return;
	}

	long stamp = 0, supervisor = 0, child = 0;
	int wanted = 0, retry = 0;
	char state[32] = "stopped";
	int ok = status_size >= 0 && sscanf(status, "%ld %ld %ld %31s %d %d", &stamp, &supervisor,
	                                    &child, state, &wanted, &retry) == 6;
	long age = ok ? (long)((stamp >= 1000000000L ? time(NULL) : monotime( )) - stamp) : -1;

	if (!ok || age < 0 || age > 30) {
		strcpy(state, "stopped");
		child = supervisor = 0;
		wanted = retry = 0;
	}

	snprintf(path, sizeof path, WEBPANEL_SERVICE_CONF "/%.63s.disabled", name);
	int enabled = access(path, F_OK) != 0;
	long long memory = -1;
	double cpu = -1;

	if (child > 0) {
		static struct lumine_process_info_v2 processes[4096];
		long count = syscall(LUMINE_PROCESS_SNAPSHOT_V2, processes, 4096, sizeof *processes);

		if (count >= 0 && count <= 4096)
			for (long i = 0; i < count; i++)
				if (processes[i].base.pid == child) {
					memory = processes[i].resident_bytes;
					cpu = processes[i].cpu_us / 1000000.0;
					break;
				}
	}

	snprintf(path, sizeof path, WEBPANEL_LOG_DIR "/%.63s.log", name);
	ssize_t n = read_regular(path, events, sizeof events, 1);

	if (n < 0)
		events[0] = 0;
	else {
		struct service_log_filter filter = { 0 };
		n = service_log_clean(&filter, events, n);
		events[n] = 0;
	}
	// Retain supervisor events, including exit codes and requested restarts.
	char *write = events, *line = events;

	while (*line) {
		char *end = strchr(line, '\n');
		size_t len = end ? (size_t)(end - line) + 1 : strlen(line);

		if (!strncmp(line, "[supervisor ", 12)) {
			memmove(write, line, len);
			write += len;
		}

		line += len;
	}

	*write = 0;
	char *out = 0;
	size_t size = 0;
	FILE *f = open_memstream(&out, &size);
	fputs("{\"name\":", f);
	json_string(f, name);
	fputs(",\"state\":", f);
	json_string(f, state);
	fprintf(
	    f,
	    ",\"pid\":%ld,\"supervisor\":%ld,\"desired\":%s,\"autostart\":%s,\"retry\":%d,\"heartbeat_age\":%ld,\"memory_bytes\":%lld,\"cpu_seconds\":%.3f,\"config\":",
	    child, supervisor, wanted ? "true" : "false", enabled ? "true" : "false", retry, age,
	    memory, cpu);
	if (config_size >= 0)
		json_string(f, config);
	else
		fputs("null", f);
	fprintf(f, ",\"config_truncated\":%s,\"events\":", config_size == 4096 ? "true" : "false");
	json_string(f, events);
	fputs("}", f);
	fclose(f);
	respond(fd, 200, "application/json", out, size);
	free(out);
}

#include "management.h"

static void asset(int fd, const char *path) {
	const char *file = !strcmp(path, "/")            ? "index.html"
	                   : !strcmp(path, "/app.js")    ? "app.js"
	                   : !strcmp(path, "/style.css") ? "style.css"
	                                                 : 0;
	if (!file) {
		message(fd, 404, "Not found\n");
		return;
	}

	char location[512];
	snprintf(location, sizeof location, WEBPANEL_ASSETS "/%s", file);
	int in = open(location, O_RDONLY | O_NOFOLLOW);

	if (in < 0) {
		message(fd, 404, "Panel asset missing\n");
		return;
	}

	char data[LIMIT];
	ssize_t n = read(in, data, sizeof data);
	close(in);

	if (n < 0)
		return;
	respond(fd, 200,
	        strstr(file, ".js")    ? "application/javascript"
	        : strstr(file, ".css") ? "text/css"
	                               : "text/html; charset=utf-8",
	        data, n);
}

static void handle(int fd) {
	char request[16385];
	size_t n = 0;
	char *finish = NULL;

	while (n < sizeof request - 1) {
		struct pollfd p = { fd, POLLIN, 0 };

		if (poll(&p, 1, 3000) <= 0)
			return;
		ssize_t got = read(fd, request + n, sizeof request - 1 - n);

		if (got <= 0)
			return;
		n += got;
		request[n] = 0;
		finish = memmem(request, n, "\r\n\r\n", 4);

		if (finish)
			break;
	}

	if (!finish) {
		message(fd, 431, "Headers too large\n");
		return;
	}

	if (memchr(request, 0, finish - request)) {
		message(fd, 400, "Invalid headers\n");
		return;
	}

	char *line = strstr(request, "\r\n");

	if (!line)
		return;
	*line = 0;
	char method[8], path[2048], version[16], extra;

	if (sscanf(request, "%7s %2047s %15s %c", method, path, version, &extra) != 3) {
		message(fd, 400, "Malformed request\n");
		return;
	}

	char *headers = line + 2;
	*finish = 0;
	char transfer[128];

	if (header_value(headers, "Transfer-Encoding", transfer, sizeof transfer)) {
		message(fd, 400, "Transfer encoding unsupported\n");
		return;
	}

	if (strncmp(path, "/api/", 5)) {
		if (strcmp(method, "GET"))
			message(fd, 405, "GET required\n");
		else
			asset(fd, path);
		return;
	}

	if (!authenticated(headers)) {
		message(fd, 401, "Authentication required\n");
		return;
	}

	if (!strcmp(method, "GET")) {
		if (management_get(fd, path))
			return;
		if (!strcmp(path, "/api/system"))
			system_info(fd);
		else if (!strcmp(path, "/api/history"))
			history(fd);
		else if (!strncmp(path, "/api/service?name=", 18))
			service_info(fd, path + 18);
		else if (!strcmp(path, "/api/logs"))
			logs(fd, 0);
		else if (!strncmp(path, "/api/log?name=", 14))
			logs(fd, path + 14);
		else if (!strncmp(path, "/api/job?id=", 12))
			job_result(fd, path + 12);
		else
			message(fd, 404, "Unknown API\n");
		return;
	}

	if (strcmp(method, "POST")) {
		message(fd, 405, "Unsupported request\n");
		return;
	}

	char token[128], length[32];

	if (!header_value(headers, "X-Webpanel-Token", token, sizeof token) || !equal(token, csrf)) {
		message(fd, 403, "Request token required\n");
		return;
	}

	if (!header_value(headers, "Content-Length", length, sizeof length)) {
		message(fd, 411, "Content length required\n");
		return;
	}

	char *end;
	unsigned long size = strtoul(length, &end, 10);
	size_t limit = !strcmp(path, "/api/run") || !strcmp(path, "/api/process") ? 512
	               : !strncmp(path, "/api/config?", 12)                       ? EDIT_LIMIT
	                                                                          : FILE_LIMIT;
	if (!*length || *end || length[0] == '-' || size > limit) {
		message(fd, 413, "Body too large\n");
		return;
	}

	size_t offset = finish + 4 - request, received = n - offset;

	if (received > size) {
		message(fd, 400, "Invalid body\n");
		return;
	}

	unsigned char *body = malloc(size + 1);

	if (!body) {
		message(fd, 500, "Out of memory\n");
		return;
	}

	memcpy(body, request + offset, received);
	time_t deadline = monotime( ) + 30;

	while (received < size) {
		struct pollfd p = { fd, POLLIN, 0 };

		if (monotime( ) > deadline || poll(&p, 1, 3000) <= 0) {
			free(body);
			return;
		}

		ssize_t got = read(fd, body + received, size - received);

		if (got <= 0) {
			free(body);
			return;
		}

		received += got;
	}

	body[size] = 0;

	if (!strcmp(path, "/api/run")) {
		if (memchr(body, 0, size))
			message(fd, 400, "Invalid body\n");
		else
			command(fd, (char *)body);
	} else if (!management_post(fd, path, headers, body, size))
		message(fd, 404, "Unknown API\n");
	free(body);
}

int main(int argc, char **argv) {
	int port = 8088;

	if (argc == 2 && !strcmp(argv[1], "--help")) {
		puts(
		    "Usage: webpanel [--port PORT]\nLoopback backend. Use nginx HTTPS. Login uses the root password.");
		return 0;
	}

	if (argc == 3 && !strcmp(argv[1], "--port")) {
		char *end;
		long p = strtol(argv[2], &end, 10);

		if (*end || p < 1 || p > 65535)
			return 2;
		port = p;
	} else if (argc != 1)
		return 2;
	umask(077);
	mkdir(WEBPANEL_STATE_DIR, 0700);
	restore_history( );
	unsigned char entropy[32];
	int random = open("/dev/urandom", O_RDONLY);

	if (random < 0 || read(random, entropy, sizeof entropy) != sizeof entropy) {
		if (random >= 0)
			close(random);
		return 1;
	}

	close(random);

	for (int i = 0; i < 32; i++)
		snprintf(csrf + i * 2, 3, "%02x", entropy[i]);
	listener = socket(AF_INET, SOCK_STREAM, 0);

	if (listener < 0)
		return 1;
	int yes = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
	struct sockaddr_in a = { 0 };
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(listener, (void *)&a, sizeof a) || listen(listener, 8)) {
		perror("webpanel listen");
		return 1;
	}

	service_log_start("webpanel", 1);
	signal(SIGPIPE, SIG_IGN);

	for (;;) {
		reap( );
		struct pollfd p = { listener, POLLIN, 0 };
		int ready = poll(&p, 1, 500);

		if (ready < 0 && errno != EINTR)
			break;
		if (ready <= 0)
			continue;
		int client = accept(listener, 0, 0);

		if (client < 0)
			continue;
		handle(client);
		close(client);
	}

	close(listener);
	return 0;
}
