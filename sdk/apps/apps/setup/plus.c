// plus.c: Авторизация доступа к каналу Plus
// Автор: Елчинян Арен Артурович, 2026
// Обмен одноразового кода и хранение токена для запросов репозитория
// Подсистема: пакетный менеджер setup
// Первое изменение: до текущей уборки, исходная дата не установлена
// Последнее изменение: 14.10.2026

#include "setup.h"
#include "../common/tls/tls.h"
#include <ctype.h>
#define PLUS_PUBLIC_KEY "e7eadc21bac517e223f098e0206a7277a9fb93a1648861471becb9db089dc00f"
#include <strings.h>

// Читаем токен в этом процессе и не передаём его через argv, URL или журнал
static void account_fetch(const char *url, const char *dest, size_t limit, const char *token_file,
                          const char *code, int post) {
	if (strncmp(url, "https://", 8)) {
		fail("Plus requires HTTPS");
	}

	char credential[4096], ca[4096], host[256], port[6] = "443", authority[256], request[4096];
	char token[64];
	int fd = -1;

	if (code) {
		if (strlen(code) != 12 || strspn(code, "0123456789") != 12) {
			fail("invalid one-time login code");
		}

		strcpy(token, code);
	} else {
		path(credential, token_file);
		fd = open(credential, O_RDONLY | O_NOFOLLOW);
		struct stat st;
		// stat в SynapseOS добавляет биты исполнения
		// Проверяем запрет чтения и записи для остальных пользователей
		if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 066) ||
		    st.st_uid != geteuid( )) {
			fail("setup credential must be an owned private regular file (0600)");
		}

		ssize_t count = read(fd, token, sizeof token - 1);
		close(fd);

		if (count <= 0 || count >= (ssize_t)sizeof token - 1) {
			fail("invalid setup credential");
		}

		token[count] = 0;

		while (count && (token[count - 1] == '\n' || token[count - 1] == '\r')) {
			token[--count] = 0;
		}

		if (count != 43) {
			fail("invalid setup credential");
		}

		for (int i = 0; i < count; i++) {
			if (!isalnum((unsigned char)token[i]) && token[i] != '_' && token[i] != '-') {
				fail("invalid setup credential");
			}
		}
	}

	const char *slash = strchr(url + 8, '/');
	size_t n = slash ? (size_t)(slash - url - 8) : 0;

	if (!n || n >= sizeof host || strpbrk(url + 8, "@\\\r\n \t")) {
		fail("invalid Plus URL");
	}

	memcpy(host, url + 8, n);
	host[n] = 0;
	strcpy(authority, host);
	char *colon = strchr(host, ':');

	if (colon) {
		*colon++ = 0;
		char *end;
		errno = 0;
		unsigned long number = strtoul(colon, &end, 10);

		if (errno || *end || !number || number > 65535) {
			fail("invalid Plus HTTPS port");
		}

		snprintf(port, sizeof port, "%lu", number);
	}

	if (!*host || strchr(host, '[') || strchr(host, ']')) {
		fail("invalid Plus HTTPS host");
	}

	path(ca, "etc/ssl/certs/ca-certificates.crt");
	struct tls t;
	int rc = tls_init(&t, 0, ca, NULL);

	if (!rc) {
		rc = tls_connect(&t, host, port);
	}

	if (rc) {
		tls_free(&t);
		fail("Plus TLS verification/connection failed");
	}

	n = snprintf(request, sizeof request,
	             "%s %s HTTP/1.0\r\nHost: %s\r\nAuthorization: %s %s\r\nContent-Length: "
	             "0\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
	             post ? "POST" : "GET", slash, authority, code ? "Code" : "Bearer", token);
	memset(token, 0, sizeof token);

	if (n >= sizeof request || tls_write_all(&t, request, n)) {
		tls_free(&t);
		fail("Plus request failed");
	}

	memset(request, 0, sizeof request);
	char header[32768];
	size_t pos = 0;

	while (pos + 1 < sizeof header) {
		if (tls_read(&t, (unsigned char *)header + pos, 1) != 1) {
			break;
		}

		header[++pos] = 0;

		if (pos >= 4 && !memcmp(header + pos - 4, "\r\n\r\n", 4)) {
			break;
		}
	}

	if (pos < 4 || memcmp(header + pos - 4, "\r\n\r\n", 4)) {
		tls_free(&t);
		fail("invalid Plus response");
	}

	if (strncmp(header, "HTTP/1.0 200 ", 13) && strncmp(header, "HTTP/1.1 200 ", 13)) {
		tls_free(&t);
		fail("Account access denied or unavailable; check login code/account in the "
		     "cabinet (or setup auth logout)");
	}

	char *save;
	strtok_r(header, "\r\n", &save);
	uint64_t length = 0;
	int have = 0;

	for (char *line = strtok_r(NULL, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
		char *value = strchr(line, ':');

		if (!value) {
			tls_free(&t);
			fail("invalid Plus header");
		}

		*value++ = 0;

		while (*value == ' ' || *value == '\t') {
			value++;
		}

		if (!strcasecmp(line, "Content-Length")) {
			char *end;
			errno = 0;
			length = strtoull(value, &end, 10);

			if (have++ || *value < '0' || *value > '9' || errno || *end || length > limit) {
				tls_free(&t);
				fail("invalid Plus download length");
			}
		} else if (!strcasecmp(line, "Transfer-Encoding") ||
		           (!strcasecmp(line, "Content-Encoding") && strcasecmp(value, "identity"))) {
			tls_free(&t);
			fail("unsupported Plus response encoding");
		}
	}

	if (!have) {
		tls_free(&t);
		fail("Plus response needs Content-Length");
	}

	fd = open(dest, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);

	if (fd < 0) {
		tls_free(&t);
		fail("cannot create Plus download");
	}

	unsigned char buffer[16384];
	uint64_t total = 0;
	struct download_progress progress;
	progress_start(&progress, length);

	while (total < length) {
		size_t want = length - total;

		if (want > sizeof buffer) {
			want = sizeof buffer;
		}

		int got = tls_read(&t, buffer, want);

		if (got <= 0) {
			close(fd);
			unlink(dest);
			tls_free(&t);
			fail("Plus download truncated");
		}

		size_t off = 0;

		while (off < (size_t)got) {
			ssize_t wrote = write(fd, buffer + off, got - off);

			if (wrote < 0 && errno == EINTR) {
				continue;
			}

			if (wrote <= 0) {
				close(fd);
				unlink(dest);
				tls_free(&t);
				fail("Plus download write failed");
			}

			off += wrote;
		}

		total += got;
		progress_update(&progress, total, 0);
	}

	tls_free(&t);

	if (fsync(fd) || close(fd)) {
		fail("Plus download sync failed");
	}

	progress_update(&progress, total, 1);
}

