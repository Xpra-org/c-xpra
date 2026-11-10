/* Xlib backend */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "backend.h"

struct be_window {
	Window xid;
	uint64_t wid;
	int override_redirect;
	int width, height;		/* backing store size */
	XImage *img;
	uint32_t *pixels;
	int dx0, dy0, dx1, dy1;		/* damaged area, empty when dx1 <= dx0 */
	be_window *next;
};

static Display *dpy;
static int screen;
static Visual *visual;
static int depth;
static GC gc;
static Atom wm_protocols, wm_delete_window, net_wm_name, utf8_string;
static be_window *windows;
static char layout[64] = "us";

static int on_x_error(Display *d, XErrorEvent *e)
{
	char msg[128];
	XGetErrorText(d, e->error_code, msg, sizeof(msg));
	fprintf(stderr, "X11 error: %s (request %d)\n", msg, e->request_code);
	return 0;
}

/* layout from the _XKB_RULES_NAMES root property: rules, model, layout, variant, options */
static void read_layout(void)
{
	Atom prop = XInternAtom(dpy, "_XKB_RULES_NAMES", True), type;
	int fmt;
	unsigned long n, after;
	unsigned char *data = NULL;
	if (prop == None || XGetWindowProperty(dpy, RootWindow(dpy, screen), prop, 0, 1024, False,
					       AnyPropertyType, &type, &fmt, &n, &after, &data) != Success || !data)
		return;
	const char *s = (const char *)data;
	unsigned long off = 0;
	for (int i = 0; i < 2 && off < n; i++)
		off += strlen(s + off) + 1;
	if (off < n && s[off]) {
		snprintf(layout, sizeof(layout), "%s", s + off);
		layout[strcspn(layout, ",")] = 0;
	}
	XFree(data);
}

int be_init(const char *display_name)
{
	dpy = XOpenDisplay(display_name);
	if (!dpy) {
		fprintf(stderr, "cannot open X11 display\n");
		return -1;
	}
	XSetErrorHandler(on_x_error);
	screen = DefaultScreen(dpy);
	XVisualInfo tmpl = { .screen = screen, .class = TrueColor, .red_mask = 0xff0000,
			     .green_mask = 0xff00, .blue_mask = 0xff };
	int n = 0;
	XVisualInfo *vi = XGetVisualInfo(dpy, VisualScreenMask | VisualClassMask | VisualRedMaskMask |
					 VisualGreenMaskMask | VisualBlueMaskMask, &tmpl, &n);
	for (int i = 0; i < n && !visual; i++) {
		if (vi[i].visual == DefaultVisual(dpy, screen)) {
			visual = vi[i].visual;
			depth = vi[i].depth;
		}
	}
	if (vi)
		XFree(vi);
	if (!visual || (depth != 24 && depth != 32)) {
		fprintf(stderr, "the default X11 visual must be 24-bit TrueColor\n");
		XCloseDisplay(dpy);
		dpy = NULL;
		return -1;
	}
	gc = DefaultGC(dpy, screen);
	wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
	wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
	utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
	/* no fake release events for auto-repeat */
	XkbSetDetectableAutoRepeat(dpy, True, NULL);
	read_layout();
	return 0;
}

void be_close(void)
{
	if (dpy)
		XCloseDisplay(dpy);
	dpy = NULL;
}

int be_fd(void)
{
	return dpy ? ConnectionNumber(dpy) : -1;
}

void be_screen_size(int *w, int *h)
{
	*w = DisplayWidth(dpy, screen);
	*h = DisplayHeight(dpy, screen);
}

const char *be_layout(void)
{
	return layout;
}

static be_window *find(Window xid)
{
	for (be_window *w = windows; w; w = w->next)
		if (w->xid == xid)
			return w;
	return NULL;
}

/* (re)allocate the backing store, preserving the existing contents */
static void resize_backing(be_window *w, int width, int height)
{
	if (w->img && w->width == width && w->height == height)
		return;
	uint32_t *pixels = calloc((size_t)width * (size_t)height, 4);
	if (!pixels) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	if (w->img) {
		int cw = width < w->width ? width : w->width, ch = height < w->height ? height : w->height;
		for (int y = 0; y < ch; y++)
			memcpy(pixels + (size_t)y * width, w->pixels + (size_t)y * w->width, (size_t)cw * 4);
		w->img->data = NULL;
		XDestroyImage(w->img);
		free(w->pixels);
	}
	w->pixels = pixels;
	w->width = width;
	w->height = height;
	w->img = XCreateImage(dpy, visual, (unsigned)depth, ZPixmap, 0, (char *)pixels,
			      (unsigned)width, (unsigned)height, 32, width * 4);
	/* the pixels are native endian 32-bit values: Xlib swaps if the server needs it */
	const uint16_t one = 1;
	w->img->byte_order = *(const uint8_t *)&one ? LSBFirst : MSBFirst;
}

