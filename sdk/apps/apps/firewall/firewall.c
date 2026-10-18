// firewall.c: Управление сетевым фильтром
// Автор: Елчинян Арен Артурович, 2026
// Проверка правил и передача конфигурации через управляющее устройство
// Подсистема: пользовательские системные утилиты
// Создан: 12.10.2026
// Последнее изменение: 18.10.2026

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <dev/firewall.h>
// Проверка пользовательских полей реализована отдельно от фильтра ядра
bool syn_fw_validate(const struct syn_fw_config *config);

static int number(const char *text, unsigned maximum, unsigned *value) {
	if (!text[0]) {
		return -1;
	}

	unsigned n = 0;

	for (; *text; text++) {
		if (*text < '0' || *text > '9' || n > maximum / 10) {
			return -1;
		}

		n = n * 10 + *text - '0';

		if (n > maximum) {
			return -1;
		}
	}

	*value = n;
	return 0;
}

static int port(char *text, uint16_t *lo, uint16_t *hi) {
	if (!strcmp(text, "any")) {
		*lo = 0;
		*hi = 65535;
		return 0;
	}

	char *colon = strchr(text, ':');
	unsigned l, h;

	if (colon) {
		*colon++ = 0;
	}

	if (number(text, 65535, &l) < 0 || (colon && number(colon, 65535, &h) < 0)) {
		return -1;
	}

	if (!colon) {
		h = l;
	}

	if (l > h) {
		return -1;
	}

	*lo = l;
	*hi = h;
	return 0;
}

static int address(char *text, unsigned char *out, uint8_t *prefix, uint8_t *family) {
	if (!strcmp(text, "any")) {
		*prefix = 0;
		return 0;
	}

	char *slash = strchr(text, '/');

	if (slash) {
		*slash++ = 0;
	}

	unsigned f = strchr(text, ':') ? 6 : 4, bits = f == 4 ? 32 : 128;

	if ((*family && *family != f) || inet_pton(f == 4 ? AF_INET : AF_INET6, text, out) != 1 ||
	    (slash && number(slash, bits, &bits) < 0)) {
		return -1;
	}

	*family = f;
	*prefix = bits;
	return 0;
}

static int action(const char *t) {
	return !strcmp(t, "allow") ? SYN_FW_ALLOW : !strcmp(t, "drop") ? SYN_FW_DROP : -1;
}

static int direction(const char *t) {
	return !strcmp(t, "in") ? SYN_FW_IN : !strcmp(t, "out") ? SYN_FW_OUT : -1;
}

static int protocol(const char *t) {
	const char *names[] = { "any", "icmp", "tcp", "udp", "icmp6" };
	const int codes[] = { 0, 1, 6, 17, 58 };

	for (unsigned i = 0; i < 5; i++) {
		if (!strcmp(t, names[i])) {
			return codes[i];
		}
	}

	return -1;
}

