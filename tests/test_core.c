/* unit tests for the protocol building blocks (no display needed) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "cursor.h"
#include "lz4.h"
#include "rencode.h"
#include "sha256.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void unhex(const char *hex, uint8_t *out, size_t *len)
{
	size_t n = strlen(hex) / 2;
	for (size_t i = 0; i < n; i++)
		sscanf(hex + i * 2, "%2hhx", &out[i]);
	*len = n;
}

static void check_bytes(buf_t *b, const char *hex)
{
	uint8_t exp[256];
	size_t n;
	unhex(hex, exp, &n);
	CHECK(b->len == n && !memcmp(b->data, exp, n));
	if (b->len != n || memcmp(b->data, exp, n)) {
		fprintf(stderr, "  expected %s got ", hex);
		for (size_t i = 0; i < b->len; i++)
			fprintf(stderr, "%02x", b->data[i]);
		fprintf(stderr, "\n");
	}
	b->len = 0;
}

static void test_rencode_ints(void)
{
	static const int64_t values[] = {
		0, 1, 43, 44, -1, -32, -33, 127, -128, 128, 32767, -32768, 32768, 2147483647,
		-2147483648LL, 2147483648LL, INT64_MAX, INT64_MIN,
	};
	buf_t b = { 0 };
	/* known encodings */
	enc_int(&b, 0);
	check_bytes(&b, "00");
	enc_int(&b, 43);
	check_bytes(&b, "2b");
	enc_int(&b, 44);
	check_bytes(&b, "3e2c");
	enc_int(&b, -1);
	check_bytes(&b, "46");
	enc_int(&b, -32);
	check_bytes(&b, "65");
	enc_int(&b, -33);
	check_bytes(&b, "3edf");
	enc_int(&b, 1000);
	check_bytes(&b, "3f03e8");
	enc_int(&b, 0xa0000c);
	check_bytes(&b, "4000a0000c");
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		b.len = 0;
		enc_int(&b, values[i]);
		rv *v = rv_decode(b.data, b.len);
		CHECK(v && v->type == RV_INT && v->u.i == values[i]);
		rv_free(v);
	}
	buf_free(&b);
}

static void test_rencode_strings(void)
{
	buf_t b = { 0 };
	enc_bytes(&b, "", 0);
	check_bytes(&b, "302f");	/* "0/" */
	enc_str(&b, "hello");
	check_bytes(&b, "8568656c6c6f");
	enc_bool(&b, 1);
	enc_bool(&b, 0);
	enc_none(&b);
	check_bytes(&b, "434445");

	/* long string uses "len:" */
	char longstr[100];
	memset(longstr, 'x', sizeof(longstr));
	enc_strn(&b, longstr, sizeof(longstr));
	CHECK(!memcmp(b.data, "100:", 4));
	rv *v = rv_decode(b.data, b.len);
	CHECK(v && v->type == RV_STR && v->u.s.len == 100);
	rv_free(v);
	b.len = 0;

	/* the long form must be accepted for short strings too */
	v = rv_decode((const uint8_t *)"3:abc", 5);
	CHECK(v && v->type == RV_STR && v->u.s.len == 3 && !memcmp(v->u.s.p, "abc", 3));
	rv_free(v);
	v = rv_decode((const uint8_t *)"3/abc", 5);
	CHECK(v && v->type == RV_BYTES && v->u.s.len == 3);
	rv_free(v);
	/* truncated */
	CHECK(!rv_decode((const uint8_t *)"5/abc", 5));
	CHECK(!rv_decode((const uint8_t *)"\x85" "abc", 4));
	buf_free(&b);
}

