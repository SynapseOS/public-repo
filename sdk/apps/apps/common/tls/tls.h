#ifndef LUMINE_TLS_H
#define LUMINE_TLS_H
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/net_sockets.h>

struct tls {
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context rng;
	mbedtls_x509_crt cert;
	mbedtls_pk_context key;
	mbedtls_net_context net;
};

int tls_init(struct tls *, int server, const char *cert, const char *key);
int tls_connect(struct tls *, const char *host, const char *port);
int tls_accept(struct tls *, int fd);
int tls_write_all(struct tls *, const void *, size_t);
int tls_read(struct tls *, void *, size_t);
void tls_free(struct tls *);
void tls_error(const char *, int);
#endif
