/* platform backend interface: implemented by x11.c (other backends: GDI, Cocoa, ...) */
#ifndef BACKEND_H
#define BACKEND_H

#include <stddef.h>
#include <stdint.h>

/* modifier bits, backend neutral (named after the X11 modifier masks, see Keyboard.md) */
#define MOD_SHIFT	(1 << 0)
#define MOD_LOCK	(1 << 1)
#define MOD_CONTROL	(1 << 2)
#define MOD_MOD1	(1 << 3)	/* mod1 .. mod5 are consecutive bits */
#define MOD_COUNT	8

typedef struct be_window be_window;

int be_init(const char *display_name);
void be_close(void);
int be_fd(void);			/* fd to poll for events, -1 if none */
void be_screen_size(int *w, int *h);
const char *be_layout(void);		/* keyboard layout name, ie: "us" */

be_window *be_window_new(uint64_t wid, int x, int y, int w, int h, int override_redirect);
void be_window_title(be_window *win, const char *utf8);
void be_window_transient_for(be_window *win, be_window *parent);
void be_window_size_constraints(be_window *win, int minw, int minh, int maxw, int maxh);
void be_window_show(be_window *win);
void be_window_move_resize(be_window *win, int x, int y, int w, int h);
void be_window_raise(be_window *win);
/* ring the bell for win (NULL: no window), percent is -100..100 relative to the base volume */
void be_bell(be_window *win, int percent);
void be_window_destroy(be_window *win);
/* the window's icon: w * h native endian 0xAARRGGBB pixels, straight alpha */
void be_window_icon(be_window *win, const uint32_t *argb, int w, int h);
/* the cursor for all windows: w * h native endian 0xAARRGGBB premultiplied pixels,
 * hotspot inside the image, NULL restores the default cursor */
void be_cursor(const uint32_t *argb, int w, int h, int xhot, int yhot);
/* the backing store: 32-bit native endian 0x00RRGGBB pixels, stride in bytes */
uint32_t *be_window_pixels(be_window *win, int *w, int *h, int *stride);
void be_window_damage(be_window *win, int x, int y, int w, int h);
void be_window_present(be_window *win);

/* text clipboard, for each selection: */
#define SEL_CLIPBOARD	0
#define SEL_PRIMARY	1
#define SEL_COUNT	2
int be_clipboard_available(void);
/* call client_on_clipboard_owner for each selection that has a local owner */
void be_clipboard_announce(void);
/* own the selection on behalf of the server: local requests come back as client_on_clipboard_want */
void be_clipboard_claim(int sel);
/* read the local text, answered with exactly one client_on_clipboard_data(id, ...) */
void be_clipboard_fetch(uint64_t id, int sel);
/* answer a client_on_clipboard_want request, utf8 NULL refuses it; unknown ids are ignored */
void be_clipboard_reply(uint64_t id, const char *utf8, size_t len);
/* milliseconds until a clipboard request times out, -1 if none is pending */
int be_clipboard_timeout(void);

/* process pending events and expired clipboard requests, calling the client_on_* callbacks */
void be_process_events(void);

/* callbacks implemented by the client, all positions are window relative
 * except for client_on_configure which uses absolute screen coordinates */
void client_on_configure(uint64_t wid, int x, int y, int w, int h);
void client_on_close(uint64_t wid);
void client_on_focus(uint64_t wid, int focused, unsigned mods);
void client_on_motion(uint64_t wid, int x, int y, unsigned mods, unsigned buttons);
void client_on_button(uint64_t wid, int button, int pressed, int x, int y, unsigned mods, unsigned buttons);
void client_on_key(uint64_t wid, const char *keyname, int pressed, unsigned mods,
		   uint32_t keyval, const char *string, int keycode);
/* a local application now owns the selection */
void client_on_clipboard_owner(int sel);
/* a local application wants the text of the selection we own: answer with be_clipboard_reply(id) */
void client_on_clipboard_want(uint64_t id, int sel);
/* the answer to be_clipboard_fetch: utf8 NULL means no text */
void client_on_clipboard_data(uint64_t id, int sel, const char *utf8, size_t len);

#endif