void private_fetch(const char *url, const char *dest, size_t limit, const char *token_file) {
	account_fetch(url, dest, limit, token_file, NULL, 0);
}

static void configure_plus(const char *token) {
	char dir[4096], credential[4096], config[4096], key[4096];
	path(credential, "etc/setup/auth/plus.token");
	path(config, "etc/setup/repos/plus.json");
	path(dir, "etc/setup/auth");
	mkdirs(dir);

	if (chmod(dir, 0700)) {
		fail("protect Plus credentials");
	}

	write_bytes(credential, token, strlen(token), 0600);

	if (chmod(credential, 0600)) {
		fail("protect Plus credential");
	}

	path(dir, "etc/setup/keys");
	mkdirs(dir);
	path(key, "etc/setup/keys/plus.pub");
	write_bytes(key, PLUS_PUBLIC_KEY, 64, 0644);
	path(dir, "etc/setup/repos");
	mkdirs(dir);
	static const char json[] =
	    "{\"id\":\"plus\",\"url\":\"https://synapseos.ru/lk/"
	    "repository\",\"key\":\"plus\",\"prefix\":\"\",\"priority\":90,\"official\":"
	    "false,\"enabled\":true,\"min_generation\":1,\"token_file\":\"etc/setup/auth/"
	    "plus.token\"}";
	write_bytes(config, json, sizeof json - 1, 0600);
}

void plus_command(int argc, char **argv) {
	if (argc != 1 ||
	    (strcmp(argv[0], "enable") && strcmp(argv[0], "disable") && strcmp(argv[0], "status"))) {
		fail("usage: setup plus enable|disable|status");
	}

	char config[4096], credential[4096], dir[4096];
	path(config, "etc/setup/repos/plus.json");
	path(credential, "etc/setup/auth/plus.token");

	if (!strcmp(argv[0], "status")) {
		struct stat st;
		puts(lstat(config, &st) ? "Plus repository is disabled"
		                        : "Plus repository is configured; server checks "
		                          "subscription on every access");
		return;
	}

	if (dry_run) {
		fail("plus configuration does not support --dry-run");
	}

	if (geteuid( ) != 0 && !*root) {
		fail("administrative privileges required");
	}

	path(dir, "var/lib/setup");
	mkdirs(dir);
	char lockpath[4096];
	path(lockpath, "var/lib/setup/lock");
	int lock = open(lockpath, O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);

	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
		fail("setup is busy or lock unavailable");
	}

	if (!strcmp(argv[0], "disable")) {
		if (unlink(config) && errno != ENOENT) {
			fail("cannot disable Plus repository");
		}

		if (unlink(credential) && errno != ENOENT) {
			fail("cannot remove Plus credential");
		}

		close(lock);
		puts("Plus disabled. Installed packages retained.");
		return;
	}

	char token[64];

	if (isatty(STDIN_FILENO)) {
		char *value = getpass("Plus access key: ");

		if (!value || strlen(value) >= sizeof token) {
			fail("invalid Plus key");
		}

		strcpy(token, value);
		memset(value, 0, strlen(value));
	} else {
		if (!fgets(token, sizeof token, stdin)) {
			fail("read Plus key from stdin");
		}

		token[strcspn(token, "\r\n")] = 0;
	}

	if (strlen(token) != 43) {
		fail("invalid Plus key");
	}

	for (int i = 0; token[i]; i++) {
		if (!isalnum((unsigned char)token[i]) && token[i] != '_' && token[i] != '-') {
			fail("invalid Plus key");
		}
	}

	configure_plus(token);
	memset(token, 0, sizeof token);
	close(lock);
	puts("Plus configured. Run setup update. Subscription is checked by the server.");
}