static void test_rencode_containers(void)
{
	buf_t b = { 0 };
	enc_list(&b, 3);
	enc_str(&b, "ping");
	enc_int(&b, 5);
	enc_dict(&b, 1);
	enc_str(&b, "a");
	enc_list(&b, 2);
	enc_int(&b, 1);
	enc_int(&b, 2);
	rv *v = rv_decode(b.data, b.len);
	CHECK(v && v->type == RV_LIST && v->u.l.n == 3);
	CHECK(rv_is_str(rv_list_get(v, 0), "ping"));
	CHECK(rv_int(rv_list_get(v, 1), 0) == 5);
	rv *a = rv_dict_get(rv_list_get(v, 2), "a");
	CHECK(a && rv_len(a) == 2 && rv_int(rv_list_get(a, 1), 0) == 2);
	rv_free(v);

	/* variable list with 100 items, variable dict with 30 pairs */
	b.len = 0;
	enc_list_var(&b);
	for (int i = 0; i < 100; i++)
		enc_int(&b, i);
	enc_end(&b);
	v = rv_decode(b.data, b.len);
	CHECK(v && v->type == RV_LIST && v->u.l.n == 100 && rv_int(rv_list_get(v, 99), 0) == 99);
	rv_free(v);

	b.len = 0;
	enc_dict_var(&b);
	for (int i = 0; i < 30; i++) {
		char k[8];
		snprintf(k, sizeof(k), "k%d", i);
		enc_str(&b, k);
		enc_int(&b, i * 10);
	}
	enc_end(&b);
	v = rv_decode(b.data, b.len);
	CHECK(v && v->type == RV_DICT && v->u.l.n == 30 && rv_int(rv_dict_get(v, "k29"), 0) == 290);
	rv_free(v);

	/* unterminated variable list */
	b.len = 0;
	enc_list_var(&b);
	enc_int(&b, 1);
	CHECK(!rv_decode(b.data, b.len));

	/* integer dict keys: {0: "x"} */
	b.len = 0;
	enc_dict(&b, 1);
	enc_int(&b, 0);
	enc_str(&b, "x");
	v = rv_decode(b.data, b.len);
	CHECK(v && v->type == RV_DICT && v->u.l.n == 1);
	rv_free(v);

	/* nesting limit */
	b.len = 0;
	for (int i = 0; i < 100; i++)
		enc_list(&b, 1);
	enc_int(&b, 0);
	CHECK(!rv_decode(b.data, b.len));
	buf_free(&b);
}

static void test_rencode_lookup(void)
{
	buf_t b = { 0 };
	/* {"wheel.precise": true, "encoding": {"options": ["rgb"]}, "encodings": {"": ["png"], "core": ["png"]}} */
	enc_dict(&b, 3);
	enc_str(&b, "wheel.precise");
	enc_bool(&b, 1);
	enc_str(&b, "encoding");
	enc_dict(&b, 1);
	enc_str(&b, "options");
	enc_list(&b, 1);
	enc_str(&b, "rgb");
	enc_str(&b, "encodings");
	enc_dict(&b, 2);
	enc_str(&b, "");
	enc_list(&b, 1);
	enc_str(&b, "png");
	enc_str(&b, "core");
	enc_list(&b, 1);
	enc_str(&b, "jpeg");
	rv *v = rv_decode(b.data, b.len);
	CHECK(rv_bool(rv_lookup(v, "wheel.precise"), 0));
	CHECK(rv_list_has_str(rv_lookup(v, "encoding.options"), "rgb"));
	CHECK(rv_list_has_str(rv_lookup(v, "encodings"), "png"));
	CHECK(rv_list_has_str(rv_lookup(v, "encodings.core"), "jpeg"));
	CHECK(!rv_lookup(v, "missing.key"));
	rv_free(v);
	buf_free(&b);
}

static void test_rencode_print(void)
{
	buf_t b = { 0 };
	enc_list(&b, 7);
	enc_str(&b, "a\"\n");
	enc_bytes(&b, "\x01z", 2);
	enc_int(&b, -5);
	enc_bool(&b, 1);
	enc_none(&b);
	enc_list(&b, 0);
	enc_dict(&b, 1);
	enc_str(&b, "k");
	enc_int(&b, 1);
	rv *v = rv_decode(b.data, b.len);
	char *out = NULL;
	size_t len = 0;
	FILE *f = open_memstream(&out, &len);
	rv_print(f, v);
	rv_print(f, NULL);
	fclose(f);
	CHECK(out && !strcmp(out, "[\"a\\\"\\x0a\", b\"\\x01z\", -5, True, None, [], {\"k\": 1}]None"));
	free(out);
	rv_free(v);
	buf_free(&b);
}

static void test_float(void)
{
	/* 1.5 as binary64 */
	const uint8_t f64[] = { 44, 0x3f, 0xf8, 0, 0, 0, 0, 0, 0 };
	rv *v = rv_decode(f64, sizeof(f64));
	CHECK(v && v->type == RV_FLOAT && v->u.f == 1.5);
	rv_free(v);
	const uint8_t f32[] = { 66, 0x3f, 0xc0, 0, 0 };
	v = rv_decode(f32, sizeof(f32));
	CHECK(v && v->type == RV_FLOAT && v->u.f == 1.5);
	rv_free(v);
}

