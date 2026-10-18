// qemu-ga.c: Гостевой агент QEMU
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: Гостевой агент QEMU в пользовательском пространстве
// Создан: 11.10.2026
// Последнее изменение: 14.10.2026

#include "json.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "../common/service_log.h"
#define VERSION "synapse-qga-0.1"
#define FRAME_MAX 65536
#define RESPONSE_MAX 262144
#define IO_MAX 32768
#define FILE_MAX 16
#define EXEC_MAX 8
#define ARG_MAX_COUNT 64
#define LOG_PATH "/var/log/qga.log"
#define LOG_MAX_SIZE (10 * 1024 * 1024)
#define LOG_KEEP 3

static char frame[FRAME_MAX + 1], response[RESPONSE_MAX];
static size_t response_len;
static int response_overflow, transport_in, transport_out;
static const char *error_class, *error_desc;
static char error_buffer[256];
static struct json request;

static struct {
	FILE *stream;
	int64_t id;
} files[FILE_MAX];

static int64_t next_handle = 1;

struct exec_process {
	pid_t pid;
	int status, exited, capture, in, out, err;
	unsigned char input[IO_MAX], output[2][IO_MAX];
	size_t input_len, input_pos, output_len[2];
	int truncated[2];
};
static struct exec_process processes[EXEC_MAX];

static void log_msg(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[qemu-ga] ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
}

static void append(const char *fmt, ...) {
	if (response_overflow) {
		return;
	}

	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(response + response_len, RESPONSE_MAX - response_len, fmt, args);
	va_end(args);

	if (n < 0 || (size_t)n >= RESPONSE_MAX - response_len) {
		response_overflow = 1;
		return;
	}

	response_len += n;
}

static void quote(const char *s) {
	append("\"");

	for (const unsigned char *p = (const void *)s; *p; ++p) {
		if (*p == '"' || *p == '\\') {
			append("\\%c", *p);
		} else if (*p < 32) {
			append("\\u%04x", *p);
		} else {
			append("%c", *p);
		}
	}

	append("\"");
}

static void fail(const char *desc) {
	error_class = "GenericError";
	error_desc = desc;
}

static void fail_errno(const char *operation) {
	snprintf(error_buffer, sizeof(error_buffer), "%s: %s", operation, strerror(errno));
	fail(error_buffer);
}

static int integer(int args, const char *name, int64_t *out) {
	if (json_integer(&request, json_get(&request, args, name), out)) {
		fail("Missing or invalid integer argument");
		return -1;
	}

	return 0;
}

static int string(int args, const char *name, char *out, size_t cap) {
	if (json_string(&request, json_get(&request, args, name), out, cap) < 0) {
		fail("Missing, invalid or oversized string argument");
		return -1;
	}

	return 0;
}

static int count_arg(int args, int64_t default_count, int64_t max, size_t *out) {
	int64_t n = default_count;
	int token = json_get(&request, args, "count");

	if ((token >= 0 && json_integer(&request, token, &n)) || n < 0 || n > max) {
		fail("Invalid count or transfer exceeds 32768 bytes");
		return -1;
	}

	*out = (size_t)n;
	return 0;
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(const unsigned char *data, size_t len) {
	append("\"");

	for (size_t i = 0; i < len; i += 3) {
		unsigned n = (unsigned)data[i] << 16;

		if (i + 1 < len) {
			n |= (unsigned)data[i + 1] << 8;
		}

		if (i + 2 < len) {
			n |= data[i + 2];
		}

		append("%c%c%c%c", b64[n >> 18], b64[(n >> 12) & 63],
		       i + 1 < len ? b64[(n >> 6) & 63] : '=', i + 2 < len ? b64[n & 63] : '=');
	}

	append("\"");
}

static int decode64(const char *s, unsigned char *out, size_t cap, size_t *len) {
	size_t n = strlen(s), used = 0;

	if (n % 4) {
		return -1;
	}

	for (size_t i = 0; i < n; i += 4) {
		unsigned v[4];

		for (int k = 0; k < 4; ++k) {
			const char *p = s[i + k] ? strchr(b64, s[i + k]) : NULL;

			if (p) {
				v[k] = p - b64;
			} else if (s[i + k] == '=' && k >= 2 && i + 4 == n) {
				v[k] = 0;
			} else {
				return -1;
			}
		}

		int padding = s[i + 3] == '=' ? 1 : 0;

		if (s[i + 2] == '=') {
			if (!padding || (v[1] & 15)) {
				return -1;
			}

			padding = 2;
		} else if (padding && (v[2] & 3)) {
			return -1;
		}

		if (used + 3 - padding > cap) {
			return -1;
		}

		unsigned bits = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
		out[used++] = bits >> 16;

		if (padding < 2) {
			out[used++] = bits >> 8;
		}

		if (!padding) {
			out[used++] = bits;
		}
	}

	*len = used;
	return 0;
}

static int decoded_arg(int args, const char *name, unsigned char *out, size_t *len) {
	static char encoded[IO_MAX * 4 / 3 + 8];

	if (string(args, name, encoded, sizeof(encoded))) {
		return -1;
	}

	if (decode64(encoded, out, IO_MAX, len)) {
		fail("Invalid base64 data or transfer too large");
		return -1;
	}

	return 0;
}

static const char *const commands[] = {
	"guest-sync",          "guest-sync-delimited", "guest-ping",        "guest-info",
	"guest-get-host-name", "guest-get-osinfo",     "guest-get-time",    "guest-file-open",
	"guest-file-close",    "guest-file-read",      "guest-file-write",  "guest-file-seek",
	"guest-file-flush",    "guest-exec",           "guest-exec-status", "guest-shutdown"
};

static void info(void) {
	append("{\"version\":\"%s\",\"supported_commands\":[", VERSION);

	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
		if (i) {
			append(",");
		}

		append("{\"name\":\"%s\",\"enabled\":true,\"success-response\":%s}", commands[i],
		       !strcmp(commands[i], "guest-shutdown") ? "false" : "true");
	}

	append("]}");
}

