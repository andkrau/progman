/*
 * Copyright 2020 joshua stein <jcs@jcs.org>
 * Copyright 1998-2007 Decklin Foster <decklin@red-bean.com>.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifdef __linux__
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <locale.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/extensions/shape.h>
#ifdef USE_GDK_PIXBUF
#include <gdk-pixbuf-xlib/gdk-pixbuf-xlib.h>
#endif
#include "progman.h"
#include "atom.h"
#include "parser.h"

#ifdef HIDPI
#include "icons/close_hidpi.xpm"
#include "icons/utility_close_hidpi.xpm"
#include "icons/iconify_hidpi.xpm"
#include "icons/zoom_hidpi.xpm"
#include "icons/unzoom_hidpi.xpm"
#include "icons/default_icon_hidpi.xpm"
#else
#include "icons/close.xpm"
#include "icons/utility_close.xpm"
#include "icons/iconify.xpm"
#include "icons/zoom.xpm"
#include "icons/unzoom.xpm"
#include "icons/default_icon.xpm"
#endif

#ifndef WAIT_ANY
#define WAIT_ANY (-1)
#endif

client_t *focused, *dragging;
int screen;
unsigned long ndesks = DEF_NDESKS;
unsigned long cur_desk = 0;
unsigned int focus_order = 0;
Bool shape_support;
int shape_event;
Window supporting_wm_win;

XftFont *font;
XftFont *iconfont;
XftColor xft_fg;
XftColor xft_fg_unfocused;
XftColor xft_launcher;
XftColor xft_launcher_highlighted;

Colormap def_cmap;
XColor fg;
XColor bg;
XColor unfocused_fg;
XColor unfocused_bg;
XColor button_bg;
XColor bevel_dark;
XColor bevel_light;
XColor border_fg;
XColor border_bg;
XColor launcher_fg;
XColor launcher_bg;
GC pixmap_gc;
GC invert_gc;
Pixmap close_pm;
Pixmap close_pm_mask;
XpmAttributes close_pm_attrs;
Pixmap utility_close_pm;
Pixmap utility_close_pm_mask;
XpmAttributes utility_close_pm_attrs;
Pixmap iconify_pm;
Pixmap iconify_pm_mask;
XpmAttributes iconify_pm_attrs;
Pixmap zoom_pm;
Pixmap zoom_pm_mask;
XpmAttributes zoom_pm_attrs;
Pixmap unzoom_pm;
Pixmap unzoom_pm_mask;
XpmAttributes unzoom_pm_attrs;
Pixmap default_icon_pm;
Pixmap default_icon_pm_mask;
XpmAttributes default_icon_pm_attrs;
Cursor map_curs;
Cursor move_curs;
Cursor resize_n_curs;
Cursor resize_s_curs;
Cursor resize_e_curs;
Cursor resize_w_curs;
Cursor resize_nw_curs;
Cursor resize_sw_curs;
Cursor resize_ne_curs;
Cursor resize_se_curs;

int exitmsg[2];

char *opt_config_file = NULL;
char *opt_font = DEF_FONT;
char *opt_iconfont = DEF_ICONFONT;
char *opt_fg = DEF_FG;
char *opt_bg = DEF_BG;
char *opt_unfocused_fg = DEF_UNFOCUSED_FG;
char *opt_unfocused_bg = DEF_UNFOCUSED_BG;
char *opt_button_bg = DEF_BUTTON_BG;
char *opt_bevel_dark = DEF_BEVEL_DARK;
char *opt_bevel_light = DEF_BEVEL_LIGHT;
char *opt_border_fg = DEF_BORDER_FG;
char *opt_border_bg = DEF_BORDER_BG;
char *opt_launcher_fg = DEF_LAUNCHER_FG;
char *opt_launcher_bg = DEF_LAUNCHER_BG;
char *opt_root_bg = DEF_ROOTBG;
int opt_bw = DEF_BW;
int opt_pad = DEF_PAD;
int opt_bevel = DEF_BEVEL;
int opt_edge_resist = DEF_EDGE_RES;

static void cleanup(void);
static void read_config(void);
static void setup_display(void);

int
main(int argc, char **argv)
{
	struct sigaction act;
	int ch;

	setlocale(LC_ALL, "");

	while ((ch = getopt(argc, argv, "c:")) != -1) {
		switch (ch) {
		case 'c':
			if (opt_config_file)
				free(opt_config_file);
			opt_config_file = strdup(optarg);
			break;
		default:
			printf("usage: %s [-c <config file>]\n", argv[0]);
			exit(1);
		}
	}

	/* parsing the config file may need dpy, so connect early */
	dpy = XOpenDisplay(NULL);
	if (!dpy)
		err(1, "can't open $DISPLAY \"%s\"", getenv("DISPLAY"));

	XSetErrorHandler(handle_xerror);
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);

	read_config();

	if (pipe2(exitmsg, O_CLOEXEC) != 0)
		err(1, "pipe2");

	act.sa_handler = sig_handler;
	act.sa_flags = 0;
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGINT, &act, NULL);
	sigaction(SIGHUP, &act, NULL);
	sigaction(SIGCHLD, &act, NULL);

	setup_display();
	launcher_setup();
	event_loop();
	cleanup();

	return 0;
}

