#define _GNU_SOURCE
#include "wireguard.h"
#include "crypto.h"
#include <dev/tun.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <arpa/inet.h>

struct settings {
	uint8_t private[32], public[32], psk[32];
	uint32_t address, mask, allowed, allowed_mask;
	unsigned listen, mtu, keepalive;
	struct sockaddr_in endpoint;
};

struct engine {
	struct wireguard_device device;
	struct settings cfg;
	int udp, tun;
	struct sockaddr_in endpoint;
	uint8_t pending[1500];
	size_t pending_size;
	uint32_t last_handshake;
	unsigned request_second, requests;
};

static volatile sig_atomic_t stop;

static void stopped(int signal) {
	(void)signal;
	stop = 1;
}

uint32_t wireguard_sys_now(void) {
	struct timespec t;

	if (clock_gettime(CLOCK_MONOTONIC, &t) < 0) {
		perror("wireguard clock");
		exit(1);
	}

	return (uint32_t)((uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

void wireguard_random_bytes(void *out, size_t n) {
	size_t done = 0;

	while (done < n) {
		ssize_t got = getrandom((char *)out + done, n - done, 0);

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0) {
			perror("wireguard: secure randomness unavailable");
			exit(1);
		}

		done += got;
	}
}

void wireguard_tai64n_now(uint8_t *out) {
	static uint64_t last_sec;
	static uint32_t last_nano;
	struct timespec t;

	if (clock_gettime(CLOCK_REALTIME, &t) < 0) {
		perror("wireguard realtime");
		exit(1);
	}

	uint64_t sec = 0x400000000000000aULL + t.tv_sec;
	uint32_t nano = t.tv_nsec;

	if (sec < last_sec || (sec == last_sec && nano <= last_nano)) {
		sec = last_sec;
		nano = last_nano + 1;

		if (nano >= 1000000000) {
			sec++;
			nano = 0;
		}
	}

	last_sec = sec;
	last_nano = nano;
	U64TO8_BIG(out, sec);
	U32TO8_BIG(out + 8, nano);
}

bool wireguard_is_under_load(void) {
	return false;
}

static unsigned be16(const uint8_t *b) {
	return ((unsigned)b[0] << 8) | b[1];
}

static int number(const char *s, unsigned max, unsigned *v) {
	if (!*s)
		return -1;
	unsigned n = 0;

	for (; *s; s++) {
		if (*s < '0' || *s > '9' || n > max / 10)
			return -1;
		n = n * 10 + *s - '0';

		if (n > max)
			return -1;
	}

	*v = n;
	return 0;
}

static char *trim(char *s) {
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);

	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

static bool key(const char *text, uint8_t *out) {
	if (strlen(text) != 44)
		return false;
	size_t n = 32;

	if (!wireguard_base64_decode(text, out, &n) || n != 32)
		return false;
	char canonical[45];
	n = sizeof(canonical);

	if (!wireguard_base64_encode(out, 32, canonical, &n))
		return false;
	return !strcmp(text, canonical);
}

static bool subnet(char *text, uint32_t *ip, uint32_t *mask) {
	char *slash = strchr(text, '/');

	if (!slash)
		return false;
	*slash++ = 0;
	unsigned prefix;

	if (number(slash, 32, &prefix) < 0 || !prefix || inet_pton(AF_INET, text, ip) != 1)
		return false;
	*mask = htonl(prefix == 32 ? 0xffffffffu : 0xffffffffu << (32 - prefix));
	return true;
}

static bool endpoint(char *text, struct sockaddr_in *out) {
	char *colon = strrchr(text, ':');

	if (!colon)
		return false;
	*colon++ = 0;
	unsigned port;

	if (number(colon, 65535, &port) < 0 || !port || inet_pton(AF_INET, text, &out->sin_addr) != 1)
		return false;
	out->sin_family = AF_INET;
	out->sin_port = htons(port);
	return true;
}

static bool nonzero(const uint8_t *key) {
	uint8_t n = 0;

	for (unsigned i = 0; i < 32; i++)
		n |= key[i];
	return n != 0;
}

static bool parse(const char *path, struct settings *c) {
	FILE *f = fopen(path, "r");

	if (!f) {
		perror(path);
		return false;
	}

	memset(c, 0, sizeof(*c));
	c->mtu = 1420;
	char line[512];
	unsigned section = 0, seen = 0, lineno = 0;
	bool ok = false;

	while (fgets(line, sizeof(line), f)) {
		lineno++;

		if (!strchr(line, '\n') && !feof(f))
			goto invalid;
		char *comment = strchr(line, '#');

		if (comment)
			*comment = 0;
		char *s = trim(line);

		if (!*s)
			continue;
		if (!strcmp(s, "[Interface]")) {
			if (section)
				goto invalid;
			section = 1;
			continue;
		}

		if (!strcmp(s, "[Peer]")) {
			if (section != 1)
				goto invalid;
			section = 2;
			continue;
		}

		char *equals = strchr(s, '=');

		if (!equals || !section)
			goto invalid;
		*equals++ = 0;
		char *name = trim(s), *value = trim(equals);
		unsigned bit = 0;

		if (section == 1 && !strcmp(name, "PrivateKey")) {
			bit = 1;

			if (!key(value, c->private) || !nonzero(c->private))
				goto invalid;
		} else if (section == 1 && !strcmp(name, "Address")) {
			bit = 2;

			if (!subnet(value, &c->address, &c->mask))
				goto invalid;
		} else if (section == 1 && !strcmp(name, "ListenPort")) {
			bit = 4;

			if (number(value, 65535, &c->listen) < 0)
				goto invalid;
		} else if (section == 1 && !strcmp(name, "MTU")) {
			bit = 8;

			if (number(value, 1420, &c->mtu) < 0 || c->mtu < 576)
				goto invalid;
		} else if (section == 2 && !strcmp(name, "PublicKey")) {
			bit = 16;

			if (!key(value, c->public) || !nonzero(c->public))
				goto invalid;
		} else if (section == 2 && !strcmp(name, "PresharedKey")) {
			bit = 32;

			if (!key(value, c->psk))
				goto invalid;
		} else if (section == 2 && !strcmp(name, "Endpoint")) {
			bit = 64;

			if (!endpoint(value, &c->endpoint))
				goto invalid;
		} else if (section == 2 && !strcmp(name, "AllowedIPs")) {
			bit = 128;

			if (!subnet(value, &c->allowed, &c->allowed_mask))
				goto invalid;
		} else if (section == 2 && !strcmp(name, "PersistentKeepalive")) {
			bit = 256;

			if (number(value, 65535, &c->keepalive) < 0)
				goto invalid;
		} else
			goto invalid;
		if (seen & bit)
			goto invalid;
		seen |= bit;
	}

	if (ferror(f)) {
		perror(path);
		goto done;
	}

	if ((seen & (1 | 2 | 16 | 64 | 128)) != (1 | 2 | 16 | 64 | 128))
		goto invalid;
	/* Native TUN only routes its configured IPv4 subnet. No default/full tunnels. */
	if ((c->allowed & c->mask) != (c->address & c->mask) ||
	    (ntohl(c->allowed_mask) & ntohl(c->mask)) != ntohl(c->mask) ||
	    (c->endpoint.sin_addr.s_addr & c->mask) == (c->address & c->mask))
		goto invalid;
	ok = true;
	goto done;
invalid:
	fprintf(stderr, "wireguard: invalid/unsupported config at %s:%u\n", path, lineno);
done:
	fclose(f);
	return ok;
}

static bool safe_endpoint(struct engine *e, const struct sockaddr_in *addr) {
	return addr->sin_family == AF_INET && addr->sin_port &&
	       (addr->sin_addr.s_addr & e->cfg.mask) != (e->cfg.address & e->cfg.mask);
}

static bool send_udp(struct engine *e, const void *data, size_t n) {
	return sendto(e->udp, data, n, 0, (struct sockaddr *)&e->endpoint, sizeof(e->endpoint)) ==
	       (ssize_t)n;
}

static struct wireguard_keypair *sending(struct wireguard_peer *p) {
	struct wireguard_keypair *k = &p->curr_keypair;

	if (k->valid && !k->initiator && !k->last_rx)
		k = &p->prev_keypair;
	return k;
}

static bool transport(struct engine *e, const uint8_t *plain, size_t n) {
	struct wireguard_peer *p = &e->device.peers[0];
	struct wireguard_keypair *k = sending(p);

	if (!k->valid || !k->sending_valid || wireguard_expired(k->keypair_millis, REJECT_AFTER_TIME) ||
	    k->sending_counter >= REJECT_AFTER_MESSAGES)
		return false;
	uint8_t out[1536] = { 0 };
	size_t padded = (n + 15) & ~15u;

	if (padded + 32 > sizeof(out))
		return false;
	out[0] = MESSAGE_TRANSPORT_DATA;
	U32TO8_LITTLE(out + 4, k->remote_index);
	U64TO8_LITTLE(out + 8, k->sending_counter);

	if (n)
		memcpy(out + 16, plain, n);
	wireguard_encrypt_packet(out + 16, out + 16, padded, k);

	if (!send_udp(e, out, padded + 32))
		return false;
	k->last_tx = p->last_tx = wireguard_sys_now( );

	if (k->sending_counter >= REKEY_AFTER_MESSAGES ||
	    (k->initiator && wireguard_expired(k->keypair_millis, REKEY_AFTER_TIME)))
		p->send_handshake = true;
	return true;
}

static void initiation(struct engine *e) {
	struct wireguard_peer *p = &e->device.peers[0];
	struct message_handshake_initiation msg;

	if (wireguard_create_handshake_initiation(&e->device, p, &msg) &&
	    send_udp(e, &msg, sizeof(msg))) {
		e->last_handshake = wireguard_sys_now( );
		p->last_initiation_tx = e->last_handshake;
		p->send_handshake = false;
		memcpy(p->handshake_mac1, msg.mac1, 16);
		p->handshake_mac1_valid = true;
	}
}

static void input_udp(struct engine *e) {
	uint8_t in[1600], plain[1600];
	struct sockaddr_in addr;
	socklen_t alen = sizeof(addr);
	ssize_t n = recvfrom(e->udp, in, sizeof(in), 0, (struct sockaddr *)&addr, &alen);

	if (n < 4 || !safe_endpoint(e, &addr) || in[1] || in[2] || in[3])
		return;
	struct wireguard_peer *p = &e->device.peers[0];

	if (in[0] == MESSAGE_HANDSHAKE_INITIATION && n == sizeof(struct message_handshake_initiation)) {
		struct message_handshake_initiation *msg = (void *)in;

		if (!wireguard_check_mac1(&e->device, in, n - 32, msg->mac1))
			return;
		unsigned sec = wireguard_sys_now( ) / 1000;

		if (sec != e->request_second) {
			e->request_second = sec;
			e->requests = 0;
		}

		uint8_t source[6];
		memcpy(source, &addr.sin_addr, 4);
		memcpy(source + 4, &addr.sin_port, 2);

		if (++e->requests > 8 &&
		    !wireguard_check_mac2(&e->device, in, n - 16, source, 6, msg->mac2)) {
			struct message_cookie_reply cookie;
			wireguard_create_cookie_reply(&e->device, &cookie, msg->mac1, msg->sender, source, 6);
			sendto(e->udp, &cookie, sizeof(cookie), 0, (struct sockaddr *)&addr, sizeof(addr));
			return;
		}

		if (wireguard_process_initiation_message(&e->device, msg) != p)
			return;
		e->endpoint = addr;
		struct message_handshake_response response;

		if (wireguard_create_handshake_response(&e->device, p, &response)) {
			wireguard_start_session(p, false);
			send_udp(e, &response, sizeof(response));
		}
	} else if (in[0] == MESSAGE_HANDSHAKE_RESPONSE &&
	           n == sizeof(struct message_handshake_response)) {
		struct message_handshake_response *msg = (void *)in;

		if (!wireguard_check_mac1(&e->device, in, n - 32, msg->mac1) ||
		    peer_lookup_by_handshake(&e->device, msg->receiver) != p)
			return;
		if (wireguard_process_handshake_response(&e->device, p, msg)) {
			e->endpoint = addr;
			wireguard_start_session(p, true);
			transport(e, NULL, 0);
			fprintf(stderr, "wireguard: authenticated handshake\n");
		}
	} else if (in[0] == MESSAGE_COOKIE_REPLY && n == sizeof(struct message_cookie_reply)) {
		struct message_cookie_reply *msg = (void *)in;

		if (peer_lookup_by_handshake(&e->device, msg->receiver) == p &&
		    wireguard_process_cookie_message(&e->device, p, msg))
			p->send_handshake = true;
	} else if (in[0] == MESSAGE_TRANSPORT_DATA && n >= 32 && ((n - 32) & 15) == 0) {
		uint32_t receiver = U8TO32_LITTLE(in + 4);
		uint64_t counter = U8TO64_LITTLE(in + 8);
		struct wireguard_keypair *k = get_peer_keypair_for_idx(p, receiver);

		if (!k || !k->valid || !k->receiving_valid ||
		    wireguard_expired(k->keypair_millis, REJECT_AFTER_TIME) ||
		    counter >= REJECT_AFTER_MESSAGES)
			return;
		if (!wireguard_decrypt_packet(plain, in + 16, n - 16, counter, k) ||
		    !wireguard_check_replay(k, counter))
			return;
		k->last_rx = p->last_rx = wireguard_sys_now( );
		e->endpoint = addr;
		keypair_update(p, k);
		size_t length = n - 32;

		if (!length)
			return;
		if (length < 20 || (plain[0] >> 4) != 4)
			return;
		unsigned header = (plain[0] & 15u) * 4, total = be16(plain + 2);

		if (header < 20 || header > total || total > length || total > e->cfg.mtu)
			return;
		uint32_t source;
		memcpy(&source, plain + 12, 4);

		if ((source & e->cfg.allowed_mask) != (e->cfg.allowed & e->cfg.allowed_mask))
			return;
		if (write(e->tun, plain, total) != (ssize_t)total && errno != EAGAIN)
			perror("wireguard tun write");
	}
}

static void input_tun(struct engine *e) {
	uint8_t plain[1500];
	ssize_t n = read(e->tun, plain, sizeof(plain));

	if (n < 20 || (plain[0] >> 4) != 4 || n > e->cfg.mtu || be16(plain + 2) != n)
		return;
	uint32_t dst;
	memcpy(&dst, plain + 16, 4);

	if ((dst & e->cfg.allowed_mask) != (e->cfg.allowed & e->cfg.allowed_mask))
		return;
	if (!transport(e, plain, n)) {
		memcpy(e->pending, plain, n);
		e->pending_size = n;
		e->device.peers[0].send_handshake = true;
	}
}

static void tick(struct engine *e) {
	struct wireguard_peer *p = &e->device.peers[0];
	uint32_t now = wireguard_sys_now( );
	struct wireguard_keypair *keys[] = { &p->curr_keypair, &p->prev_keypair, &p->next_keypair };

	for (unsigned i = 0; i < 3; i++)
		if (keys[i]->valid && wireguard_expired(keys[i]->keypair_millis, REJECT_AFTER_TIME))
			keypair_destroy(keys[i]);
	struct wireguard_keypair *k = sending(p);

	if (k->valid && k->initiator && wireguard_expired(k->keypair_millis, REKEY_AFTER_TIME))
		p->send_handshake = true;
	if ((!k->valid || p->send_handshake) &&
	    (!e->last_handshake || (uint32_t)(now - e->last_handshake) >= REKEY_TIMEOUT * 1000))
		initiation(e);
	if (e->pending_size && transport(e, e->pending, e->pending_size))
		e->pending_size = 0;
	if (e->cfg.keepalive && k->valid && (uint32_t)(now - p->last_tx) >= e->cfg.keepalive * 1000)
		transport(e, NULL, 0);
}

static int run(const char *path, int supplied_tun) {
	struct engine e;
	memset(&e, 0, sizeof(e));

	if (!parse(path, &e.cfg))
		return 1;
	wireguard_init( );

	if (!wireguard_device_init(&e.device, e.cfg.private) ||
	    !wireguard_peer_init(&e.device, &e.device.peers[0], e.cfg.public, e.cfg.psk)) {
		fprintf(stderr, "wireguard: invalid key agreement\n");
		return 1;
	}

	crypto_zero(e.cfg.private, 32);
	e.endpoint = e.cfg.endpoint;
	e.tun = supplied_tun >= 0 ? supplied_tun : open("/dev/tun", O_RDWR | O_NONBLOCK);

	if (e.tun < 0) {
		perror("wireguard tun");
		return 1;
	}

	if (supplied_tun < 0) {
		struct syn_tun_config tun = { .version = SYN_TUN_VERSION,
			                          .address = e.cfg.address,
			                          .netmask = e.cfg.mask,
			                          .mtu = e.cfg.mtu };
		strcpy(tun.name, "wg0");

		if (ioctl(e.tun, SYN_TUN_CONFIG, &tun) < 0) {
			perror("wireguard tun config");
			close(e.tun);
			return 1;
		}
	}

	e.udp = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);

	if (e.udp < 0) {
		perror("wireguard UDP");
		close(e.tun);
		return 1;
	}

	struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(e.cfg.listen) };

	if (bind(e.udp, (struct sockaddr *)&local, sizeof(local)) < 0) {
		perror("wireguard bind");
		close(e.udp);
		close(e.tun);
		return 1;
	}

	signal(SIGINT, stopped);
	signal(SIGTERM, stopped);
	struct pollfd fds[2] = { { e.udp, POLLIN, 0 }, { e.tun, POLLIN, 0 } };
	fprintf(stderr, "wireguard: IPv4 split-tunnel worker started\n");

	while (!stop) {
		tick(&e);
		int ready = poll(fds, 2, 100);

		if (ready < 0 && errno != EINTR) {
			perror("wireguard poll");
			break;
		}

		if (fds[0].revents & POLLIN)
			input_udp(&e);
		if (fds[1].revents & POLLIN)
			input_tun(&e);
		if ((fds[0].revents | fds[1].revents) & (POLLERR | POLLHUP | POLLNVAL))
			break;
	}

	close(e.udp);
	close(e.tun);
	crypto_zero(&e, sizeof(e));
	return stop ? 0 : 1;
}