static FILE *file_arg(int args, int *slot) {
	int64_t handle;

	if (integer(args, "handle", &handle)) {
		return NULL;
	}

	for (int i = 0; i < FILE_MAX; ++i) {
		if (files[i].stream && files[i].id == handle) {
			*slot = i;
			return files[i].stream;
		}
	}

	fail("Unknown file handle");
	return NULL;
}

static void file_open(int args) {
	char path[4096], mode[8] = "r";

	if (string(args, "path", path, sizeof(path))) {
		return;
	}

	if (path[0] != '/') {
		fail("File path must be absolute");
		return;
	}

	if (json_get(&request, args, "mode") >= 0 && string(args, "mode", mode, sizeof(mode))) {
		return;
	}

	const char *const modes[] = { "r",   "rb",  "r+", "r+b", "rb+", "w",   "wb", "w+",
		                          "w+b", "wb+", "a",  "ab",  "a+",  "a+b", "ab+" };
	int valid = 0, slot = -1;

	for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
		if (!strcmp(mode, modes[i])) {
			valid = 1;
		}
	}

	if (!valid) {
		fail("Invalid file mode");
		return;
	}

	for (int i = 0; i < FILE_MAX; ++i) {
		if (!files[i].stream) {
			slot = i;
			break;
		}
	}

	if (slot < 0 || next_handle == INT64_MAX) {
		fail("Too many open files");
		return;
	}

	struct stat st;

	if (!stat(path, &st) && !S_ISREG(st.st_mode)) {
		fail("Only regular files are supported");
		return;
	}

	int flags = strchr(mode, '+') ? O_RDWR : mode[0] == 'r' ? O_RDONLY : O_WRONLY;

	if (mode[0] != 'r') {
		flags |= O_CREAT;
	}

	if (mode[0] == 'a') {
		flags |= O_APPEND;
	}

	int fd = open(path, flags | O_NONBLOCK | O_CLOEXEC, 0600);

	if (fd < 0) {
		fail_errno("open");
		return;
	}

	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		close(fd);
		fail("Only regular files are supported");
		return;
	}

	if (mode[0] == 'w' && ftruncate(fd, 0)) {
		close(fd);
		fail_errno("truncate");
		return;
	}

	FILE *stream = fdopen(fd, mode);

	if (!stream) {
		close(fd);
		fail_errno("fdopen");
		return;
	}

	files[slot].stream = stream;
	files[slot].id = next_handle++;
	append("%" PRId64, files[slot].id);
}

