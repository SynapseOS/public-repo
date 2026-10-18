/* Synapse native capture backend, backed by independent /dev/packet queues. */
#include <config.h>
#include "pcap-int.h"
#include <dev/packet_capture.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <netinet/in.h>

struct syn_priv {
	struct syn_capture_bind bind;
	int nonblock;
	unsigned char record[sizeof(struct syn_capture_header) + SYN_CAPTURE_MAX_FRAME];
};

static int info_name(int fd, const char *name, struct syn_capture_info *info) {
	for (unsigned i = 0; i < 8; i++) {
		memset(info, 0, sizeof(*info));
		info->version = SYN_CAPTURE_VERSION;
		info->ifindex = i;

		if (ioctl(fd, SYN_CAPTURE_INFO, info) < 0)
			break;
		if (!strcmp(name, info->name))
			return 0;
	}

	errno = ENODEV;
	return -1;
}

static int syn_read(pcap_t *p, int max, pcap_handler callback, u_char *user) {
	struct syn_priv *s = p->priv;
	int delivered = 0;

	for (;;) {
		if (p->break_loop) {
			if (!delivered)
				p->break_loop = 0;
			return delivered ? delivered : PCAP_ERROR_BREAK;
		}

		ssize_t n = read(p->fd, s->record, sizeof(s->record));

		if (n < 0) {
			if (errno != EAGAIN && errno != EINTR) {
				snprintf(p->errbuf, PCAP_ERRBUF_SIZE, "packet read: %s", strerror(errno));
				return PCAP_ERROR;
			}

			if (delivered || s->nonblock)
				return delivered;
			struct pollfd fd = { p->fd, POLLIN, 0 };
			int ready = poll(&fd, 1, p->opt.timeout > 0 ? p->opt.timeout : 100);

			if (ready < 0 && errno != EINTR) {
				snprintf(p->errbuf, PCAP_ERRBUF_SIZE, "packet poll: %s", strerror(errno));
				return PCAP_ERROR;
			}

			if (!ready && p->opt.timeout > 0)
				return 0;
			continue;
		}

		struct syn_capture_header h;

		if ((size_t)n < sizeof(h)) {
			snprintf(p->errbuf, PCAP_ERRBUF_SIZE, "short capture record");
			return PCAP_ERROR;
		}

		memcpy(&h, s->record, sizeof(h));

		if (h.caplen > SYN_CAPTURE_MAX_FRAME || h.caplen > h.wirelen ||
		    (size_t)n != sizeof(h) + h.caplen) {
			snprintf(p->errbuf, PCAP_ERRBUF_SIZE, "invalid capture record");
			return PCAP_ERROR;
		}

		unsigned char *data = s->record + sizeof(h);

		if (p->fcode.bf_insns && !pcapint_filter(p->fcode.bf_insns, data, h.wirelen, h.caplen))
			continue;
		struct pcap_pkthdr ph;
		ph.ts.tv_sec = h.timestamp_us / 1000000;
		ph.ts.tv_usec = h.timestamp_us % 1000000;
		ph.caplen = h.caplen;
		ph.len = h.wirelen;
		callback(user, &ph, data);
		delivered++;

		if (max > 0 && delivered >= max)
			return delivered;
	}
}

static int syn_inject(pcap_t *p, const void *data, int size) {
	ssize_t n = write(p->fd, data, size);

	if (n < 0)
		snprintf(p->errbuf, PCAP_ERRBUF_SIZE, "packet inject: %s", strerror(errno));
	return n;
}

static int syn_stats(pcap_t *p, struct pcap_stat *out) {
	struct syn_capture_stats stats;

	if (ioctl(p->fd, SYN_CAPTURE_STATS, &stats) < 0)
		return PCAP_ERROR;
	memset(out, 0, sizeof(*out));
	out->ps_recv = stats.received;
	out->ps_drop = stats.dropped;
	return 0;
}

static int syn_getnonblock(pcap_t *p) {
	struct syn_priv *s = p->priv;
	return s->nonblock;
}

static int syn_setnonblock(pcap_t *p, int value) {
	struct syn_priv *s = p->priv;
	s->nonblock = !!value;
	return 0;
}

static int syn_direction(pcap_t *p, pcap_direction_t d) {
	struct syn_priv *s = p->priv;
	s->bind.directions = d == PCAP_D_IN ? SYN_CAPTURE_IN : d == PCAP_D_OUT ? SYN_CAPTURE_OUT : 3;
	return ioctl(p->fd, SYN_CAPTURE_BIND, &s->bind);
}

