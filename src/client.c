/* xpra protocol logic: handshake, authentication, windows and input
 * see Porting/Minimal-Client.md and Porting/Handshake.md */
#include "client.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "backend.h"
#include "clipboard.h"
#include "cursor.h"
#include "rencode.h"
#include "sha256.h"
#include "lz4.h"
#include "pngdec.h"
#include "jpeg.h"
#include "webp.h"

#define PROTOCOL_MAJOR 8
#define PROTOCOL_MINOR 0
#define CLIENT_VERSION "0.2.0"
#define AUTH_DIGEST "hmac+sha256"

/* the largest window icon accepted (the server scales them down to 64x64 by default) */
#define ICON_MAX 256

typedef struct win {
	uint64_t wid;
	int x, y, w, h;		/* server geometry */
	int override_redirect;
	int64_t resize_counter;
	be_window *be;
	struct win *next;
} win;

static struct {
	const client_opts *opts;
	conn_t conn;
	char uuid[33];
	int connected;		/* server hello received */
	int challenged;
	int readonly;
	int wheel;		/* server accepts pointer-wheel: wheel.precise or wheel.emulation */
	int exit_code;
	int done;
	uint64_t focused;
	win *windows;
	/* clipboard: the directions the server allows, and the selections it synchronizes */
	int clip_enabled, clip_to_server, clip_to_client;
	unsigned clip_sels;
	char clip_origin[17];	/* of our last clipboard-data, to detect loops */
} C;

static volatile sig_atomic_t quit_signal;

static const char *const MOD_NAMES[MOD_COUNT] = {
	"shift", "lock", "control", "mod1", "mod2", "mod3", "mod4", "mod5",
};

/* ------------------------------------------------------------------ helpers */

static int64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void random_bytes(uint8_t *p, size_t n)
{
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	size_t got = 0;
	while (fd >= 0 && got < n) {
		ssize_t r = read(fd, p + got, n - got);
		if (r <= 0)
			break;
		got += (size_t)r;
	}
	if (fd >= 0)
		close(fd);
	if (got < n) {
		fprintf(stderr, "cannot read random bytes\n");
		exit(1);
	}
}

static void send_packet(buf_t *b)
{
	if (C.opts->debug) {
		rv *v = rv_decode(b->data, b->len);
		rv *t = rv_list_get(v, 0);
		if (t)
			fprintf(stderr, "> %.*s\n", (int)t->u.s.len, (const char *)t->u.s.p);
		rv_free(v);
	}
	if (C.conn.fd >= 0 && conn_send(&C.conn, b)) {
		fprintf(stderr, "write error: %s\n", strerror(errno));
		C.done = 1;
		C.exit_code = 1;
	}
	buf_free(b);
}

static void enc_mods(buf_t *b, unsigned mods)
{
	int n = 0;
	for (int i = 0; i < MOD_COUNT; i++)
		n += (mods >> i) & 1;
	enc_list(b, (size_t)n);
	for (int i = 0; i < MOD_COUNT; i++)
		if (mods & (1u << i))
			enc_str(b, MOD_NAMES[i]);
}

static void enc_buttons(buf_t *b, unsigned buttons)
{
	int n = 0;
	for (int i = 0; i < 32; i++)
		n += (buttons >> i) & 1;
	enc_list(b, (size_t)n);
	for (int i = 0; i < 32; i++)
		if (buttons & (1u << i))
			enc_int(b, i + 1);
}

static void enc_pair(buf_t *b, int64_t a, int64_t c)
{
	enc_list(b, 2);
	enc_int(b, a);
	enc_int(b, c);
}

static win *find_window(uint64_t wid)
{
	for (win *w = C.windows; w; w = w->next)
		if (w->wid == wid)
			return w;
	return NULL;
}

/* -------------------------------------------------------------- handshake */