static int parse(const char *path, struct syn_fw_config *c) {
	FILE *f = fopen(path, "r");

	if (!f) {
		perror(path);
		return -1;
	}

	memset(c, 0, sizeof(*c));
	c->version = SYN_FW_VERSION;
	c->policy[0] = c->policy[1] = SYN_FW_ALLOW;
	char line[1024];
	unsigned lineno = 0, enabled_seen = 0, policy_seen[2] = { 0 };
	int result = -1;

	while (fgets(line, sizeof(line), f)) {
		lineno++;

		if (!strchr(line, '\n') && !feof(f)) {
			goto invalid;
		}

		char *comment = strchr(line, '#');

		if (comment) {
			*comment = 0;
		}

		char *tokens[8], *save = NULL;
		unsigned count = 0;

		for (char *t = strtok_r(line, " \t\r\n", &save); t; t = strtok_r(NULL, " \t\r\n", &save)) {
			if (count == 8) {
				goto invalid;
			}

			tokens[count++] = t;
		}

		if (!count) {
			continue;
		}

		if (count == 2 && !strcmp(tokens[0], "enabled")) {
			unsigned enabled;

			if (enabled_seen++ || number(tokens[1], 1, &enabled) < 0) {
				goto invalid;
			}

			c->enabled = enabled;
		} else if (count == 3 && !strcmp(tokens[0], "policy")) {
			int d = direction(tokens[1]), a = action(tokens[2]);

			if (d < 0 || a < 0 || policy_seen[d]++) {
				goto invalid;
			}

			c->policy[d] = a;
		} else if (count == 7) {
			if (c->count == SYN_FW_MAX_RULES) {
				goto invalid;
			}

			struct syn_fw_rule *r = &c->rules[c->count++];
			int a = action(tokens[0]), d = direction(tokens[1]), p = protocol(tokens[2]);

			if (a < 0 || d < 0 || p < 0) {
				goto invalid;
			}

			r->action = a;
			r->direction = d;
			r->protocol = p;

			if (address(tokens[3], r->src, &r->src_prefix, &r->family) < 0 ||
			    address(tokens[4], r->dst, &r->dst_prefix, &r->family) < 0 ||
			    port(tokens[5], &r->sport_lo, &r->sport_hi) < 0 ||
			    port(tokens[6], &r->dport_lo, &r->dport_hi) < 0) {
				goto invalid;
			}
		} else {
			goto invalid;
		}
	}

	if (ferror(f)) {
		perror(path);
		goto done;
	}
	// Требуем явный режим и обе политики, чтобы неполный файл не открывал доступ
	if (!enabled_seen || !policy_seen[0] || !policy_seen[1] || !syn_fw_validate(c)) {
		goto invalid;
	}

	result = 0;
	goto done;
invalid:
	fprintf(stderr, "firewall: invalid configuration at %s:%u (see firewall help)\n", path, lineno);
done:
	fclose(f);
	return result;
}

static const char *proto_name(unsigned p) {
	switch (p) {
		case 0:
			return "any";
		case 1:
			return "icmp";
		case 6:
			return "tcp";
		case 17:
			return "udp";
		case 58:
			return "icmp6";
		default:
			return "?";
	}
}

static void format_addr(char *out, size_t cap, const unsigned char *addr, unsigned family,
                        unsigned bits) {
	if (!family) {
		snprintf(out, cap, "any");
		return;
	}

	char text[INET6_ADDRSTRLEN];
	inet_ntop(family == 4 ? AF_INET : AF_INET6, addr, text, sizeof(text));
	snprintf(out, cap, "%s/%u", text, bits);
}

static void format_port(char *out, size_t cap, unsigned lo, unsigned hi) {
	if (!lo && hi == 65535) {
		snprintf(out, cap, "any");
	} else if (lo == hi) {
		snprintf(out, cap, "%u", lo);
	} else {
		snprintf(out, cap, "%u:%u", lo, hi);
	}
}

static int write_config(FILE *f, const struct syn_fw_config *c, int counters) {
	fprintf(f, "enabled %u\npolicy in %s\npolicy out %s\n", c->enabled,
	        c->policy[0] ? "allow" : "drop", c->policy[1] ? "allow" : "drop");
	if (counters) {
		fprintf(f,
		        "# counters in: allow=%llu drop=%llu malformed/unsupported=%llu; out: "
		        "allow=%llu drop=%llu malformed/unsupported=%llu\n",
		        (unsigned long long)c->allowed[0], (unsigned long long)c->dropped[0],
		        (unsigned long long)c->malformed[0], (unsigned long long)c->allowed[1],
		        (unsigned long long)c->dropped[1], (unsigned long long)c->malformed[1]);
	}

	for (unsigned i = 0; i < c->count; i++) {
		const struct syn_fw_rule *r = &c->rules[i];
		char src[64], dst[64], sport[16], dport[16];
		format_addr(src, sizeof(src), r->src, r->family, r->src_prefix);
		format_addr(dst, sizeof(dst), r->dst, r->family, r->dst_prefix);
		format_port(sport, sizeof(sport), r->sport_lo, r->sport_hi);
		format_port(dport, sizeof(dport), r->dport_lo, r->dport_hi);
		fprintf(f, "%s %s %s %s %s %s %s", r->action ? "allow" : "drop",
		        r->direction ? "out" : "in", proto_name(r->protocol), src, dst, sport, dport);
		if (counters) {
			fprintf(f, " # packets=%llu bytes=%llu", (unsigned long long)r->packets,
			        (unsigned long long)r->bytes);
		}

		fputc('\n', f);
	}

	return ferror(f) ? -1 : 0;
}