static void
read_config(void)
{
	FILE *ini = NULL;
	char *key, *val;

	ini = open_ini(opt_config_file);

	if (find_ini_section(ini, "progman")) {
		while (get_ini_kv(ini, &key, &val)) {
			if (strcmp(key, "font") == 0)
				opt_font = strdup(val);
			else if (strcmp(key, "iconfont") == 0)
				opt_iconfont = strdup(val);
			else if (strcmp(key, "fgcolor") == 0)
				opt_fg = strdup(val);
			else if (strcmp(key, "bgcolor") == 0)
				opt_bg = strdup(val);
			else if (strcmp(key, "unfocused_fgcolor") == 0)
				opt_unfocused_fg = strdup(val);
			else if (strcmp(key, "unfocused_bgcolor") == 0)
				opt_unfocused_bg = strdup(val);
			else if (strcmp(key, "button_bgcolor") == 0)
				opt_button_bg = strdup(val);
			else if (strcmp(key, "border_fgcolor") == 0)
				opt_border_fg = strdup(val);
			else if (strcmp(key, "border_bgcolor") == 0)
				opt_border_bg = strdup(val);
			else if (strcmp(key, "launcher_fgcolor") == 0)
				opt_launcher_fg = strdup(val);
			else if (strcmp(key, "launcher_bgcolor") == 0)
				opt_launcher_bg = strdup(val);
			else if (strcmp(key, "border_width") == 0)
				opt_bw = atoi(val);
			else if (strcmp(key, "title_padding") == 0)
				opt_pad = atoi(val);
			else if (strcmp(key, "edgeresist") == 0)
				opt_edge_resist = atoi(val);
			else if (strcmp(key, "root_bgcolor") == 0)
				opt_root_bg = strdup(val);
			else
				warnx("unknown key \"%s\" and value \"%s\" in "
				    "ini", key, val);

			free(key);
			free(val);
		}
	}

	if (find_ini_section(ini, "keyboard"))
		while (get_ini_kv(ini, &key, &val))
			bind_key(key, val);

	fclose(ini);
}