static void send_hello(const uint8_t *response, size_t rlen, const uint8_t *client_salt, size_t slen,
		       const uint8_t *padding, size_t plen)
{
	static const char *const encoders[] = { "rencodeplus", NULL };
	static const char *const compressors[] = { "lz4", "none", NULL };
	static const char *const digests[] = { AUTH_DIGEST, NULL };
	/* png, webp and jpeg are only advertised when libpng / libwebp / libturbojpeg can be loaded */
	static const char *const pictures[] = { "png", "png/P", "png/L", "webp", "jpeg" };
	int png = pngdec_available();
	const int have[] = { png, png, png, webp_available(), jpeg_available() };
	const char *enc_options[7] = { "rgb" }, *enc_core[8] = { "rgb24", "rgb32" };
	size_t no = 1, nc = 2;
	for (size_t i = 0; i < 5; i++) {
		if (have[i])
			enc_options[no++] = enc_core[nc++] = pictures[i];
	}
	static const char *const rgb_formats[] = { "BGRX", "BGRA", "RGBX", "RGBA", "BGR", "RGB", NULL };
	static const char *const cursor_encodings[] = { "raw", NULL };
	static const char *const icon_encodings[] = { "png", NULL };
	static const char *const metadata[] = {
		"title", "override-redirect", "transient-for", "size-constraints", NULL,
	};
	const char *user = getenv("USER");
	int dw, dh;
	be_screen_size(&dw, &dh);

	buf_t b = { 0 };
	enc_list(&b, 2);
	enc_str(&b, "hello");
	enc_dict_var(&b);
	enc_str(&b, "version");
	enc_str(&b, CLIENT_VERSION);
	enc_str(&b, "protocol-version");
	enc_pair(&b, PROTOCOL_MAJOR, PROTOCOL_MINOR);
	enc_str(&b, "encoders");
	enc_strlist(&b, encoders);
	enc_str(&b, "compressors");
	enc_strlist(&b, compressors);
	enc_str(&b, "compression_level");
	enc_int(&b, 1);
	enc_str(&b, "uuid");
	enc_str(&b, C.uuid);
	enc_str(&b, "client_type");
	enc_str(&b, "c-xpra");
	enc_str(&b, "username");
	enc_str(&b, user ? user : "");
	enc_str(&b, "digest");
	enc_strlist(&b, digests);
	enc_str(&b, "salt-digest");
	enc_strlist(&b, digests);
	enc_str(&b, "ping");
	enc_bool(&b, 1);
	enc_str(&b, "server-features");
	enc_bool(&b, 1);
	enc_str(&b, "build");
	enc_dict(&b, 1);
	enc_str(&b, "version");
	enc_str(&b, CLIENT_VERSION);

	enc_str(&b, "display");
	enc_dict(&b, 3);
	enc_str(&b, "desktop_size");
	enc_pair(&b, dw, dh);
	enc_str(&b, "monitor-relative");
	enc_bool(&b, 0);
	enc_str(&b, "monitors");
	enc_dict(&b, 1);
	enc_int(&b, 0);
	enc_dict(&b, 2);
	enc_str(&b, "geometry");
	enc_list(&b, 4);
	enc_int(&b, 0);
	enc_int(&b, 0);
	enc_int(&b, dw);
	enc_int(&b, dh);
	enc_str(&b, "primary");
	enc_bool(&b, 1);

	enc_str(&b, "window");
	enc_dict(&b, 1);
	enc_str(&b, "enabled");
	enc_bool(&b, 1);
	enc_str(&b, "metadata.supported");
	enc_strlist(&b, metadata);

	enc_str(&b, "encoding");
	enc_dict(&b, 5 + png);
	if (png) {
		/* Window.md: window icons are png files */
		enc_str(&b, "window-icon");
		enc_strlist(&b, icon_encodings);
	}
	enc_str(&b, "options");
	enc_strlist(&b, enc_options);
	enc_str(&b, "core");
	enc_strlist(&b, enc_core);
	enc_str(&b, "rgb_formats");
	enc_strlist(&b, rgb_formats);
	enc_str(&b, "rgb_lz4");
	enc_bool(&b, 1);
	enc_str(&b, "transparency");
	enc_bool(&b, 0);

	enc_str(&b, "bell");
	enc_bool(&b, 1);
	enc_str(&b, "events");
	enc_bool(&b, 1);
	enc_str(&b, "notification");
	enc_bool(&b, 1);
	enc_str(&b, "keyboard");
	enc_bool(&b, 1);
	enc_str(&b, "modifiers");
	enc_list(&b, 0);
	enc_str(&b, "keymap");
	enc_dict(&b, 1);
	enc_str(&b, "layout");
	enc_str(&b, be_layout());
	enc_str(&b, "pointer");
	enc_dict(&b, 1);
	enc_str(&b, "double_click");
	enc_dict(&b, 2);
	enc_str(&b, "time");
	enc_int(&b, 400);
	enc_str(&b, "distance");
	enc_pair(&b, 5, 5);	/* x, y: the server rejects a plain int */
	enc_str(&b, "cursor");
	enc_dict(&b, 2);
	enc_str(&b, "encodings");
	enc_strlist(&b, cursor_encodings);
	enc_str(&b, "max");
	enc_pair(&b, CURSOR_MAX, CURSOR_MAX);
	if (be_clipboard_available()) {
		/* not greedy: the contents are only sent when requested */
		static const char *const selections[] = { "CLIPBOARD", "PRIMARY", NULL };
		enc_str(&b, "clipboard");
		enc_dict(&b, 3);
		enc_str(&b, "enabled");
		enc_bool(&b, 1);
		enc_str(&b, "selections");
		enc_strlist(&b, selections);
		enc_str(&b, "preferred-targets");
		enc_strlist(&b, CLIP_TARGETS);
	}

	if (response) {
		enc_str(&b, "challenge_response");
		enc_bytes(&b, response, rlen);
		enc_str(&b, "challenge_client_salt");
		enc_bytes(&b, client_salt, slen);
		enc_str(&b, "challenge_padding");
		enc_bytes(&b, padding, plen);
	}
	enc_end(&b);
	send_packet(&b);
}

static char *read_password_tty(const char *prompt)
{
	static char pw[256];
	FILE *tty = fopen("/dev/tty", "r+");
	if (!tty) {
		fprintf(stderr, "no password available and no terminal to ask for one\n");
		return NULL;
	}
	struct termios old, noecho;
	int fd = fileno(tty), restore = !tcgetattr(fd, &old);
	if (restore) {
		noecho = old;
		noecho.c_lflag &= ~(tcflag_t)ECHO;
		tcsetattr(fd, TCSAFLUSH, &noecho);
	}
	fprintf(tty, "%s: ", prompt);
	fflush(tty);
	char *r = fgets(pw, sizeof(pw), tty);
	if (restore)
		tcsetattr(fd, TCSAFLUSH, &old);
	fprintf(tty, "\n");
	fclose(tty);
	if (!r)
		return NULL;
	pw[strcspn(pw, "\r\n")] = 0;
	return pw;
}

/* Authentication.md: Latin-1 when every character fits, UTF-8 otherwise */
static size_t password_bytes(const char *utf8, uint8_t *out, size_t outlen)
{
	size_t n = 0, len = strlen(utf8);
	const uint8_t *s = (const uint8_t *)utf8;
	for (size_t i = 0; i < len; n++) {
		unsigned cp;
		if (s[i] < 0x80) {
			cp = s[i++];
		} else if ((s[i] & 0xe0) == 0xc0 && i + 1 < len) {
			cp = (unsigned)(s[i] & 0x1f) << 6 | (s[i + 1] & 0x3f);
			i += 2;
		} else {
			cp = 0x100;	/* 3 or 4 byte sequences are never Latin-1 */
		}
		if (cp > 0xff || n >= outlen) {
			n = len < outlen ? len : outlen;
			memcpy(out, utf8, n);
			return n;
		}
		out[n] = (uint8_t)cp;
	}
	return n;
}

