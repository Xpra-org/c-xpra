/* rencodeplus, implemented from Network/Protocol.md section 4.2 */
#include "rencode.h"

#include <stdlib.h>
#include <string.h>

#define CHR_FLOAT64	44
#define CHR_LIST	59
#define CHR_DICT	60
#define CHR_INT		61
#define CHR_INT1	62
#define CHR_INT2	63
#define CHR_INT4	64
#define CHR_INT8	65
#define CHR_FLOAT32	66
#define CHR_TRUE	67
#define CHR_FALSE	68
#define CHR_NONE	69
#define CHR_TERM	127

#define INT_POS_FIXED_START	0
#define INT_POS_FIXED_COUNT	44
#define INT_NEG_FIXED_START	70
#define INT_NEG_FIXED_COUNT	32
#define DICT_FIXED_START	102
#define DICT_FIXED_COUNT	25
#define STR_FIXED_START		128
#define STR_FIXED_COUNT		64
#define LIST_FIXED_START	(STR_FIXED_START + STR_FIXED_COUNT)
#define LIST_FIXED_COUNT	64

#define MAX_DEPTH 32

/* ------------------------------------------------------------------ decoder */

typedef struct {
	const uint8_t *p;
	const uint8_t *end;
} dec_t;

static int decode_value(dec_t *d, rv *out, int depth);

static uint64_t be_read(const uint8_t *p, int n)
{
	uint64_t v = 0;
	for (int i = 0; i < n; i++)
		v = (v << 8) | p[i];
	return v;
}

static void free_contents(rv *v)
{
	if (v->type == RV_LIST || v->type == RV_DICT) {
		size_t n = v->type == RV_DICT ? v->u.l.n * 2 : v->u.l.n;
		for (size_t i = 0; i < n; i++)
			free_contents(&v->u.l.items[i]);
		free(v->u.l.items);
		v->u.l.items = NULL;
		v->u.l.n = 0;
	}
}

/* decode `count` values, or until CHR_TERM if count < 0 */
static int decode_items(dec_t *d, rv *out, rv_type type, long count, int depth)
{
	int per = type == RV_DICT ? 2 : 1;
	size_t cap = count >= 0 ? (size_t)count * per : 8;
	size_t n = 0;
	rv *items = cap ? calloc(cap, sizeof(rv)) : NULL;
	if (cap && !items)
		return -1;
	out->type = type;
	out->u.l.items = items;
	out->u.l.n = 0;
	for (;;) {
		if (count >= 0) {
			if (n == (size_t)count * per)
				break;
		} else {
			if (d->p >= d->end)
				goto fail;
			if (*d->p == CHR_TERM) {
				d->p++;
				if (n % per)
					goto fail;
				break;
			}
			if (n == cap) {
				rv *ni = realloc(items, cap * 2 * sizeof(rv));
				if (!ni)
					goto fail;
				memset(ni + cap, 0, cap * sizeof(rv));
				items = ni;
				cap *= 2;
				out->u.l.items = items;
			}
		}
		if (decode_value(d, &items[n], depth + 1))
			goto fail;
		n++;
		out->u.l.n = n / per;
	}
	out->u.l.n = n / per;
	return 0;
fail:
	/* free what was decoded so far, including a partial item */
	for (size_t i = 0; i <= n && i < cap; i++)
		free_contents(&items[i]);
	free(items);
	out->type = RV_NONE;
	out->u.l.items = NULL;
	return -1;
}

