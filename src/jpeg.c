/* jpeg picture decoding through the system libturbojpeg (see Subsystems/Encoding.md, "Picture data")
 * uses the TurboJPEG 1.4 API: an opaque handle, no libjpeg structs and no setjmp in the caller,
 * and the same soname (libturbojpeg.so.0) everywhere, libjpeg-turbo 3.x included */
#include "jpeg.h"

#include <dlfcn.h>
#include <stdlib.h>

#define TJPF_BGRA 8

typedef void *(*init_fn)(void);
typedef int (*header_fn)(void *handle, const uint8_t *data, unsigned long len, int *w, int *h,
			 int *subsamp, int *colorspace);
typedef int (*decompress_fn)(void *handle, const uint8_t *data, unsigned long len, uint8_t *out,
			     int w, int pitch, int h, int format, int flags);

static header_fn header;
static decompress_fn decompress;
static void *handle;	/* kept for the lifetime of the process */

int jpeg_available(void)
{
	static int loaded;
	if (!loaded) {
		loaded = 1;
		void *lib = dlopen("libturbojpeg.so.0", RTLD_LAZY);
		if (lib) {
			init_fn init;
			*(void **)&init = dlsym(lib, "tjInitDecompress");
			*(void **)&header = dlsym(lib, "tjDecompressHeader3");
			*(void **)&decompress = dlsym(lib, "tjDecompress2");
			if (init)
				handle = init();
		}
	}
	return handle && header && decompress;
}

uint8_t *jpeg_decode(const uint8_t *data, size_t len, int max_w, int max_h, int *w, int *h)
{
	int iw, ih, subsamp, colorspace;
	if (!jpeg_available() || header(handle, data, (unsigned long)len, &iw, &ih, &subsamp, &colorspace) ||
	    iw < 1 || ih < 1 || iw > max_w || ih > max_h)
		return NULL;
	uint8_t *out = malloc((size_t)iw * (size_t)ih * 4);
	/* a warning (ie: truncated data) also returns -1: the server refreshes the area */
	if (out && decompress(handle, data, (unsigned long)len, out, iw, iw * 4, ih, TJPF_BGRA, 0)) {
		free(out);
		return NULL;
	}
	*w = iw;
	*h = ih;
	return out;
}