static void handle_challenge(rv *pk)
{
	rv *salt = rv_list_get(pk, 1);
	char digest[64] = "", salt_digest[64] = "", prompt[256] = "password";
	rv_strcpy(rv_list_get(pk, 3), digest, sizeof(digest));
	rv_strcpy(rv_list_get(pk, 4), salt_digest, sizeof(salt_digest));
	rv_strcpy(rv_list_get(pk, 5), prompt, sizeof(prompt));
	digest[strcspn(digest, ":")] = 0;
	if (!salt || (salt->type != RV_BYTES && salt->type != RV_STR)) {
		fprintf(stderr, "invalid challenge packet\n");
		C.done = C.exit_code = 1;
		return;
	}
	if (strcmp(digest, AUTH_DIGEST) || strcmp(salt_digest, AUTH_DIGEST)) {
		fprintf(stderr, "unsupported authentication digest '%s' / '%s'\n", digest, salt_digest);
		C.done = C.exit_code = 1;
		return;
	}
	C.challenged = 1;
	const char *password = C.opts->password;
	if (!password)
		password = read_password_tty(prompt);
	if (!password) {
		C.done = C.exit_code = 1;
		return;
	}
	uint8_t pw[1024], client_salt[32], mac[32], padding[512];
	char salt_hex[65], response[65];
	size_t pwlen = password_bytes(password, pw, sizeof(pw));
	random_bytes(client_salt, sizeof(client_salt));
	hmac_sha256(client_salt, sizeof(client_salt), salt->u.s.p, salt->u.s.len, mac);
	tohex(mac, 32, salt_hex);
	hmac_sha256(pw, pwlen, (const uint8_t *)salt_hex, 64, mac);
	tohex(mac, 32, response);
	memset(pw, 0, sizeof(pw));
	size_t plen = 512 - 64;	/* max(32, 512 - len(response)) */
	random_bytes(padding, plen);
	send_hello((const uint8_t *)response, 64, client_salt, sizeof(client_salt), padding, plen);
}

static void handle_hello(rv *pk)
{
	rv *caps = rv_list_get(pk, 1);
	if (!caps || caps->type != RV_DICT) {
		fprintf(stderr, "invalid server hello\n");
		C.done = C.exit_code = 1;
		return;
	}
	rv *pv = rv_lookup(caps, "protocol-version");
	int64_t major = 7, minor = 0;
	if (pv) {
		major = rv_int(rv_list_get(pv, 0), -1);
		minor = rv_int(rv_list_get(pv, 1), 0);
		if (major < 0) {
			fprintf(stderr, "invalid server protocol-version\n");
			C.done = C.exit_code = 1;
			return;
		}
	}
	if (major > PROTOCOL_MAJOR || (major == PROTOCOL_MAJOR && minor > PROTOCOL_MINOR)) {
		fprintf(stderr, "server requires protocol version %lld.%lld\n", (long long)major, (long long)minor);
		C.done = C.exit_code = 1;
		return;
	}
	if (!rv_list_has_str(rv_lookup(caps, "encoders"), "rencodeplus")) {
		fprintf(stderr, "server does not support rencodeplus\n");
		C.done = C.exit_code = 1;
		return;
	}
	C.readonly = rv_bool(rv_lookup(caps, "readonly"), 0);
	C.wheel = rv_bool(rv_lookup(caps, "wheel.precise"), 0) || rv_bool(rv_lookup(caps, "wheel.emulation"), 0);
	C.connected = 1;
	rv *clip = rv_dict_get(caps, "clipboard");
	if (be_clipboard_available() && clip && clip->type == RV_DICT) {
		char dir[16] = "both";
		rv_strcpy(rv_dict_get(clip, "direction"), dir, sizeof(dir));
		C.clip_to_server = !strcmp(dir, "both") || !strcmp(dir, "to-server");
		C.clip_to_client = !strcmp(dir, "both") || !strcmp(dir, "to-client");
		C.clip_sels = (1u << SEL_COUNT) - 1;
		C.clip_enabled = 1;
		be_clipboard_announce();
	}
	if (C.opts->password && !C.challenged)
		fprintf(stderr, "warning: the server did not verify the password\n");
	char mode[64] = "", name[256] = "";
	rv_strcpy(rv_lookup(caps, "mode"), mode, sizeof(mode));
	rv_strcpy(rv_lookup(caps, "session_name"), name, sizeof(name));
	fprintf(stderr, "connected to %s session%s%s%s\n", mode[0] ? mode : "xpra",
		name[0] ? " '" : "", name, name[0] ? "'" : "");
	if (C.readonly)
		fprintf(stderr, "readonly connection: input disabled\n");
}

static void handle_connection_close(rv *pk)
{
	char reason[256] = "", detail[256] = "";
	rv_strcpy(rv_list_get(pk, 1), reason, sizeof(reason));
	rv_strcpy(rv_list_get(pk, 2), detail, sizeof(detail));
	fprintf(stderr, "server closed the connection: %s%s%s\n", reason, detail[0] ? ": " : "", detail);
	C.done = 1;
	/* Protocol.md 5.3: only the "normal" reasons are not failures */
	static const char *const normal[] = {
		"done", "server exit", "server upgrade", "server shutdown", "client request",
		"detach request", "new client", "session busy", "idle timeout", NULL,
	};
	C.exit_code = 1;
	for (int i = 0; normal[i]; i++)
		if (!strcmp(reason, normal[i]))
			C.exit_code = 0;
}

static void handle_ping(rv *pk)
{
	rv *sid = rv_list_get(pk, 3);
	buf_t b = { 0 };
	enc_list(&b, sid ? 7 : 6);
	enc_str(&b, "ping-echo");
	enc_int(&b, rv_int(rv_list_get(pk, 1), 0));
	enc_int(&b, 0);
	enc_int(&b, 0);
	enc_int(&b, 0);
	enc_int(&b, -1);
	if (sid)
		enc_strn(&b, (const char *)sid->u.s.p, rv_len(sid));
	send_packet(&b);
}

/* ---------------------------------------------------------------- windows */

