/* clipboard-contents: request-id, selection, data-type, data-format, wire-encoding, data, truncated
 * (Protocol.md 8.6), only text is exchanged */
#include "clipboard.h"

#include <stdlib.h>

static const char *const SELECTIONS[SEL_COUNT] = { "CLIPBOARD", "PRIMARY" };

const char *const CLIP_TARGETS[] = {
	"UTF8_STRING", "TEXT", "STRING", "text/plain;charset=utf-8", "text/plain", NULL,
};

int clip_selection(const rv *name)
{
	for (int i = 0; i < SEL_COUNT; i++)
		if (rv_is_str(name, SELECTIONS[i]))
			return i;
	return -1;
}

const char *clip_selection_name(int sel)
{
	return SELECTIONS[sel];
}

int clip_is_text_target(const rv *target)
{
	for (int i = 0; CLIP_TARGETS[i]; i++)
		if (rv_is_str(target, CLIP_TARGETS[i]))
			return 1;
	return 0;
}

static int fail(const char **err, const char *msg)
{
	*err = msg;
	return -1;
}

int clip_contents(const rv *pk, const uint8_t **data, size_t *len, int *latin1, const char **err)
{
	if (!pk || pk->type != RV_LIST || rv_len(pk) < 7)
		return fail(err, "truncated packet");
	const rv *id = rv_list_get(pk, 1), *type = rv_list_get(pk, 3), *fmt = rv_list_get(pk, 4);
	const rv *d = rv_list_get(pk, 6);
	if (id->type != RV_INT)
		return fail(err, "invalid request id");
	if (!clip_is_text_target(type))
		return fail(err, "not text");
	if (fmt->type != RV_INT || fmt->u.i != 8 || !rv_is_str(rv_list_get(pk, 5), "bytes"))
		return fail(err, "unsupported data format");
	if (d->type != RV_BYTES && d->type != RV_STR)
		return fail(err, "invalid data");
	if (d->u.s.len > CLIP_MAX)
		return fail(err, "too large");
	*data = d->u.s.p;
	*len = d->u.s.len;
	*latin1 = rv_is_str(type, "STRING");
	return 0;
}

char *clip_latin1_to_utf8(const uint8_t *s, size_t n, size_t *outlen)
{
	char *out = malloc(n * 2 + 1), *o = out;
	if (!out)
		return NULL;
	for (size_t i = 0; i < n; i++) {
		if (s[i] < 0x80) {
			*o++ = (char)s[i];
		} else {
			*o++ = (char)(0xc0 | s[i] >> 6);
			*o++ = (char)(0x80 | (s[i] & 0x3f));
		}
	}
	*o = 0;
	*outlen = (size_t)(o - out);
	return out;
}

char *clip_utf8_to_latin1(const uint8_t *s, size_t n, size_t *outlen)
{
	char *out = malloc(n + 1), *o = out;
	if (!out)
		return NULL;
	size_t i = 0;
	while (i < n) {
		uint8_t c = s[i];
		/* sequence length from the lead byte, 0 when invalid */
		size_t len = c < 0x80 ? 1 : c >= 0xc2 && c < 0xe0 ? 2 : c >= 0xe0 && c < 0xf0 ? 3 :
			     c >= 0xf0 && c < 0xf5 ? 4 : 0;
		size_t k = 1;
		while (k < len && i + k < n && (s[i + k] & 0xc0) == 0x80)
			k++;
		if (len == 1)
			*o++ = (char)c;
		else if (len == 2 && k == 2 && c < 0xc4)
			*o++ = (char)((c & 0x1f) << 6 | (s[i + 1] & 0x3f));
		else
			*o++ = '?';
		/* skip the whole sequence, or only the bad lead byte */
		i += len && k == len ? len : k;
	}
	*o = 0;
	*outlen = (size_t)(o - out);
	return out;
}
