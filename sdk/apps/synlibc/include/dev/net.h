#ifndef SYNLIBC_NET
#define SYNLIBC_NET 1

#include <stdint.h>
#include <stddef.h>

int syn_netdev_get_count(void);
int syn_netdev_get_name(int dev_id, char *buf, size_t size);
int syn_netdev_get_ipv4(int dev_id, char *buf, size_t size);
int syn_netdev_get_link(int dev_id);
ssize_t syn_netdev_raw_send(int dev_id, const void *buf, size_t len);
int syn_tcp_connect(const char *ip, uint16_t port);
ssize_t syn_tcp_send(int conn_id, const void *buf, size_t len);
int syn_tcp_close(int conn_id);
ssize_t syn_tcp_recv(int conn_id, void *buf, size_t len, uint32_t timeout_ms);

#endif