static void test_lz4(void)
{
	const char *expected = "hello hello hello hello hello world, hello hello hello!";
	uint8_t src[128], dst[128];
	size_t n;
	unhex("6f68656c6c6f200600056a776f726c642c1f0050656c6c6f21", src, &n);
	CHECK(!lz4_decompress(src, n, dst, strlen(expected)));
	CHECK(!memcmp(dst, expected, strlen(expected)));
	/* wrong output size */
	CHECK(lz4_decompress(src, n, dst, strlen(expected) - 1));
	CHECK(lz4_decompress(src, n, dst, strlen(expected) + 1));
	/* truncated input */
	CHECK(lz4_decompress(src, n - 3, dst, strlen(expected)));
	/* offset pointing before the start of the output */
	const uint8_t bad[] = { 0x10, 'a', 0x05, 0x00 };
	CHECK(lz4_decompress(bad, sizeof(bad), dst, 5));
	/* sized variant */
	uint8_t sized[132];
	size_t len = strlen(expected), outlen = 0;
	sized[0] = (uint8_t)len;
	sized[1] = sized[2] = sized[3] = 0;
	memcpy(sized + 4, src, n);
	uint8_t *out = lz4_decompress_sized(sized, n + 4, 1024, &outlen);
	CHECK(out && outlen == len && !memcmp(out, expected, len));
	free(out);
	CHECK(!lz4_decompress_sized(sized, n + 4, 10, &outlen));
}

static void test_sha256(void)
{
	uint8_t out[32];
	char hex[65];
	sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, "abc", 3);
	sha256_final(&c, out);
	tohex(out, 32, hex);
	CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

	/* Porting/Authentication.md test vectors */
	uint8_t server_salt[64], client_salt[32];
	for (int i = 0; i < 64; i++)
		server_salt[i] = (uint8_t)i;
	for (int i = 0; i < 32; i++)
		client_salt[i] = (uint8_t)(0xa0 + i);
	char salt[65];
	hmac_sha256(client_salt, 32, server_salt, 64, out);
	tohex(out, 32, salt);
	CHECK(!strcmp(salt, "9af35b27ab791298b28e4e70a13342e8f94ae85ef9f8a7bcf2e28d49a113ede6"));
	hmac_sha256((const uint8_t *)"secret", 6, (const uint8_t *)salt, 64, out);
	tohex(out, 32, hex);
	CHECK(!strcmp(hex, "9d85c3522cfa016edb779f946f077ee9c34d647b35e3b6cc388d090dc809c323"));
	/* "pässwörd€" UTF-8 encoded */
	const char *pw = "p\xc3\xa4ssw\xc3\xb6rd\xe2\x82\xac";
	hmac_sha256((const uint8_t *)pw, strlen(pw), (const uint8_t *)salt, 64, out);
	tohex(out, 32, hex);
	CHECK(!strcmp(hex, "35ecd3610ec3de9d10cefd1d5be0719c4f0a10a4a821b0dada4392fb9f010850"));
}

/* build ["cursor-data", enc, w, h, xhot, yhot, serial, data] with data of len bytes, then decode it */
static int cursor_packet(const char *enc, int64_t w, int64_t h, int64_t xhot, int64_t yhot,
			 const uint8_t *data, size_t len, size_t nitems, cursor_image *ci)
{
	buf_t b = { 0 };
	enc_list(&b, nitems);
	enc_str(&b, "cursor-data");
	enc_str(&b, enc);
	int64_t ints[] = { w, h, xhot, yhot, 7 };
	for (size_t i = 0; i < 5 && i + 2 < nitems; i++)
		enc_int(&b, ints[i]);
	if (nitems > 7)
		enc_bytes(&b, data, len);
	if (nitems > 8)
		enc_str(&b, "left_ptr");
	rv *pk = rv_decode(b.data, b.len);
	CHECK(pk != NULL);
	const char *err = NULL;
	int r = cursor_decode(pk, ci, &err);
	CHECK(r >= 0 || err != NULL);
	CHECK(r > 0 || ci->pixels == NULL);
	rv_free(pk);
	buf_free(&b);
	return r;
}

