#ifndef LZ4_H
#define LZ4_H

#include <stddef.h>
#include <stdint.h>

/* decompress one raw LZ4 block, the output must be exactly dstlen bytes
 * returns 0 on success, -1 on any error */
int lz4_decompress(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);

/* xpra framing: little-endian u32 uncompressed size, then one LZ4 block
 * returns a malloc'd buffer and sets *outlen, or NULL on error / if size > maxlen */
uint8_t *lz4_decompress_sized(const uint8_t *src, size_t srclen, size_t maxlen, size_t *outlen);

#endif