static void
setup_display(void)
{
	XGCValues gv;
	XColor exact;
	XSetWindowAttributes sattr;
	XWindowAttributes attr;
	XIconSize *xis;
	XColor root_bg;
	Pixmap rootpx;
	int shape_err;
	Window qroot, qparent, *wins;
	unsigned int nwins, i;
	client_t *c;

	focused = NULL;
	dragging = NULL;

#ifdef USE_GDK_PIXBUF
	gdk_pixbuf_xlib_init(dpy, screen);
#endif

	map_curs = XCreateFontCursor(dpy, XC_dotbox);
	move_curs = XCreateFontCursor(dpy, XC_fleur);
	resize_n_curs = XCreateFontCursor(dpy, XC_top_side);
	resize_s_curs = XCreateFontCursor(dpy, XC_bottom_side);
	resize_e_curs = XCreateFontCursor(dpy, XC_right_side);
	resize_w_curs = XCreateFontCursor(dpy, XC_left_side);
	resize_nw_curs = XCreateFontCursor(dpy, XC_top_left_corner);
	resize_sw_curs = XCreateFontCursor(dpy, XC_bottom_left_corner);
	resize_ne_curs = XCreateFontCursor(dpy, XC_top_right_corner);
	resize_se_curs = XCreateFontCursor(dpy, XC_bottom_right_corner);

#define alloc_color(val, var, name) \
	if (!XAllocNamedColor(dpy, def_cmap, val, var, &exact)) \
		warnx("invalid %s value \"%s\"", name, val);

	def_cmap = DefaultColormap(dpy, screen);
	alloc_color(opt_fg, &fg, "fgcolor");
	alloc_color(opt_bg, &bg, "opt_fg");
	alloc_color(opt_unfocused_fg, &unfocused_fg, "unfocused_fgcolor");
	alloc_color(opt_unfocused_bg, &unfocused_bg, "unfocused_bgcolor");
	alloc_color(opt_button_bg, &button_bg, "button_bgcolor");
	alloc_color(opt_bevel_dark, &bevel_dark, "bevel_darkcolor");
	alloc_color(opt_bevel_light, &bevel_light, "bevel_lightcolor");
	alloc_color(opt_border_fg, &border_fg, "border_fgcolor");
	alloc_color(opt_border_bg, &border_bg, "border_bgcolor");
	alloc_color(opt_launcher_fg, &launcher_fg, "launcher_fgcolor");
	alloc_color(opt_launcher_bg, &launcher_bg, "launcher_bgcolor");

	XSetLineAttributes(dpy, DefaultGC(dpy, screen), 1, LineSolid, CapButt,
	    JoinBevel);
	XSetFillStyle(dpy, DefaultGC(dpy, screen), FillSolid);

#define create_xft_color(_xft, _pixel) \
	(_xft).color.red = (_pixel).red; \
	(_xft).color.green = (_pixel).green; \
	(_xft).color.blue = (_pixel).blue; \
	(_xft).color.alpha = 0xffff; \
	(_xft).pixel = (_pixel).pixel;

	create_xft_color(xft_fg, fg);
	create_xft_color(xft_fg_unfocused, unfocused_fg);
	create_xft_color(xft_launcher, launcher_fg);
	create_xft_color(xft_launcher_highlighted, launcher_bg);

	font = XftFontOpenName(dpy, screen, opt_font);
	if (!font)
		errx(1, "Xft font \"%s\" not found", opt_font);

	iconfont = XftFontOpenName(dpy, screen, opt_iconfont);
	if (!iconfont)
		errx(1, "icon Xft font \"%s\" not found", opt_iconfont);

	pixmap_gc = XCreateGC(dpy, root, 0, &gv);

	gv.function = GXinvert;
	gv.subwindow_mode = IncludeInferiors;
	invert_gc = XCreateGC(dpy, root,
	    GCFunction | GCSubwindowMode | GCLineWidth, &gv);

	XpmCreatePixmapFromData(dpy, root, close_xpm, &close_pm, &close_pm_mask,
	    &close_pm_attrs);
	XpmCreatePixmapFromData(dpy, root, utility_close_xpm, &utility_close_pm,
	    &utility_close_pm_mask, &utility_close_pm_attrs);
	XpmCreatePixmapFromData(dpy, root, iconify_xpm, &iconify_pm,
	    &iconify_pm_mask, &iconify_pm_attrs);
	XpmCreatePixmapFromData(dpy, root, zoom_xpm, &zoom_pm, &zoom_pm_mask,
	    &zoom_pm_attrs);
	XpmCreatePixmapFromData(dpy, root, unzoom_xpm, &unzoom_pm,
	    &unzoom_pm_mask, &unzoom_pm_attrs);
	XpmCreatePixmapFromData(dpy, root, default_icon_xpm, &default_icon_pm,
	    &default_icon_pm_mask, &default_icon_pm_attrs);

	xis = XAllocIconSize();
	xis->min_width = ICON_SIZE;
	xis->min_height = ICON_SIZE;
	xis->max_width = ICON_SIZE;
	xis->max_height = ICON_SIZE;
	xis->width_inc = 1;
	xis->height_inc = 1;
	XSetIconSizes(dpy, root, xis, 1);
	XFree(xis);

	find_supported_atoms();

	if (opt_root_bg != NULL && strlen(opt_root_bg) &&
	    XAllocNamedColor(dpy, def_cmap, opt_root_bg, &root_bg, &exact)) {
		rootpx = XCreatePixmap(dpy, root, 1, 1,
		    DefaultDepth(dpy, screen));
		XSetForeground(dpy, pixmap_gc, root_bg.pixel);
		XFillRectangle(dpy, rootpx, pixmap_gc, 0, 0, 1, 1);
		XSetWindowBackgroundPixmap(dpy, root, rootpx);
		XClearWindow(dpy, root);
		set_atoms(root, xrootpmap_id, XA_PIXMAP, &rootpx, 1);
	} else if (opt_root_bg)
		warnx("invalid root_bgcolor value \"%s\"", opt_root_bg);

	set_atoms(root, net_num_desks, XA_CARDINAL, &ndesks, 1);
	get_atoms(root, net_cur_desk, XA_CARDINAL, 0, &cur_desk, 1, NULL);
	if (cur_desk >= ndesks) {
		cur_desk = ndesks - 1;
		set_atoms(root, net_cur_desk, XA_CARDINAL, &cur_desk, 1);
	}

	shape_support = XShapeQueryExtension(dpy, &shape_event, &shape_err);

	XQueryTree(dpy, root, &qroot, &qparent, &wins, &nwins);
	for (i = 0; i < nwins; i++) {
		XGetWindowAttributes(dpy, wins[i], &attr);
		if (!attr.override_redirect && attr.map_state == IsViewable) {
			c = new_client(wins[i]);
			c->placed = 1;
			map_client(c);
			map_if_desk(c);
		}
	}
	XFree(wins);

	/* become "the" window manager with SubstructureRedirectMask on root */
	sattr.event_mask = SubMask | ColormapChangeMask | ButtonMask;
	XChangeWindowAttributes(dpy, root, CWEventMask, &sattr);

	/* create a hidden window for _NET_SUPPORTING_WM_CHECK */
	supporting_wm_win = XCreateWindow(dpy, root, 0, 0, 1, 1,
	    0, DefaultDepth(dpy, screen), CopyFromParent,
	    DefaultVisual(dpy, screen), 0, NULL);
	set_string_atom(supporting_wm_win, net_wm_name,
	    (unsigned char *)"progman", 7);
	set_atoms(root, net_supporting_wm, XA_WINDOW, &supporting_wm_win, 1);
}