static void file_command(const char *cmd, int args) {
	int slot;
	FILE *f = file_arg(args, &slot);

	if (!f) {
		return;
	}

	if (!strcmp(cmd, "guest-file-close")) {
		files[slot].stream = NULL;

		if (fclose(f)) {
			fail_errno("close");
		} else {
			append("{}");
		}
	} else if (!strcmp(cmd, "guest-file-flush")) {
		if (fflush(f)) {
			fail_errno("flush");
		} else {
			append("{}");
		}
	} else if (!strcmp(cmd, "guest-file-read")) {
		size_t count;
		unsigned char data[IO_MAX];

		if (count_arg(args, 4096, IO_MAX, &count)) {
			return;
		}

		size_t n = fread(data, 1, count, f);

		if (ferror(f)) {
			clearerr(f);
			fail_errno("read");
			return;
		}

		append("{\"count\":%zu,\"eof\":%s,\"buf-b64\":", n, feof(f) ? "true" : "false");
		base64(data, n);
		append("}");
	} else if (!strcmp(cmd, "guest-file-write")) {
		size_t len, count;
		unsigned char data[IO_MAX];

		if (decoded_arg(args, "buf-b64", data, &len) || count_arg(args, len, len, &count)) {
			return;
		}

		size_t n = fwrite(data, 1, count, f);

		if (ferror(f)) {
			clearerr(f);
			fail_errno("write");
			return;
		}

		append("{\"count\":%zu,\"eof\":false}", n);
	} else if (!strcmp(cmd, "guest-file-seek")) {
		int64_t offset, whence;
		int token = json_get(&request, args, "whence");
		char name[8];

		if (integer(args, "offset", &offset)) {
			return;
		}

		if (json_integer(&request, token, &whence)) {
			if (json_string(&request, token, name, sizeof(name)) < 0) {
				fail("Invalid whence");
				return;
			}

			whence = !strcmp(name, "set")   ? 0
			         : !strcmp(name, "cur") ? 1
			         : !strcmp(name, "end") ? 2
			                                : -1;
		}

		if (whence < 0 || whence > 2) {
			fail("Invalid whence");
			return;
		}

		const int origin[] = { SEEK_SET, SEEK_CUR, SEEK_END };

		if (fseeko(f, offset, origin[whence])) {
			fail_errno("seek");
			return;
		}

		off_t position = ftello(f);

		if (position < 0) {
			fail_errno("tell");
			return;
		}

		append("{\"position\":%" PRId64 ",\"eof\":%s}", (int64_t)position,
		       feof(f) ? "true" : "false");
	}
}

static void close_fd(int *fd) {
	if (*fd >= 0) {
		close(*fd);
	}

	*fd = -1;
}

static int pipe_cloexec(int fds[2]) {
	if (pipe(fds)) {
		return -1;
	}

	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	return 0;
}

static void pump_exec(void) {
	for (int i = 0; i < EXEC_MAX; ++i) {
		struct exec_process *p = &processes[i];

		if (!p->pid) {
			continue;
		}

		if (p->in >= 0) {
			ssize_t n = write(p->in, p->input + p->input_pos, p->input_len - p->input_pos);

			if (n > 0) {
				p->input_pos += n;
			}

			if (p->input_pos == p->input_len || (n < 0 && errno != EAGAIN && errno != EINTR)) {
				close_fd(&p->in);
			}
		}

		for (int s = 0; s < 2; ++s) {
			int *fd = s ? &p->err : &p->out;

			for (int rounds = 0; *fd >= 0 && rounds < 8; ++rounds) {
				unsigned char buf[4096];
				ssize_t n = read(*fd, buf, sizeof(buf));

				if (n > 0) {
					size_t take = n;

					if (take > IO_MAX - p->output_len[s]) {
						take = IO_MAX - p->output_len[s];
						p->truncated[s] = 1;
					}

					memcpy(p->output[s] + p->output_len[s], buf, take);
					p->output_len[s] += take;
				} else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
					close_fd(fd);
				} else {
					break;
				}
			}
		}

		if (!p->exited && waitpid(p->pid, &p->status, WNOHANG) == p->pid) {
			p->exited = 1;
			close_fd(&p->in);
		}
	}
}

static int array_strings(int args, const char *name, char **out, size_t first) {
	int token = json_get(&request, args, name);

	if (token < 0) {
		return 0;
	}

	if (request.t[token].type != J_ARRAY ||
	    request.t[token].children >= ARG_MAX_COUNT - (int)first) {
		fail("Invalid argument/environment array");
		return -1;
	}

	size_t count = first;

	for (int i = token + 1; i < request.t[token].next; i = request.t[i].next) {
		size_t cap = request.t[i].end - request.t[i].start + 1;
		out[count] = malloc(cap);

		if (!out[count]) {
			fail("Out of memory");
			return -1;
		}

		if (json_string(&request, i, out[count], cap) < 0) {
			fail("Invalid array string");
			return -1;
		}

		count++;
	}

	return 0;
}

