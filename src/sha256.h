#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t h[8];
	uint64_t total;
	uint8_t block[64];
	size_t used;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);

void hmac_sha256(const uint8_t *key, size_t keylen, const uint8_t *msg, size_t msglen, uint8_t out[32]);
/* writes 2*len lowercase hex characters plus a nul terminator */
void tohex(const uint8_t *in, size_t len, char *out);

#endif
