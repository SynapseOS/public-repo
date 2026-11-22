// lan.c: Обнаружение узлов локальной сети и ответы на запросы
// Автор: Елчинян Арен Артурович, 2026
// Сканер использует ICMP и запросы идентификации, служба отвечает соседям в своей
// подсети Подсистема: пользовательские сетевые утилиты Первое изменение: до текущей
// уборки, исходная дата не установлена Последнее изменение: 14.10.2026 Доступ:
// общедоступный

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "protocol.h"

#define LAN_TCP_PORT 39422
#define LAN_INBOX "/var/spool/lan/inbox"
#define LAN_MAX_TEXT (64u * 1024u)
#define LAN_MAX_FILE (256u * 1024u * 1024u)
#define LAN_MAX_NAME 128u

struct iface {
	int index;
	uint32_t ip, mask;
};

static volatile sig_atomic_t running = 1;

static void stop(int sig) {
	(void)sig;
	running = 0;
}

static uint64_t now(void) {
	struct timespec t;

	if (clock_gettime(CLOCK_MONOTONIC, &t) < 0) {
		return 0;
	}

	return (uint64_t)t.tv_sec * 1000 + (unsigned)t.tv_nsec / 1000000;
}

static size_t interfaces(struct iface *out, size_t cap) {
	FILE *f = fopen("/proc/net-interfaces", "r");

	if (!f) {
		return 0;
	}

	char line[128];
	size_t count = 0;

	while (fgets(line, sizeof(line), f) && count < cap) {
		char ip[16], mask[16], gw[16], extra;
		int idx, up;
		struct in_addr a, m, g;

		if (sscanf(line, "%d %15s %15s %15s %d %c", &idx, ip, mask, gw, &up, &extra) != 5 ||
		    idx < 0 || up != 1 || inet_pton(AF_INET, ip, &a) != 1 ||
		    inet_pton(AF_INET, mask, &m) != 1 || inet_pton(AF_INET, gw, &g) != 1) {
			continue;
		}

		if (!a.s_addr || !lan_mask_valid(ntohl(m.s_addr))) {
			continue;
		}

		out[count++] = (struct iface){ idx, ntohl(a.s_addr), ntohl(m.s_addr) };
	}

	fclose(f);
	return count;
}

static int local_peer(uint32_t ip) {
	struct iface list[8];
	size_t count = interfaces(list, 8);

	for (size_t i = 0; i < count; i++) {
		if (lan_onlink(ip, list[i].ip, list[i].mask)) {
			return 1;
		}
	}

	return 0;
}

// Проверить, включено ли обнаружение устройств в локальной сети
// Настройка LanDiscovery.Enabled в /etc/settings.ini, по умолчанию выключено
static int lan_enabled(void) {
	FILE *f = fopen("/etc/settings.ini", "r");

	if (!f) {
		return 0;
	}

	char line[256], section[64] = { 0 };
	int enabled = 0;

	while (fgets(line, sizeof(line), f)) {
		char *p = line;

		while (*p == ' ' || *p == '\t') {
			p++;
		}

		if (*p == '#' || *p == ';' || !*p) {
			continue;
		}

		if (*p == '[') {
			char *e = strchr(p, ']');

			if (e) {
				size_t n = (size_t)(e - p - 1);

				if (n >= sizeof(section)) {
					n = sizeof(section) - 1;
				}

				memcpy(section, p + 1, n);
				section[n] = 0;
			}

			continue;
		}

		char *eq = strchr(p, '=');

		if (!eq || strcmp(section, "LanDiscovery")) {
			continue;
		}

		char *key = p;
		char *kend = eq;

		while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t')) {
			kend--;
		}

		*kend = 0;

		if (strcmp(key, "Enabled")) {
			continue;
		}

		char *v = eq + 1;

		while (*v == ' ' || *v == '\t') {
			v++;
		}

		enabled = (*v == '1');
	}

	fclose(f);
	return enabled;
}