static void exec_start(int args) {
	char path[4096], capture_mode[16];
	char *argv[ARG_MAX_COUNT] = { NULL }, *env[ARG_MAX_COUNT] = { NULL };
	int pipes[3][2] = { { -1, -1 }, { -1, -1 }, { -1, -1 } };
	int capture = 0, token = json_get(&request, args, "capture-output");

	if (string(args, "path", path, sizeof(path)) || !*path) {
		fail("Missing executable path");
		return;
	}

	if (token >= 0) {
		int flag;

		if (!json_boolean(&request, token, &flag)) {
			capture = flag ? 3 : 0;
		} else if (json_string(&request, token, capture_mode, sizeof(capture_mode)) >= 0) {
			capture = !strcmp(capture_mode, "none")        ? 0
			          : !strcmp(capture_mode, "stdout")    ? 1
			          : !strcmp(capture_mode, "stderr")    ? 2
			          : !strcmp(capture_mode, "separated") ? 3
			          : !strcmp(capture_mode, "merged")    ? 4
			                                               : -1;
		} else {
			capture = -1;
		}

		if (capture < 0) {
			fail("Invalid capture-output mode");
			return;
		}
	}

	struct exec_process *p = NULL;

	for (int i = 0; i < EXEC_MAX; ++i) {
		if (!processes[i].pid) {
			p = &processes[i];
			break;
		}
	}

	if (!p) {
		fail("Too many pending processes; collect guest-exec-status");
		return;
	}

	memset(p, 0, sizeof(*p));
	p->in = p->out = p->err = -1;
	p->capture = capture;

	if (json_get(&request, args, "input-data") >= 0 &&
	    decoded_arg(args, "input-data", p->input, &p->input_len)) {
		return;
	}

	argv[0] = path;

	if (array_strings(args, "arg", argv, 1) || array_strings(args, "env", env, 0)) {
		goto done;
	}

	for (int i = 0; i < 3; ++i) {
		if (pipe_cloexec(pipes[i])) {
			fail_errno("pipe");
			goto done;
		}
	}

	pid_t pid = fork( );

	if (pid < 0) {
		fail_errno("fork");
		goto done;
	}

	if (pid == 0) {
		dup2(pipes[0][0], 0);
		dup2(pipes[1][1], 1);
		dup2(capture == 4 ? pipes[1][1] : pipes[2][1], 2);

		for (int i = 0; i < 3; ++i) {
			close(pipes[i][0]);
			close(pipes[i][1]);
		}

		if (json_get(&request, args, "env") >= 0) {
			extern char **environ;
			environ = env;
		}

		execvp(path, argv);
		dprintf(2, "exec %s: %s\n", path, strerror(errno));
		_exit(127);
	}

	p->pid = pid;
	p->in = pipes[0][1];
	pipes[0][1] = -1;
	p->out = pipes[1][0];
	pipes[1][0] = -1;
	p->err = pipes[2][0];
	pipes[2][0] = -1;
	fcntl(p->in, F_SETFL, O_NONBLOCK);
	fcntl(p->out, F_SETFL, O_NONBLOCK);
	fcntl(p->err, F_SETFL, O_NONBLOCK);
	append("{\"pid\":%d}", pid);
done:
	for (int i = 0; i < 3; ++i) {
		close_fd(&pipes[i][0]);
		close_fd(&pipes[i][1]);
	}

	for (int i = 1; i < ARG_MAX_COUNT; ++i) {
		free(argv[i]);
	}

	for (int i = 0; i < ARG_MAX_COUNT; ++i) {
		free(env[i]);
	}
}

static void exec_status(int args) {
	int64_t pid;

	if (integer(args, "pid", &pid)) {
		return;
	}

	pump_exec( );
	struct exec_process *p = NULL;

	for (int i = 0; i < EXEC_MAX; ++i) {
		if (processes[i].pid && processes[i].pid == pid) {
			p = &processes[i];
		}
	}

	if (!p) {
		fail("Unknown process ID");
		return;
	}

	int exited = p->exited && p->out < 0 && p->err < 0;
	append("{\"exited\":%s", exited ? "true" : "false");

	if (exited) {
		if (WIFEXITED(p->status)) {
			append(",\"exitcode\":%d", WEXITSTATUS(p->status));
		} else if (WIFSIGNALED(p->status)) {
			append(",\"signal\":%d", WTERMSIG(p->status));
		}

		for (int s = 0; s < 2; ++s) {
			if (!(p->capture & (1 << s)) && !(p->capture == 4 && !s)) {
				continue;
			}

			append(",\"%s-data\":", s ? "err" : "out");
			base64(p->output[s], p->output_len[s]);
			append(",\"%s-truncated\":%s", s ? "err" : "out", p->truncated[s] ? "true" : "false");
		}

		p->pid = 0;
	}

	append("}");
}