static void apply_metadata(win *w, rv *meta)
{
	char title[1024];
	if (!rv_strcpy(rv_dict_get(meta, "title"), title, sizeof(title)))
		be_window_title(w->be, title);
	rv *tf = rv_dict_get(meta, "transient-for");
	if (tf) {
		win *parent = find_window((uint64_t)rv_int(tf, 0));
		if (parent)
			be_window_transient_for(w->be, parent->be);
	}
	rv *sc = rv_dict_get(meta, "size-constraints");
	if (sc && sc->type == RV_DICT) {
		rv *mn = rv_dict_get(sc, "minimum-size"), *mx = rv_dict_get(sc, "maximum-size");
		be_window_size_constraints(w->be,
					   (int)rv_int(rv_list_get(mn, 0), 0), (int)rv_int(rv_list_get(mn, 1), 0),
					   (int)rv_int(rv_list_get(mx, 0), 0), (int)rv_int(rv_list_get(mx, 1), 0));
	}
}

static int valid_size(int64_t w, int64_t h)
{
	return w > 0 && h > 0 && w <= 32767 && h <= 32767;
}

static void handle_window_create(rv *pk)
{
	uint64_t wid = (uint64_t)rv_int(rv_list_get(pk, 1), 0);
	int64_t x = rv_int(rv_list_get(pk, 2), 0), y = rv_int(rv_list_get(pk, 3), 0);
	int64_t w = rv_int(rv_list_get(pk, 4), 0), h = rv_int(rv_list_get(pk, 5), 0);
	rv *meta = rv_list_get(pk, 6);
	if (!wid || find_window(wid) || !valid_size(w, h) || x < -65535 || x > 65535 || y < -65535 || y > 65535) {
		fprintf(stderr, "invalid window-create for %#llx\n", (unsigned long long)wid);
		return;
	}
	win *nw = calloc(1, sizeof(win));
	nw->wid = wid;
	nw->x = (int)x;
	nw->y = (int)y;
	nw->w = (int)w;
	nw->h = (int)h;
	nw->override_redirect = rv_bool(rv_dict_get(meta, "override-redirect"), 0);
	nw->be = be_window_new(wid, nw->x, nw->y, nw->w, nw->h, nw->override_redirect);
	if (!nw->be) {
		free(nw);
		return;
	}
	nw->next = C.windows;
	C.windows = nw;
	apply_metadata(nw, meta);
	be_window_show(nw->be);
	if (!nw->override_redirect) {
		buf_t b = { 0 };
		enc_list(&b, 8);
		enc_str(&b, "window-map");
		enc_int(&b, (int64_t)wid);
		enc_int(&b, nw->x);
		enc_int(&b, nw->y);
		enc_int(&b, nw->w);
		enc_int(&b, nw->h);
		enc_dict(&b, 0);
		enc_dict(&b, 0);
		send_packet(&b);
	}
}

static void handle_window_metadata(rv *pk)
{
	win *w = find_window((uint64_t)rv_int(rv_list_get(pk, 1), 0));
	rv *meta = rv_list_get(pk, 2);
	if (w && meta && meta->type == RV_DICT)
		apply_metadata(w, meta);
}

static void handle_window_move_resize(rv *pk)
{
	win *w = find_window((uint64_t)rv_int(rv_list_get(pk, 1), 0));
	int resized = rv_is_str(rv_list_get(pk, 0), "window-resized");
	int i = resized ? 2 : 4;
	int64_t nw = rv_int(rv_list_get(pk, i), 0), nh = rv_int(rv_list_get(pk, i + 1), 0);
	if (!w || !valid_size(nw, nh))
		return;
	if (!resized) {
		w->x = (int)rv_int(rv_list_get(pk, 2), w->x);
		w->y = (int)rv_int(rv_list_get(pk, 3), w->y);
	}
	w->w = (int)nw;
	w->h = (int)nh;
	w->resize_counter = rv_int(rv_list_get(pk, i + 2), w->resize_counter);
	be_window_move_resize(w->be, w->x, w->y, w->w, w->h);
}

static void handle_window_raise(rv *pk)
{
	win *w = find_window((uint64_t)rv_int(rv_list_get(pk, 1), 0));
	if (w)
		be_window_raise(w->be);
}

static void handle_window_bell(rv *pk)
{
	/* Window.md: wid, device, percent (-100..100), pitch (Hz), duration (ms), class, id, name
	 * only the volume is honoured, an unknown or zero wid rings the bell without a window */
	win *w = find_window((uint64_t)rv_int(rv_list_get(pk, 1), 0));
	int64_t percent = rv_int(rv_list_get(pk, 3), 0);
	if (percent < -100)
		percent = -100;
	else if (percent > 100)
		percent = 100;
	be_bell(w ? w->be : NULL, (int)percent);
}

