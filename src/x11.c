/* Xlib backend */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>

#include "backend.h"
#include "clipboard.h"

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
static Atom wm_protocols, wm_delete_window, net_wm_name, net_wm_icon, utf8_string;
static be_window *windows;
static char layout[64] = "us";
static Cursor cursor = None;	/* None: the default cursor */

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

/* -------------------------------------------------------------- clipboard
 * ICCCM selections, text only and without INCR transfers: our hidden window owns the
 * selections on behalf of the server, XFixes tells us when a local application takes them */

#define CLIP_SLOTS 8
#define CLIP_TIMEOUT_MS 2500	/* Clipboard.md: how long a peer waits for an answer */

/* a conversion we asked a local owner for, or a local request waiting for the server */
typedef struct {
	int used;
	uint64_t id;
	int sel;
	int64_t deadline;
	Atom target, property;
	Window requestor;	/* parked requests only */
	Time time;
} clip_slot;

static int xfixes_event = -1;	/* -1: no clipboard */
static Window clip_win;
static Atom sel_atoms[SEL_COUNT], targets_atom, text_atom, slot_props[CLIP_SLOTS];
static Atom text_atoms[5];	/* the CLIP_TARGETS, in order */
static int clip_owned[SEL_COUNT];
static size_t clip_max;		/* also limited by the X11 request size */
static clip_slot fetches[CLIP_SLOTS], parked[CLIP_SLOTS];
static uint64_t parked_id;

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void clip_init(void)
{
	int ev, err, major = 2, minor = 0;
	if (!XFixesQueryExtension(dpy, &ev, &err) || !XFixesQueryVersion(dpy, &major, &minor) || major < 1)
		return;
	static const char *names[] = { "CLIPBOARD", "PRIMARY", "TARGETS" };
	Atom a[3];
	XInternAtoms(dpy, (char **)names, 3, False, a);
	sel_atoms[SEL_CLIPBOARD] = a[0];
	sel_atoms[SEL_PRIMARY] = a[1];
	targets_atom = a[2];
	XInternAtoms(dpy, (char **)CLIP_TARGETS, 5, False, text_atoms);
	text_atom = text_atoms[1];
	char pnames[CLIP_SLOTS][16], *pp[CLIP_SLOTS];
	for (int i = 0; i < CLIP_SLOTS; i++) {
		snprintf(pnames[i], sizeof(pnames[i]), "C_XPRA_CLIP%d", i);
		pp[i] = pnames[i];
	}
	XInternAtoms(dpy, pp, CLIP_SLOTS, False, slot_props);
	long req = XExtendedMaxRequestSize(dpy);
	if (!req)
		req = XMaxRequestSize(dpy);
	clip_max = (size_t)req * 4 - 64;	/* minus the ChangeProperty header */
	if (clip_max > CLIP_MAX)
		clip_max = CLIP_MAX;
	clip_win = XCreateWindow(dpy, RootWindow(dpy, screen), -1, -1, 1, 1, 0, 0, InputOnly,
				 CopyFromParent, 0, NULL);
	for (int i = 0; i < SEL_COUNT; i++)
		XFixesSelectSelectionInput(dpy, clip_win, sel_atoms[i], XFixesSetSelectionOwnerNotifyMask);
	xfixes_event = ev + XFixesSelectionNotify;
}

int be_clipboard_available(void)
{
	return xfixes_event >= 0;
}

static int sel_index(Atom a)
{
	for (int i = 0; i < SEL_COUNT; i++)
		if (sel_atoms[i] == a)
			return i;
	return -1;
}

static int is_text_atom(Atom a)
{
	for (int i = 0; i < 5; i++)
		if (text_atoms[i] == a)
			return 1;
	return 0;
}

static clip_slot *free_slot(clip_slot *slots)
{
	for (int i = 0; i < CLIP_SLOTS; i++)
		if (!slots[i].used)
			return &slots[i];
	return NULL;
}