static void test_cursor(void)
{
	static uint8_t big[4 * CURSOR_MAX * CURSOR_MAX + 4];
	/* 2x1 RGBA, straight alpha: opaque red, half transparent white */
	const uint8_t px[8] = { 0xff, 0, 0, 0xff, 0xff, 0xff, 0xff, 0x80 };
	cursor_image ci;
	CHECK(cursor_packet("raw", 2, 1, 1, 0, px, 8, 9, &ci) == 1);
	CHECK(ci.w == 2 && ci.h == 1 && ci.xhot == 1 && ci.yhot == 0);
	CHECK(ci.pixels && ci.pixels[0] == 0xffff0000u && ci.pixels[1] == 0x80808080u);
	free(ci.pixels);
	/* the name is optional */
	CHECK(cursor_packet("raw", 2, 1, 0, 0, px, 8, 8, &ci) == 1);
	free(ci.pixels);
	/* the largest size */
	CHECK(cursor_packet("raw", CURSOR_MAX, CURSOR_MAX, CURSOR_MAX - 1, CURSOR_MAX - 1,
			    big, 4 * CURSOR_MAX * CURSOR_MAX, 9, &ci) == 1);
	free(ci.pixels);
	/* empty encoding: default cursor, even without the other fields */
	CHECK(cursor_packet("", 0, 0, 0, 0, NULL, 0, 2, &ci) == 0);
	CHECK(cursor_packet("", 2, 1, 0, 0, px, 8, 9, &ci) == 0);
	/* not advertised */
	CHECK(cursor_packet("png", 2, 1, 0, 0, px, 8, 9, &ci) < 0);
	CHECK(cursor_packet("rawx", 2, 1, 0, 0, px, 8, 9, &ci) < 0);
	/* truncated */
	CHECK(cursor_packet("raw", 2, 1, 0, 0, px, 8, 7, &ci) < 0);
	CHECK(cursor_packet("raw", 2, 1, 0, 0, px, 8, 3, &ci) < 0);
	/* sizes */
	CHECK(cursor_packet("raw", 0, 1, 0, 0, px, 0, 9, &ci) < 0);
	CHECK(cursor_packet("raw", 2, -1, 0, 0, px, 8, 9, &ci) < 0);
	CHECK(cursor_packet("raw", CURSOR_MAX + 1, 1, 0, 0, big, 4 * (CURSOR_MAX + 1), 9, &ci) < 0);
	CHECK(cursor_packet("raw", (int64_t)1 << 32, 1, 0, 0, px, 8, 9, &ci) < 0);
	CHECK(cursor_packet("raw", INT64_MIN, 1, 0, 0, px, 8, 9, &ci) < 0);
	/* hotspot outside the image */
	CHECK(cursor_packet("raw", 2, 1, 2, 0, px, 8, 9, &ci) < 0);
	CHECK(cursor_packet("raw", 2, 1, 0, 1, px, 8, 9, &ci) < 0);
	CHECK(cursor_packet("raw", 2, 1, -1, 0, px, 8, 9, &ci) < 0);
	/* the pixel data must be exactly w * h * 4 bytes */
	CHECK(cursor_packet("raw", 2, 1, 0, 0, px, 7, 9, &ci) < 0);
	CHECK(cursor_packet("raw", 2, 1, 0, 0, big, 12, 9, &ci) < 0);
	CHECK(cursor_packet("raw", 2, 1, 0, 0, px, 0, 9, &ci) < 0);

	/* wrong types: bool size, int encoding, int data, not a list */
	const char *err;
	buf_t b = { 0 };
	enc_list(&b, 8);
	enc_str(&b, "cursor-data");
	enc_str(&b, "raw");
	enc_bool(&b, 1);
	enc_bool(&b, 1);
	enc_int(&b, 0);
	enc_int(&b, 0);
	enc_int(&b, 0);
	enc_bytes(&b, px, 4);
	rv *pk = rv_decode(b.data, b.len);
	CHECK(cursor_decode(pk, &ci, &err) < 0 && !ci.pixels);
	rv_free(pk);
	b.len = 0;
	enc_list(&b, 8);
	enc_str(&b, "cursor-data");
	enc_str(&b, "raw");
	for (int i = 0; i < 5; i++)
		enc_int(&b, i < 2 ? 1 : 0);
	enc_int(&b, 0);
	pk = rv_decode(b.data, b.len);
	CHECK(cursor_decode(pk, &ci, &err) < 0 && !ci.pixels);
	rv_free(pk);
	b.len = 0;
	enc_list(&b, 2);
	enc_str(&b, "cursor-data");
	enc_int(&b, 0);
	pk = rv_decode(b.data, b.len);
	CHECK(cursor_decode(pk, &ci, &err) < 0 && !ci.pixels);
	rv_free(pk);
	b.len = 0;
	enc_dict(&b, 0);
	pk = rv_decode(b.data, b.len);
	CHECK(cursor_decode(pk, &ci, &err) < 0 && !ci.pixels);
	CHECK(cursor_decode(NULL, &ci, &err) < 0 && !ci.pixels);
	rv_free(pk);
	buf_free(&b);
}

int main(void)
{
	test_rencode_ints();
	test_rencode_strings();
	test_rencode_containers();
	test_rencode_lookup();
	test_rencode_print();
	test_float();
	test_lz4();
	test_sha256();
	test_cursor();
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
