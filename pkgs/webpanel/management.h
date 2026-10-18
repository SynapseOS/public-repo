/* Native management endpoints. All file paths are resolved through directory FDs. */
#include <arpa/inet.h>
#include <sys/statvfs.h>
#include <limits.h>
#ifndef WEBPANEL_FILE_ROOT
#define WEBPANEL_FILE_ROOT ""
#endif
#ifndef WEBPANEL_NGINX_BIN
#define WEBPANEL_NGINX_BIN "/bin/nginx"
#endif
#define FILE_LIMIT (1024 * 1024)
#define EDIT_LIMIT 65536
#define PATH_LIMIT 256

static int url_decode(const char *value, char *out, size_t cap, int plus) {
	size_t n = 0;

	for (; *value; value++) {
		unsigned char c = *value;

		if (c == '%' && value[1] && value[2]) {
			int a = isdigit((unsigned char)value[1]) ? value[1] - '0'
			                                         : tolower((unsigned char)value[1]) - 'a' + 10;
			int b = isdigit((unsigned char)value[2]) ? value[2] - '0'
			                                         : tolower((unsigned char)value[2]) - 'a' + 10;
			if (a < 0 || a > 15 || b < 0 || b > 15)
				return 0;
			c = (a << 4) | b;
			value += 2;
		} else if (c == '%')
			return 0;
		else if (c == '+' && plus)
			c = ' ';
		if (c < 32 || c == 127 || n + 1 >= cap)
			return 0;
		out[n++] = c;
	}

	out[n] = 0;
	return 1;
}

static int decoded_form(const char *body, const char *key, char *out, size_t cap) {
	char encoded[1024];
	return form(body, key, encoded, sizeof encoded) && url_decode(encoded, out, cap, 1);
}