static void usage(void) {
	puts("usage: firewall status | load FILE | check FILE | save FILE | disable | "
	     "reset-counters\n"
	     "Config requires: enabled 0|1; policy in allow|drop; policy out allow|drop\n"
	     "Rules: allow|drop in|out any|tcp|udp|icmp|icmp6 SRC DST SPORT DPORT\n"
	     "Addresses: any or IPv4/IPv6[/prefix]; ports: any, NUMBER, LOW:HIGH.\n"
	     "First matching rule wins. Stateless: reply traffic needs explicit rules.\n"
	     "ARP passes; enabled filtering drops IP fragments and unsupported frames.\n"
	     "Rules load atomically; no reject, conntrack or NAT in this version.");
}

int main(int argc, char **argv) {
	if (argc == 2 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help"))) {
		usage( );
		return 0;
	}

	if (argc < 2) {
		usage( );
		return 2;
	}

	int load = !strcmp(argv[1], "load"), check = !strcmp(argv[1], "check"),
	    save = !strcmp(argv[1], "save");
	int status = !strcmp(argv[1], "status"), disable = !strcmp(argv[1], "disable"),
	    reset = !strcmp(argv[1], "reset-counters");
	if ((load || check || save) ? argc != 3 : argc != 2 || !(status || disable || reset)) {
		usage( );
		return 2;
	}

	struct syn_fw_config *c = calloc(1, sizeof(*c));

	if (!c) {
		perror("firewall");
		return 1;
	}

	int result = 1, fd = -1;

	if (load || check) {
		if (parse(argv[2], c) < 0) {
			goto done;
		}

		if (check) {
			puts("Configuration valid.");
			result = 0;
			goto done;
		}
	}

	fd = open("/dev/firewall", load || disable || reset ? O_RDWR : O_RDONLY);

	if (fd < 0) {
		perror("firewall: /dev/firewall");
		goto done;
	}

	if (reset) {
		if (ioctl(fd, SYN_FW_RESET_COUNTERS, 0) < 0) {
			perror("firewall: reset");
			goto done;
		}
	} else {
		if (!load && ioctl(fd, SYN_FW_GET, c) < 0) {
			perror("firewall: status");
			goto done;
		}

		if (disable) {
			c->enabled = 0;
		}

		if ((load || disable) && ioctl(fd, SYN_FW_REPLACE, c) < 0) {
			perror("firewall: replace");
			goto done;
		}

		if (status) {
			if (write_config(stdout, c, 1) < 0) {
				goto done;
			}
		}

		if (save) {
			size_t cap = strlen(argv[2]) + 40;
			char *tmp = malloc(cap);

			if (!tmp) {
				goto done;
			}

			snprintf(tmp, cap, "%s.tmp.%ld", argv[2], (long)getpid( ));
			int out = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
			FILE *f = out < 0 ? NULL : fdopen(out, "w");
			int failed = 1;

			if (f) {
				failed = write_config(f, c, 0) < 0;

				if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
					failed = 1;
				}

				if (fclose(f) != 0) {
					failed = 1;
				}
			} else if (out >= 0) {
				close(out);
			}

			if (!failed && rename(tmp, argv[2]) == 0) {
				failed = 0;
			} else {
				failed = 1;
			}

			if (failed) {
				perror("firewall: save");
				unlink(tmp);
				free(tmp);
				goto done;
			}

			free(tmp);
		}
	}

	result = 0;
done:
	if (fd >= 0) {
		close(fd);
	}

	free(c);
	return result;
}
