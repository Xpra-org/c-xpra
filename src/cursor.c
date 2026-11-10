/* cursor-data: encoding, w, h, xhot, yhot, serial, data, name (Protocol.md 8.5)
 * only the "raw" encoding is advertised: RGBA bytes, straight alpha, rows of w * 4 bytes */
#include "cursor.h"

#include <stdlib.h>

/* a strict integer: booleans, floats and out of range values are rejected */
static int get_int(const rv *v, int64_t min, int64_t max, int *out)
{
	if (!v || v->type != RV_INT || v->u.i < min || v->u.i > max)
		return -1;
	*out = (int)v->u.i;
	return 0;
}

static int fail(const char **err, const char *msg)
{
	*err = msg;
	return -1;
}

int cursor_decode(const rv *pk, cursor_image *out, const char **err)
{
	out->pixels = NULL;
	const rv *enc = rv_list_get(pk, 1);
	if (!pk || pk->type != RV_LIST || !enc || (enc->type != RV_STR && enc->type != RV_BYTES))
		return fail(err, "missing encoding");
	if (!enc->u.s.len)
		return 0;	/* no encoding in common: the client's default cursor */
	if (!rv_is_str(enc, "raw"))
		return fail(err, "unsupported encoding");
	if (rv_len(pk) < 8)
		return fail(err, "truncated packet");
	int w, h, xhot, yhot;
	if (get_int(rv_list_get(pk, 2), 1, CURSOR_MAX, &w) ||
	    get_int(rv_list_get(pk, 3), 1, CURSOR_MAX, &h))
		return fail(err, "invalid size");
	/* the hotspot must lie within the image */
	if (get_int(rv_list_get(pk, 4), 0, w - 1, &xhot) ||
	    get_int(rv_list_get(pk, 5), 0, h - 1, &yhot))
		return fail(err, "invalid hotspot");
	const rv *serial = rv_list_get(pk, 6);
	if (!serial || serial->type != RV_INT)
		return fail(err, "invalid serial");
	const rv *data = rv_list_get(pk, 7);
	size_t npixels = (size_t)w * (size_t)h;	/* at most CURSOR_MAX^2: no overflow */
	if (!data || (data->type != RV_BYTES && data->type != RV_STR) || data->u.s.len != npixels * 4)
		return fail(err, "invalid pixel data");
	uint32_t *pixels = malloc(npixels * 4);
	if (!pixels)
		return fail(err, "out of memory");
	const uint8_t *s = data->u.s.p;
	for (size_t i = 0; i < npixels; i++, s += 4) {
		uint32_t a = s[3];
		uint32_t r = (s[0] * a + 127) / 255, g = (s[1] * a + 127) / 255, b = (s[2] * a + 127) / 255;
		pixels[i] = a << 24 | r << 16 | g << 8 | b;
	}
	out->w = w;
	out->h = h;
	out->xhot = xhot;
	out->yhot = yhot;
	out->pixels = pixels;
	return 1;
}
