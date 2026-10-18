// net.c: Сетевые вызовы SynAPI и простой HTTP-клиент
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: synlibc, пользовательская библиотека нативного API
// Создан: 09.10.2026
// Последнее изменение: 14.10.2026

#include <synlibc.h>
#include <dev/net.h>

int syn_netdev_get_count(void) {
	return (int)syscall0(SYS_SYN_NETDEV_GET_COUNT);
}

int syn_netdev_get_name(int dev_id, char *buf, size_t size) {
	if (!buf || size == 0) {
		return -1;
	}

	return (int)syscall3(SYS_SYN_NETDEV_GET_NAME, (uint64_t)dev_id, (uint64_t)buf, size);
}

int syn_netdev_get_ipv4(int dev_id, char *buf, size_t size) {
	if (!buf || size < 16) {
		return -1;
	}

	return (int)syscall3(SYS_SYN_NETDEV_GET_IPV4, (uint64_t)dev_id, (uint64_t)buf, size);
}

int syn_netdev_get_ipv6(int dev_id, char *buf, size_t size) {
	if (!buf || size < 40) {
		return -1;
	}

	return (int)syscall3(SYS_SYN_NETDEV_GET_IPV6, (uint64_t)dev_id, (uint64_t)buf, size);
}

int syn_netdev_get_link(int dev_id) {
	return (int)syscall1(SYS_SYN_NETDEV_GET_LINK_STATUS, (uint64_t)dev_id);
}

int syn_netdev_mac_get(int dev_id, uint8_t *mac, size_t size) {
	if (!mac || size < 6) {
		return -1;
	}

	return (int)syscall3(SYS_SYN_NETDEV_MAC_GET, (uint64_t)dev_id, (uint64_t)mac, size);
}

int syn_netdev_mac_set(int dev_id, const uint8_t *mac, size_t size) {
	if (!mac || size < 6) {
		return -1;
	}

	return (int)syscall3(SYS_SYN_NETDEV_MAC_SET, (uint64_t)dev_id, (uint64_t)mac, size);
}

uint64_t syn_netdev_tx_get(void) {
	return syscall0(SYS_SYN_NETDEV_TX_GET);
}

uint64_t syn_netdev_rx_get(void) {
	return syscall0(SYS_SYN_NETDEV_RX_GET);
}

// Инкременты оставлены для совместимости, но из user-space их вызывать не стоит
void syn_netdev_tx_inc(void) {
	syscall0(SYS_SYN_NETDEV_TX_INC);
}

void syn_netdev_rx_inc(void) {
	syscall0(SYS_SYN_NETDEV_RX_INC);
}

ssize_t syn_netdev_raw_send(int dev_id, const void *buf, size_t len) {
	if (!buf || len == 0) {
		return -1;
	}

	return (ssize_t)syscall3(SYS_SYN_NETDEV_RAW_SEND, (uint64_t)dev_id, (uint64_t)buf, len);
}

ssize_t syn_netdev_raw_recv(int dev_id, void *buf, size_t size) {
	if (!buf || size == 0) {
		return -1;
	}

	return (ssize_t)syscall3(SYS_SYN_NETDEV_RAW_RECV, (uint64_t)dev_id, (uint64_t)buf, size);
}

int syn_netdev_tun_create(const char *name) {
	return (int)syscall1(SYS_SYN_NETDEV_TUN_CREATE, (uint64_t)name);
}

int syn_netdev_tun_destroy(int tun_id) {
	return (int)syscall1(SYS_SYN_NETDEV_TUN_DESTROY, (uint64_t)tun_id);
}

ssize_t syn_netdev_tun_send(int tun_id, const void *buf, size_t len) {
	return (ssize_t)syscall3(SYS_SYN_NETDEV_TUN_SEND, (uint64_t)tun_id, (uint64_t)buf, len);
}

ssize_t syn_netdev_tun_recv(int tun_id, void *buf, size_t size) {
	return (ssize_t)syscall3(SYS_SYN_NETDEV_TUN_RECV, (uint64_t)tun_id, (uint64_t)buf, size);
}

int syn_netdev_tun_get_count(void) {
	return (int)syscall0(SYS_SYN_NETDEV_TUN_GET_COUNT);
}