static int decode_value(dec_t *d, rv *out, int depth)
{
	if (depth > MAX_DEPTH || d->p >= d->end)
		return -1;
	uint8_t c = *d->p++;
	size_t avail = (size_t)(d->end - d->p);
	memset(out, 0, sizeof(*out));

	if (c < INT_POS_FIXED_START + INT_POS_FIXED_COUNT) {
		out->type = RV_INT;
		out->u.i = c;
		return 0;
	}
	if (c >= INT_NEG_FIXED_START && c < INT_NEG_FIXED_START + INT_NEG_FIXED_COUNT) {
		out->type = RV_INT;
		out->u.i = -1 - (int64_t)(c - INT_NEG_FIXED_START);
		return 0;
	}
	if (c >= DICT_FIXED_START && c < DICT_FIXED_START + DICT_FIXED_COUNT)
		return decode_items(d, out, RV_DICT, c - DICT_FIXED_START, depth);
	if (c >= STR_FIXED_START && c < STR_FIXED_START + STR_FIXED_COUNT) {
		size_t len = c - STR_FIXED_START;
		if (len > avail)
			return -1;
		out->type = RV_STR;
		out->u.s.p = d->p;
		out->u.s.len = len;
		d->p += len;
		return 0;
	}
	if (c >= LIST_FIXED_START)
		return decode_items(d, out, RV_LIST, c - LIST_FIXED_START, depth);
	if (c >= '0' && c <= '9') {
		/* "len:" utf8 string or "len/" byte string */
		size_t len = c - '0';
		for (;;) {
			if (d->p >= d->end)
				return -1;
			uint8_t ch = *d->p++;
			if (ch == ':' || ch == '/') {
				out->type = ch == ':' ? RV_STR : RV_BYTES;
				break;
			}
			if (ch < '0' || ch > '9' || len > (SIZE_MAX - 9) / 10)
				return -1;
			len = len * 10 + (ch - '0');
		}
		if (len > (size_t)(d->end - d->p))
			return -1;
		out->u.s.p = d->p;
		out->u.s.len = len;
		d->p += len;
		return 0;
	}
	switch (c) {
	case CHR_LIST:
		return decode_items(d, out, RV_LIST, -1, depth);
	case CHR_DICT:
		return decode_items(d, out, RV_DICT, -1, depth);
	case CHR_INT1:
	case CHR_INT2:
	case CHR_INT4:
	case CHR_INT8: {
		int n = c == CHR_INT1 ? 1 : c == CHR_INT2 ? 2 : c == CHR_INT4 ? 4 : 8;
		if (avail < (size_t)n)
			return -1;
		uint64_t u = be_read(d->p, n);
		d->p += n;
		/* sign extend */
		if (n < 8 && (u & ((uint64_t)1 << (n * 8 - 1))))
			u |= ~(uint64_t)0 << (n * 8);
		out->type = RV_INT;
		out->u.i = (int64_t)u;
		return 0;
	}
	case CHR_INT: {
		/* big integer as decimal digits: we only need to skip it, keep an approximation */
		double v = 0;
		int neg = 0, digits = 0;
		for (;;) {
			if (d->p >= d->end)
				return -1;
			uint8_t ch = *d->p++;
			if (ch == CHR_TERM)
				break;
			if (ch == '-' && !digits && !neg)
				neg = 1;
			else if (ch >= '0' && ch <= '9' && digits < 64) {
				v = v * 10 + (ch - '0');
				digits++;
			} else
				return -1;
		}
		if (!digits)
			return -1;
		out->type = RV_FLOAT;
		out->u.f = neg ? -v : v;
		return 0;
	}
	case CHR_FLOAT64: {
		if (avail < 8)
			return -1;
		uint64_t u = be_read(d->p, 8);
		d->p += 8;
		out->type = RV_FLOAT;
		memcpy(&out->u.f, &u, 8);
		return 0;
	}
	case CHR_FLOAT32: {
		if (avail < 4)
			return -1;
		uint32_t u = (uint32_t)be_read(d->p, 4);
		float f;
		d->p += 4;
		memcpy(&f, &u, 4);
		out->type = RV_FLOAT;
		out->u.f = f;
		return 0;
	}
	case CHR_TRUE:
	case CHR_FALSE:
		out->type = RV_BOOL;
		out->u.i = c == CHR_TRUE;
		return 0;
	case CHR_NONE:
		out->type = RV_NONE;
		return 0;
	}
	return -1;
}

rv *rv_decode(const uint8_t *data, size_t len)
{
	dec_t d = { data, data + len };
	rv *v = calloc(1, sizeof(rv));
	if (!v)
		return NULL;
	if (decode_value(&d, v, 0)) {
		free(v);
		return NULL;
	}
	return v;
}

void rv_free(rv *v)
{
	if (!v)
		return;
	free_contents(v);
	free(v);
}

/* ---------------------------------------------------------------- accessors */

rv *rv_list_get(const rv *list, size_t i)
{
	if (!list || list->type != RV_LIST || i >= list->u.l.n)
		return NULL;
	return &list->u.l.items[i];
}

size_t rv_len(const rv *v)
{
	if (!v)
		return 0;
	if (v->type == RV_LIST || v->type == RV_DICT)
		return v->u.l.n;
	if (v->type == RV_STR || v->type == RV_BYTES)
		return v->u.s.len;
	return 0;
}

static int keyeq(const rv *k, const char *key, size_t keylen)
{
	return (k->type == RV_STR || k->type == RV_BYTES) && k->u.s.len == keylen &&
	       memcmp(k->u.s.p, key, keylen) == 0;
}

static rv *dict_getn(const rv *dict, const char *key, size_t keylen)
{
	if (!dict || dict->type != RV_DICT)
		return NULL;
	for (size_t i = 0; i < dict->u.l.n; i++)
		if (keyeq(&dict->u.l.items[i * 2], key, keylen))
			return &dict->u.l.items[i * 2 + 1];
	return NULL;
}

rv *rv_dict_get(const rv *dict, const char *key)
{
	return dict_getn(dict, key, strlen(key));
}

/* Protocol.md section 2: literal flat key first, then split at the first dot and recurse */
static rv *lookup(const rv *dict, const char *path)
{
	rv *v = rv_dict_get(dict, path);
	if (v)
		return v;
	const char *dot = strchr(path, '.');
	if (!dot)
		return NULL;
	rv *sub = dict_getn(dict, path, (size_t)(dot - path));
	if (!sub || sub->type != RV_DICT)
		return NULL;
	return lookup(sub, dot + 1);
}