static int socket_udp(void) {
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (fd >= 0 && fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

static void lan_handle_conn(int fd, struct sockaddr_in peer);

// Служба обнаружения и приёма файлов, отвечает только при включённой настройке
static int serve(void) {
	int fd = socket_udp( );

	if (fd < 0) {
		perror("lan: UDP");
		return 1;
	}

	struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(LAN_PORT) };

	if (bind(fd, (void *)&address, sizeof(address)) < 0) {
		perror("lan: bind");
		close(fd);
		return 1;
	}

	// TCP порт для сообщений и файлов, без bind служба всё равно отвечает на UDP
	int tcp = socket(AF_INET, SOCK_STREAM, 0);
	int tcp_listening = 0;

	if (tcp >= 0) {
		struct sockaddr_in taddress = { .sin_family = AF_INET,
		                                .sin_port = htons(LAN_TCP_PORT) };
		int one = 1;

		setsockopt(tcp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

		if (bind(tcp, (void *)&taddress, sizeof(taddress)) == 0 &&
		    listen(tcp, 8) == 0 && fcntl(tcp, F_SETFL, O_NONBLOCK) == 0) {
			tcp_listening = 1;
		} else {
			close(tcp);
			tcp = -1;
		}
	}

	signal(SIGTERM, stop);
	signal(SIGINT, stop);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);
	struct utsname identity;

	if (uname(&identity) < 0) {
		close(fd);

		if (tcp >= 0) {
			close(tcp);
		}

		return 1;
	}

	if (strcmp(identity.sysname, "SynapseOS")) {
		fprintf(stderr, "lan: identity service requires SynapseOS\n");
		close(fd);

		if (tcp >= 0) {
			close(tcp);
		}

		return 1;
	}

	mkdir(LAN_INBOX, 0755);
	unsigned char reply[LAN_REPLY_SIZE] = { 0 };
	memcpy(reply, "SYNLAN1R", 8);
	memcpy(reply + 24, identity.nodename, strnlen(identity.nodename, 63));
	memcpy(reply + 88, identity.release, strnlen(identity.release, 15));
	uint64_t period = now( );
	unsigned accepted = 0;
	int enabled = 0;
	uint64_t enabled_check = 0;

	while (running) {
		struct pollfd p[2] = { { fd, POLLIN, 0 }, { tcp, POLLIN, 0 } };
		int nfds = tcp_listening ? 2 : 1;
		int r = poll(p, (nfds_t)nfds, 1000);

		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}

			perror("lan: poll");
			break;
		}

		uint64_t t = now( );

		if (t - enabled_check >= 2000) {
			enabled_check = t;
			enabled = lan_enabled();
		}

		if (tcp_listening && (p[1].revents & POLLIN) && enabled) {
			for (;;) {
				struct sockaddr_in peer;
				socklen_t plen = sizeof(peer);
				int conn = accept(tcp, (void *)&peer, &plen);

				if (conn < 0) {
					break;
				}

				pid_t child = fork();

				if (child == 0) {
					close(fd);
					close(tcp);
					lan_handle_conn(conn, peer);
					_exit(0);
				}

				close(conn);
			}
		}

		if (!r || !(p[0].revents & POLLIN) || !enabled) {
			continue;
		}

		unsigned char query[LAN_QUERY_SIZE + 1];
		struct sockaddr_in peer;
		socklen_t len = sizeof(peer);
		ssize_t n = recvfrom(fd, query, sizeof(query), 0, (void *)&peer, &len);

		if (n < 0) {
			continue;
		}

		t = now( );

		if (t - period >= 1000) {
			period = t;
			accepted = 0;
		}

		if (accepted >= 20 || len != sizeof(peer) || peer.sin_family != AF_INET ||
		    !lan_query_valid(query, (size_t)n) || !local_peer(ntohl(peer.sin_addr.s_addr))) {
			continue;
		}

		accepted++;
		memcpy(reply + 8, query + 8, 16);

		if (sendto(fd, reply, sizeof(reply), 0, (void *)&peer, sizeof(peer)) < 0 &&
		    errno != EAGAIN) {
			perror("lan: reply");
		}
	}

	close(fd);

	if (tcp >= 0) {
		close(tcp);
	}

	return running ? 1 : 0;
}

