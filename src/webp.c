/* webp picture decoding through the system libwebp (see Subsystems/Encoding.md, "Picture data")
 * only two functions of its stable decoding API are used, so any libwebp.so.7 works */
#include "webp.h"

#include <dlfcn.h>
#include <stdlib.h>

typedef int (*get_info_fn)(const uint8_t *data, size_t len, int *w, int *h);
typedef uint8_t *(*decode_into_fn)(const uint8_t *data, size_t len, uint8_t *out, size_t out_size, int stride);

static get_info_fn get_info;
static decode_into_fn decode_into;

int webp_available(void)
{
	static int loaded;
	if (!loaded) {
		loaded = 1;
		void *lib = dlopen("libwebp.so.7", RTLD_LAZY);
		if (lib) {
			*(void **)&get_info = dlsym(lib, "WebPGetInfo");
			*(void **)&decode_into = dlsym(lib, "WebPDecodeBGRAInto");
		}
	}
	return get_info && decode_into;
}

uint8_t *webp_decode(const uint8_t *data, size_t len, int max_w, int max_h, int *w, int *h)
{
	int iw, ih;
	if (!webp_available() || !get_info(data, len, &iw, &ih) || iw < 1 || ih < 1 || iw > max_w || ih > max_h)
		return NULL;
	size_t size = (size_t)iw * (size_t)ih * 4;
	uint8_t *out = malloc(size);
	if (out && !decode_into(data, len, out, size, iw * 4)) {
		free(out);
		return NULL;
	}
	*w = iw;
	*h = ih;
	return out;
}