int syn_netdev_tun_get_name(int tun_id, char *buf, size_t size) {
	return (int)syscall3(SYS_SYN_NETDEV_TUN_GET_NAME, (uint64_t)tun_id, (uint64_t)buf, size);
}

int syn_tcp_connect(const char *ip, uint16_t port) {
	if (!ip) {
		return -1;
	}

	return (int)syscall2(SYS_SYN_TCP_CONNECT, (uint64_t)ip, port);
}

ssize_t syn_tcp_send(int conn_id, const void *buf, size_t len) {
	if (!buf || len == 0) {
		return -1;
	}

	return (ssize_t)syscall3(SYS_SYN_TCP_SEND, (uint64_t)conn_id, (uint64_t)buf, len);
}

ssize_t syn_tcp_recv(int conn_id, void *buf, size_t len, uint32_t timeout_ms) {
	if (!buf || len == 0) {
		return -1;
	}

	return (ssize_t)syscall4(SYS_SYN_TCP_RECV, (uint64_t)conn_id, (uint64_t)buf, len, timeout_ms);
}

int syn_tcp_close(int conn_id) {
	return (int)syscall1(SYS_SYN_TCP_CLOSE, (uint64_t)conn_id);
}

// Отправить ВСЕ данные, даже если lwIP принял только часть
// Возвращает 0 при успехе, -1 при ошибке
int syn_tcp_send_all(int conn_id, const void *buf, size_t len) {
	if (!buf || len == 0) {
		return -1;
	}

	const uint8_t *p = (const uint8_t *)buf;
	size_t remaining = len;

	while (remaining > 0) {
		ssize_t sent = syn_tcp_send(conn_id, p, remaining);

		if (sent < 0) {
			return -1;
		}

		if (sent == 0) {
			return -1;
		}

		p += sent;
		remaining -= (size_t)sent;
	}

	return 0;
}

// Читать из сокета, пока не получим expected_len байт,
// либо не наступит таймаут, либо соединение не закроется
// Возвращает количество реально прочитанных байт
ssize_t syn_tcp_recv_all(int conn_id, void *buf, size_t expected_len, uint32_t timeout_ms) {
	if (!buf || expected_len == 0) {
		return -1;
	}

	uint8_t *p = (uint8_t *)buf;
	size_t total = 0;

	while (total < expected_len) {
		ssize_t r = syn_tcp_recv(conn_id, p + total, expected_len - total, timeout_ms);

		if (r < 0) {
			return -1;
		}

		if (r == 0) {
			break; // соединение закрыто или таймаут
		}

		total += (size_t)r;
	}

	return (ssize_t)total;
}

// Читать до тех пор, пока сервер не закроет соединение
// или не наступит таймаут.
ssize_t syn_tcp_recv_until_close(int conn_id, void *buf, size_t size, uint32_t timeout_ms) {
	if (!buf || size == 0) {
		return -1;
	}

	uint8_t *p = (uint8_t *)buf;
	size_t total = 0;

	while (total < size) {
		ssize_t r = syn_tcp_recv(conn_id, p + total, size - total, timeout_ms);

		if (r < 0) {
			return -1;
		}

		if (r == 0) {
			break; // сервер закрыл соединение
		}

		total += (size_t)r;
	}

	return (ssize_t)total;
}

// Простой HTTP GET
ssize_t syn_http_get(const char *host_ip, uint16_t port, const char *host_hdr, const char *path,
                     char *resp_buf, size_t resp_size, uint32_t timeout_ms) {
	if (!host_ip || !path || !resp_buf || resp_size == 0) {
		return -1;
	}

	int conn = syn_tcp_connect(host_ip, port);

	if (conn < 0) {
		return -1;
	}

	// Формируем запрос
	char req[512];
	int req_len = snprintf(req, sizeof(req),
	                       "GET %s HTTP/1.1\r\n"
	                       "Host: %s\r\n"
	                       "User-Agent: SynapseOS/1.0\r\n"
	                       "Accept: */*\r\n"
	                       "Connection: close\r\n"
	                       "\r\n",
	                       path, host_hdr ? host_hdr : host_ip);

	if (req_len < 0 || req_len >= (int)sizeof(req)) {
		syn_tcp_close(conn);
		return -1;
	}

	if (syn_tcp_send_all(conn, req, (size_t)req_len) != 0) {
		syn_tcp_close(conn);
		return -1;
	}

	ssize_t received = syn_tcp_recv_until_close(conn, resp_buf, resp_size - 1, timeout_ms);
	syn_tcp_close(conn);

	if (received > 0) {
		resp_buf[received] = '\0';
	}

	return received;
}