static int dispatch(const char *command, int args) {
	if (!strcmp(command, "guest-ping")) {
		append("{}");
	} else if (!strcmp(command, "guest-sync") || !strcmp(command, "guest-sync-delimited")) {
		int64_t id;

		if (!integer(args, "id", &id)) {
			append("%" PRId64, id);
		}
	} else if (!strcmp(command, "guest-info")) {
		info( );
	} else if (!strcmp(command, "guest-get-time")) {
		struct timespec now;

		if (clock_gettime(CLOCK_REALTIME, &now)) {
			fail_errno("clock_gettime");
		} else {
			append("%" PRId64, (int64_t)now.tv_sec * 1000000000 + now.tv_nsec);
		}
	} else if (!strcmp(command, "guest-get-host-name") || !strcmp(command, "guest-get-osinfo")) {
		struct utsname u;

		if (uname(&u)) {
			fail_errno("uname");
			return 0;
		}

		if (!strcmp(command, "guest-get-host-name")) {
			append("{\"host-name\":");
			quote(u.nodename);
		} else {
			append("{\"id\":\"synapseos\",\"name\":");
			quote(u.sysname);
			append(",\"kernel-release\":");
			quote(u.release);
			append(",\"kernel-version\":");
			quote(u.version);
			append(",\"machine\":");
			quote(u.machine);
		}

		append("}");
	} else if (!strcmp(command, "guest-file-open")) {
		file_open(args);
	} else if (!strcmp(command, "guest-file-close") || !strcmp(command, "guest-file-read") ||
	           !strcmp(command, "guest-file-write") || !strcmp(command, "guest-file-seek") ||
	           !strcmp(command, "guest-file-flush")) {
		file_command(command, args);
	} else if (!strcmp(command, "guest-exec")) {
		exec_start(args);
	} else if (!strcmp(command, "guest-exec-status")) {
		exec_status(args);
	} else if (!strcmp(command, "guest-shutdown")) {
		char mode[16] = "powerdown";

		if (json_get(&request, args, "mode") >= 0 && string(args, "mode", mode, sizeof(mode))) {
			return 0;
		}

		if (strcmp(mode, "powerdown")) {
			fail("Only powerdown is supported");
			return 0;
		}

		sync( );

		if (reboot(RB_POWER_OFF)) {
			fail_errno("powerdown");
		} else {
			return 1;
		}
	} else {
		error_class = "CommandNotFound";
		error_desc = "Command is not supported by SynapseOS";
	}

	return 0;
}

static void send_response(void) {
	size_t sent = 0;

	for (int attempts = 0; sent < response_len && attempts < 5000; ++attempts) {
		ssize_t n = write(transport_out, response + sent, response_len - sent);

		if (n > 0) {
			sent += n;
		} else if (n < 0 && errno != EAGAIN && errno != EINTR) {
			break;
		} else {
			pump_exec( );
			usleep(1000);
		}
	}
}

static void handle_frame(size_t len) {
	error_class = error_desc = NULL;
	response_len = 0;
	response_overflow = 0;
	char command[128];
	int id = -1, args = -1, delimited = 0;

	if (json_parse(&request, frame, len) || request.t[0].type != J_OBJECT) {
		fail("Invalid JSON request");
	} else {
		id = json_get(&request, 0, "id");
		args = json_get(&request, 0, "arguments");

		if (json_string(&request, json_get(&request, 0, "execute"), command, sizeof(command)) < 0) {
			fail("Missing or invalid execute field");
		} else if (args >= 0 && request.t[args].type != J_OBJECT) {
			fail("arguments must be an object");
		} else {
			delimited = !strcmp(command, "guest-sync-delimited");

			if (delimited) {
				append("%c", 0xff);
			}

			append("{\"return\":");

			if (dispatch(command, args)) {
				return;
			}
		}
	}

	if (response_overflow) {
		fail("Response exceeds size limit");
	}

	if (error_class) {
		response_len = 0;
		response_overflow = 0;

		if (delimited) {
			append("%c", 0xff);
		}

		append("{\"error\":{\"class\":");
		quote(error_class);
		append(",\"desc\":");
		quote(error_desc);
		append("}");
	}

	if (id >= 0) {
		append(",\"id\":%.*s", (int)(request.t[id].end - request.t[id].start),
		       request.text + request.t[id].start);
	}

	append("}\n");
	send_response( );
}

