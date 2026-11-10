/* LZ4 block format decoder (public format specification) */
#include "lz4.h"

#include <stdlib.h>

int lz4_decompress(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen)
{
	const uint8_t *ip = src, *iend = src + srclen;
	size_t op = 0;

	while (ip < iend) {
		uint8_t token = *ip++;
		/* literals */
		size_t lit = token >> 4;
		if (lit == 15) {
			uint8_t b;
			do {
				if (ip >= iend)
					return -1;
				b = *ip++;
				lit += b;
			} while (b == 255);
		}
		if (lit > (size_t)(iend - ip) || lit > dstlen - op)
			return -1;
		for (size_t i = 0; i < lit; i++)
			dst[op + i] = ip[i];
		ip += lit;
		op += lit;
		if (ip == iend)
			break;		/* last sequence has literals only */
		/* match */
		if (iend - ip < 2)
			return -1;
		size_t offset = ip[0] | ((size_t)ip[1] << 8);
		ip += 2;
		if (offset == 0 || offset > op)
			return -1;
		size_t mlen = token & 15;
		if (mlen == 15) {
			uint8_t b;
			do {
				if (ip >= iend)
					return -1;
				b = *ip++;
				mlen += b;
			} while (b == 255);
		}
		mlen += 4;
		if (mlen > dstlen - op)
			return -1;
		/* byte by byte: the match may overlap the output */
		for (size_t i = 0; i < mlen; i++)
			dst[op + i] = dst[op - offset + i];
		op += mlen;
	}
	return op == dstlen ? 0 : -1;
}

uint8_t *lz4_decompress_sized(const uint8_t *src, size_t srclen, size_t maxlen, size_t *outlen)
{
	if (srclen < 4)
		return NULL;
	size_t size = src[0] | (src[1] << 8) | ((size_t)src[2] << 16) | ((size_t)src[3] << 24);
	if (size > maxlen)
		return NULL;
	uint8_t *out = malloc(size ? size : 1);
	if (!out)
		return NULL;
	if (lz4_decompress(src + 4, srclen - 4, out, size)) {
		free(out);
		return NULL;
	}
	*outlen = size;
	return out;
}