void auth_command(int argc, char **argv) {
	if (argc > 1 || (argc && strcmp(argv[0], "status") && strcmp(argv[0], "logout"))) {
		fail("usage: setup auth [status|logout]");
	}

	if (dry_run) {
		fail("auth does not support --dry-run");
	}

	if (geteuid( ) != 0 && !*root) {
		fail("administrative privileges required");
	}

	char dir[4096], lockpath[4096], response[4096], credential[4096], config[4096], userpath[4096];
	path(dir, "var/lib/setup");
	mkdirs(dir);
	path(lockpath, "var/lib/setup/lock");
	int lock = open(lockpath, O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);

	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
		fail("setup is busy or lock unavailable");
	}

	path(dir, "var/cache/setup");
	mkdirs(dir);
	path(response, "var/cache/setup/auth-response.download");
	path(credential, "etc/setup/auth/plus.token");
	path(config, "etc/setup/repos/plus.json");
	path(userpath, "etc/setup/auth/account");
	struct stat st;

	if (argc && lstat(credential, &st)) {
		if (errno != ENOENT) {
			fail("cannot inspect setup credential");
		}

		puts("Not logged in. Run setup auth.");
		close(lock);
		return;
	}

	unlink(response);

	if (argc && !strcmp(argv[0], "logout")) {
		account_fetch("https://synapseos.ru/lk/auth/logout", response, 4096,
		              "etc/setup/auth/plus.token", NULL, 1);
		unlink(response);

		if ((unlink(config) && errno != ENOENT) || (unlink(userpath) && errno != ENOENT) ||
		    (unlink(credential) && errno != ENOENT)) {
			fail("cannot remove setup account configuration");
		}

		close(lock);
		puts("Logged out. This setup session revoked; installed packages retained.");
		return;
	}

	if (argc) {
		account_fetch("https://synapseos.ru/lk/repository/status.json", response, 4096,
		              "etc/setup/auth/plus.token", NULL, 0);
		size_t len;
		char *data = read_bytes(response, 4096, &len);
		unlink(response);
		struct json *j = parse_json(data, len);
		char username[65];
		int active;
		jstr(j, 0, "username", username, sizeof username);

		if (json_boolean(j, json_get(j, 0, "active"), &active)) {
			fail("invalid account status");
		}

		printf("Account: %s\nPlus: %s\n", username,
		       active ? "active" : "inactive; installed packages retained");
		free(j);
		free(data);
		close(lock);
		return;
	}

	puts("Open https://synapseos.ru/lk/login in a browser.\nSign in and choose the "
	     "one-time setup login code (valid for 10 minutes).");
	char code[32];

	if (isatty(STDIN_FILENO)) {
		char *value = getpass("One-time login code: ");

		if (!value || strlen(value) >= sizeof code) {
			fail("invalid login code");
		}

		strcpy(code, value);
		memset(value, 0, strlen(value));
	} else {
		if (!fgets(code, sizeof code, stdin)) {
			fail("read one-time login code from stdin");
		}

		code[strcspn(code, "\r\n")] = 0;
	}

	account_fetch("https://synapseos.ru/lk/auth/exchange", response, 4096, NULL, code, 1);
	memset(code, 0, sizeof code);
	size_t len;
	char *data = read_bytes(response, 4096, &len);
	unlink(response);
	struct json *j = parse_json(data, len);
	char token[64], username[65];
	jstr(j, 0, "access_token", token, sizeof token);
	jstr(j, 0, "username", username, sizeof username);

	if (strlen(token) != 43) {
		fail("invalid account credential");
	}

	for (int i = 0; token[i]; i++) {
		if (!isalnum((unsigned char)token[i]) && token[i] != '_' && token[i] != '-') {
			fail("invalid account credential");
		}
	}

	configure_plus(token);
	memset(token, 0, sizeof token);
	write_bytes(userpath, username, strlen(username), 0600);
	memset(data, 0, len);
	free(j);
	free(data);
	close(lock);
	printf("Logged in as %s. Run setup update. Plus availability follows your "
	       "subscription.\n",
	       username);
}