// Прочитать ровно len байт с контрольной суммой времени, вернуть 0 при успехе
static int read_full(int fd, unsigned char *buf, size_t len, uint64_t deadline) {
	while (len) {
		uint64_t left = deadline > now( ) ? deadline - now( ) : 0;

		if (!left) {
			return -1;
		}

		struct pollfd p = { fd, POLLIN, 0 };

		if (poll(&p, 1, left > 60000 ? 60000 : (int)left) <= 0) {
			return -1;
		}

		ssize_t n = recv(fd, buf, len, 0);

		if (n <= 0) {
			return -1;
		}

		buf += n;
		len -= (size_t)n;
	}

	return 0;
}

// Отправить ответную строку и закрыть соединение
static void conn_reply(int fd, const char *text) {
	size_t n = strlen(text);

	while (n) {
		ssize_t k = send(fd, text, n, 0);

		if (k <= 0) {
			break;
		}

		text += k;
		n -= (size_t)k;
	}
}

// Очистить имя файла от путей и управляющих символов
static void safe_name(char *dst, const unsigned char *src, size_t n) {
	size_t i = 0;

	while (i < n && src[i] && src[i] != '/' && src[i] != '\\' &&
	       src[i] >= 32 && src[i] <= 126) {
		dst[i] = (char)src[i];
		i++;
	}

	dst[i] = 0;

	if (!i) {
		strcpy(dst, "file");
	}
}

// Создать уникальное имя в каталоге приёма по времени и отправителю
static void inbox_name(char *out, size_t cap, const char *sender, const char *name,
                       int is_message) {
	time_t t = time(NULL);
	struct tm tmv;

	if (!gmtime_r(&t, &tmv)) {
		memset(&tmv, 0, sizeof(tmv));
	}

	char safe_sender[64], stamp[32];

	safe_name(safe_sender, (const unsigned char *)sender, strnlen(sender, sizeof(safe_sender) - 1));
	snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d", tmv.tm_year + 1900,
	         tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

	if (is_message) {
		snprintf(out, cap, "%s/%s-msg-%s.txt", LAN_INBOX, stamp, safe_sender);
	} else {
		snprintf(out, cap, "%s/%s-%s-%s", LAN_INBOX, stamp, safe_sender, name);
	}
}

// Принять одно TCP соединение с сообщением или файлом от соседа в сети
static void lan_handle_conn(int fd, struct sockaddr_in peer) {
	uint32_t ip = ntohl(peer.sin_addr.s_addr);

	if (!local_peer(ip) || !lan_enabled()) {
		conn_reply(fd, "ERR forbidden\n");
		close(fd);
		return;
	}

	unsigned char header[160];
	uint64_t deadline = now( ) + 120000;
	size_t hlen = 0;

	// Читаем строку заголовка побайтно, она короткая
	while (hlen < sizeof(header) - 1) {
		uint64_t left = deadline > now( ) ? deadline - now( ) : 0;

		if (!left) {
			conn_reply(fd, "ERR timeout\n");
			close(fd);
			return;
		}

		struct pollfd p = { fd, POLLIN, 0 };

		if (poll(&p, 1, left > 30000 ? 30000 : (int)left) <= 0) {
			conn_reply(fd, "ERR timeout\n");
			close(fd);
			return;
		}

		ssize_t n = recv(fd, header + hlen, 1, 0);

		if (n <= 0) {
			close(fd);
			return;
		}

		if (header[hlen] == '\n') {
			break;
		}

		hlen++;
	}

	header[hlen] = 0;

	char command[16], sender[LAN_MAX_NAME + 1], name[LAN_MAX_NAME + 1];
	unsigned slen = 0, nlen = 0;
	unsigned long long size64 = 0;
	uint64_t size;

	if (sscanf((char *)header, "SYNLAN2 %15s %u %u %llu", command, &slen, &nlen,
	           &size64) != 4) {
		conn_reply(fd, "ERR header\n");
		close(fd);
		return;
	}

	size = size64;
	int is_message = !strcmp(command, "MSG");

	if ((!is_message && strcmp(command, "FILE")) || slen > LAN_MAX_NAME ||
	    nlen > LAN_MAX_NAME || size > (is_message ? LAN_MAX_TEXT : LAN_MAX_FILE)) {
		conn_reply(fd, "ERR request\n");
		close(fd);
		return;
	}

	if (read_full(fd, (unsigned char *)sender, slen, deadline) < 0 ||
	    read_full(fd, (unsigned char *)name, nlen, deadline) < 0) {
		conn_reply(fd, "ERR body\n");
		close(fd);
		return;
	}

	sender[slen] = 0;
	name[nlen] = 0;
	safe_name(name, (const unsigned char *)name, strnlen(name, sizeof(name) - 1));

	char path[512];
	inbox_name(path, sizeof(path), sender, name, is_message);
	int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (out < 0) {
		conn_reply(fd, "ERR store\n");
		close(fd);
		return;
	}

	unsigned char chunk[16384];
	uint64_t left = size;
	int ok = 1;

	while (left) {
		size_t want = left > sizeof(chunk) ? sizeof(chunk) : (size_t)left;

		if (read_full(fd, chunk, want, deadline) < 0) {
			ok = 0;
			break;
		}

		size_t done = 0;

		while (done < want) {
			ssize_t k = write(out, chunk + done, want - done);

			if (k < 0 && errno == EINTR) {
				continue;
			}

			if (k <= 0) {
				ok = 0;
				break;
			}

			done += (size_t)k;
		}

		if (!ok) {
			break;
		}

		left -= want;
	}

	close(out);
	conn_reply(fd, ok ? "OK\n" : "ERR write\n");
	close(fd);
}