static int syn_activate(pcap_t *p) {
	struct syn_priv *s = p->priv;
	struct syn_capture_info info;
	p->fd = open("/dev/packet", O_RDWR | O_NONBLOCK);

	if (p->fd < 0 || info_name(p->fd, p->opt.device, &info) < 0) {
		snprintf(p->errbuf, PCAP_ERRBUF_SIZE, "native packet device/interface: %s",
		         strerror(errno));
		if (p->fd >= 0)
			close(p->fd);
		p->fd = -1;
		return PCAP_ERROR;
	}

	if (p->snapshot <= 0 || p->snapshot > SYN_CAPTURE_MAX_FRAME)
		p->snapshot = SYN_CAPTURE_MAX_FRAME;
	s->bind = (struct syn_capture_bind){ SYN_CAPTURE_VERSION, info.ifindex, p->snapshot, 3 };

	if (ioctl(p->fd, SYN_CAPTURE_BIND, &s->bind) < 0) {
		close(p->fd);
		p->fd = -1;
		return PCAP_ERROR;
	}

	p->linktype = DLT_EN10MB;
	p->selectable_fd = p->fd;
	p->read_op = syn_read;
	p->inject_op = syn_inject;
	p->setfilter_op = pcapint_install_bpf_program;
	p->setdirection_op = syn_direction;
	p->getnonblock_op = syn_getnonblock;
	p->setnonblock_op = syn_setnonblock;
	p->stats_op = syn_stats;
	p->cleanup_op = pcapint_cleanup_live_common;

	if (p->opt.rfmon) {
		snprintf(p->errbuf, PCAP_ERRBUF_SIZE, "monitor mode unsupported");
		return PCAP_ERROR_RFMON_NOTSUP;
	}

	if (p->opt.promisc) {
		snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
		         "promiscuous driver mode unavailable; local-interface capture only");
		return PCAP_WARNING_PROMISC_NOTSUP;
	}

	return 0;
}

pcap_t *pcapint_create_interface(const char *device _U_, char *errbuf) {
	pcap_t *p = PCAP_CREATE_COMMON(errbuf, struct syn_priv);

	if (p)
		p->activate_op = syn_activate;
	return p;
}

int pcapint_platform_finddevs(pcap_if_list_t *list, char *errbuf) {
	int fd = open("/dev/packet", O_RDONLY);

	if (fd < 0) {
		snprintf(errbuf, PCAP_ERRBUF_SIZE, "native packet device: %s", strerror(errno));
		return -1;
	}

	for (unsigned i = 0; i < 8; i++) {
		struct syn_capture_info info = { .version = SYN_CAPTURE_VERSION, .ifindex = i };

		if (ioctl(fd, SYN_CAPTURE_INFO, &info) < 0)
			break;
		pcap_if_t *dev = pcapint_add_dev(list, info.name, PCAP_IF_UP | PCAP_IF_RUNNING,
		                                 "Synapse Ethernet capture", errbuf);
		if (!dev) {
			close(fd);
			return -1;
		}

		if (info.ipv4) {
			struct sockaddr_in ip = { .sin_family = AF_INET }, mask = { .sin_family = AF_INET };
			ip.sin_addr.s_addr = info.ipv4;
			mask.sin_addr.s_addr = info.netmask;

			if (pcapint_add_addr_to_dev(dev, (struct sockaddr *)&ip, sizeof(ip),
			                            (struct sockaddr *)&mask, sizeof(mask), NULL, 0, NULL, 0,
			                            errbuf) < 0) {
				close(fd);
				return -1;
			}
		}
	}

	close(fd);
	return 0;
}

int pcap_lookupnet(const char *name, bpf_u_int32 *net, bpf_u_int32 *mask, char *errbuf) {
	int fd = open("/dev/packet", O_RDONLY);
	struct syn_capture_info info;

	if (fd < 0 || info_name(fd, name, &info) < 0) {
		if (fd >= 0)
			close(fd);
		snprintf(errbuf, PCAP_ERRBUF_SIZE, "native interface information unavailable");
		return -1;
	}

	close(fd);
	*mask = info.netmask;
	*net = info.ipv4 & info.netmask;
	return 0;
}

const char *pcap_lib_version(void) {
	return PCAP_VERSION_STRING " (Synapse native packet capture)";
}
