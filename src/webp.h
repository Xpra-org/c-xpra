/* webp picture decoding through the system libwebp, loaded at runtime so that it stays optional */
#ifndef WEBP_H
#define WEBP_H

#include <stddef.h>
#include <stdint.h>

/* 1 when libwebp could be loaded */
int webp_available(void);

/* decode a complete WebP file (lossy or lossless) into a malloc'd BGRA buffer of *w x *h pixels,
 * rows of *w * 4 bytes; pictures larger than max_w x max_h are rejected. NULL on error */
uint8_t *webp_decode(const uint8_t *data, size_t len, int max_w, int max_h, int *w, int *h);

#endif