// Отправить соседу сообщение или файл, вернуть 0 при подтверждении OK
static int lan_push(uint32_t ip, const char *command, const char *sender,
                    const char *name, const char *data, uint64_t size) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0) {
		return -1;
	}

	struct sockaddr_in peer = { .sin_family = AF_INET,
	                            .sin_port = htons(LAN_TCP_PORT),
	                            .sin_addr.s_addr = htonl(ip) };

	if (connect(fd, (void *)&peer, sizeof(peer)) < 0) {
		close(fd);
		return -1;
	}

	size_t slen = strnlen(sender, 64), nlen = strnlen(name, LAN_MAX_NAME);
	char header[96];
	int hn = snprintf(header, sizeof(header), "SYNLAN2 %s %zu %zu %llu\n", command,
	                  slen, nlen, (unsigned long long)size);

	if (hn <= 0 || (size_t)hn >= sizeof(header) || send(fd, header, (size_t)hn, 0) != hn) {
		close(fd);
		return -1;
	}

	uint64_t done = 0;

	while (done < slen + nlen + size) {
		const unsigned char *p;
		uint64_t chunk;

		if (done < slen) {
			p = (const unsigned char *)sender + done;
			chunk = slen - done;
		} else if (done < slen + nlen) {
			p = (const unsigned char *)name + (done - slen);
			chunk = slen + nlen - done;
		} else {
			p = (const unsigned char *)data + (done - slen - nlen);
			chunk = slen + nlen + size - done;
		}

		if (chunk > 65536) {
			chunk = 65536;
		}

		ssize_t k = send(fd, p, (size_t)chunk, 0);

		if (k <= 0) {
			close(fd);
			return -1;
		}

		done += (uint64_t)k;
	}

	unsigned char reply[64];
	ssize_t n = recv(fd, reply, sizeof(reply) - 1, 0);
	close(fd);

	if (n <= 0) {
		return -1;
	}

	reply[n] = 0;
	return !strncmp((char *)reply, "OK", 2) ? 0 : -1;
}

static int peer_address(const char *text, uint32_t *ip) {
	struct in_addr a;

	if (inet_pton(AF_INET, text, &a) != 1 || !local_peer(ntohl(a.s_addr))) {
		return -1;
	}

	*ip = ntohl(a.s_addr);
	return 0;
}