static int canonical_path(const char *path) {
	size_t n = strlen(path);

	if (!n || n >= PATH_LIMIT || path[0] != '/')
		return 0;
	if (n == 1)
		return 1;
	if (path[n - 1] == '/')
		return 0;
	const char *p = path + 1;

	while (*p) {
		const char *end = strchr(p, '/');
		size_t len = end ? (size_t)(end - p) : strlen(p);

		if (!len || (len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.'))
			return 0;
		for (size_t i = 0; i < len; i++)
			if ((unsigned char)p[i] < 32 || p[i] == 127 || p[i] == '\\')
				return 0;
		if (!end)
			break;
		p = end + 1;
	}

	return 1;
}

static int below(const char *path, const char *root) {
	size_t n = strlen(root);
	return !strncmp(path, root, n) && (!path[n] || path[n] == '/');
}

static int readable_path(const char *path) {
	return canonical_path(path) &&
	       (!strcmp(path, "/") || below(path, "/etc") || below(path, "/var") ||
	        below(path, "/home") || below(path, "/root") || below(path, "/tmp") ||
	        below(path, "/boot"));
}

static int upload_path(const char *path) {
	return canonical_path(path) && (below(path, "/home") || below(path, "/root") ||
	                                below(path, "/var/www") || below(path, "/tmp"));
}

/* Returns the containing directory and basename, refusing symlinks in every component. */
static int parent_fd(const char *path, char *name, size_t cap) {
	if (!readable_path(path) || !strcmp(path, "/")) {
		errno = EINVAL;
		return -1;
	}

	char copy[PATH_LIMIT];
	snprintf(copy, sizeof copy, "%s", path + 1);
	int fd = open(WEBPANEL_FILE_ROOT "/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);

	if (fd < 0)
		return -1;
	char *p = copy, *end;

	while ((end = strchr(p, '/'))) {
		*end = 0;
		int next = openat(fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		close(fd);

		if (next < 0)
			return -1;
		fd = next;
		p = end + 1;
	}

	if (strlen(p) >= cap) {
		close(fd);
		errno = ENAMETOOLONG;
		return -1;
	}

	snprintf(name, cap, "%s", p);
	return fd;
}

static int path_fd(const char *path, int flags) {
	if (!strcmp(path, "/"))
		return open(WEBPANEL_FILE_ROOT "/", flags | O_NOFOLLOW);
	char name[PATH_LIMIT];
	int dir = parent_fd(path, name, sizeof name);

	if (dir < 0)
		return -1;
	int fd = openat(dir, name, flags | O_NOFOLLOW | O_NONBLOCK);
	close(dir);
	return fd;
}

static int text_bytes(const unsigned char *s, size_t n) {
	for (size_t i = 0; i < n; i++) {
		unsigned c = s[i];

		if (c < 32 && c != '\n' && c != '\r' && c != '\t')
			return 0;
		if (c == 127)
			return 0;
		if (c < 128)
			continue;
		unsigned count, min, code;

		if (c >= 0xc2 && c <= 0xdf) {
			count = 1;
			min = 0x80;
			code = c & 31;
		} else if (c >= 0xe0 && c <= 0xef) {
			count = 2;
			min = 0x800;
			code = c & 15;
		} else if (c >= 0xf0 && c <= 0xf4) {
			count = 3;
			min = 0x10000;
			code = c & 7;
		} else
			return 0;
		if (i + count >= n)
			return 0;
		while (count--) {
			c = s[++i];

			if ((c & 0xc0) != 0x80)
				return 0;
			code = (code << 6) | (c & 63);
		}

		if (code < min || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
			return 0;
	}

	return 1;
}

static void revision_of(const unsigned char *data, size_t n, char out[17]) {
	uint64_t hash = 14695981039346656037ULL;

	for (size_t i = 0; i < n; i++)
		hash = (hash ^ data[i]) * 1099511628211ULL;
	snprintf(out, 17, "%016llx", (unsigned long long)hash);
}

static unsigned char *read_fd(int fd, size_t limit, size_t *size) {
	struct stat st;

	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > (off_t)limit)
		return NULL;
	unsigned char *data = malloc((size_t)st.st_size + 1);

	if (!data)
		return NULL;
	size_t n = 0;

	while (n < (size_t)st.st_size) {
		ssize_t got = read(fd, data + n, st.st_size - n);

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0) {
			free(data);
			return NULL;
		}

		n += got;
	}

	data[n] = 0;
	*size = n;
	return data;
}

static int editable_path(const char *path) {
	if (!strcmp(path, "/etc/hosts") || !strcmp(path, "/etc/resolv.conf") ||
	    !strcmp(path, "/etc/nginx/nginx.conf"))
		return 1;
	return below(path, "/etc/services") && safe_identifier(path + 14) &&
	       strcmp(path + 14, "webpanel") && strcmp(path + 14, "nginx");
}

static int tasks_running(void) {
	reap( );

	for (int i = 0; i < NJOBS; i++)
		if (jobs[i].id && !jobs[i].done)
			return 1;
	return 0;
}

static unsigned audit_action(const char *op, const char *action, const char *name,
                             const char *output, int code) {
	struct job *j = &jobs[sequence % NJOBS];

	if (j->id)
		clear_job(j);
	memset(j, 0, sizeof *j);
	j->id = ++sequence;
	j->created = j->finished = time(NULL);
	j->done = 1;
	j->code = code;
	snprintf(j->operation, sizeof j->operation, "%s", op);
	snprintf(j->action, sizeof j->action, "%s", action);
	snprintf(j->name, sizeof j->name, "%s", name);
	snprintf(j->path, sizeof j->path, WEBPANEL_STATE_DIR "/job-%u.log", j->id);
	int fd = open(j->path, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);

	if (fd >= 0) {
		log_write_all(fd, output, strlen(output));
		fsync(fd);
		close(fd);
	}

	persist_history( );
	return j->id;
}

static void file_list(int fd, const char *path) {
	if (!readable_path(path)) {
		message(fd, 400, "Недопустимый путь.\n");
		return;
	}

	int in = path_fd(path, O_RDONLY | O_DIRECTORY);

	if (in < 0) {
		message(fd, 404, "Каталог недоступен; переход по символическим ссылкам не разрешён.\n");
		return;
	}

	DIR *d = fdopendir(in);

	if (!d) {
		close(in);
		message(fd, 500, "Не удалось прочитать каталог.\n");
		return;
	}

	char *out = 0;
	size_t size = 0;
	FILE *f = open_memstream(&out, &size);
	fputs("{\"path\":", f);
	json_string(f, path);
	fputs(",\"entries\":[", f);
	struct dirent *e;
	int count = 0, truncated = 0;

	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (count == 512) {
			truncated = 1;
			break;
		}

		struct stat st;

		if (fstatat(in, e->d_name, &st, AT_SYMLINK_NOFOLLOW))
			continue;
		fprintf(f, "%s{\"name\":", count ? "," : "");
		json_string(f, e->d_name);
		fputs(",\"type\":", f);
		json_string(f, S_ISDIR(st.st_mode)   ? "directory"
		               : S_ISREG(st.st_mode) ? "file"
		               : S_ISLNK(st.st_mode) ? "symlink"
		                                     : "special");
		fprintf(f, ",\"size\":%lld,\"modified\":%ld,\"mode\":%u}", (long long)st.st_size,
		        (long)st.st_mtime, (unsigned)(st.st_mode & 0777));
		count++;
	}

	fprintf(f, "],\"truncated\":%s}", truncated ? "true" : "false");
	closedir(d);
	fclose(f);
	respond(fd, 200, "application/json", out, size);
	free(out);
}

static void file_read(int fd, const char *path, int download) {
	if (!readable_path(path)) {
		message(fd, 400, "Недопустимый путь.\n");
		return;
	}

	int in = path_fd(path, O_RDONLY);

	if (in < 0) {
		message(fd, 404, "Файл недоступен; символические ссылки не разрешены.\n");
		return;
	}

	struct stat st;

	if (fstat(in, &st) || !S_ISREG(st.st_mode)) {
		close(in);
		message(fd, 400, "Можно открывать только обычные файлы.\n");
		return;
	}

	if (st.st_size > FILE_LIMIT) {
		close(in);
		message(fd, 413, "Максимальный размер файла — 1 МиБ.\n");
		return;
	}

	size_t n;
	unsigned char *data = read_fd(in, FILE_LIMIT, &n);
	close(in);

	if (!data) {
		message(fd, 500, "Файл изменился или не удалось его прочитать.\n");
		return;
	}

	if (download) {
		respond(fd, 200, "application/octet-stream", (char *)data, n);
		free(data);
		return;
	}

	char rev[17];
	revision_of(data, n, rev);
	int text = text_bytes(data, n), editable = text && n <= EDIT_LIMIT && editable_path(path);
	char *out = 0;
	size_t size = 0;
	FILE *f = open_memstream(&out, &size);
	fputs("{\"path\":", f);
	json_string(f, path);
	fprintf(f, ",\"size\":%zu,\"text\":%s,\"editable\":%s,\"revision\":", n,
	        text ? "true" : "false", editable ? "true" : "false");
	json_string(f, rev);
	fprintf(f, ",\"truncated\":%s,\"content\":", n > EDIT_LIMIT ? "true" : "false");

	if (text) {
		unsigned char old = data[n > EDIT_LIMIT ? EDIT_LIMIT : n];
		size_t length = n > EDIT_LIMIT ? EDIT_LIMIT : n;

		while (length && !text_bytes(data, length))
			length--;
		data[length] = 0;
		json_string(f, (char *)data);
		data[n > EDIT_LIMIT ? EDIT_LIMIT : n] = old;
	} else
		fputs("null", f);
	fputs("}", f);
	fclose(f);
	respond(fd, 200, "application/json", out, size);
	free(out);
	free(data);
}

static void process_list(int fd) {
	static struct lumine_process_info_v2 list[4096], previous[4096];
	static long old_count;
	static uint64_t old_time;
	long count = syscall(LUMINE_PROCESS_SNAPSHOT_V2, list, 4096, sizeof *list);

	if (count < 0 || count > 4096) {
		message(fd, 503, "Ядро не предоставило список процессов.\n");
		return;
	}

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
	char *out = 0;
	size_t size = 0;
	FILE *f = open_memstream(&out, &size);
	fputc('[', f);

	for (long i = 0; i < count; i++) {
		list[i].base.name[sizeof list[i].base.name - 1] = 0;
		double cpu = 0;

		for (long j = 0; j < old_count; j++)
			if (previous[j].base.pid == list[i].base.pid &&
			    !strcmp(previous[j].base.name, list[i].base.name) &&
			    list[i].cpu_us >= previous[j].cpu_us && old_time && now > old_time) {
				cpu = 100.0 * (list[i].cpu_us - previous[j].cpu_us) / (now - old_time);
				break;
			}

		fprintf(
		    f,
		    "%s{\"pid\":%lld,\"state\":%u,\"threads\":%u,\"memory_bytes\":%llu,\"cpu_percent\":%.2f,\"cpu_us\":\"%llu\",\"name\":",
		    i ? "," : "", (long long)list[i].base.pid, list[i].base.state, list[i].base.threads,
		    (unsigned long long)list[i].resident_bytes, cpu, (unsigned long long)list[i].cpu_us);
		json_string(f, list[i].base.name);
		fprintf(f, ",\"protected\":%s}",
		        list[i].base.pid <= 1 || list[i].base.pid == getpid( ) ||
		                list[i].base.pid == getppid( ) || strstr(list[i].base.name, "webpanel") ||
		                strstr(list[i].base.name, "nginx")
		            ? "true"
		            : "false");
	}

	fputc(']', f);
	fclose(f);
	memcpy(previous, list, count * sizeof *list);
	old_count = count;
	old_time = now;
	respond(fd, 200, "application/json", out, size);
	free(out);
}

static void process_signal(int fd, const char *body) {
	char pidtext[32], signaltext[16], name[256], cpu[32], *end;

	if (!decoded_form(body, "pid", pidtext, sizeof pidtext) ||
	    !decoded_form(body, "signal", signaltext, sizeof signaltext) ||
	    !decoded_form(body, "name", name, sizeof name) ||
	    !decoded_form(body, "cpu_us", cpu, sizeof cpu)) {
		message(fd, 400, "Не выбран процесс.\n");
		return;
	}

	long pid = strtol(pidtext, &end, 10);

	if (*end || pid <= 1 || pid > INT_MAX || pid == getpid( ) || pid == getppid( )) {
		message(fd, 400, "Системный процесс защищён.\n");
		return;
	}

	unsigned long long minimum = strtoull(cpu, &end, 10);

	if (*end || !*cpu || cpu[0] == '-') {
		message(fd, 400, "Неверные сведения о процессе.\n");
		return;
	}

	int sig = !strcmp(signaltext, "TERM") ? SIGTERM : !strcmp(signaltext, "KILL") ? SIGKILL : 0;

	if (!sig) {
		message(fd, 400, "Разрешены только TERM и KILL.\n");
		return;
	}

	static struct lumine_process_info_v2 list[4096];
	long count = syscall(LUMINE_PROCESS_SNAPSHOT_V2, list, 4096, sizeof *list);
	int found = 0;

	if (count < 0 || count > 4096) {
		message(fd, 503, "Не удалось проверить процесс.\n");
		return;
	}

	for (long i = 0; i < count; i++)
		if (list[i].base.pid == pid) {
			list[i].base.name[255] = 0;

			if (strcmp(name, list[i].base.name) || list[i].cpu_us < minimum ||
			    list[i].base.state == 2) {
				message(fd, 409, "Процесс изменился. Обновите список.\n");
				return;
			}

			if (strstr(name, "webpanel") || strstr(name, "nginx")) {
				message(fd, 400, "Процессы панели и nginx обслуживайте через SSH.\n");
				return;
			}

			found = 1;
			break;
		}

	if (!found) {
		message(fd, 404, "Процесс уже завершён.\n");
		return;
	}

	if (tasks_running( )) {
		message(fd, 409, "Дождитесь завершения команды.\n");
		return;
	}

	int result = kill((pid_t)pid, sig);
	char output[512];
	snprintf(output, sizeof output, "PID %ld (%s): сигнал %s — %s\n", pid, name, signaltext,
	         result ? strerror(errno) : "отправлен");
	unsigned job = audit_action("process", signaltext, pidtext, output, result ? 1 : 0);
	char reply[64];
	snprintf(reply, sizeof reply, "{\"job\":%u}", job);
	respond(fd, 200, "application/json", reply, strlen(reply));
}

static void disk_list(int fd) {
	const char *paths[] = { "/", "/boot", "/home", "/tmp", "/var/log", "/var/lib/setup", NULL };
	char *out = 0;
	size_t size = 0;
	FILE *f = open_memstream(&out, &size);
	fputc('[', f);
	int first = 1;

	for (int i = 0; paths[i]; i++) {
		int in = path_fd(paths[i], O_RDONLY | O_DIRECTORY);

		if (in < 0)
			continue;
		struct statvfs st;
		struct stat file;
		int ok = !fstatvfs(in, &st) && !fstat(in, &file);
		close(in);

		if (!ok)
			continue;
		fprintf(f, "%s{\"path\":", first ? "" : ",");
		json_string(f, paths[i]);
		fprintf(
		    f,
		    ",\"device\":%llu,\"total\":%llu,\"free\":%llu,\"available\":%llu,\"files\":%llu,\"files_free\":%llu}",
		    (unsigned long long)file.st_dev, (unsigned long long)st.f_blocks * st.f_frsize,
		    (unsigned long long)st.f_bfree * st.f_frsize,
		    (unsigned long long)st.f_bavail * st.f_frsize, (unsigned long long)st.f_files,
		    (unsigned long long)st.f_ffree);
		first = 0;
	}

	fputc(']', f);
	fclose(f);
	respond(fd, 200, "application/json", out, size);
	free(out);
}

struct usage_scan {
	unsigned entries;
	int incomplete;
	dev_t device;
	time_t deadline;
};

static unsigned long long folder_bytes(int fd, unsigned depth, struct usage_scan *scan) {
	unsigned long long total = 0;
	DIR *d = fdopendir(fd);

	if (!d) {
		close(fd);
		scan->incomplete = 1;
		return 0;
	}

	struct dirent *e;

	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (++scan->entries > 100000 || monotime( ) > scan->deadline) {
			scan->incomplete = 1;
			break;
		}

		struct stat st;

		if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
			scan->incomplete = 1;
			continue;
		}

		if (st.st_dev != scan->device || S_ISLNK(st.st_mode))
			continue;
		if (S_ISREG(st.st_mode))
			total += st.st_size;
		else if (S_ISDIR(st.st_mode)) {
			if (depth >= 32) {
				scan->incomplete = 1;
				continue;
			}

			int child = openat(dirfd(d), e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);

			if (child < 0)
				scan->incomplete = 1;
			else
				total += folder_bytes(child, depth + 1, scan);
		}
	}

	closedir(d);
	return total;
}