be_window *be_window_new(uint64_t wid, int x, int y, int w, int h, int override_redirect)
{
	be_window *bw = calloc(1, sizeof(*bw));
	if (!bw)
		return NULL;
	XSetWindowAttributes attrs = {
		.background_pixmap = None,
		.bit_gravity = NorthWestGravity,
		.override_redirect = override_redirect ? True : False,
		.colormap = DefaultColormap(dpy, screen),
		.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask |
			      ButtonPressMask | ButtonReleaseMask | PointerMotionMask | FocusChangeMask,
	};
	bw->xid = XCreateWindow(dpy, RootWindow(dpy, screen), x, y, (unsigned)w, (unsigned)h, 0, depth,
				InputOutput, visual,
				CWBackPixmap | CWBitGravity | CWOverrideRedirect | CWColormap | CWEventMask, &attrs);
	bw->wid = wid;
	bw->override_redirect = override_redirect;
	resize_backing(bw, w, h);
	XSetWMProtocols(dpy, bw->xid, &wm_delete_window, 1);
	XWMHints hints = { .flags = InputHint, .input = True };
	XSetWMHints(dpy, bw->xid, &hints);
	XSizeHints size = { .flags = USPosition | PPosition, .x = x, .y = y };
	XSetWMNormalHints(dpy, bw->xid, &size);
	bw->next = windows;
	windows = bw;
	return bw;
}

void be_window_title(be_window *w, const char *utf8)
{
	XChangeProperty(dpy, w->xid, net_wm_name, utf8_string, 8, PropModeReplace,
			(const unsigned char *)utf8, (int)strlen(utf8));
	XStoreName(dpy, w->xid, utf8);
}

void be_window_transient_for(be_window *w, be_window *parent)
{
	XSetTransientForHint(dpy, w->xid, parent->xid);
}

void be_window_size_constraints(be_window *w, int minw, int minh, int maxw, int maxh)
{
	XSizeHints size = { 0 };
	long supplied;
	XGetWMNormalHints(dpy, w->xid, &size, &supplied);
	size.flags &= ~(PMinSize | PMaxSize);
	if (minw > 0 && minh > 0) {
		size.flags |= PMinSize;
		size.min_width = minw;
		size.min_height = minh;
	}
	if (maxw > 0 && maxh > 0 && maxw < 32768 && maxh < 32768) {
		size.flags |= PMaxSize;
		size.max_width = maxw;
		size.max_height = maxh;
	}
	XSetWMNormalHints(dpy, w->xid, &size);
}

void be_window_show(be_window *w)
{
	XMapRaised(dpy, w->xid);
}

void be_window_move_resize(be_window *w, int x, int y, int width, int height)
{
	resize_backing(w, width, height);
	XMoveResizeWindow(dpy, w->xid, x, y, (unsigned)width, (unsigned)height);
}

void be_window_raise(be_window *w)
{
	XRaiseWindow(dpy, w->xid);
}

void be_bell(be_window *w, int percent)
{
	/* XkbBell associates the bell with the window (for visual bells, urgency hints),
	 * it fails without the XKB extension */
	if (!XkbBell(dpy, w ? w->xid : None, percent, None))
		XBell(dpy, percent);
	XFlush(dpy);
}

void be_window_destroy(be_window *w)
{
	for (be_window **p = &windows; *p; p = &(*p)->next) {
		if (*p == w) {
			*p = w->next;
			break;
		}
	}
	XDestroyWindow(dpy, w->xid);
	if (w->img) {
		w->img->data = NULL;
		XDestroyImage(w->img);
	}
	free(w->pixels);
	free(w);
}

uint32_t *be_window_pixels(be_window *w, int *width, int *height, int *stride)
{
	*width = w->width;
	*height = w->height;
	*stride = w->width * 4;
	return w->pixels;
}

