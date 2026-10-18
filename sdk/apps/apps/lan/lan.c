// lan.c: Обнаружение узлов локальной сети и ответы на запросы
// Автор: Елчинян Арен Артурович, 2026
// Сканер использует ICMP и запросы идентификации, служба отвечает соседям в своей
// подсети Подсистема: пользовательские сетевые утилиты Первое изменение: до текущей
// уборки, исходная дата не установлена Последнее изменение: 14.10.2026 Доступ:
// общедоступный

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "protocol.h"

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

static int socket_udp(void) {
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (fd >= 0 && fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

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

	signal(SIGTERM, stop);
	signal(SIGINT, stop);
	struct utsname identity;

	if (uname(&identity) < 0) {
		close(fd);
		return 1;
	}

	if (strcmp(identity.sysname, "SynapseOS")) {
		fprintf(stderr, "lan: identity service requires SynapseOS\n");
		close(fd);
		return 1;
	}

	unsigned char reply[LAN_REPLY_SIZE] = { 0 };
	memcpy(reply, "SYNLAN1R", 8);
	memcpy(reply + 24, identity.nodename, strnlen(identity.nodename, 63));
	memcpy(reply + 88, identity.release, strnlen(identity.release, 15));
	uint64_t period = now( );
	unsigned accepted = 0;

	while (running) {
		struct pollfd p = { fd, POLLIN, 0 };
		int r = poll(&p, 1, 1000);

		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}

			perror("lan: poll");
			break;
		}

		if (!r) {
			continue;
		}

		unsigned char query[LAN_QUERY_SIZE + 1];
		struct sockaddr_in peer;
		socklen_t len = sizeof(peer);
		ssize_t n = recvfrom(fd, query, sizeof(query), 0, (void *)&peer, &len);

		if (n < 0) {
			continue;
		}

		uint64_t t = now( );

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
	return running ? 1 : 0;
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

		if (syscall(106001, list[i].index, candidate, sizeof(candidate)) >= 0 &&
		    !strcmp(name, candidate)) {
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
	if (argc == 2 && !strcmp(argv[1], "serve")) {
		return serve( );
	}

	if (argc == 3 && !strcmp(argv[1], "scan")) {
		return scan(argv[2]);
	}

	fprintf(stderr, "usage: lan scan INTERFACE | lan serve\n");
	return 1;
}