rv *rv_lookup(const rv *dict, const char *path)
{
	rv *v = lookup(dict, path);
	/* scalar or list lookups that hit a map use its "" entry */
	if (v && v->type == RV_DICT) {
		rv *e = dict_getn(v, "", 0);
		if (e)
			return e;
	}
	return v;
}

int rv_is_str(const rv *v, const char *s)
{
	return v && keyeq(v, s, strlen(s));
}

int rv_strcpy(const rv *v, char *dst, size_t dstlen)
{
	if (!v || (v->type != RV_STR && v->type != RV_BYTES) || !dstlen)
		return -1;
	size_t n = v->u.s.len < dstlen - 1 ? v->u.s.len : dstlen - 1;
	memcpy(dst, v->u.s.p, n);
	dst[n] = 0;
	return 0;
}

int64_t rv_int(const rv *v, int64_t def)
{
	if (v && (v->type == RV_INT || v->type == RV_BOOL))
		return v->u.i;
	return def;
}

int rv_bool(const rv *v, int def)
{
	if (v && (v->type == RV_INT || v->type == RV_BOOL))
		return v->u.i != 0;
	return def;
}

int rv_list_has_str(const rv *list, const char *s)
{
	for (size_t i = 0; i < rv_len(list) && list->type == RV_LIST; i++)
		if (rv_is_str(rv_list_get(list, i), s))
			return 1;
	return 0;
}

/* ------------------------------------------------------------------ encoder */

static void be_write(buf_t *b, uint64_t v, int n)
{
	buf_reserve(b, n);
	for (int i = n - 1; i >= 0; i--)
		b->data[b->len++] = (uint8_t)(v >> (i * 8));
}

void enc_int(buf_t *b, int64_t v)
{
	if (v >= 0 && v < INT_POS_FIXED_COUNT)
		buf_addc(b, (uint8_t)(INT_POS_FIXED_START + v));
	else if (v < 0 && v >= -INT_NEG_FIXED_COUNT)
		buf_addc(b, (uint8_t)(INT_NEG_FIXED_START - 1 - v));
	else if (v >= INT8_MIN && v <= INT8_MAX) {
		buf_addc(b, CHR_INT1);
		be_write(b, (uint64_t)v, 1);
	} else if (v >= INT16_MIN && v <= INT16_MAX) {
		buf_addc(b, CHR_INT2);
		be_write(b, (uint64_t)v, 2);
	} else if (v >= INT32_MIN && v <= INT32_MAX) {
		buf_addc(b, CHR_INT4);
		be_write(b, (uint64_t)v, 4);
	} else {
		buf_addc(b, CHR_INT8);
		be_write(b, (uint64_t)v, 8);
	}
}

void enc_bool(buf_t *b, int v)
{
	buf_addc(b, v ? CHR_TRUE : CHR_FALSE);
}

void enc_none(buf_t *b)
{
	buf_addc(b, CHR_NONE);
}

static void enc_len_prefix(buf_t *b, size_t len, char sep)
{
	char tmp[32];
	int n = 0;
	do {
		tmp[n++] = (char)('0' + len % 10);
		len /= 10;
	} while (len);
	while (n)
		buf_addc(b, (uint8_t)tmp[--n]);
	buf_addc(b, (uint8_t)sep);
}

void enc_strn(buf_t *b, const char *s, size_t len)
{
	if (len < STR_FIXED_COUNT)
		buf_addc(b, (uint8_t)(STR_FIXED_START + len));
	else
		enc_len_prefix(b, len, ':');
	buf_add(b, s, len);
}

void enc_str(buf_t *b, const char *s)
{
	enc_strn(b, s, strlen(s));
}

void enc_bytes(buf_t *b, const void *p, size_t len)
{
	enc_len_prefix(b, len, '/');
	buf_add(b, p, len);
}

void enc_list_var(buf_t *b)
{
	buf_addc(b, CHR_LIST);
}

void enc_dict_var(buf_t *b)
{
	buf_addc(b, CHR_DICT);
}

void enc_end(buf_t *b)
{
	buf_addc(b, CHR_TERM);
}

/* the fixed forms only: larger containers must use the variable forms */
void enc_list(buf_t *b, size_t n)
{
	if (n >= LIST_FIXED_COUNT)
		abort();
	buf_addc(b, (uint8_t)(LIST_FIXED_START + n));
}

void enc_dict(buf_t *b, size_t n)
{
	if (n >= DICT_FIXED_COUNT)
		abort();
	buf_addc(b, (uint8_t)(DICT_FIXED_START + n));
}

void enc_strlist(buf_t *b, const char *const *strs)
{
	size_t n = 0;
	while (strs[n])
		n++;
	if (n < LIST_FIXED_COUNT) {
		enc_list(b, n);
		for (size_t i = 0; i < n; i++)
			enc_str(b, strs[i]);
	} else {
		enc_list_var(b);
		for (size_t i = 0; i < n; i++)
			enc_str(b, strs[i]);
		enc_end(b);
	}
}