void
sig_handler(int signum)
{
	pid_t pid;
	int status;

	switch (signum) {
	case SIGINT:
	case SIGTERM:
	case SIGHUP:
		quit();
		break;
	case SIGCHLD:
		while ((pid = waitpid(WAIT_ANY, &status, WNOHANG)) > 0 ||
		    (pid < 0 && errno == EINTR))
			;
		break;
	}
}

void
quit(void)
{
	if (write(exitmsg[1], &exitmsg, 1))
		return;

	warn("failed to exit cleanly");
	exit(0);
}

int
handle_xerror(Display * dpy, XErrorEvent * e)
{
	char msg[255];

	if (e->error_code == BadAccess && e->resourceid == root)
		errx(1, "root window unavailable");

	XGetErrorText(dpy, e->error_code, msg, sizeof msg);
	warnx("X error (%#lx): %s", e->resourceid, msg);

	return 0;
}

int
ignore_xerror(Display * dpy, XErrorEvent * e)
{
	return 0;
}

/*
 * We use XQueryTree here to preserve the window stacking order, since the
 * order in our linked list is different.
 */
static void
cleanup(void)
{
	unsigned int nwins, i;
	Window qroot, qparent, *wins;
	client_t *c;

	XQueryTree(dpy, root, &qroot, &qparent, &wins, &nwins);
	for (i = 0; i < nwins; i++) {
		c = find_client(wins[i], MATCH_FRAME);
		if (c)
			del_client(c, DEL_REMAP);
	}
	XFree(wins);

	XftFontClose(dpy, font);
	XftFontClose(dpy, iconfont);
	XFreeCursor(dpy, map_curs);
	XFreeCursor(dpy, move_curs);
	XFreeCursor(dpy, resize_n_curs);
	XFreeCursor(dpy, resize_s_curs);
	XFreeCursor(dpy, resize_e_curs);
	XFreeCursor(dpy, resize_w_curs);
	XFreeCursor(dpy, resize_nw_curs);
	XFreeCursor(dpy, resize_sw_curs);
	XFreeCursor(dpy, resize_ne_curs);
	XFreeCursor(dpy, resize_se_curs);
	XFreeGC(dpy, pixmap_gc);
	XFreeGC(dpy, invert_gc);
	XFreePixmap(dpy, close_pm);
	XFreePixmap(dpy, close_pm_mask);
	XFreePixmap(dpy, utility_close_pm);
	XFreePixmap(dpy, utility_close_pm_mask);
	XFreePixmap(dpy, iconify_pm);
	XFreePixmap(dpy, iconify_pm_mask);
	XFreePixmap(dpy, zoom_pm);
	XFreePixmap(dpy, zoom_pm_mask);
	XFreePixmap(dpy, unzoom_pm);
	XFreePixmap(dpy, unzoom_pm_mask);
	XFreePixmap(dpy, default_icon_pm);
	XFreePixmap(dpy, default_icon_pm_mask);

	XInstallColormap(dpy, DefaultColormap(dpy, screen));
	XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);

	XDeleteProperty(dpy, root, net_supporting_wm);
	XDestroyWindow(dpy, supporting_wm_win);

	XDeleteProperty(dpy, root, net_supported);
	XDeleteProperty(dpy, root, net_client_list);

	launcher_programs_free();

	XCloseDisplay(dpy);
	exit(0);
}
