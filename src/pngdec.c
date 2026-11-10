/* png picture decoding through libpng's simplified API (libpng 1.6+), which handles
 * every colour type and bit depth (png, png/P and png/L) without setjmp in the caller.
 * see Subsystems/Encoding.md, "Picture data" */
#include "pngdec.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

/* libpng's png_image (a stable ABI) */
typedef struct {
	void *opaque;
	uint32_t version, width, height, format, flags, colormap_entries, warning_or_error;
	char message[64];
} png_image;
#define PNG_IMAGE_VERSION 1
#define PNG_FORMAT_BGRA 0x13	/* alpha | colour | BGR */

typedef int (*begin_fn)(png_image *image, const void *data, size_t len);
typedef int (*finish_fn)(png_image *image, const void *background, void *buffer, int32_t stride, void *colormap);
typedef void (*free_fn)(png_image *image);

static begin_fn begin;
static finish_fn finish;
static free_fn image_free;

int pngdec_available(void)
{
	static int loaded;
	if (!loaded) {
		loaded = 1;
		void *lib = dlopen("libpng16.so.16", RTLD_LAZY);
		if (lib) {
			*(void **)&begin = dlsym(lib, "png_image_begin_read_from_memory");
			*(void **)&finish = dlsym(lib, "png_image_finish_read");
			*(void **)&image_free = dlsym(lib, "png_image_free");
		}
	}
	return begin && finish && image_free;
}

uint8_t *pngdec_decode(const uint8_t *data, size_t len, int max_w, int max_h, int *w, int *h)
{
	if (!pngdec_available())
		return NULL;
	png_image img;
	memset(&img, 0, sizeof(img));
	img.version = PNG_IMAGE_VERSION;
	if (!begin(&img, data, len))
		return NULL;	/* libpng has already freed img */
	uint8_t *out = NULL;
	if (img.width >= 1 && img.height >= 1 && img.width <= (uint32_t)max_w && img.height <= (uint32_t)max_h) {
		img.format = PNG_FORMAT_BGRA;
		out = malloc((size_t)img.width * img.height * 4);
		/* finish_read releases img, whatever the outcome */
		if (out && !finish(&img, NULL, out, 0, NULL)) {
			free(out);
			out = NULL;
		}
	}
	image_free(&img);
	if (out) {
		*w = (int)img.width;
		*h = (int)img.height;
	}
	return out;
}