// Команда lan msg: отправить текст соседу
static int cmd_msg(int argc, char **argv) {
	uint32_t ip;
	struct utsname identity;

	if (argc < 3 || peer_address(argv[2], &ip) < 0 ||
	    uname(&identity) < 0 || !lan_enabled()) {
		fprintf(stderr, "usage: lan msg IP TEXT (LanDiscovery.Enabled=1)\n");
		return 1;
	}

	char joined[8192] = { 0 };
	size_t used = 0;

	for (int i = 3; i < argc; i++) {
		size_t n = strnlen(argv[i], sizeof(joined) - used - 1);

		if (used + n + 1 >= sizeof(joined)) {
			break;
		}

		if (used) {
			joined[used++] = ' ';
		}

		memcpy(joined + used, argv[i], n);
		used += n;
		joined[used] = 0;
	}

	if (lan_push(ip, "MSG", identity.nodename, "", joined, strlen(joined)) < 0) {
		fprintf(stderr, "lan: message was not accepted\n");
		return 1;
	}

	printf("message sent to %s\n", argv[2]);
	return 0;
}

// Команда lan send: отправить файл соседу потоком
static int cmd_send(int argc, char **argv) {
	uint32_t ip;
	struct utsname identity;

	if (argc != 4 || peer_address(argv[2], &ip) < 0 ||
	    uname(&identity) < 0 || !lan_enabled()) {
		fprintf(stderr, "usage: lan send IP FILE (LanDiscovery.Enabled=1)\n");
		return 1;
	}

	const char *path = argv[3];
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		perror("lan: open");
		return 1;
	}

	struct stat st;

	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
	    (uint64_t)st.st_size > LAN_MAX_FILE) {
		close(fd);
		fprintf(stderr, "lan: not a regular file or too large\n");
		return 1;
	}

	// Для файла используем тот же потоковый протокол, читая кусками
	int tcp = socket(AF_INET, SOCK_STREAM, 0);

	if (tcp < 0) {
		close(fd);
		return 1;
	}

	struct sockaddr_in peer = { .sin_family = AF_INET,
	                            .sin_port = htons(LAN_TCP_PORT),
	                            .sin_addr.s_addr = htonl(ip) };

	if (connect(tcp, (void *)&peer, sizeof(peer)) < 0) {
		perror("lan: connect");
		close(tcp);
		close(fd);
		return 1;
	}

	size_t slen = strnlen(identity.nodename, sizeof(identity.nodename) - 1),
	       nlen = strnlen(base, LAN_MAX_NAME);
	char header[96];
	int hn = snprintf(header, sizeof(header), "SYNLAN2 FILE %zu %zu %lld\n", slen, nlen,
	                  (long long)st.st_size);

	if (hn <= 0 || (size_t)hn >= sizeof(header) ||
	    send(tcp, header, (size_t)hn, 0) != hn ||
	    send(tcp, identity.nodename, slen, 0) != (ssize_t)slen ||
	    send(tcp, base, nlen, 0) != (ssize_t)nlen) {
		close(tcp);
		close(fd);
		fprintf(stderr, "lan: send failed\n");
		return 1;
	}

	unsigned char chunk[65536];
	uint64_t left = (uint64_t)st.st_size;
	int ok = 1;

	while (left) {
		size_t want = left > sizeof(chunk) ? sizeof(chunk) : (size_t)left;
		ssize_t n = read(fd, chunk, want);

		if (n <= 0) {
			ok = 0;
			break;
		}

		size_t done = 0;

		while (done < (size_t)n) {
			ssize_t k = send(tcp, chunk + done, (size_t)n - done, 0);

			if (k <= 0) {
				ok = 0;
				break;
			}

			done += (size_t)k;
		}

		if (!ok) {
			break;
		}

		left -= (uint64_t)n;
	}

	close(fd);

	char reply[64];
	ssize_t n = ok ? recv(tcp, reply, sizeof(reply) - 1, 0) : -1;
	close(tcp);

	if (n <= 0) {
		reply[0] = 0;
	} else {
		reply[n] = 0;
	}

	if (!ok || strncmp(reply, "OK", 2)) {
		fprintf(stderr, "lan: file was not accepted\n");
		return 1;
	}

	printf("file sent to %s\n", argv[2]);
	return 0;
}

