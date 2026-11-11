/* jpeg picture decoding through the system libturbojpeg, loaded at runtime so that it stays optional */
#ifndef JPEG_H
#define JPEG_H

#include <stddef.h>
#include <stdint.h>

/* 1 when libturbojpeg could be loaded */
int jpeg_available(void);

/* decode a complete JPEG (JFIF) file into a malloc'd BGRA buffer of *w x *h pixels,
 * rows of *w * 4 bytes; pictures larger than max_w x max_h are rejected. NULL on error */
uint8_t *jpeg_decode(const uint8_t *data, size_t len, int max_w, int max_h, int *w, int *h);

#endif
