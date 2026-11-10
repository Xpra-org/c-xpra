/* png picture decoding through the system libpng, loaded at runtime so that it stays optional
 * (named pngdec so that it is never confused with libpng's own png.h) */
#ifndef PNGDEC_H
#define PNGDEC_H

#include <stddef.h>
#include <stdint.h>

/* 1 when libpng could be loaded */
int pngdec_available(void);

/* decode a complete PNG file (any colour type and bit depth) into a malloc'd BGRA buffer of *w x *h pixels,
 * rows of *w * 4 bytes; pictures larger than max_w x max_h are rejected. NULL on error */
uint8_t *pngdec_decode(const uint8_t *data, size_t len, int max_w, int max_h, int *w, int *h);

#endif