static int usage_worker(void *context) {
	const char *path = context;
	int in = path_fd(path, O_RDONLY | O_DIRECTORY);

	if (in < 0) {
		perror("directory scan");
		return 1;
	}

	struct stat root;

	if (fstat(in, &root)) {
		close(in);
		return 1;
	}

	DIR *d = fdopendir(in);

	if (!d) {
		close(in);
		return 1;
	}

	struct usage_scan scan = { .device = root.st_dev, .deadline = monotime( ) + 120 };
	struct dirent *e;
	unsigned long long total = 0;
	printf("Логический размер файлов в %s; ссылки и другие файловые системы пропущены.\n", path);

	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (++scan.entries > 100000 || monotime( ) > scan.deadline) {
			scan.incomplete = 1;
			break;
		}

		struct stat st;

		if (fstatat(in, e->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
			scan.incomplete = 1;
			continue;
		}

		if (st.st_dev != root.st_dev || S_ISLNK(st.st_mode))
			continue;
		unsigned long long bytes = 0;

		if (S_ISREG(st.st_mode))
			bytes = st.st_size;
		else if (S_ISDIR(st.st_mode)) {
			int child = openat(in, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);

			if (child < 0)
				scan.incomplete = 1;
			else
				bytes = folder_bytes(child, 1, &scan);
		} else
			continue;
		printf("%llu\t%s\n", bytes, e->d_name);
		total += bytes;
	}

	closedir(d);
	printf("Итого: %llu байт.\n%s\n", total,
	       scan.incomplete ? "Результат неполный: достигнут лимит или часть каталогов недоступна."
	                       : "Сканирование завершено.");
	return scan.incomplete ? 1 : 0;
}

