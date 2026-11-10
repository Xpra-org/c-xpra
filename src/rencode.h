/* rencodeplus encoder / decoder, see Network/Protocol.md section 4.2 */
#ifndef RENCODE_H
#define RENCODE_H

#include <stddef.h>
#include <stdint.h>

#include "buf.h"

typedef enum {
	RV_NONE, RV_BOOL, RV_INT, RV_FLOAT, RV_STR, RV_BYTES, RV_LIST, RV_DICT
} rv_type;

typedef struct rv rv;
struct rv {
	rv_type type;
	union {
		int64_t i;	/* RV_INT, RV_BOOL */
		double f;	/* RV_FLOAT */
		struct {	/* RV_STR, RV_BYTES: points into the source buffer, NOT nul terminated */
			const uint8_t *p;
			size_t len;
		} s;
		struct {	/* RV_LIST: n items, RV_DICT: n pairs stored as 2n items (k0,v0,k1,v1...) */
			rv *items;
			size_t n;
		} l;
	} u;
};

/* decode exactly one value from data[0..len), returns NULL on error
 * the strings point into data, which must outlive the result */
rv *rv_decode(const uint8_t *data, size_t len);
void rv_free(rv *v);

/* accessors, all NULL / default safe */
rv *rv_list_get(const rv *list, size_t i);
size_t rv_len(const rv *v);
rv *rv_dict_get(const rv *dict, const char *key);	/* exact key */
rv *rv_lookup(const rv *dict, const char *path);	/* "a.b": flat key first, then nested, "" rule */
int rv_is_str(const rv *v, const char *s);
int rv_strcpy(const rv *v, char *dst, size_t dstlen);	/* str or bytes, returns 0 on success */
int64_t rv_int(const rv *v, int64_t def);			/* int or bool */
int rv_bool(const rv *v, int def);
int rv_list_has_str(const rv *list, const char *s);

/* encoder */
void enc_int(buf_t *b, int64_t v);
void enc_bool(buf_t *b, int v);
void enc_none(buf_t *b);
void enc_str(buf_t *b, const char *s);
void enc_strn(buf_t *b, const char *s, size_t len);
void enc_bytes(buf_t *b, const void *p, size_t len);
void enc_list(buf_t *b, size_t n);	/* n < 64 values must follow */
void enc_dict(buf_t *b, size_t n);	/* n < 25 key/value pairs must follow */
/* variable forms, terminated with enc_end() */
void enc_list_var(buf_t *b);
void enc_dict_var(buf_t *b);
void enc_end(buf_t *b);
void enc_strlist(buf_t *b, const char *const *strs);	/* NULL terminated */

#endif