void be_clipboard_announce(void)
{
	for (int i = 0; xfixes_event >= 0 && i < SEL_COUNT; i++) {
		Window owner = XGetSelectionOwner(dpy, sel_atoms[i]);
		if (owner != None && owner != clip_win)
			client_on_clipboard_owner(i);
	}
}

void be_clipboard_claim(int sel)
{
	if (xfixes_event < 0)
		return;
	XSetSelectionOwner(dpy, sel_atoms[sel], clip_win, CurrentTime);
	clip_owned[sel] = XGetSelectionOwner(dpy, sel_atoms[sel]) == clip_win;
}

static void convert(clip_slot *s, Atom target)
{
	s->target = target;
	XConvertSelection(dpy, sel_atoms[s->sel], target, s->property, clip_win, CurrentTime);
}

void be_clipboard_fetch(uint64_t id, int sel)
{
	clip_slot *s = xfixes_event >= 0 ? free_slot(fetches) : NULL;
	if (!s) {
		client_on_clipboard_data(id, sel, NULL, 0);
		return;
	}
	*s = (clip_slot){ .used = 1, .id = id, .sel = sel, .deadline = now_ms() + CLIP_TIMEOUT_MS,
			  .property = slot_props[s - fetches] };
	convert(s, utf8_string);
}

/* the answer to one of our XConvertSelection requests */
static void clip_converted(XSelectionEvent *e)
{
	clip_slot *s = NULL;
	for (int i = 0; i < CLIP_SLOTS && !s; i++) {
		clip_slot *f = &fetches[i];
		if (f->used && sel_atoms[f->sel] == e->selection && f->target == e->target &&
		    (e->property == None || e->property == f->property))
			s = f;
	}
	if (!s)
		return;
	if (e->property == None && s->target == utf8_string) {
		convert(s, XA_STRING);	/* an older application: try Latin-1 */
		return;
	}
	char *text = NULL;
	size_t len = 0;
	if (e->property != None) {
		Atom type;
		int fmt;
		unsigned long n, after;
		unsigned char *data = NULL;
		if (XGetWindowProperty(dpy, clip_win, s->property, 0, (long)(clip_max + 3) / 4, True,
				       AnyPropertyType, &type, &fmt, &n, &after, &data) == Success &&
		    data && fmt == 8 && !after && (type == utf8_string || type == XA_STRING)) {
			if (type == XA_STRING)
				text = clip_latin1_to_utf8(data, n, &len);
			else if ((text = malloc(n + 1))) {
				memcpy(text, data, n);
				len = n;
			}
		}
		if (data)
			XFree(data);
		/* INCR and oversized transfers are not supported: discard what is left */
		XDeleteProperty(dpy, clip_win, s->property);
	}
	s->used = 0;
	client_on_clipboard_data(s->id, s->sel, text, len);
	free(text);
}

static void selection_notify(Window requestor, Atom selection, Atom target, Atom property, Time time)
{
	XEvent ev = { .xselection = { .type = SelectionNotify, .display = dpy, .requestor = requestor,
				      .selection = selection, .target = target, .property = property,
				      .time = time } };
	XSendEvent(dpy, requestor, False, NoEventMask, &ev);
}

/* a local application wants the contents of a selection we own */
static void clip_request(XSelectionRequestEvent *e)
{
	int sel = sel_index(e->selection);
	/* obsolete clients do not name a property: use the target */
	Atom property = e->property != None ? e->property : e->target;
	clip_slot *s = NULL;
	if (sel >= 0 && clip_owned[sel] && e->target == targets_atom) {
		Atom targets[6] = { targets_atom };
		memcpy(targets + 1, text_atoms, sizeof(text_atoms));
		XChangeProperty(dpy, e->requestor, property, XA_ATOM, 32, PropModeReplace,
				(unsigned char *)targets, 6);
	} else if (sel >= 0 && clip_owned[sel] && is_text_atom(e->target) && (s = free_slot(parked))) {
		*s = (clip_slot){ .used = 1, .id = ++parked_id, .sel = sel, .target = e->target,
				  .property = property, .requestor = e->requestor, .time = e->time,
				  .deadline = now_ms() + CLIP_TIMEOUT_MS };
		client_on_clipboard_want(s->id, sel);
		return;
	} else {
		property = None;	/* refused */
	}
	selection_notify(e->requestor, e->selection, e->target, property, e->time);
}