static void disk_usage(int fd, const char *path) {
	if (!readable_path(path)) {
		message(fd, 400, "Недопустимый путь.\n");
		return;
	}

	int in = path_fd(path, O_RDONLY | O_DIRECTORY);

	if (in < 0) {
		message(fd, 404, "Каталог недоступен.\n");
		return;
	}

	close(in);
	unsigned job = launch_impl(NULL, usage_worker, (void *)path, "disk-usage", "scan", "directory");

	if (!job) {
		message(fd, 409, "Дождитесь завершения команды.\n");
		return;
	}

	char out[64];
	snprintf(out, sizeof out, "{\"job\":%u}", job);
	respond(fd, 200, "application/json", out, strlen(out));
}

static int host_token(const char *s) {
	if (!*s)
		return 0;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-' && *s != '_')
			return 0;
	return 1;
}

static int validate_config(const char *path, const char *text, char *error, size_t cap) {
	if (!text_bytes((const unsigned char *)text, strlen(text))) {
		snprintf(error, cap, "Конфигурация должна содержать текст UTF-8.");
		return 0;
	}

	char *copy = strdup(text);

	if (!copy)
		return 0;
	char *next = copy;
	int active = 0, ok = 1;

	while (next && ok) {
		char *line = next;
		next = strchr(line, '\n');

		if (next)
			*next++ = 0;
		while (isspace((unsigned char)*line))
			line++;
		if (!*line || *line == '#')
			continue;
		if (below(path, "/etc/services")) {
			if (active++ || strncmp(line, "exec ", 5)) {
				ok = 0;
				break;
			}

			char *p = line + 5;
			int quote = 0, words = 0, inword = 0;

			for (; *p; p++) {
				char c = *p;

				if (!quote && isspace((unsigned char)c)) {
					inword = 0;
					continue;
				}

				if (!inword) {
					words++;
					inword = 1;
				}

				if (c == '\\' && quote != '\'' && p[1]) {
					p++;
					continue;
				}

				if ((c == '\'' || c == '"') && (!quote || quote == c)) {
					quote = quote ? 0 : c;
					continue;
				}

				if (!quote && strchr(";|&<>$`", c)) {
					ok = 0;
					break;
				}
			}

			if (quote || !words || words > 11)
				ok = 0;
		} else if (!strcmp(path, "/etc/hosts")) {
			char *comment = strchr(line, '#');

			if (comment)
				*comment = 0;
			char *save, *ip = strtok_r(line, " \t\r", &save), *host;
			unsigned char address[16];
			int hosts = 0;

			if (!ip || (!inet_pton(AF_INET, ip, address) && !inet_pton(AF_INET6, ip, address))) {
				ok = 0;
				break;
			}
			while ((host = strtok_r(NULL, " \t\r", &save))) {
				if (!host_token(host))
					ok = 0;
				hosts++;
			}

			if (!hosts)
				ok = 0;
			active++;
		} else if (!strcmp(path, "/etc/resolv.conf")) {
			char *comment = strchr(line, '#');

			if (comment)
				*comment = 0;
			char *save, *key = strtok_r(line, " \t\r", &save),
			            *value = strtok_r(NULL, " \t\r", &save);
			unsigned char address[16];

			if (!key || !value) {
				ok = 0;
				break;
			}

			if (!strcmp(key, "nameserver")) {
				if (!inet_pton(AF_INET, value, address) && !inet_pton(AF_INET6, value, address))
					ok = 0;
				if (strtok_r(NULL, " \t\r", &save))
					ok = 0;
			} else if (!strcmp(key, "search") || !strcmp(key, "domain")) {
				do {
					if (!host_token(value))
						ok = 0;
				} while ((value = strtok_r(NULL, " \t\r", &save)));
			} else if (!strcmp(key, "options")) {
				do {
					for (char *p = value; *p; p++)
						if (!isalnum((unsigned char)*p) && !strchr("-_:.", *p))
							ok = 0;
				} while ((value = strtok_r(NULL, " \t\r", &save)));
			} else
				ok = 0;
			active++;
		}
	}

	free(copy);

	if (below(path, "/etc/services") && !active)
		ok = 0;
	if (!ok)
		snprintf(error, cap,
		         "Неверный формат конфигурации: проверьте директивы, аргументы и кавычки.");
	return ok;
}