int main(int argc, char **argv) {
	const char *path = "/dev/virtio-ports/org.qemu.guest_agent.0", *method = "virtio-serial";
	int stdio_mode = 0;

	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--stdio")) {
			stdio_mode = 1;
		} else if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--path")) && i + 1 < argc) {
			path = argv[++i];
		} else if ((!strcmp(argv[i], "-m") || !strcmp(argv[i], "--method")) && i + 1 < argc) {
			method = argv[++i];
		} else if (!strcmp(argv[i], "-V") || !strcmp(argv[i], "--version")) {
			puts(VERSION);
			return 0;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			puts("qemu-ga [-m virtio-serial|isa-serial] [-p DEVICE] [--stdio] [-V]");
			return 0;
		} else {
			fprintf(stderr, "qemu-ga: invalid option %s\n", argv[i]);
			return 1;
		}
	}

	if (strcmp(method, "virtio-serial") && strcmp(method, "isa-serial")) {
		fprintf(stderr, "qemu-ga: unsupported transport %s\n", method);
		return 1;
	}

	service_log_start("qga", 0);
	signal(SIGPIPE, SIG_IGN);
	log_msg("Starting %s", VERSION);

	if (stdio_mode) {
		transport_in = 0;
		transport_out = 1;
	} else {
		log_msg("Waiting for %s", path);

		while ((transport_in = open(path, O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC)) < 0) {
			if (errno != ENOENT && errno != ENODEV) {
				log_msg("Open transport failed: %s", strerror(errno));
				return 1;
			}

			sleep(1);
		}

		transport_out = transport_in;

		if (!strcmp(method, "isa-serial")) {
			struct termios t;

			if (tcgetattr(transport_in, &t)) {
				log_msg("Termios failed: %s", strerror(errno));
				return 1;
			}

			cfmakeraw(&t);
			t.c_cflag |= CLOCAL | CREAD;

			if (tcsetattr(transport_in, TCSANOW, &t)) {
				log_msg("Raw mode failed: %s", strerror(errno));
				return 1;
			}
		}
	}

	fcntl(transport_in, F_SETFL, O_NONBLOCK);
	log_msg("%s ready", VERSION);
	size_t len = 0;
	int depth = 0, in_string = 0, escape = 0, discard = 0;

	for (;;) {
		pump_exec( );
		unsigned char input[4096];
		ssize_t n = read(transport_in, input, sizeof(input));

		if (n == 0 && stdio_mode) {
			break;
		}

		if (n <= 0) {
			if (n < 0 && errno != EAGAIN && errno != EINTR) {
				log_msg("Read failed: %s", strerror(errno));
				return 1;
			}

			int running = 0;

			for (int j = 0; j < EXEC_MAX; ++j) {
				if (processes[j].pid && !processes[j].exited) {
					running = 1;
					break;
				}
			}

			usleep(running ? 10000 : 100000);
			continue;
		}

		for (ssize_t i = 0; i < n; ++i) {
			unsigned char c = input[i];

			if (c == 0xff) {
				len = 0;
				depth = in_string = escape = discard = 0;
				continue;
			}

			if (discard) {
				if (c == '\n') {
					discard = 0;
					len = 0;
					depth = in_string = escape = 0;
				}

				continue;
			}

			if (!len && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
				continue;
			}

			if (!len && c != '{') {
				frame[0] = c;
				handle_frame(1);
				discard = 1;
				continue;
			}

			if (len == FRAME_MAX) {
				frame[0] = '!';
				handle_frame(1);
				discard = 1;
				len = 0;
				continue;
			}

			frame[len++] = c;

			if (in_string) {
				if (escape) {
					escape = 0;
				} else if (c == '\\') {
					escape = 1;
				} else if (c == '"') {
					in_string = 0;
				}
			} else {
				if (c == '"') {
					in_string = 1;
				} else if (c == '{' || c == '[') {
					depth++;
				} else if (c == '}' || c == ']') {
					depth--;
				}
			}

			if (!depth && !in_string) {
				handle_frame(len);
				len = 0;
				escape = 0;
			}
		}
	}

	log_msg("Shutting down");
	return 0;
}