void be_clipboard_reply(uint64_t id, const char *utf8, size_t len)
{
	clip_slot *p = NULL;
	for (int i = 0; i < CLIP_SLOTS && !p; i++)
		if (parked[i].used && parked[i].id == id)
			p = &parked[i];
	if (!p)
		return;
	p->used = 0;
	char *latin1 = NULL;
	if (utf8 && p->target == XA_STRING)
		utf8 = latin1 = clip_utf8_to_latin1((const uint8_t *)utf8, len, &len);
	if (!utf8 || len > clip_max) {
		selection_notify(p->requestor, sel_atoms[p->sel], p->target, None, p->time);
	} else {
		/* TEXT lets the owner pick the type */
		Atom type = p->target == text_atom ? utf8_string : p->target;
		XChangeProperty(dpy, p->requestor, p->property, type, 8, PropModeReplace,
				(const unsigned char *)utf8, (int)len);
		selection_notify(p->requestor, sel_atoms[p->sel], p->target, p->property, p->time);
	}
	free(latin1);
	XFlush(dpy);
}

int be_clipboard_timeout(void)
{
	int64_t now = now_ms(), next = -1;
	for (int i = 0; i < CLIP_SLOTS; i++) {
		const clip_slot *s[2] = { &fetches[i], &parked[i] };
		for (int k = 0; k < 2; k++)
			if (s[k]->used && (next < 0 || s[k]->deadline < next))
				next = s[k]->deadline;
	}
	return next < 0 ? -1 : next <= now ? 0 : (int)(next - now);
}

static void clip_expire(void)
{
	int64_t now = now_ms();
	for (int i = 0; i < CLIP_SLOTS; i++) {
		if (fetches[i].used && fetches[i].deadline <= now) {
			fetches[i].used = 0;
			client_on_clipboard_data(fetches[i].id, fetches[i].sel, NULL, 0);
		}
		if (parked[i].used && parked[i].deadline <= now)
			be_clipboard_reply(parked[i].id, NULL, 0);
	}
}

