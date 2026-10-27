#ifndef SYNAPSE_ACME_H
#define SYNAPSE_ACME_H
#include <stddef.h>
#define ACME_URL 2048
#define ACME_BODY (128 * 1024)
struct http_reply {
    int status, retry;
    char nonce[256], location[ACME_URL];
    char *body;
    size_t size;
};
int acme_http(const char *url, const char *method, const char *payload,
              const char *ca, int test_http, struct http_reply *reply);
void acme_http_free(struct http_reply *reply);
#endif
