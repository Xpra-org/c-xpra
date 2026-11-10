#include "buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void buf_reserve(buf_t *b, size_t extra)
{
	if (b->len + extra <= b->cap)
		return;
	size_t cap = b->cap ? b->cap : 256;
	while (cap < b->len + extra)
		cap *= 2;
	uint8_t *p = realloc(b->data, cap);
	if (!p) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	b->data = p;
	b->cap = cap;
}

void buf_add(buf_t *b, const void *data, size_t len)
{
	if (!len)
		return;
	buf_reserve(b, len);
	memcpy(b->data + b->len, data, len);
	b->len += len;
}

void buf_addc(buf_t *b, uint8_t c)
{
	buf_reserve(b, 1);
	b->data[b->len++] = c;
}

void buf_consume(buf_t *b, size_t n)
{
	if (n >= b->len) {
		b->len = 0;
		return;
	}
	memmove(b->data, b->data + n, b->len - n);
	b->len -= n;
}

void buf_free(buf_t *b)
{
	free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}