static int write_file_at(int dir, const char *name, const void *data, size_t size, mode_t mode) {
	int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);

	if (fd < 0)
		return 0;
	int ok = !log_write_all(fd, data, size) && !fchmod(fd, mode) && !fsync(fd);

	if (close(fd))
		ok = 0;
	if (!ok)
		unlinkat(dir, name, 0);
	return ok;
}

static int nginx_validate(const char *candidate, char *output, size_t cap) {
	int pipes[2];

	if (pipe(pipes)) {
		snprintf(output, cap, "Не удалось запустить nginx -t.");
		return 0;
	}

	pid_t pid = fork( );

	if (!pid) {
		dup2(pipes[1], 1);
		dup2(pipes[1], 2);
		int null = open("/dev/null", O_RDONLY);
		dup2(null, 0);

		for (int fd = 3; fd < 256; fd++)
			close(fd);
		execl(WEBPANEL_NGINX_BIN, "nginx", "-t", "-c", candidate, (char *)NULL);
		_exit(127);
	}

	close(pipes[1]);

	if (pid < 0) {
		close(pipes[0]);
		return 0;
	}

	fcntl(pipes[0], F_SETFL, O_NONBLOCK);
	size_t n = 0;
	int status = 0, done = 0;
	time_t deadline = monotime( ) + 8;

	while (monotime( ) < deadline) {
		char chunk[512];
		ssize_t got = read(pipes[0], chunk, sizeof chunk);

		if (got > 0 && n + 1 < cap) {
			size_t take = (size_t)got < cap - n - 1 ? (size_t)got : cap - n - 1;
			memcpy(output + n, chunk, take);
			n += take;
		}

		if (waitpid(pid, &status, WNOHANG) == pid) {
			done = 1;
			break;
		}

		usleep(20000);
	}

	if (!done) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
	}

	close(pipes[0]);
	output[n] = 0;

	if (!done)
		snprintf(output, cap, "nginx -t не завершился за 8 секунд.");
	return done && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void config_save(int fd, const char *path, const unsigned char *body, size_t size,
                        const char *revision, const char *restore) {
	if (!readable_path(path) || !editable_path(path)) {
		message(fd, 400, "Редактирование этого файла не поддерживается.\n");
		return;
	}

	if (tasks_running( )) {
		message(fd, 409, "Дождитесь завершения команды.\n");
		return;
	}

	char name[PATH_LIMIT];
	int dir = parent_fd(path, name, sizeof name);

	if (dir < 0) {
		message(fd, 404, "Каталог недоступен.\n");
		return;
	}

	int original = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	struct stat stat;
	size_t oldsize = 0;
	unsigned char *old =
	    original >= 0 && !fstat(original, &stat) ? read_fd(original, EDIT_LIMIT, &oldsize) : NULL;
	if (original >= 0)
		close(original);
	if (!old) {
		close(dir);
		message(fd, 400, "Конфигурация недоступна или больше 64 КиБ.\n");
		return;
	}

	char actual[17];
	revision_of(old, oldsize, actual);

	if (!equal(actual, revision)) {
		free(old);
		close(dir);
		message(
		    fd, 409,
		    "Файл изменился после открытия. Откройте его заново; ваши правки сохранены в редакторе.\n");
		return;
	}

	unsigned char *restored = NULL;

	if (*restore) {
		char *end;
		unsigned long id = strtoul(restore, &end, 10);
		char backup[512], binding[PATH_LIMIT + 1];

		if (*end || !id || id > UINT_MAX) {
			free(old);
			close(dir);
			message(fd, 400, "Неверная резервная копия.\n");
			return;
		}

		snprintf(backup, sizeof backup, WEBPANEL_STATE_DIR "/backups/config-%lu.path", id);

		if (read_regular(backup, binding, sizeof binding, 0) < 0 || strcmp(binding, path)) {
			free(old);
			close(dir);
			message(fd, 404, "Копия не относится к этому файлу или уже удалена.\n");
			return;
		}

		snprintf(backup, sizeof backup, WEBPANEL_STATE_DIR "/backups/config-%lu.bak", id);
		int in = open(backup, O_RDONLY | O_NOFOLLOW);

		if (in >= 0) {
			restored = read_fd(in, EDIT_LIMIT, &size);
			close(in);
		}

		if (!restored) {
			free(old);
			close(dir);
			message(fd, 404, "Резервная копия недоступна.\n");
			return;
		}

		body = restored;
	}

	char report[2048] = "", candidate[80], bak[80], binding[80];
	unsigned id = sequence + 1;
	snprintf(candidate, sizeof candidate, ".webpanel-%ld-%u.tmp", (long)getpid( ), id);
	int ok = size <= EDIT_LIMIT && text_bytes(body, size) && !memchr(body, 0, size) &&
	         validate_config(path, (char *)body, report, sizeof report);
	if (!ok && !*report)
		snprintf(report, sizeof report,
		         "Конфигурация должна быть текстом UTF-8 размером до 64 КиБ.");
	if (ok && !write_file_at(dir, candidate, body, size, stat.st_mode & 0777)) {
		ok = 0;
		snprintf(report, sizeof report, "Не удалось создать файл для проверки.");
	}

	if (ok && fchownat(dir, candidate, stat.st_uid, stat.st_gid, AT_SYMLINK_NOFOLLOW)) {
		ok = 0;
		snprintf(report, sizeof report, "Не удалось сохранить владельца файла.");
	}

	if (ok && !strcmp(path, "/etc/nginx/nginx.conf")) {
		char physical[512];
		snprintf(physical, sizeof physical, WEBPANEL_FILE_ROOT "/etc/nginx/%s", candidate);
		ok = nginx_validate(physical, report, sizeof report);

		if (!ok && !*report)
			snprintf(report, sizeof report, "nginx -t отклонил конфигурацию.");
	}

	int backups = -1;

	if (ok) {
		mkdir(WEBPANEL_STATE_DIR "/backups", 0700);
		backups = open(WEBPANEL_STATE_DIR "/backups", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		snprintf(bak, sizeof bak, "config-%u.bak", id);
		snprintf(binding, sizeof binding, "config-%u.path", id);

		if (backups < 0 || !write_file_at(backups, bak, old, oldsize, 0600) ||
		    !write_file_at(backups, binding, path, strlen(path), 0600)) {
			ok = 0;
			snprintf(report, sizeof report,
			         "Не удалось сохранить резервную копию. Исходный файл не изменён.");
		}
	}
	// Compare the complete content again immediately before the atomic replacement.
	if (ok) {
		size_t checksize = 0;
		int check = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
		unsigned char *checkdata = check >= 0 ? read_fd(check, EDIT_LIMIT, &checksize) : NULL;

		if (check >= 0)
			close(check);
		if (!checkdata || checksize != oldsize || memcmp(checkdata, old, oldsize)) {
			ok = 0;
			snprintf(report, sizeof report,
			         "Файл изменился во время проверки. Исходный файл не заменён.");
		}

		free(checkdata);
	}

	if (ok && renameat(dir, candidate, dir, name)) {
		ok = 0;
		snprintf(report, sizeof report, "Не удалось заменить конфигурацию.");
	}

	if (!ok)
		unlinkat(dir, candidate, 0);
	if (backups >= 0) {
		fsync(backups);
		close(backups);
	}

	fsync(dir);
	close(dir);
	char output[3072];
	snprintf(output, sizeof output, "%s: %s\n%s\n%s", *restore ? "Восстановление" : "Сохранение",
	         path,
	         ok ? "Конфигурация проверена и сохранена. Служба автоматически не перезапускается."
	            : report,
	         ok ? "Предыдущее содержимое сохранено в резервной копии.\n" : "");
	unsigned job =
	    audit_action("config", *restore ? "restore" : "save", "config", output, ok ? 0 : 1);
	char newrev[17];
	revision_of(body, size, newrev);
	char reply[256];
	snprintf(reply, sizeof reply, "{\"job\":%u,\"saved\":%s,\"revision\":\"%s\",\"backup\":%u}",
	         job, ok ? "true" : "false", newrev, ok ? job : 0);
	respond(fd, 200, "application/json", reply, strlen(reply));
	free(old);
	free(restored);
}

static void file_upload(int fd, const char *path, const unsigned char *body, size_t size,
                        int directory) {
	if (!upload_path(path)) {
		message(fd, 400,
		        "Загрузка и создание каталогов доступны в /home, /root, /var/www и /tmp.\n");
		return;
	}

	if (tasks_running( )) {
		message(fd, 409, "Дождитесь завершения команды.\n");
		return;
	}

	char name[PATH_LIMIT];
	int dir = parent_fd(path, name, sizeof name);

	if (dir < 0) {
		message(fd, 404, "Каталог недоступен.\n");
		return;
	}

	struct stat stat;

	if (!fstatat(dir, name, &stat, AT_SYMLINK_NOFOLLOW)) {
		close(dir);
		message(fd, 409, "Файл или каталог уже существует. Выберите другое имя.\n");
		return;
	}

	int ok = directory ? !mkdirat(dir, name, 0700) : write_file_at(dir, name, body, size, 0600);
	fsync(dir);
	close(dir);
	char output[512];
	snprintf(output, sizeof output, "%s: %s — %s\n",
	         directory ? "Создание каталога" : "Загрузка файла", path,
	         ok ? "готово" : strerror(errno));
	unsigned job = audit_action("file", directory ? "mkdir" : "upload", "file", output, ok ? 0 : 1);
	char reply[64];
	snprintf(reply, sizeof reply, "{\"job\":%u}", job);
	respond(fd, 200, "application/json", reply, strlen(reply));
}

static int management_get(int fd, const char *url) {
	if (!strcmp(url, "/api/processes")) {
		process_list(fd);
		return 1;
	}

	if (!strcmp(url, "/api/disks")) {
		disk_list(fd);
		return 1;
	}

	const char *prefixes[] = { "/api/files?path=", "/api/file?path=", "/api/download?path=", NULL };

	for (int i = 0; prefixes[i]; i++)
		if (!strncmp(url, prefixes[i], strlen(prefixes[i]))) {
			char path[PATH_LIMIT];

			if (!url_decode(url + strlen(prefixes[i]), path, sizeof path, 0))
				message(fd, 400, "Недопустимый путь.\n");
			else if (i == 0)
				file_list(fd, path);
			else
				file_read(fd, path, i == 2);
			return 1;
		}

	return 0;
}

static int management_post(int fd, const char *url, const char *headers, const unsigned char *body,
                           size_t size) {
	if (!strcmp(url, "/api/process")) {
		if (memchr(body, 0, size))
			message(fd, 400, "Invalid body\n");
		else
			process_signal(fd, (char *)body);
		return 1;
	}

	const char *prefixes[] = { "/api/usage?path=", "/api/config?path=", "/api/upload?path=",
		                       "/api/mkdir?path=", NULL };
	for (int i = 0; prefixes[i]; i++)
		if (!strncmp(url, prefixes[i], strlen(prefixes[i]))) {
			char path[PATH_LIMIT];

			if (!url_decode(url + strlen(prefixes[i]), path, sizeof path, 0)) {
				message(fd, 400, "Недопустимый путь.\n");
				return 1;
			}

			if (i == 0)
				disk_usage(fd, path);
			else if (i == 1) {
				char revision[32] = "", restore[32] = "";
				header_value(headers, "X-Webpanel-Revision", revision, sizeof revision);
				header_value(headers, "X-Webpanel-Restore", restore, sizeof restore);
				config_save(fd, path, body, size, revision, restore);
			} else
				file_upload(fd, path, body, size, i == 3);
			return 1;
		}

	return 0;
}
