/* clipboard packet validation and text conversion, see Subsystems/Clipboard.md and Protocol.md 8.6 */
#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include <stddef.h>
#include <stdint.h>

#include "backend.h"
#include "rencode.h"

/* the largest clipboard text exchanged, in bytes */
#define CLIP_MAX (1 << 20)

/* SEL_CLIPBOARD or SEL_PRIMARY, -1 for any other selection */
int clip_selection(const rv *name);
const char *clip_selection_name(int sel);

/* the text targets we offer and accept, NULL terminated */
extern const char *const CLIP_TARGETS[];
int clip_is_text_target(const rv *target);

/* validate a clipboard-contents packet: 8-bit "bytes" of a text type, at most CLIP_MAX
 * 0: *data and *len point into the packet, *latin1 is set for the STRING type
 * -1: invalid or not text, *err says why */
int clip_contents(const rv *pk, const uint8_t **data, size_t *len, int *latin1, const char **err);

/* conversions to a malloc'd, nul terminated buffer (NULL when out of memory)
 * utf8 to latin1 replaces the characters Latin-1 lacks, and invalid sequences, with '?' */
char *clip_latin1_to_utf8(const uint8_t *s, size_t n, size_t *outlen);
char *clip_utf8_to_latin1(const uint8_t *s, size_t n, size_t *outlen);

#endif