static void handle_window_icon(rv *pk)
{
	/* Window.md: wid, w, h, encoding, data - only png is advertised, the size is the png's own */
	win *w = find_window((uint64_t)rv_int(rv_list_get(pk, 1), 0));
	rv *data = rv_list_get(pk, 5);
	char encoding[16] = "";
	rv_strcpy(rv_list_get(pk, 4), encoding, sizeof(encoding));
	if (!w || strcmp(encoding, "png") || !data || (data->type != RV_BYTES && data->type != RV_STR))
		return;
	int iw, ih;
	uint8_t *bgra = pngdec_decode(data->u.s.p, data->u.s.len, ICON_MAX, ICON_MAX, &iw, &ih);
	if (!bgra) {
		fprintf(stderr, "invalid window-icon for window %#llx\n", (unsigned long long)w->wid);
		return;
	}
	/* convert in place to native endian 0xAARRGGBB, straight alpha */
	uint32_t *argb = (uint32_t *)(void *)bgra;
	for (size_t i = 0; i < (size_t)iw * ih; i++) {
		const uint8_t *p = bgra + i * 4;
		argb[i] = (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
	}
	be_window_icon(w->be, argb, iw, ih);
	free(bgra);
}

static void handle_events(rv *pk)
{
	/* Protocol.md 7.1: name, values... - lifecycle notifications, only logged */
	fputs("server event:", stdout);
	for (size_t i = 1; i < rv_len(pk); i++) {
		fputc(' ', stdout);
		rv_print(stdout, rv_list_get(pk, i));
	}
	fputc('\n', stdout);
	fflush(stdout);
}

static void handle_notification_show(rv *pk)
{
	/* Notification.md: dbus_id, id, app name, replaced id, app icon, summary, body, timeout, ...
	 * only printed, never shown: icons, actions and hints are ignored */
	printf("notification %lld from ", (long long)rv_int(rv_list_get(pk, 2), 0));
	rv_print(stdout, rv_list_get(pk, 3));
	fputs(": ", stdout);
	rv_print(stdout, rv_list_get(pk, 6));
	fputc(' ', stdout);
	rv_print(stdout, rv_list_get(pk, 7));
	fputc('\n', stdout);
	fflush(stdout);
}

static void handle_notification_close(rv *pk)
{
	printf("notification %lld closed\n", (long long)rv_int(rv_list_get(pk, 1), 0));
	fflush(stdout);
}

static void handle_window_destroy(rv *pk)
{
	uint64_t wid = (uint64_t)rv_int(rv_list_get(pk, 1), 0);
	for (win **pw = &C.windows; *pw; pw = &(*pw)->next) {
		if ((*pw)->wid == wid) {
			win *w = *pw;
			*pw = w->next;
			be_window_destroy(w->be);
			free(w);
			if (C.focused == wid)
				C.focused = 0;
			return;
		}
	}
}

static void handle_cursor_data(rv *pk)
{
	cursor_image ci;
	const char *err = "";
	int r = cursor_decode(pk, &ci, &err);
	if (r < 0)
		fprintf(stderr, "invalid cursor-data: %s\n", err);
	if (r > 0)
		be_cursor(ci.pixels, ci.w, ci.h, ci.xhot, ci.yhot);
	else
		be_cursor(NULL, 0, 0, 0, 0);
	free(ci.pixels);
}

static void handle_cursor_default(rv *pk)
{
	(void)pk;
	be_cursor(NULL, 0, 0, 0, 0);
}

/* -------------------------------------------------------------- clipboard */

/* is the selection synchronized in this direction? */
static int clip_ok(int sel, int direction)
{
	return C.clip_enabled && direction && sel >= 0 && (C.clip_sels & (1u << sel));
}

static void send_clip_none(uint64_t id, int sel)
{
	buf_t b = { 0 };
	enc_list(&b, 3);
	enc_str(&b, "clipboard-contents-none");
	enc_int(&b, (int64_t)id);
	enc_str(&b, clip_selection_name(sel));
	send_packet(&b);
}

/* the start of a clipboard-contents packet: the data and the truncated count must follow */
static void enc_clip_contents(buf_t *b, uint64_t id, int sel, const char *type, int format, const char *wire)
{
	enc_list(b, 8);
	enc_str(b, "clipboard-contents");
	enc_int(b, (int64_t)id);
	enc_str(b, clip_selection_name(sel));
	enc_str(b, type);
	enc_int(b, format);
	enc_str(b, wire);
}

static void handle_clipboard_data(rv *pk)
{
	/* selection, options: the server's application took ownership */
	int sel = clip_selection(rv_list_get(pk, 1));
	rv *opts = rv_list_get(pk, 2);
	if (!clip_ok(sel, C.clip_to_client))
		return;
	char origin[65] = "";
	rv_strcpy(rv_dict_get(opts, "origin"), origin, sizeof(origin));
	if (origin[0] && !strcmp(origin, C.clip_origin))
		return;	/* our own announcement, coming back */
	rv *targets = rv_dict_get(opts, "targets");
	int text = !targets || !rv_len(targets);
	for (size_t i = 0; !text && i < rv_len(targets); i++)
		text = clip_is_text_target(rv_list_get(targets, i));
	if (text && rv_bool(rv_dict_get(opts, "claim"), 1))
		be_clipboard_claim(sel);
}

static void handle_clipboard_request(rv *pk)
{
	/* request-id, selection, target: the server's application wants our text */
	rv *rid = rv_list_get(pk, 1), *target = rv_list_get(pk, 3);
	int sel = clip_selection(rv_list_get(pk, 2));
	if (!rid || rid->type != RV_INT || sel < 0)
		return;
	uint64_t id = (uint64_t)rid->u.i;
	if (!clip_ok(sel, C.clip_to_server) || C.readonly)
		send_clip_none(id, sel);
	else if (clip_is_text_target(target))
		be_clipboard_fetch(id, sel);
	else if (rv_is_str(target, "TARGETS")) {
		buf_t b = { 0 };
		enc_clip_contents(&b, id, sel, "ATOM", 32, "atoms");
		enc_strlist(&b, CLIP_TARGETS);
		enc_int(&b, 0);
		send_packet(&b);
	} else
		send_clip_none(id, sel);
}

static void handle_clipboard_contents(rv *pk)
{
	uint64_t id = (uint64_t)rv_int(rv_list_get(pk, 1), 0);
	const uint8_t *data = NULL;
	size_t len = 0;
	int latin1 = 0;
	const char *err = "";
	if (clip_contents(pk, &data, &len, &latin1, &err)) {
		fprintf(stderr, "invalid clipboard-contents: %s\n", err);
		be_clipboard_reply(id, NULL, 0);
		return;
	}
	char *utf8 = latin1 ? clip_latin1_to_utf8(data, len, &len) : NULL;
	be_clipboard_reply(id, latin1 ? utf8 : (const char *)data, len);
	free(utf8);
}

static void handle_clipboard_contents_none(rv *pk)
{
	be_clipboard_reply((uint64_t)rv_int(rv_list_get(pk, 1), 0), NULL, 0);
}

static void handle_clipboard_status(rv *pk)
{
	C.clip_enabled = rv_bool(rv_list_get(pk, 1), 0);
}

static void handle_clipboard_enable_selections(rv *pk)
{
	rv *sels = rv_list_get(pk, 1);
	C.clip_sels = 0;
	for (size_t i = 0; i < rv_len(sels); i++) {
		int sel = clip_selection(rv_list_get(sels, i));
		if (sel >= 0)
			C.clip_sels |= 1u << sel;
	}
}

/* copy a sw x sh picture to the width x height area at x, y of the window's backing store,
 * upscaling it (nearest neighbour) when it is smaller.
 * pixels have bpp bytes, with red, green and blue at offsets ri, gi and bi */
static void blit(win *w, int x, int y, int width, int height, const uint8_t *pixels, size_t rowstride,
		 size_t bpp, int ri, int gi, int bi, int sw, int sh)
{
	int bw, bh, stride;
	uint32_t *dst = be_window_pixels(w->be, &bw, &bh, &stride);
	int scaled = sw != width || sh != height;
	/* clip to the backing store: the window may have been resized since */
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
	int x1 = x + width > bw ? bw : x + width, y1 = y + height > bh ? bh : y + height;
	for (int py = y0; py < y1; py++) {
		size_t sy = scaled ? (size_t)(py - y) * (size_t)sh / (size_t)height : (size_t)(py - y);
		const uint8_t *srow = pixels + sy * rowstride;
		uint32_t *row = (uint32_t *)((uint8_t *)dst + (size_t)py * (size_t)stride);
		for (int px = x0; px < x1; px++) {
			size_t sx = scaled ? (size_t)(px - x) * (size_t)sw / (size_t)width : (size_t)(px - x);
			const uint8_t *src = srow + sx * bpp;
			row[px] = (uint32_t)src[ri] << 16 | (uint32_t)src[gi] << 8 | src[bi];
		}
	}
	if (x1 > x0 && y1 > y0)
		be_window_damage(w->be, x0, y0, x1 - x0, y1 - y0);
}

/* paint rgb24 / rgb32 pixels into the window's backing store
 * returns 0 on success, -1 on decoding error */
static int paint_rgb(win *w, int x, int y, int width, int height, rv *data, int64_t rowstride,
		     rv *opts, const char *encoding)
{
	char fmt[16];
	if (rv_strcpy(rv_dict_get(opts, "rgb_format"), fmt, sizeof(fmt)))
		strcpy(fmt, strcmp(encoding, "rgb24") ? "RGBX" : "RGB");
	size_t bpp = strlen(fmt);
	int ri = -1, gi = -1, bi = -1;
	for (size_t i = 0; i < bpp; i++) {
		switch (fmt[i]) {
		case 'R': ri = (int)i; break;
		case 'G': gi = (int)i; break;
		case 'B': bi = (int)i; break;
		case 'X': case 'A': break;
		default: return -1;
		}
	}
	if ((bpp != 3 && bpp != 4) || ri < 0 || gi < 0 || bi < 0)
		return -1;
	if (!data || (data->type != RV_BYTES && data->type != RV_STR))
		return -1;
	if (rowstride < (int64_t)width * (int64_t)bpp || rowstride > 32768 * 8)
		return -1;
	const uint8_t *pixels = data->u.s.p;
	size_t len = data->u.s.len;
	size_t need = (size_t)rowstride * (size_t)(height - 1) + (size_t)width * bpp;
	uint8_t *decompressed = NULL;
	if (rv_int(rv_dict_get(opts, "lz4"), 0)) {
		decompressed = lz4_decompress_sized(pixels, len, (size_t)rowstride * (size_t)height, &len);
		if (!decompressed)
			return -1;
		pixels = decompressed;
	} else if (rv_dict_get(opts, "zstd")) {
		return -1;	/* not advertised */
	}
	if (len < need) {
		free(decompressed);
		return -1;
	}
	blit(w, x, y, width, height, pixels, (size_t)rowstride, bpp, ri, gi, bi, width, height);
	free(decompressed);
	return 0;
}

typedef uint8_t *(*picture_decoder)(const uint8_t *data, size_t len, int max_w, int max_h, int *w, int *h);

/* the decoder for a picture encoding (decoding to BGRA), NULL for rgb24 / rgb32 */
static picture_decoder find_decoder(const char *encoding)
{
	if (!strcmp(encoding, "webp"))
		return webp_decode;
	if (!strcmp(encoding, "jpeg"))
		return jpeg_decode;
	if (!strcmp(encoding, "png") || !strcmp(encoding, "png/P") || !strcmp(encoding, "png/L"))
		return pngdec_decode;
	return NULL;
}

/* paint a png, webp or jpeg picture, which may have been encoded at a smaller size (scaled_size)
 * returns 0 on success, -1 on decoding error */
static int paint_picture(win *w, int x, int y, int width, int height, rv *data, picture_decoder decode)
{
	int pw, ph;
	if (!data || (data->type != RV_BYTES && data->type != RV_STR))
		return -1;
	uint8_t *pixels = decode(data->u.s.p, data->u.s.len, width, height, &pw, &ph);
	if (!pixels)
		return -1;
	blit(w, x, y, width, height, pixels, (size_t)pw * 4, 4, 2, 1, 0, pw, ph);	/* BGRA */
	free(pixels);
	return 0;
}

static void handle_window_draw(rv *pk)
{
	int64_t start = now_us();
	uint64_t wid = (uint64_t)rv_int(rv_list_get(pk, 1), 0);
	int64_t x = rv_int(rv_list_get(pk, 2), 0), y = rv_int(rv_list_get(pk, 3), 0);
	int64_t width = rv_int(rv_list_get(pk, 4), 0), height = rv_int(rv_list_get(pk, 5), 0);
	int64_t seq = rv_int(rv_list_get(pk, 8), 0);
	int64_t rowstride = rv_int(rv_list_get(pk, 9), 0);
	rv *opts = rv_list_get(pk, 10);
	char encoding[32] = "";
	rv_strcpy(rv_list_get(pk, 6), encoding, sizeof(encoding));
	char message[64] = "";
	int64_t result;
	picture_decoder decode = find_decoder(encoding);
	win *w = find_window(wid);
	if (!w) {
		result = -2;
	} else if (!valid_size(width, height) || x < -32768 || x > 32767 || y < -32768 || y > 32767) {
		result = -1;
		strcpy(message, "invalid geometry");
	} else if (!decode && strcmp(encoding, "rgb24") && strcmp(encoding, "rgb32")) {
		result = -1;
		snprintf(message, sizeof(message), "unsupported encoding %s", encoding);
	} else if (decode ?
		   paint_picture(w, (int)x, (int)y, (int)width, (int)height, rv_list_get(pk, 7), decode) :
		   paint_rgb(w, (int)x, (int)y, (int)width, (int)height, rv_list_get(pk, 7), rowstride, opts, encoding)) {
		result = -1;
		strcpy(message, "decoding error");
	} else {
		if (!rv_int(rv_dict_get(opts, "flush"), 0))
			be_window_present(w->be);
		result = now_us() - start;
		if (result < 1)
			result = 1;
		if (result > INT32_MAX)
			result = INT32_MAX;
	}
	if (result < 0 && C.opts->debug)
		fprintf(stderr, "draw %lld for %#llx failed: %s\n", (long long)seq, (unsigned long long)wid, message);
	buf_t b = { 0 };
	enc_list(&b, 7);
	enc_str(&b, "window-ack");
	enc_int(&b, (int64_t)wid);
	enc_int(&b, width);
	enc_int(&b, height);
	enc_int(&b, seq);
	enc_int(&b, result);
	enc_str(&b, message);
	send_packet(&b);
}

/* --------------------------------------------------------------- dispatch */

typedef struct {
	const char *name;
	void (*handler)(rv *pk);
	int needs_hello;
} handler_t;

static const handler_t HANDLERS[] = {
	{ "hello", handle_hello, 0 },
	{ "challenge", handle_challenge, 0 },
	{ "connection-close", handle_connection_close, 0 },
	{ "ping", handle_ping, 1 },
	{ "window-create", handle_window_create, 1 },
	{ "window-metadata", handle_window_metadata, 1 },
	{ "window-move-resize", handle_window_move_resize, 1 },
	{ "window-resized", handle_window_move_resize, 1 },
	{ "window-raise", handle_window_raise, 1 },
	{ "window-destroy", handle_window_destroy, 1 },
	{ "window-bell", handle_window_bell, 1 },
	{ "window-icon", handle_window_icon, 1 },
	{ "window-draw", handle_window_draw, 1 },
	{ "cursor-data", handle_cursor_data, 1 },
	{ "cursor-default", handle_cursor_default, 1 },
	{ "events", handle_events, 1 },
	{ "notification-show", handle_notification_show, 1 },
	{ "notification-close", handle_notification_close, 1 },
	{ "clipboard-data", handle_clipboard_data, 1 },
	{ "clipboard-request", handle_clipboard_request, 1 },
	{ "clipboard-contents", handle_clipboard_contents, 1 },
	{ "clipboard-contents-none", handle_clipboard_contents_none, 1 },
	{ "clipboard-status", handle_clipboard_status, 1 },
	{ "clipboard-enable-selections", handle_clipboard_enable_selections, 1 },
};

static void dispatch(rv *pk)
{
	rv *type = rv_list_get(pk, 0);
	if (C.opts->debug)
		fprintf(stderr, "< %.*s\n", (int)type->u.s.len, (const char *)type->u.s.p);
	for (size_t i = 0; i < sizeof(HANDLERS) / sizeof(HANDLERS[0]); i++) {
		if (rv_is_str(type, HANDLERS[i].name)) {
			if (HANDLERS[i].needs_hello && !C.connected) {
				fprintf(stderr, "unexpected packet before hello\n");
				C.done = C.exit_code = 1;
				return;
			}
			HANDLERS[i].handler(pk);
			return;
		}
	}
	/* anything else is an optional feature we did not ask for: ignore it */
}

/* ------------------------------------------------------- backend callbacks */

static int input_allowed(uint64_t wid)
{
	return C.connected && !C.readonly && find_window(wid);
}

void client_on_configure(uint64_t wid, int x, int y, int w, int h)
{
	win *wn = find_window(wid);
	if (!wn || !C.connected || (wn->x == x && wn->y == y && wn->w == w && wn->h == h))
		return;
	wn->x = x;
	wn->y = y;
	wn->w = w;
	wn->h = h;
	if (wn->override_redirect)
		return;
	buf_t b = { 0 };
	enc_list(&b, 3);
	enc_str(&b, "window-configure");
	enc_int(&b, (int64_t)wid);
	enc_dict(&b, 2);
	enc_str(&b, "geometry");
	enc_list(&b, 4);
	enc_int(&b, x);
	enc_int(&b, y);
	enc_int(&b, w);
	enc_int(&b, h);
	enc_str(&b, "resize-counter");
	enc_int(&b, wn->resize_counter);
	send_packet(&b);
}

void client_on_close(uint64_t wid)
{
	if (!input_allowed(wid))
		return;
	buf_t b = { 0 };
	enc_list(&b, 2);
	enc_str(&b, "window-close");
	enc_int(&b, (int64_t)wid);
	send_packet(&b);
}

void client_on_focus(uint64_t wid, int focused, unsigned mods)
{
	if (!input_allowed(wid))
		return;
	if (focused) {
		if (C.focused == wid)
			return;
		C.focused = wid;
	} else {
		if (C.focused != wid)
			return;
		C.focused = 0;
	}
	buf_t b = { 0 };
	enc_list(&b, 3);
	enc_str(&b, "window-focus");
	enc_int(&b, (int64_t)C.focused);
	enc_mods(&b, mods);
	send_packet(&b);
}

static void enc_pointer_props(buf_t *b, win *w, int x, int y, unsigned mods, unsigned buttons)
{
	enc_list(b, 2);
	enc_int(b, w->x + x);
	enc_int(b, w->y + y);
	enc_dict(b, 2);
	enc_str(b, "modifiers");
	enc_mods(b, mods);
	enc_str(b, "buttons");
	enc_buttons(b, buttons);
}

void client_on_motion(uint64_t wid, int x, int y, unsigned mods, unsigned buttons)
{
	if (!input_allowed(wid))
		return;
	buf_t b = { 0 };
	enc_list(&b, 6);
	enc_str(&b, "pointer-motion");
	enc_int(&b, -1);
	enc_int(&b, 0);
	enc_int(&b, (int64_t)wid);
	enc_pointer_props(&b, find_window(wid), x, y, mods, buttons);
	send_packet(&b);
}

void client_on_button(uint64_t wid, int button, int pressed, int x, int y, unsigned mods, unsigned buttons)
{
	if (!input_allowed(wid) || button < 1 || button > 255)
		return;
	win *w = find_window(wid);
	buf_t b = { 0 };
	if (button >= 4 && button <= 7 && C.wheel) {
		/* Pointer.md "mouse wheel": one discrete click is a distance of 1000 on that button,
		 * the server emulates button presses itself when its device is not precise */
		if (!pressed)
			return;
		enc_list(&b, 8);
		enc_str(&b, "pointer-wheel");
		enc_int(&b, (int64_t)wid);
		enc_int(&b, button);
		enc_int(&b, 1000);
		enc_pair(&b, w->x + x, w->y + y);
		enc_mods(&b, mods);
		enc_buttons(&b, buttons);
		enc_dict(&b, 0);
		send_packet(&b);
		return;
	}
	/* regular buttons, and wheel emulation for servers without pointer-wheel support */
	enc_list(&b, 8);
	enc_str(&b, "pointer-button");
	enc_int(&b, -1);
	enc_int(&b, 0);
	enc_int(&b, (int64_t)wid);
	enc_int(&b, button);
	enc_bool(&b, pressed);
	enc_pointer_props(&b, w, x, y, mods, buttons);
	send_packet(&b);
}

void client_on_key(uint64_t wid, const char *keyname, int pressed, unsigned mods,
		   uint32_t keyval, const char *string, int keycode)
{
	if (!input_allowed(wid) || !keyname || !*keyname)
		return;
	buf_t b = { 0 };
	enc_list(&b, 5);
	enc_str(&b, "keyboard-event");
	enc_int(&b, (int64_t)wid);
	enc_str(&b, keyname);
	enc_bool(&b, pressed);
	enc_dict(&b, 4);
	enc_str(&b, "modifiers");
	enc_mods(&b, mods);
	enc_str(&b, "keyval");
	enc_int(&b, keyval);
	enc_str(&b, "string");
	enc_str(&b, string ? string : "");
	enc_str(&b, "keycode");
	enc_int(&b, keycode);
	send_packet(&b);
}

void client_on_clipboard_owner(int sel)
{
	if (!C.connected || !clip_ok(sel, C.clip_to_server) || C.readonly)
		return;
	uint8_t r[8];
	random_bytes(r, sizeof(r));
	tohex(r, sizeof(r), C.clip_origin);
	/* announce only: the server requests the contents when they are pasted */
	buf_t b = { 0 };
	enc_list(&b, 3);
	enc_str(&b, "clipboard-data");
	enc_str(&b, clip_selection_name(sel));
	enc_dict(&b, 2);
	enc_str(&b, "claim");
	enc_bool(&b, 1);
	enc_str(&b, "origin");
	enc_str(&b, C.clip_origin);
	send_packet(&b);
}

void client_on_clipboard_want(uint64_t id, int sel)
{
	if (!C.connected || !clip_ok(sel, C.clip_to_client)) {
		be_clipboard_reply(id, NULL, 0);
		return;
	}
	buf_t b = { 0 };
	enc_list(&b, 4);
	enc_str(&b, "clipboard-request");
	enc_int(&b, (int64_t)id);
	enc_str(&b, clip_selection_name(sel));
	enc_str(&b, "UTF8_STRING");
	send_packet(&b);
}

void client_on_clipboard_data(uint64_t id, int sel, const char *utf8, size_t len)
{
	if (!C.connected)
		return;
	if (!utf8) {
		send_clip_none(id, sel);
		return;
	}
	buf_t b = { 0 };
	enc_clip_contents(&b, id, sel, "UTF8_STRING", 8, "bytes");
	enc_bytes(&b, utf8, len);
	enc_int(&b, 0);
	send_packet(&b);
}

/* --------------------------------------------------------------- main loop */

static void on_signal(int sig)
{
	(void)sig;
	quit_signal = 1;
}

int client_run(const client_opts *opts)
{
	C.opts = opts;
	uint8_t id[16];
	random_bytes(id, sizeof(id));
	tohex(id, sizeof(id), C.uuid);

	if (be_init(NULL))
		return 1;
	if (net_connect(&C.conn, opts->target)) {
		be_close();
		return 1;
	}
	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	send_hello(NULL, 0, NULL, 0, NULL, 0);

	while (!C.done) {
		be_process_events();
		if (C.done)
			break;
		struct pollfd fds[2] = {
			{ .fd = C.conn.fd, .events = POLLIN },
			{ .fd = be_fd(), .events = POLLIN },
		};
		int r = poll(fds, fds[1].fd >= 0 ? 2 : 1, be_clipboard_timeout());
		if (quit_signal) {
			buf_t b = { 0 };
			enc_list(&b, 2);
			enc_str(&b, "connection-close");
			enc_str(&b, "client exit");
			send_packet(&b);
			break;
		}
		if (r < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			C.exit_code = 1;
			break;
		}
		if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			if (conn_fill(&C.conn)) {
				fprintf(stderr, "connection lost\n");
				C.exit_code = 1;
				break;
			}
			packet_t p;
			int got = 0;
			while (!C.done && (got = conn_next_packet(&C.conn, &p)) > 0) {
				dispatch(p.v);
				packet_free(&p);
			}
			if (got < 0) {
				buf_t b = { 0 };
				enc_list(&b, 2);
				enc_str(&b, "connection-close");
				enc_str(&b, "protocol error");
				send_packet(&b);
				C.exit_code = 1;
				break;
			}
		}
	}
	while (C.windows) {
		win *w = C.windows;
		C.windows = w->next;
		be_window_destroy(w->be);
		free(w);
	}
	conn_close(&C.conn);
	be_close();
	return C.exit_code;
}