// Команда lan inbox: показать файлы, принятые от соседей
static int cmd_inbox(void) {
	DIR *d = opendir(LAN_INBOX);

	if (!d) {
		printf("inbox is empty\n");
		return 0;
	}

	struct dirent *e;
	unsigned total = 0;

	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') {
			continue;
		}

		printf("%s/%s\n", LAN_INBOX, e->d_name);
		total++;
	}

	closedir(d);
	printf("%u files in inbox\n", total);
	return 0;
}

static uint16_t checksum(const unsigned char *p, size_t n) {
	uint32_t sum = 0;

	for (size_t i = 0; i + 1 < n; i += 2) {
		sum += ((unsigned)p[i] << 8) | p[i + 1];
	}

	if (n & 1) {
		sum += (unsigned)p[n - 1] << 8;
	}
	while (sum >> 16) {
		sum = (sum & 65535) + (sum >> 16);
	}

	return (uint16_t)~sum;
}

static int random_nonce(unsigned char *p) {
	int fd = open("/dev/urandom", O_RDONLY);

	if (fd < 0) {
		return -1;
	}

	size_t used = 0;

	while (used < 16) {
		ssize_t n = read(fd, p + used, 16 - used);

		if (n < 0 && errno == EINTR) {
			continue;
		}

		if (n <= 0) {
			close(fd);
			return -1;
		}

		used += (size_t)n;
	}

	close(fd);
	return 0;
}

