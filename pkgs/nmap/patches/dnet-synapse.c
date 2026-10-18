/* Synapse native Ethernet/interface/route adapter. No Linux packet/procfs API. */
#include "config.h"
#include <sys/types.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "dnet.h"
#include <dev/packet_capture.h>

struct eth_handle {
	int fd;
	struct syn_capture_info info;
};

struct intf_handle {
	int fd;
};

struct route_handle {
	int fd;
};

static int info(int fd, unsigned idx, struct syn_capture_info *p) {
	memset(p, 0, sizeof(*p));
	p->version = SYN_CAPTURE_VERSION;
	p->ifindex = idx;
	return ioctl(fd, SYN_CAPTURE_INFO, p);
}

static int find(int fd, const char *name, struct syn_capture_info *p) {
	for (unsigned i = 0; info(fd, i, p) == 0; i++)
		if (!strcmp(name, p->name))
			return 0;
	errno = ENODEV;
	return -1;
}

static unsigned bits(uint32_t mask) {
	unsigned n = 0;
	mask = ntohl(mask);

	while (mask & 0x80000000u) {
		n++;
		mask <<= 1;
	}

	return n;
}

eth_t *eth_open(const char *name) {
	struct eth_handle *e = calloc(1, sizeof(*e));

	if (!e)
		return NULL;
	e->fd = open("/dev/packet", O_RDWR | O_NONBLOCK);

	if (e->fd < 0 || find(e->fd, name, &e->info) < 0)
		goto fail;
	struct syn_capture_bind b = { SYN_CAPTURE_VERSION, e->info.ifindex, 1518,
		                          SYN_CAPTURE_IN | SYN_CAPTURE_OUT };
	if (ioctl(e->fd, SYN_CAPTURE_BIND, &b) < 0)
		goto fail;
	return e;
fail:
	if (e->fd >= 0)
		close(e->fd);
	free(e);
	return NULL;
}

int eth_get(eth_t *e, eth_addr_t *ea) {
	memcpy(ea->data, e->info.mac, 6);
	return 0;
}

int eth_set(eth_t *e, const eth_addr_t *ea) {
	(void)e;
	(void)ea;
	errno = EOPNOTSUPP;
	return -1;
}

ssize_t eth_send(eth_t *e, const void *b, size_t n) {
	return write(e->fd, b, n);
}

eth_t *eth_close(eth_t *e) {
	if (e) {
		close(e->fd);
		free(e);
	}

	return NULL;
}

intf_t *intf_open(void) {
	struct intf_handle *i = malloc(sizeof(*i));

	if (!i)
		return NULL;
	i->fd = open("/dev/packet", O_RDONLY | O_NONBLOCK);

	if (i->fd < 0) {
		free(i);
		return NULL;
	}

	return i;
}

static int fill(struct intf_entry *e, const struct syn_capture_info *p) {
	unsigned size = e->intf_len;

	if (size < sizeof(*e)) {
		errno = EINVAL;
		return -1;
	}

	memset(e, 0, sizeof(*e));
	e->intf_len = sizeof(*e);
	strncpy(e->intf_name, p->name, INTF_NAME_LEN - 1);
	e->intf_index = p->ifindex + 1;
	e->intf_type = INTF_TYPE_ETH;
	e->intf_flags = INTF_FLAG_UP | INTF_FLAG_BROADCAST | INTF_FLAG_MULTICAST;
	e->intf_mtu = 1500;
	addr_pack(&e->intf_addr, ADDR_TYPE_IP, bits(p->netmask), &p->ipv4, 4);
	addr_pack(&e->intf_link_addr, ADDR_TYPE_ETH, 48, p->mac, 6);
	return 0;
}

int intf_get(intf_t *i, struct intf_entry *e) {
	struct syn_capture_info p;

	if (find(i->fd, e->intf_name, &p) < 0)
		return -1;
	return fill(e, &p);
}

int intf_get_index(intf_t *i, struct intf_entry *e, int af, unsigned idx) {
	struct syn_capture_info p;

	if (af != AF_INET || !idx) {
		errno = EAFNOSUPPORT;
		return -1;
	}

	if (info(i->fd, idx - 1, &p) < 0)
		return -1;
	return fill(e, &p);
}

