/* growable byte buffer */
#ifndef BUF_H
#define BUF_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint8_t *data;
	size_t len;
	size_t cap;
} buf_t;

void buf_reserve(buf_t *b, size_t extra);
void buf_add(buf_t *b, const void *data, size_t len);
void buf_addc(buf_t *b, uint8_t c);
void buf_consume(buf_t *b, size_t n);	/* drop n bytes from the front */
void buf_free(buf_t *b);

#endif
