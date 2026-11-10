/* platform backend interface: implemented by x11.c (other backends: GDI, Cocoa, ...) */
#ifndef BACKEND_H
#define BACKEND_H

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
/* the backing store: 32-bit native endian 0x00RRGGBB pixels, stride in bytes */
uint32_t *be_window_pixels(be_window *win, int *w, int *h, int *stride);
void be_window_damage(be_window *win, int x, int y, int w, int h);
void be_window_present(be_window *win);

/* process pending events, calling the client_on_* callbacks */
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

#endif