int main(int argc, char **argv) {
	if (argc == 2 && !strcmp(argv[1], "genkey")) {
		uint8_t key[32];
		char text[45];
		size_t n = sizeof(text);
		wireguard_random_bytes(key, 32);
		key[0] &= 248;
		key[31] = (key[31] & 127) | 64;
		wireguard_base64_encode(key, 32, text, &n);
		puts(text);
		crypto_zero(key, 32);
		return 0;
	}

	if (argc == 2 && !strcmp(argv[1], "pubkey")) {
		char text[128];
		uint8_t keybuf[32];

		if (!fgets(text, sizeof(text), stdin) || !key(trim(text), keybuf)) {
			fprintf(stderr, "wireguard: expected base64 private key on stdin\n");
			return 1;
		}

		struct wireguard_device d;
		memset(&d, 0, sizeof(d));
		wireguard_init( );

		if (!wireguard_device_init(&d, keybuf))
			return 1;
		char out[45];
		size_t n = sizeof(out);
		wireguard_base64_encode(d.public_key, 32, out, &n);
		puts(out);
		crypto_zero(&d, sizeof(d));
		crypto_zero(keybuf, 32);
		return 0;
	}

	if (argc == 3 && !strcmp(argv[1], "check")) {
		struct settings s;
		bool ok = parse(argv[2], &s);
		crypto_zero(&s, sizeof(s));

		if (ok)
			puts("WireGuard configuration valid.");
		return ok ? 0 : 1;
	}

	if (argc == 2 && !strcmp(argv[1], "--help")) {
		puts(
		    "wireguard CONFIG | check CONFIG | genkey | pubkey\nNative IPv4 split tunnel, one peer; numeric endpoint, no full/default routes.");
		return 0;
	}

	if (argc == 4 && !strcmp(argv[1], "--test-tun-fd")) {
		unsigned fd;

		if (number(argv[2], 1024, &fd) < 0)
			return 1;
		return run(argv[3], fd);
	}

	if (argc != 2) {
		fprintf(stderr, "usage: wireguard CONFIG | check CONFIG | genkey | pubkey\n");
		return 2;
	}

	return run(argv[1], -1);
}