static int scan(const char *name) {
	struct iface list[8], *selected = NULL;
	size_t count = interfaces(list, 8);

	for (size_t i = 0; i < count; i++) {
		char candidate[64] = { 0 };

		// Без имени интерфейса берём первый поднятый с адресом IPv4
		if (!name || !*name ||
		    (syscall(106001, list[i].index, candidate, sizeof(candidate)) >= 0 &&
		     !strcmp(name, candidate))) {
			selected = &list[i];
			break;
		}
	}

	if (!selected) {
		fprintf(stderr, "lan: interface not UP or IPv4 metadata unavailable\n");
		return 1;
	}

	uint32_t network = selected->ip & selected->mask, hosts = ~selected->mask;

	if (hosts > 255 || hosts < 3) {
		fprintf(stderr, "lan: scan requires an on-link /24 through /30 subnet\n");
		return 1;
	}

	int udp = socket_udp( ), raw = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);

	if (udp < 0 || raw < 0) {
		perror("lan: sockets");

		if (udp >= 0) {
			close(udp);
		}

		if (raw >= 0) {
			close(raw);
		}

		return 1;
	}

	if (fcntl(raw, F_SETFL, O_NONBLOCK) < 0) {
		perror("lan: nonblocking");
		close(raw);
		close(udp);
		return 1;
	}

	struct sockaddr_in own = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(selected->ip) };

	if (bind(udp, (void *)&own, sizeof(own)) < 0 || bind(raw, (void *)&own, sizeof(own)) < 0) {
		perror("lan: interface bind");
		close(raw);
		close(udp);
		return 1;
	}

	unsigned char query[LAN_QUERY_SIZE];
	memcpy(query, "SYNLAN1Q", 8);

	if (random_nonce(query + 8) < 0) {
		perror("lan: random nonce");
		close(raw);
		close(udp);
		return 1;
	}

	unsigned char echo[24] = { 8, 0 };
	memcpy(echo + 8, query + 8, 16);
	echo[4] = query[8];
	echo[5] = query[9];
	unsigned char found[256] = { 0 }, synapse[256] = { 0 };
	char names[256][64] = { { 0 } }, versions[256][16] = { { 0 } };

	for (uint32_t host = 1; host < hosts; host++) {
		if (network + host == selected->ip) {
			continue;
		}

		struct sockaddr_in peer = { .sin_family = AF_INET,
			                        .sin_port = htons(LAN_PORT),
			                        .sin_addr.s_addr = htonl(network + host) };
		echo[6] = (unsigned char)(host >> 8);
		echo[7] = (unsigned char)host;
		echo[2] = echo[3] = 0;
		uint16_t sum = checksum(echo, sizeof(echo));
		echo[2] = (unsigned char)(sum >> 8);
		echo[3] = (unsigned char)sum;

		if (sendto(raw, echo, sizeof(echo), 0, (void *)&peer, sizeof(peer)) < 0 &&
		    errno != EAGAIN) {
			perror("lan: ICMP probe");
		}

		if (sendto(udp, query, sizeof(query), 0, (void *)&peer, sizeof(peer)) < 0 &&
		    errno != EAGAIN) {
			perror("lan: identity probe");
		}

		struct timespec pace = { 0, 5000000 };
		nanosleep(&pace, NULL);
	}

	uint64_t start = now( );

	if (!start) {
		close(raw);
		close(udp);
		return 1;
	}

	for (unsigned rounds = 0; rounds < 300 && now( ) - start < 3000; rounds++) {
		struct pollfd p[2] = { { raw, POLLIN, 0 }, { udp, POLLIN, 0 } };

		if (poll(p, 2, 100) < 0) {
			if (errno == EINTR) {
				continue;
			}

			break;
		}

		for (int i = 0; i < 2; i++) {
			if (p[i].revents & POLLIN) {
				unsigned char data[2048];
				struct sockaddr_in peer;
				socklen_t len = sizeof(peer);
				ssize_t n = recvfrom(p[i].fd, data, sizeof(data), 0, (void *)&peer, &len);

				if (n < 0 || len != sizeof(peer) || peer.sin_family != AF_INET) {
					continue;
				}

				uint32_t ip = ntohl(peer.sin_addr.s_addr), host = ip - network;

				if (!lan_onlink(ip, selected->ip, selected->mask) || host >= 256 ||
				    ip == selected->ip) {
					continue;
				}

				if (i == 1) {
					if (peer.sin_port != htons(LAN_PORT) ||
					    !lan_reply_valid(data, (size_t)n, query + 8)) {
						continue;
					}

					found[host] = synapse[host] = 1;
					lan_safe_text(names[host], data + 24, 63);
					lan_safe_text(versions[host], data + 88, 15);
				} else {
					size_t off = 0;

					if (n >= 20 && (data[0] >> 4) == 4) {
						off = (data[0] & 15) * 4;

						if (off < 20 || off > (size_t)n || data[9] != IPPROTO_ICMP ||
						    ((size_t)data[2] << 8 | data[3]) != (size_t)n ||
						    memcmp(data + 12, &peer.sin_addr.s_addr, 4)) {
							continue;
						}
					}

					if ((size_t)n - off != 24 || data[off] != 0 || data[off + 1] != 0 ||
					    checksum(data + off, 24) != 0 || data[off + 4] != echo[4] ||
					    data[off + 5] != echo[5] || data[off + 6] != (host >> 8) ||
					    data[off + 7] != (host & 255) || memcmp(data + off + 8, query + 8, 16)) {
						continue;
					}

					found[host] = 1;
				}
			}
		}
	}

	unsigned total = 0;

	for (uint32_t host = 1; host < hosts; host++) {
		if (found[host]) {
			struct in_addr ip = { htonl(network + host) };
			char text[16];
			inet_ntop(AF_INET, &ip, text, sizeof(text));
			printf("%s%s", text,
			       synapse[host] ? "  SynapseOS (self-reported)" : "  ICMP reachable");
			if (synapse[host]) {
				printf("  %s %s", names[host], versions[host]);
			}

			putchar('\n');
			total++;
		}
	}

	printf("%u devices responded; silent hosts may still be present.\n", total);
	close(raw);
	close(udp);
	return 0;
}

int main(int argc, char **argv) {
	signal(SIGPIPE, SIG_IGN);

	if (argc == 2 && !strcmp(argv[1], "serve")) {
		return serve( );
	}

	if (argc >= 2 && !strcmp(argv[1], "scan")) {
		return scan(argc == 3 ? argv[2] : "");
	}

	if (argc >= 3 && !strcmp(argv[1], "msg")) {
		return cmd_msg(argc, argv);
	}

	if (argc == 4 && !strcmp(argv[1], "send")) {
		return cmd_send(argc, argv);
	}

	if (argc == 2 && !strcmp(argv[1], "inbox")) {
		return cmd_inbox( );
	}

	fprintf(stderr, "usage: lan scan [INTERFACE] | lan serve | lan msg IP TEXT | "
	                "lan send IP FILE | lan inbox\n");
	return 1;
}