void be_window_damage(be_window *w, int x, int y, int width, int height)
{
	if (w->dx1 <= w->dx0) {
		w->dx0 = x;
		w->dy0 = y;
		w->dx1 = x + width;
		w->dy1 = y + height;
		return;
	}
	if (x < w->dx0) w->dx0 = x;
	if (y < w->dy0) w->dy0 = y;
	if (x + width > w->dx1) w->dx1 = x + width;
	if (y + height > w->dy1) w->dy1 = y + height;
}

static void put(be_window *w, int x, int y, int width, int height)
{
	if (x < 0) { width += x; x = 0; }
	if (y < 0) { height += y; y = 0; }
	if (x + width > w->width) width = w->width - x;
	if (y + height > w->height) height = w->height - y;
	if (width > 0 && height > 0)
		XPutImage(dpy, w->xid, gc, w->img, x, y, x, y, (unsigned)width, (unsigned)height);
}

void be_window_present(be_window *w)
{
	if (w->dx1 > w->dx0)
		put(w, w->dx0, w->dy0, w->dx1 - w->dx0, w->dy1 - w->dy0);
	w->dx0 = w->dx1 = 0;
}

static unsigned buttons_from_state(unsigned state)
{
	return (state >> 8) & 0x1f;	/* Button1Mask .. Button5Mask */
}

static void handle_event(XEvent *ev)
{
	be_window *w = find(ev->xany.window);
	if (!w)
		return;
	switch (ev->type) {
	case Expose:
		put(w, ev->xexpose.x, ev->xexpose.y, ev->xexpose.width, ev->xexpose.height);
		break;
	case ConfigureNotify: {
		int x = ev->xconfigure.x, y = ev->xconfigure.y;
		Window child;
		/* the event coordinates are relative to the window manager's frame, unless synthetic */
		if (!ev->xconfigure.send_event)
			XTranslateCoordinates(dpy, w->xid, RootWindow(dpy, screen), 0, 0, &x, &y, &child);
		resize_backing(w, ev->xconfigure.width, ev->xconfigure.height);
		client_on_configure(w->wid, x, y, ev->xconfigure.width, ev->xconfigure.height);
		break;
	}
	case ClientMessage:
		if (ev->xclient.message_type == wm_protocols && (Atom)ev->xclient.data.l[0] == wm_delete_window)
			client_on_close(w->wid);
		break;
	case FocusIn:
	case FocusOut:
		if (ev->xfocus.detail == NotifyPointer || ev->xfocus.mode == NotifyGrab ||
		    ev->xfocus.mode == NotifyUngrab)
			break;
		client_on_focus(w->wid, ev->type == FocusIn, 0);
		break;
	case MotionNotify:
		/* only send the latest position */
		while (XCheckTypedWindowEvent(dpy, w->xid, MotionNotify, ev))
			;
		client_on_motion(w->wid, ev->xmotion.x, ev->xmotion.y, ev->xmotion.state & 0xff,
				 buttons_from_state(ev->xmotion.state));
		break;
	case ButtonPress:
	case ButtonRelease:
		client_on_button(w->wid, (int)ev->xbutton.button, ev->type == ButtonPress,
				 ev->xbutton.x, ev->xbutton.y, ev->xbutton.state & 0xff,
				 buttons_from_state(ev->xbutton.state));
		break;
	case KeyPress:
	case KeyRelease: {
		char text[32] = "";
		KeySym keysym = NoSymbol;
		int n = XLookupString(&ev->xkey, text, sizeof(text) - 1, &keysym, NULL);
		text[n > 0 ? n : 0] = 0;
		for (int i = 0; i < n; i++)
			if ((unsigned char)text[i] >= 0x80)
				text[0] = 0;	/* Latin-1, not UTF-8: leave it to the key name */
		const char *name = keysym != NoSymbol ? XKeysymToString(keysym) : NULL;
		if (!name)
			break;
		client_on_key(w->wid, name, ev->type == KeyPress, ev->xkey.state & 0xff,
			      (uint32_t)keysym, text, (int)ev->xkey.keycode);
		break;
	}
	}
}

void be_process_events(void)
{
	while (XPending(dpy)) {
		XEvent ev;
		XNextEvent(dpy, &ev);
		handle_event(&ev);
	}
	XFlush(dpy);
}
