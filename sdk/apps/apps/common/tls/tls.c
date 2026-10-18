// tls.c: Клиентский транспорт TLS
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: Клиентский транспорт TLS в пользовательском пространстве
// Создан: 11.10.2026
// Последнее изменение: 14.10.2026

#define _GNU_SOURCE
#include "tls.h"
#include <mbedtls/error.h>
#include <psa/crypto.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>

void tls_error(const char *operation, int rc) {
	char message[192];
	mbedtls_strerror(rc, message, sizeof message);
	fprintf(stderr, "%s: %s (%d)\n", operation, message, rc);
}

static int timeout(int fd) {
	struct timeval tv = { 15, 0 };
	return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) ||
	       setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

int tls_init(struct tls *t, int server, const char *cert, const char *key) {
	memset(t, 0, sizeof *t);
	mbedtls_ssl_init(&t->ssl);
	mbedtls_ssl_config_init(&t->conf);
	mbedtls_entropy_init(&t->entropy);
	mbedtls_ctr_drbg_init(&t->rng);
	mbedtls_x509_crt_init(&t->cert);
	mbedtls_pk_init(&t->key);
	mbedtls_net_init(&t->net);
	int rc;
	const unsigned char personalization[] = "SynapseOS HTTPS";

	if (psa_crypto_init( ) != PSA_SUCCESS) {
		return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
	}

	if ((rc = mbedtls_ctr_drbg_seed(&t->rng, mbedtls_entropy_func, &t->entropy, personalization,
	                                sizeof personalization))) {
		return rc;
	}

	if ((rc = mbedtls_ssl_config_defaults(
	         &t->conf, server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
	         MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT))) {
		return rc;
	}

	mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->rng);
	mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_read_timeout(&t->conf, 15000);

	if ((rc = mbedtls_x509_crt_parse_file(&t->cert, cert))) {
		return rc;
	}

	if (server) {
		if ((rc = mbedtls_pk_parse_keyfile(&t->key, key, NULL, mbedtls_ctr_drbg_random, &t->rng))) {
			return rc;
		}

		if ((rc = mbedtls_pk_check_pair(&t->cert.pk, &t->key, mbedtls_ctr_drbg_random, &t->rng))) {
			return rc;
		}

		if ((rc = mbedtls_ssl_conf_own_cert(&t->conf, &t->cert, &t->key))) {
			return rc;
		}

		mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
	} else {
		mbedtls_ssl_conf_ca_chain(&t->conf, &t->cert, NULL);
		mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	}

	return mbedtls_ssl_setup(&t->ssl, &t->conf);
}

static int handshake(struct tls *t) {
	if (timeout(t->net.fd)) {
		return MBEDTLS_ERR_NET_SOCKET_FAILED;
	}

	mbedtls_ssl_set_bio(&t->ssl, &t->net, mbedtls_net_send, mbedtls_net_recv,
	                    mbedtls_net_recv_timeout);
	return mbedtls_ssl_handshake(&t->ssl);
}

int tls_connect(struct tls *t, const char *host, const char *port) {
	// Ограничиваем ожидание соединения и чтения TLS/HTTP
	// DNS использует системный resolver
	struct addrinfo hint = { .ai_socktype = SOCK_STREAM }, *addresses;

	if (getaddrinfo(host, port, &hint, &addresses)) {
		return MBEDTLS_ERR_NET_UNKNOWN_HOST;
	}

	int fd = -1;

	for (struct addrinfo *a = addresses; a; a = a->ai_next) {
		fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);

		if (fd < 0) {
			continue;
		}

		if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
			close(fd);
			fd = -1;
			continue;
		}

		int rc = connect(fd, a->ai_addr, a->ai_addrlen);

		if (rc && errno == EINPROGRESS) {
			struct pollfd p = { fd, POLLOUT, 0 };
			int error = 0;
			socklen_t len = sizeof error;
			rc = poll(&p, 1, 15000) > 0 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) &&
			             !error
			         ? 0
			         : -1;
		}

		if (!rc && !fcntl(fd, F_SETFL, 0)) {
			break;
		}

		close(fd);
		fd = -1;
	}

	freeaddrinfo(addresses);

	if (fd < 0) {
		return MBEDTLS_ERR_NET_CONNECT_FAILED;
	}

	t->net.fd = fd;
	int rc = mbedtls_ssl_set_hostname(&t->ssl, host);

	if (rc) {
		return rc;
	}

	rc = handshake(t);

	if (!rc && mbedtls_ssl_get_verify_result(&t->ssl)) {
		rc = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
	}

	return rc;
}

int tls_accept(struct tls *t, int fd) {
	t->net.fd = fd;
	return handshake(t);
}

int tls_write_all(struct tls *t, const void *data, size_t size) {
	const unsigned char *p = data;

	while (size) {
		int n = mbedtls_ssl_write(&t->ssl, p, size);

		if (n <= 0) {
			return n ? n : MBEDTLS_ERR_NET_SEND_FAILED;
		}

		p += n;
		size -= (size_t)n;
	}

	return 0;
}

int tls_read(struct tls *t, void *data, size_t size) {
	return mbedtls_ssl_read(&t->ssl, data, size);
}

void tls_free(struct tls *t) {
	// При очистке не отправляем блокирующий запрос отключённому клиенту
	mbedtls_net_free(&t->net);
	mbedtls_ssl_free(&t->ssl);
	mbedtls_ssl_config_free(&t->conf);
	mbedtls_x509_crt_free(&t->cert);
	mbedtls_pk_free(&t->key);
	mbedtls_ctr_drbg_free(&t->rng);
	mbedtls_entropy_free(&t->entropy);
	mbedtls_psa_crypto_free( );
}