int intf_get_src(intf_t *i, struct intf_entry *e, struct addr *src) {
	struct syn_capture_info p;

	for (unsigned n = 0; info(i->fd, n, &p) == 0; n++)
		if (src->addr_type == ADDR_TYPE_IP && src->addr_ip == p.ipv4)
			return fill(e, &p);
	errno = ENODEV;
	return -1;
}

int intf_get_dst(intf_t *i, struct intf_entry *e, struct addr *dst) {
	struct syn_capture_info p, first;
	int found = 0;

	if (dst->addr_type != ADDR_TYPE_IP) {
		errno = EAFNOSUPPORT;
		return -1;
	}

	for (unsigned n = 0; info(i->fd, n, &p) == 0; n++) {
		if ((dst->addr_ip & p.netmask) == (p.ipv4 & p.netmask))
			return fill(e, &p);
		if (!found && p.gateway) {
			first = p;
			found = 1;
		}
	}

	if (found)
		return fill(e, &first);
	errno = ENETUNREACH;
	return -1;
}

int intf_set(intf_t *i, const struct intf_entry *e) {
	(void)i;
	(void)e;
	errno = EOPNOTSUPP;
	return -1;
}

int intf_loop(intf_t *i, intf_handler cb, void *arg) {
	struct syn_capture_info p;

	for (unsigned n = 0; info(i->fd, n, &p) == 0; n++) {
		struct intf_entry e = { .intf_len = sizeof(e) };
		fill(&e, &p);
		int rc = cb(&e, arg);

		if (rc)
			return rc;
	}

	return 0;
}

intf_t *intf_close(intf_t *i) {
	if (i) {
		close(i->fd);
		free(i);
	}

	return NULL;
}

route_t *route_open(void) {
	struct route_handle *r = malloc(sizeof(*r));

	if (!r)
		return NULL;
	r->fd = open("/dev/packet", O_RDONLY | O_NONBLOCK);

	if (r->fd < 0) {
		free(r);
		return NULL;
	}

	return r;
}

static void routefill(struct route_entry *e, const struct syn_capture_info *p, int def) {
	memset(e, 0, sizeof(*e));
	strncpy(e->intf_name, p->name, INTF_NAME_LEN - 1);
	uint32_t net = def ? 0 : p->ipv4 & p->netmask, gw = def ? p->gateway : 0;
	addr_pack(&e->route_dst, ADDR_TYPE_IP, def ? 0 : bits(p->netmask), &net, 4);
	addr_pack(&e->route_gw, ADDR_TYPE_IP, 32, &gw, 4);
}

int route_get(route_t *r, struct route_entry *e) {
	struct addr dst = e->route_dst;
	struct syn_capture_info p, first;
	int found = 0;

	if (dst.addr_type != ADDR_TYPE_IP) {
		errno = EAFNOSUPPORT;
		return -1;
	}

	for (unsigned n = 0; info(r->fd, n, &p) == 0; n++) {
		if ((dst.addr_ip & p.netmask) == (p.ipv4 & p.netmask)) {
			routefill(e, &p, 0);
			return 0;
		}

		if (!found && p.gateway) {
			first = p;
			found = 1;
		}
	}

	if (found) {
		routefill(e, &first, 1);
		return 0;
	}

	errno = ENETUNREACH;
	return -1;
}

int route_loop(route_t *r, route_handler cb, void *arg) {
	struct syn_capture_info p;

	for (unsigned n = 0; info(r->fd, n, &p) == 0; n++) {
		struct route_entry e;
		routefill(&e, &p, 0);
		int rc = cb(&e, arg);

		if (rc)
			return rc;
		if (p.gateway) {
			routefill(&e, &p, 1);

			if ((rc = cb(&e, arg)))
				return rc;
		}
	}

	return 0;
}

int route_add(route_t *r, const struct route_entry *e) {
	(void)r;
	(void)e;
	errno = EOPNOTSUPP;
	return -1;
}

int route_add_dev(route_t *r, const struct route_entry *e, const char *d) {
	(void)d;
	return route_add(r, e);
}

int route6_add(route_t *r, const struct route_entry *e, int i) {
	(void)i;
	return route_add(r, e);
}

int route_delete(route_t *r, const struct route_entry *e) {
	return route_add(r, e);
}

int route6_delete(route_t *r, const struct route_entry *e, int i) {
	return route6_add(r, e, i);
}

route_t *route_close(route_t *r) {
	if (r) {
		close(r->fd);
		free(r);
	}

	return NULL;
}