// Простой HTTP POST с JSON-телом
ssize_t syn_http_post_json(const char *host_ip, uint16_t port, const char *host_hdr,
                           const char *path, const char *json_body, char *resp_buf,
                           size_t resp_size, uint32_t timeout_ms) {
	if (!host_ip || !path || !json_body || !resp_buf || resp_size == 0) {
		return -1;
	}

	int conn = syn_tcp_connect(host_ip, port);

	if (conn < 0) {
		return -1;
	}

	size_t body_len = strlen(json_body);

	char req[1024];
	int req_len = snprintf(req, sizeof(req),
	                       "POST %s HTTP/1.1\r\n"
	                       "Host: %s\r\n"
	                       "User-Agent: SynapseOS-Telemetry/1.0\r\n"
	                       "Content-Type: application/json\r\n"
	                       "Content-Length: %zu\r\n"
	                       "Connection: close\r\n"
	                       "\r\n",
	                       path, host_hdr ? host_hdr : host_ip, body_len);

	if (req_len < 0 || req_len >= (int)sizeof(req)) {
		syn_tcp_close(conn);
		return -1;
	}

	// Отправляем заголовки
	if (syn_tcp_send_all(conn, req, (size_t)req_len) != 0) {
		syn_tcp_close(conn);
		return -1;
	}

	// Отправляем тело
	if (syn_tcp_send_all(conn, json_body, body_len) != 0) {
		syn_tcp_close(conn);
		return -1;
	}

	ssize_t received = syn_tcp_recv_until_close(conn, resp_buf, resp_size - 1, timeout_ms);
	syn_tcp_close(conn);

	if (received > 0) {
		resp_buf[received] = '\0';
	}

	return received;
}

// Получить HTTP-статус из ответа
// Возвращает -1, если парсинг не удался
int syn_http_get_status(const char *response) {
	if (!response) {
		return -1;
	}

	// Ответ должен начинаться с "HTTP/1.x NNN"
	if (response[0] != 'H' || response[1] != 'T' || response[2] != 'T' || response[3] != 'P' ||
	    response[4] != '/') {
		return -1;
	}

	// Ищем пробел после версии
	const char *p = response;

	while (*p && *p != ' ') {
		p++;
	}

	if (!*p) {
		return -1;
	}

	p++; // пропускаем пробел

	// Читаем 3 цифры статуса
	int status = 0;

	for (int i = 0; i < 3; i++) {
		if (p[i] < '0' || p[i] > '9') {
			return -1;
		}

		status = status * 10 + (p[i] - '0');
	}

	return status;
}

// Найти тело HTTP-ответа (после "\r\n\r\n")
// Возвращает указатель на начало тела или NULL
const char *syn_http_get_body(const char *response) {
	if (!response) {
		return NULL;
	}

	const char *sep = strstr(response, "\r\n\r\n");

	if (sep) {
		return sep + 4;
	}

	// Некоторые серверы могут использовать просто \n\n
	sep = strstr(response, "\n\n");

	if (sep) {
		return sep + 2;
	}

	return NULL;
}

// Проверка корректности IPv4-строки (без atoi, чтобы не тянуть libc)
// Возвращает true, если строка: валидный IPv4
bool syn_ip4_is_valid(const char *ip) {
	if (!ip) {
		return false;
	}

	int dots = 0;
	int num = 0;
	bool has_digit = false;

	for (const char *p = ip; *p; p++) {
		if (*p >= '0' && *p <= '9') {
			num = num * 10 + (*p - '0');

			if (num > 255) {
				return false;
			}

			has_digit = true;
		} else if (*p == '.') {
			if (!has_digit) {
				return false;
			}

			dots++;

			if (dots > 3) {
				return false;
			}

			num = 0;
			has_digit = false;
		} else {
			return false;
		}
	}

	return has_digit && dots == 3;
}