/* returns 1 if the event was for the clipboard */
static int clip_event(XEvent *ev)
{
	if (xfixes_event < 0)
		return 0;
	if (ev->type == xfixes_event) {
		XFixesSelectionNotifyEvent *e = (XFixesSelectionNotifyEvent *)ev;
		int sel = sel_index(e->selection);
		if (sel >= 0 && e->owner != clip_win) {
			clip_owned[sel] = 0;
			if (e->owner != None)
				client_on_clipboard_owner(sel);
		}
		return 1;
	}
	if (ev->xany.window != clip_win)
		return 0;
	switch (ev->type) {
	case SelectionNotify:
		clip_converted(&ev->xselection);
		break;
	case SelectionRequest:
		clip_request(&ev->xselectionrequest);
		break;
	case SelectionClear: {
		int sel = sel_index(ev->xselectionclear.selection);
		if (sel >= 0)
			clip_owned[sel] = 0;
		break;
	}
	}
	return 1;
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
	net_wm_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
	utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
	/* no fake release events for auto-repeat */
	XkbSetDetectableAutoRepeat(dpy, True, NULL);
	read_layout();
	clip_init();
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
	if (cursor != None)
		XDefineCursor(dpy, bw->xid, cursor);
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

void be_window_icon(be_window *w, const uint32_t *argb, int width, int height)
{
	/* _NET_WM_ICON: width, height, then the ARGB pixels, as format 32 data (an array of longs) */
	size_t n = (size_t)width * height;
	unsigned long *data = malloc((n + 2) * sizeof(unsigned long));
	if (!data)
		return;
	data[0] = (unsigned long)width;
	data[1] = (unsigned long)height;
	for (size_t i = 0; i < n; i++)
		data[i + 2] = argb[i];
	XChangeProperty(dpy, w->xid, net_wm_icon, XA_CARDINAL, 32, PropModeReplace,
			(const unsigned char *)data, (int)(n + 2));
	free(data);
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

/* libXcursor's XcursorImage (a stable ABI), loaded at runtime so that it stays optional */
typedef struct {
	uint32_t version, size, width, height, xhot, yhot, delay;
	uint32_t *pixels;	/* premultiplied ARGB */
} xcursor_image;
typedef Cursor (*xcursor_load_fn)(Display *, const xcursor_image *);

static Cursor argb_cursor(const uint32_t *argb, int w, int h, int xhot, int yhot)
{
	static int loaded;
	static xcursor_load_fn load;
	if (!loaded) {
		loaded = 1;
		void *lib = dlopen("libXcursor.so.1", RTLD_LAZY);
		if (lib)
			*(void **)&load = dlsym(lib, "XcursorImageLoadCursor");
	}
	if (!load)
		return None;
	xcursor_image img = {
		.version = 1, .size = (uint32_t)(w > h ? w : h), .width = (uint32_t)w, .height = (uint32_t)h,
		.xhot = (uint32_t)xhot, .yhot = (uint32_t)yhot, .pixels = (uint32_t *)argb,
	};
	return load(dpy, &img);
}

/* without libXcursor: a black and white cursor, opaque where alpha >= 50% */
static Cursor mono_cursor(const uint32_t *argb, int w, int h, int xhot, int yhot)
{
	size_t bpl = ((size_t)w + 7) / 8;	/* XBitmapFormat: LSB first, rows padded to bytes */
	char *src = calloc(bpl * (size_t)h * 2, 1);
	if (!src)
		return None;
	char *mask = src + bpl * (size_t)h;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint32_t p = argb[(size_t)y * (size_t)w + (size_t)x], a = p >> 24;
			if (a < 128)
				continue;
			/* premultiplied luminance: dark pixels use the foreground (black) */
			uint32_t lum = (((p >> 16) & 0xff) * 3 + ((p >> 8) & 0xff) * 6 + (p & 0xff)) / 10;
			size_t i = (size_t)y * bpl + (size_t)x / 8;
			mask[i] |= (char)(1 << (x & 7));
			if (lum * 2 < a)
				src[i] |= (char)(1 << (x & 7));
		}
	}
	Window root = RootWindow(dpy, screen);
	Pixmap ps = XCreateBitmapFromData(dpy, root, src, (unsigned)w, (unsigned)h);
	Pixmap pm = XCreateBitmapFromData(dpy, root, mask, (unsigned)w, (unsigned)h);
	free(src);
	XColor black = { .red = 0, .green = 0, .blue = 0 }, white = { .red = 0xffff, .green = 0xffff, .blue = 0xffff };
	Cursor c = XCreatePixmapCursor(dpy, ps, pm, &black, &white, (unsigned)xhot, (unsigned)yhot);
	XFreePixmap(dpy, ps);
	XFreePixmap(dpy, pm);
	return c;
}

void be_cursor(const uint32_t *argb, int w, int h, int xhot, int yhot)
{
	Cursor old = cursor;
	cursor = None;
	if (argb) {
		cursor = argb_cursor(argb, w, h, xhot, yhot);
		if (cursor == None)
			cursor = mono_cursor(argb, w, h, xhot, yhot);
	}
	for (be_window *bw = windows; bw; bw = bw->next) {
		if (cursor != None)
			XDefineCursor(dpy, bw->xid, cursor);
		else
			XUndefineCursor(dpy, bw->xid);
	}
	/* the windows no longer reference it */
	if (old != None)
		XFreeCursor(dpy, old);
	XFlush(dpy);
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
	if (clip_event(ev))
		return;
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
	clip_expire();
	XFlush(dpy);
}
