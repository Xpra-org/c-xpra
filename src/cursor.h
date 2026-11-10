/* cursor-data packet validation and decoding, see Subsystems/Cursor.md and Protocol.md 8.5 */
#ifndef CURSOR_H
#define CURSOR_H

#include <stdint.h>

#include "rencode.h"

/* the largest cursor accepted, also advertised as cursor.max */
#define CURSOR_MAX 256

typedef struct {
	int w, h, xhot, yhot;
	uint32_t *pixels;	/* w * h native endian 0xAARRGGBB, premultiplied alpha, malloc'd */
} cursor_image;

/* decode a cursor-data packet:
 *  1: out holds the cursor image, the caller frees out->pixels
 *  0: use the default cursor (empty encoding)
 * -1: invalid packet, *err says why */
int cursor_decode(const rv *pk, cursor_image *out, const char **err);

#endif
