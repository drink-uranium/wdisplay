/*
 * wdisplay - monitor configuration for Window Maker
 *
 * ui.c: the preferences window, in the spirit of WPrefs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#include <WINGs/WINGs.h>

#include "display.h"
#include "layout.h"
#include "profile.h"
#include "ui.h"

#include "icon.xpm"

/*
 * Plain views from the WINGs internals (WINGsP.h, which drags in pango);
 * they are exported by libWINGs and are what custom widgets are built on.
 */
WMView *W_CreateView(WMView *parent);
WMView *W_CreateUnmanagedTopView(WMScreen *screen);
void W_DestroyView(WMView *view);
void W_RealizeView(WMView *view);
void W_MapView(WMView *view);
void W_MoveView(WMView *view, int x, int y);
void W_ResizeView(WMView *view, unsigned int width, unsigned int height);
void W_DrawRelief(WMScreen *scr, Drawable d, int x, int y, unsigned int width,
		  unsigned int height, WMReliefType relief);

#define WIN_WIDTH       520
#define WIN_HEIGHT      440

#define CANVAS_WIDTH    480
#define CANVAS_HEIGHT   200
#define CANVAS_PAD      24

#define CONFIRM_SECONDS 15

/* how close (in canvas pixels) an edge has to be dropped to snap */
#define SNAP_DISTANCE   10

typedef struct {
	int width, height;
} Size;

typedef struct {
	WMScreen *scr;
	Display *dpy;
	int rr_event_base;

	WMWindow *win;

	WMFrame *arrangeF;
	WMView *canvas;
	WMLabel *hintL;
	WMButton *identifyB;

	WMFrame *displayF;
	WMLabel *outputL;
	WMPopUpButton *outputP;
	WMButton *enableB;
	WMButton *primaryB;
	WMLabel *resL;
	WMPopUpButton *resP;
	WMLabel *rateL;
	WMPopUpButton *rateP;
	WMLabel *rotL;
	WMPopUpButton *rotP;

	WMLabel *statusL;
	WMButton *revertB;
	WMButton *applyB;

	WMFont *bigFont;
	WMFont *smallFont;
	WMColor *deskColor;
	WMColor *bezelColor;
	WMColor *screenColor;
	WMColor *selScreenColor;

	DConfig *cfg;           /* what the user is editing */
	DConfig *orig;          /* what the X server is running */
	int selected;
	int dirty;
	int busy;               /* applying; ignore screen change events */
	WMHandlerID reloadTimer;

	Size *sizes;            /* resolutions of the selected output */
	int nsizes;
	int *rateModes;         /* mode index of each refresh rate item */
	int nrates;

	/* canvas mapping between screen pixels and canvas pixels */
	double scale;
	int offx, offy;

	/* dragging */
	int dragIdx;
	int dragMoved;
	int pressX, pressY;
	int grabX, grabY;
	int dragX, dragY;
} App;

static App *app;

static void update_panel(void);
static void update_status(void);
static void reload_from_server(Bool keepEdits);

/* ------------------------------------------------------------------ */
/* helpers                                                            */

#define ACTIVE(o) ((o)->enabled && (o)->mode >= 0)

static DOutput *sel_output(void)
{
	if (!app->cfg || app->selected < 0 || app->selected >= app->cfg->count)
		return NULL;
	return &app->cfg->outputs[app->selected];
}

static void output_title(const DConfig *cfg, int i, char *buf, size_t size)
{
	const DOutput *o = &cfg->outputs[i];

	if (o->monitor[0])
		snprintf(buf, size, "%i. %s (%s)", i + 1, o->monitor, o->name);
	else
		snprintf(buf, size, "%i. %s", i + 1, o->name);
}

static void set_dirty(int flag)
{
	app->dirty = flag;
	update_status();
}

static void error_panel(const char *title, const char *msg)
{
	WMRunAlertPanel(app->scr, app->win, title, msg, "OK", NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* arrangement canvas                                                 */

static void compute_transform(void)
{
	int i, minx = 0, miny = 0, maxx = 0, maxy = 0, first = 1;
	double sx, sy;

	for (i = 0; i < app->cfg->count; i++) {
		DOutput *o = &app->cfg->outputs[i];

		if (!ACTIVE(o))
			continue;
		if (first || o->x < minx)
			minx = o->x;
		if (first || o->y < miny)
			miny = o->y;
		if (first || o->x + output_width(o) > maxx)
			maxx = o->x + output_width(o);
		if (first || o->y + output_height(o) > maxy)
			maxy = o->y + output_height(o);
		first = 0;
	}
	if (first) {
		app->scale = 0.05;
		app->offx = app->offy = 0;
		return;
	}

	/* leave some room around the layout so displays can be dragged there */
	sx = (double)(CANVAS_WIDTH - 2 * CANVAS_PAD) / (maxx - minx);
	sy = (double)(CANVAS_HEIGHT - 2 * CANVAS_PAD) / (maxy - miny);
	app->scale = sx < sy ? sx : sy;
	if (app->scale > 0.1)
		app->scale = 0.1;
	app->offx = (CANVAS_WIDTH - (maxx - minx) * app->scale) / 2 - minx * app->scale;
	app->offy = (CANVAS_HEIGHT - (maxy - miny) * app->scale) / 2 - miny * app->scale;
}

static void output_canvas_rect(int i, int *x, int *y, int *w, int *h)
{
	DOutput *o = &app->cfg->outputs[i];

	if (app->dragIdx == i && app->dragMoved) {
		*x = app->dragX;
		*y = app->dragY;
	} else {
		*x = app->offx + o->x * app->scale + 0.5;
		*y = app->offy + o->y * app->scale + 0.5;
	}
	*w = output_width(o) * app->scale + 0.5;
	*h = output_height(o) * app->scale + 0.5;
}

static void paint_output(Drawable d, int i)
{
	WMScreen *scr = app->scr;
	DOutput *o = &app->cfg->outputs[i];
	WMColor *white = WMWhiteColor(scr);
	WMColor *black = WMBlackColor(scr);
	WMColor *fill = i == app->selected ? app->selScreenColor : app->screenColor;
	int x, y, w, h, tw, th, sh, ty, text;
	char num[16], info[96];

	output_canvas_rect(i, &x, &y, &w, &h);

	/* bezel */
	XFillRectangle(app->dpy, d, WMColorGC(app->bezelColor), x, y, w, h);
	W_DrawRelief(scr, d, x, y, w, h, WRRaised);

	/* screen */
	XFillRectangle(app->dpy, d, WMColorGC(fill), x + 4, y + 4, w - 8, h - 8);
	XDrawRectangle(app->dpy, d, WMColorGC(black), x + 3, y + 3, w - 7, h - 7);
	if (i == app->selected)
		XDrawRectangle(app->dpy, d, WMColorGC(white), x + 4, y + 4, w - 9, h - 9);

	/* number, name and resolution */
	snprintf(num, sizeof(num), "%i", i + 1);
	tw = WMWidthOfString(app->bigFont, num, strlen(num));
	th = WMFontHeight(app->bigFont);
	sh = WMFontHeight(app->smallFont);
	/* only show the details when they fit */
	text = h > th + 2 * sh + 12;
	ty = y + (h - th - (text ? 2 + 2 * sh : 0)) / 2;
	WMDrawString(scr, d, white, app->bigFont, x + (w - tw) / 2, ty, num, strlen(num));

	if (text) {
		int lines = 0;

		snprintf(info, sizeof(info), "%s%s", o->name, o->primary ? " (primary)" : "");
		tw = WMWidthOfString(app->smallFont, info, strlen(info));
		if (tw < w - 12) {
			WMDrawString(scr, d, white, app->smallFont, x + (w - tw) / 2,
				     ty + th + 2, info, strlen(info));
			lines++;
		}
		snprintf(info, sizeof(info), "%dx%d", o->modes[o->mode].width,
			 o->modes[o->mode].height);
		tw = WMWidthOfString(app->smallFont, info, strlen(info));
		if (tw < w - 12)
			WMDrawString(scr, d, white, app->smallFont, x + (w - tw) / 2,
				     ty + th + 2 + lines * sh,
				     info, strlen(info));
	}

	WMReleaseColor(white);
	WMReleaseColor(black);
}

static void paint_canvas(void)
{
	WMView *view = app->canvas;
	Display *dpy = app->dpy;
	WMSize size = WMGetViewSize(view);
	Window win = WMViewXID(view);
	int i, width = size.width, height = size.height;
	Pixmap pix;

	if (win == None)
		return;

	pix = XCreatePixmap(dpy, win, width, height,
			    WMScreenDepth(app->scr));
	XFillRectangle(dpy, pix, WMColorGC(app->deskColor), 0, 0, width, height);

	if (app->cfg) {
		/* the selected (or dragged) display is drawn on top */
		for (i = 0; i < app->cfg->count; i++)
			if (i != app->selected && ACTIVE(&app->cfg->outputs[i]))
				paint_output(pix, i);
		if (sel_output() && ACTIVE(sel_output()))
			paint_output(pix, app->selected);

		if (display_enabled_count(app->cfg) == 0) {
			const char *msg = "No display enabled";
			WMColor *white = WMWhiteColor(app->scr);
			int tw = WMWidthOfString(app->smallFont, msg, strlen(msg));

			WMDrawString(app->scr, pix, white, app->smallFont, (width - tw) / 2,
				     height / 2, msg, strlen(msg));
			WMReleaseColor(white);
		}
	}
	W_DrawRelief(app->scr, pix, 0, 0, width, height, WRSunken);

	XCopyArea(dpy, pix, win, WMColorGC(app->deskColor),
		  0, 0, width, height, 0, 0);
	XFreePixmap(dpy, pix);
}

static int canvas_hit(int px, int py)
{
	int i, x, y, w, h;

	/* the selected one is on top, so it wins */
	if (sel_output() && ACTIVE(sel_output())) {
		output_canvas_rect(app->selected, &x, &y, &w, &h);
		if (px >= x && px < x + w && py >= y && py < y + h)
			return app->selected;
	}
	for (i = app->cfg->count - 1; i >= 0; i--) {
		if (!ACTIVE(&app->cfg->outputs[i]))
			continue;
		output_canvas_rect(i, &x, &y, &w, &h);
		if (px >= x && px < x + w && py >= y && py < y + h)
			return i;
	}
	return -1;
}

static void canvas_events(XEvent *event, void *data)
{
	int i;

	(void)data;
	if (!app->cfg)
		return;

	switch (event->type) {
	case Expose:
		if (event->xexpose.count == 0)
			paint_canvas();
		break;

	case ButtonPress:
		if (event->xbutton.button != Button1)
			break;
		i = canvas_hit(event->xbutton.x, event->xbutton.y);
		if (i < 0)
			break;
		if (i != app->selected) {
			app->selected = i;
			update_panel();
		}
		app->dragIdx = i;
		app->dragMoved = 0;
		app->pressX = event->xbutton.x;
		app->pressY = event->xbutton.y;
		{
			int x, y, w, h;

			output_canvas_rect(i, &x, &y, &w, &h);
			app->grabX = event->xbutton.x - x;
			app->grabY = event->xbutton.y - y;
			app->dragX = x;
			app->dragY = y;
		}
		paint_canvas();
		break;

	case MotionNotify:
		if (app->dragIdx < 0)
			break;
		if (!app->dragMoved &&
		    abs(event->xmotion.x - app->pressX) < 3 &&
		    abs(event->xmotion.y - app->pressY) < 3)
			break;
		app->dragMoved = 1;
		app->dragX = event->xmotion.x - app->grabX;
		app->dragY = event->xmotion.y - app->grabY;
		paint_canvas();
		break;

	case ButtonRelease:
		if (event->xbutton.button != Button1 || app->dragIdx < 0)
			break;
		if (app->dragMoved) {
			DOutput *o = &app->cfg->outputs[app->dragIdx];
			int oldx = o->x, oldy = o->y;
			int wantx = (app->dragX - app->offx) / app->scale;
			int wanty = (app->dragY - app->offy) / app->scale;

			layout_place(app->cfg, app->dragIdx, wantx, wanty,
				     SNAP_DISTANCE / app->scale);
			layout_fixup(app->cfg, app->dragIdx);
			app->dragIdx = -1;
			compute_transform();
			if (o->x != oldx || o->y != oldy)
				set_dirty(1);
		}
		app->dragIdx = -1;
		app->dragMoved = 0;
		paint_canvas();
		break;
	}
}

/* ------------------------------------------------------------------ */
/* "Identify": shows each display's number on the display itself      */

typedef struct {
	WMView *view;
	char text[64];
	char sub[96];
} IdentifyWin;

static void identify_paint(XEvent *event, void *data)
{
	IdentifyWin *iw = data;
	WMScreen *scr = app->scr;
	WMColor *white = WMWhiteColor(scr);
	WMColor *black = WMBlackColor(scr);
	WMFont *font = WMBoldSystemFontOfSize(scr, 72);
	Drawable d = WMViewXID(iw->view);
	WMSize size = WMGetViewSize(iw->view);
	int w = size.width, h = size.height;
	int tw;

	if (event->type != Expose || event->xexpose.count != 0) {
		WMReleaseColor(white);
		WMReleaseColor(black);
		WMReleaseFont(font);
		return;
	}
	XFillRectangle(app->dpy, d, WMColorGC(app->selScreenColor), 0, 0, w, h);
	XDrawRectangle(app->dpy, d, WMColorGC(black), 0, 0, w - 1, h - 1);
	W_DrawRelief(scr, d, 1, 1, w - 2, h - 2, WRRaised);

	tw = WMWidthOfString(font, iw->text, strlen(iw->text));
	WMDrawString(scr, d, white, font, (w - tw) / 2, 12, iw->text, strlen(iw->text));
	tw = WMWidthOfString(app->smallFont, iw->sub, strlen(iw->sub));
	WMDrawString(scr, d, white, app->smallFont, (w - tw) / 2,
		     h - WMFontHeight(app->smallFont) - 10, iw->sub, strlen(iw->sub));

	WMReleaseColor(white);
	WMReleaseColor(black);
	WMReleaseFont(font);
}

static IdentifyWin *identify_wins;
static int identify_count;
static WMHandlerID identify_timer;

static void identify_close(void *data)
{
	int i;

	(void)data;
	for (i = 0; i < identify_count; i++)
		W_DestroyView(identify_wins[i].view);
	wfree(identify_wins);
	identify_wins = NULL;
	identify_count = 0;
	identify_timer = NULL;
}

static void identify_action(WMWidget *w, void *data)
{
	DConfig *cur = app->orig;
	int i, j;

	(void)w;
	(void)data;
	if (identify_timer) {
		WMDeleteTimerHandler(identify_timer);
		identify_close(NULL);
	}
	if (!cur)
		return;

	identify_wins = wmalloc(sizeof(IdentifyWin) * (cur->count + 1));
	for (i = 0; i < cur->count; i++) {
		DOutput *o = &cur->outputs[i];
		IdentifyWin *iw;
		int number = i + 1;

		if (!ACTIVE(o))
			continue;
		/* number them like the canvas does */
		for (j = 0; j < app->cfg->count; j++)
			if (strcmp(app->cfg->outputs[j].name, o->name) == 0)
				number = j + 1;

		iw = &identify_wins[identify_count++];
		snprintf(iw->text, sizeof(iw->text), "%i", number);
		snprintf(iw->sub, sizeof(iw->sub), "%s", o->monitor[0] ? o->monitor : o->name);
		iw->view = W_CreateUnmanagedTopView(app->scr);
		W_ResizeView(iw->view, 180, 140);
		W_MoveView(iw->view, o->x + 40, o->y + 40);
		WMCreateEventHandler(iw->view, ExposureMask, identify_paint, iw);
		W_RealizeView(iw->view);
		W_MapView(iw->view);
		XRaiseWindow(app->dpy, WMViewXID(iw->view));
	}
	identify_timer = WMAddTimerHandler(3000, identify_close, NULL);
}

/* ------------------------------------------------------------------ */
/* the settings panel                                                 */

static int cmp_size(const void *a, const void *b)
{
	const Size *x = a, *y = b;
	long ax = (long)x->width * x->height, ay = (long)y->width * y->height;

	if (ax != ay)
		return ax < ay ? 1 : -1;
	return y->width - x->width;
}

static void clear_popup(WMPopUpButton *p)
{
	while (WMGetPopUpButtonNumberOfItems(p) > 0)
		WMRemovePopUpButtonItem(p, 0);
}

static void fill_rates(void)
{
	DOutput *o = sel_output();
	int i, j, sel = -1;
	char buf[64];

	clear_popup(app->rateP);
	app->nrates = 0;
	if (!o || o->mode < 0)
		return;

	wfree(app->rateModes);
	app->rateModes = wmalloc(sizeof(int) * (o->nmodes + 1));

	for (i = 0; i < o->nmodes; i++) {
		DMode *m = &o->modes[i];
		int dup = 0;

		if (m->width != o->modes[o->mode].width ||
		    m->height != o->modes[o->mode].height)
			continue;
		/* modes that only differ in timings look the same to the user */
		for (j = 0; j < app->nrates; j++) {
			DMode *n = &o->modes[app->rateModes[j]];

			if (n->interlaced == m->interlaced &&
			    (int)(n->rate * 100 + 0.5) == (int)(m->rate * 100 + 0.5))
				dup = 1;
		}
		if (dup && i != o->mode)
			continue;
		app->rateModes[app->nrates++] = i;
	}

	for (i = 0; i < app->nrates; i++) {
		DMode *m = &o->modes[app->rateModes[i]];

		snprintf(buf, sizeof(buf), "%.2f Hz%s", m->rate, m->interlaced ? " (interlaced)" : "");
		WMAddPopUpButtonItem(app->rateP, buf);
		if (app->rateModes[i] == o->mode)
			sel = i;
	}
	WMSetPopUpButtonSelectedItem(app->rateP, sel);
}

static void fill_sizes(void)
{
	DOutput *o = sel_output();
	int i, j, sel = -1;
	char buf[64];

	clear_popup(app->resP);
	app->nsizes = 0;
	if (!o)
		return;

	wfree(app->sizes);
	app->sizes = wmalloc(sizeof(Size) * (o->nmodes + 1));
	for (i = 0; i < o->nmodes; i++) {
		for (j = 0; j < app->nsizes; j++)
			if (app->sizes[j].width == o->modes[i].width &&
			    app->sizes[j].height == o->modes[i].height)
				break;
		if (j == app->nsizes) {
			app->sizes[j].width = o->modes[i].width;
			app->sizes[j].height = o->modes[i].height;
			app->nsizes++;
		}
	}
	qsort(app->sizes, app->nsizes, sizeof(Size), cmp_size);

	for (i = 0; i < app->nsizes; i++) {
		int pref = output_preferred_mode(o);

		snprintf(buf, sizeof(buf), "%d x %d%s", app->sizes[i].width, app->sizes[i].height,
			 pref >= 0 && o->modes[pref].width == app->sizes[i].width &&
			 o->modes[pref].height == app->sizes[i].height ? " (recommended)" : "");
		WMAddPopUpButtonItem(app->resP, buf);
		if (o->mode >= 0 && o->modes[o->mode].width == app->sizes[i].width &&
		    o->modes[o->mode].height == app->sizes[i].height)
			sel = i;
	}
	WMSetPopUpButtonSelectedItem(app->resP, sel);
}

static void update_panel(void)
{
	DOutput *o = sel_output();
	int i, active;
	char buf[160];

	clear_popup(app->outputP);
	for (i = 0; app->cfg && i < app->cfg->count; i++) {
		output_title(app->cfg, i, buf, sizeof(buf));
		if (!app->cfg->outputs[i].connected)
			strncat(buf, " - disconnected", sizeof(buf) - strlen(buf) - 1);
		WMAddPopUpButtonItem(app->outputP, buf);
	}
	WMSetPopUpButtonSelectedItem(app->outputP, o ? app->selected : -1);
	WMSetPopUpButtonEnabled(app->outputP, o != NULL);

	active = o && ACTIVE(o);
	WMSetButtonEnabled(app->enableB, o != NULL && o->nmodes > 0);
	WMSetButtonSelected(app->enableB, active);
	WMSetButtonEnabled(app->primaryB, active);
	WMSetButtonSelected(app->primaryB, active && o->primary);

	fill_sizes();
	fill_rates();
	WMSetPopUpButtonEnabled(app->resP, active);
	WMSetPopUpButtonEnabled(app->rateP, active);

	WMSetPopUpButtonSelectedItem(app->rotP, o ? o->rotation : 0);
	for (i = 0; i < ROT_COUNT; i++) {
		static const unsigned bits[ROT_COUNT] = {
			RR_Rotate_0, RR_Rotate_90, RR_Rotate_180, RR_Rotate_270
		};

		WMSetPopUpButtonItemEnabled(app->rotP, i, o && (o->rotations & bits[i]));
	}
	WMSetPopUpButtonEnabled(app->rotP, active);

	paint_canvas();
	update_status();
}

static void update_status(void)
{
	if (app->cfg && app->cfg->fake)
		WMSetLabelText(app->statusL, app->dirty ?
			       "Test mode: changes are only printed." :
			       "Test mode (WDISPLAY_FAKE): nothing is changed.");
	else if (app->dirty)
		WMSetLabelText(app->statusL, "Press Apply to use and keep the new settings.");
	else
		WMSetLabelText(app->statusL, "");
}

static void output_changed(WMWidget *w, void *data)
{
	(void)w;
	(void)data;
	app->selected = WMGetPopUpButtonSelectedItem(app->outputP);
	update_panel();
}

static void resolution_changed(WMWidget *w, void *data)
{
	DOutput *o = sel_output();
	int i = WMGetPopUpButtonSelectedItem(app->resP);
	int oldw, oldh, mode, j;
	double rate;

	(void)w;
	(void)data;
	if (!o || i < 0 || i >= app->nsizes || o->mode < 0)
		return;

	oldw = output_width(o);
	oldh = output_height(o);
	rate = o->modes[o->mode].rate;

	/* keep the refresh rate if possible, otherwise take the preferred one */
	mode = output_find_mode(o, app->sizes[i].width, app->sizes[i].height, rate);
	if (mode >= 0 && (o->modes[mode].rate < rate - 0.5 || o->modes[mode].rate > rate + 0.5)) {
		for (j = 0; j < o->nmodes; j++)
			if (o->modes[j].preferred && o->modes[j].width == app->sizes[i].width &&
			    o->modes[j].height == app->sizes[i].height)
				mode = j;
	}
	if (mode < 0 || mode == o->mode)
		return;
	o->mode = mode;
	layout_resized(app->cfg, app->selected, oldw, oldh);
	compute_transform();
	set_dirty(1);
	update_panel();
}

static void rate_changed(WMWidget *w, void *data)
{
	DOutput *o = sel_output();
	int i = WMGetPopUpButtonSelectedItem(app->rateP);

	(void)w;
	(void)data;
	if (!o || i < 0 || i >= app->nrates || o->mode == app->rateModes[i])
		return;
	o->mode = app->rateModes[i];
	set_dirty(1);
}

static void rotation_changed(WMWidget *w, void *data)
{
	DOutput *o = sel_output();
	int i = WMGetPopUpButtonSelectedItem(app->rotP);
	int oldw, oldh;

	(void)w;
	(void)data;
	if (!o || i < 0 || i >= ROT_COUNT || i == o->rotation)
		return;
	oldw = output_width(o);
	oldh = output_height(o);
	o->rotation = i;
	layout_resized(app->cfg, app->selected, oldw, oldh);
	compute_transform();
	set_dirty(1);
	update_panel();
}

static void enable_changed(WMWidget *w, void *data)
{
	DOutput *o = sel_output();
	int i;

	(void)w;
	(void)data;
	if (!o)
		return;

	if (WMGetButtonSelected(app->enableB)) {
		if (o->mode < 0)
			o->mode = output_preferred_mode(o);
		if (o->mode < 0) {
			WMSetButtonSelected(app->enableB, 0);
			return;
		}
		o->enabled = 1;
		o->rotation = ROT_NORMAL;
		layout_resized(app->cfg, app->selected, 0, 0);
	} else {
		if (display_enabled_count(app->cfg) <= 1) {
			WMSetButtonSelected(app->enableB, 1);
			error_panel("Display", "At least one display has to stay enabled.");
			return;
		}
		o->enabled = 0;
		if (o->primary) {
			o->primary = 0;
			for (i = 0; i < app->cfg->count; i++)
				if (ACTIVE(&app->cfg->outputs[i])) {
					app->cfg->outputs[i].primary = 1;
					break;
				}
		}
		layout_fixup(app->cfg, -1);
	}
	compute_transform();
	set_dirty(1);
	update_panel();
}

static void primary_changed(WMWidget *w, void *data)
{
	DOutput *o = sel_output();
	int i;

	(void)w;
	(void)data;
	if (!o)
		return;
	if (WMGetButtonSelected(app->primaryB)) {
		for (i = 0; i < app->cfg->count; i++)
			app->cfg->outputs[i].primary = 0;
		o->primary = 1;
	} else {
		o->primary = 0;
	}
	set_dirty(1);
	paint_canvas();
}

/* ------------------------------------------------------------------ */
/* applying                                                           */

typedef struct {
	WMWindow *win;
	WMLabel *msgL;
	int seconds;
	int result;             /* 1 keep, 0 revert, -1 still asking */
	WMHandlerID timer;
} Confirm;

static void confirm_update_text(Confirm *c)
{
	char buf[160];

	snprintf(buf, sizeof(buf),
		 "Do you want to keep these display settings?\n\n"
		 "Reverting to the previous settings in %i second%s.",
		 c->seconds, c->seconds == 1 ? "" : "s");
	WMSetLabelText(c->msgL, buf);
}

static void confirm_tick(void *data)
{
	Confirm *c = data;

	c->timer = NULL;
	if (--c->seconds <= 0) {
		c->result = 0;
		WMBreakModalLoop(app->scr);
		return;
	}
	confirm_update_text(c);
	c->timer = WMAddTimerHandler(1000, confirm_tick, c);
}

static void confirm_button(WMWidget *w, void *data)
{
	Confirm *c = data;

	c->result = (int)(long)WMGetHangedData(w);
	WMBreakModalLoop(app->scr);
}

static void confirm_close(WMWidget *w, void *data)
{
	Confirm *c = data;

	(void)w;
	c->result = 0;
	WMBreakModalLoop(app->scr);
}

/* Asks whether the new settings should stay; reverts automatically. */
static int confirm_settings(void)
{
	Confirm c;
	WMButton *keepB, *revertB;
	WMLabel *iconL;
	WMPixmap *icon;
	int w = 380, h = 140;

	memset(&c, 0, sizeof(c));
	c.seconds = CONFIRM_SECONDS;
	c.result = -1;

	c.win = WMCreatePanelWithStyleForWindow(app->win, "confirm",
						WMTitledWindowMask | WMClosableWindowMask);
	WMSetWindowTitle(c.win, "Display Settings");
	WMResizeWidget(c.win, w, h);
	WMSetWindowCloseAction(c.win, confirm_close, &c);

	iconL = WMCreateLabel(c.win);
	WMResizeWidget(iconL, 48, 48);
	WMMoveWidget(iconL, 14, 16);
	WMSetLabelImagePosition(iconL, WIPImageOnly);
	icon = WMCreatePixmapFromXPMData(app->scr, wdisplay_icon_xpm);
	if (icon) {
		WMSetLabelImage(iconL, icon);
		WMReleasePixmap(icon);
	}

	c.msgL = WMCreateLabel(c.win);
	WMResizeWidget(c.msgL, w - 86, 70);
	WMMoveWidget(c.msgL, 74, 14);
	WMSetLabelWraps(c.msgL, True);
	confirm_update_text(&c);

	keepB = WMCreateCommandButton(c.win);
	WMResizeWidget(keepB, 110, 24);
	WMMoveWidget(keepB, w - 120, h - 34);
	WMSetButtonText(keepB, "Keep Changes");
	WMHangData(keepB, (void *)1L);
	WMSetButtonAction(keepB, confirm_button, &c);

	revertB = WMCreateCommandButton(c.win);
	WMResizeWidget(revertB, 90, 24);
	WMMoveWidget(revertB, w - 220, h - 34);
	WMSetButtonText(revertB, "Revert");
	WMHangData(revertB, (void *)0L);
	WMSetButtonAction(revertB, confirm_button, &c);

	WMRealizeWidget(c.win);
	WMMapSubwidgets(c.win);
	WMMapWidget(c.win);

	c.timer = WMAddTimerHandler(1000, confirm_tick, &c);
	WMRunModalLoop(app->scr, WMWidgetView(c.win));

	if (c.timer)
		WMDeleteTimerHandler(c.timer);
	WMUnmapWidget(c.win);
	WMDestroyWidget(c.win);
	return c.result == 1;
}

static void revert_action(WMWidget *w, void *data)
{
	(void)w;
	(void)data;
	display_free(app->cfg);
	app->cfg = display_copy(app->orig);
	compute_transform();
	set_dirty(0);
	update_panel();
}

static void apply_action(WMWidget *w, void *data)
{
	DConfig *newcfg;
	char *err = NULL;
	char msg[1024];

	(void)w;
	(void)data;
	if (!app->cfg || app->busy)
		return;
	if (display_enabled_count(app->cfg) == 0) {
		error_panel("Display", "At least one display has to be enabled.");
		return;
	}

	layout_normalize(app->cfg);
	newcfg = display_copy(app->cfg);
	app->busy = 1;

	if (!display_apply(newcfg, &err)) {
		snprintf(msg, sizeof(msg), "The new settings could not be applied:\n\n%s",
			 err ? err : "unknown error");
		error_panel("Error", msg);
		free(err);
		app->busy = 0;
		display_free(newcfg);
		reload_from_server(True);
		return;
	}

	XSync(app->dpy, False);
	if (!confirm_settings()) {
		if (!display_apply(app->orig, &err)) {
			snprintf(msg, sizeof(msg), "Could not restore the old settings:\n\n%s",
				 err ? err : "unknown error");
			error_panel("Error", msg);
			free(err);
		}
		app->busy = 0;
		display_free(newcfg);
		reload_from_server(True);
		return;
	}

	/* keep it: remember the layout for this set of monitors, for good */
	if (!profile_save(newcfg, &err) || !profile_install_autostart(&err)) {
		snprintf(msg, sizeof(msg),
			 "The settings were applied, but could not be saved:\n\n%s",
			 err ? err : "unknown error");
		error_panel("Error", msg);
		free(err);
	}
	app->busy = 0;

	if (newcfg->fake) {
		display_free(app->orig);
		app->orig = newcfg;
		display_free(app->cfg);
		app->cfg = display_copy(newcfg);
		set_dirty(0);
		compute_transform();
		update_panel();
	} else {
		display_free(newcfg);
		reload_from_server(False);
	}
	WMSetLabelText(app->statusL, "Saved. This layout is restored every time you log in.");
}

/* ------------------------------------------------------------------ */
/* keeping up with monitors being plugged in or out                   */

static int find_output(const DConfig *cfg, const char *name)
{
	int i;

	for (i = 0; cfg && i < cfg->count; i++)
		if (strcmp(cfg->outputs[i].name, name) == 0)
			return i;
	return -1;
}

static int same_outputs(const DConfig *a, const DConfig *b)
{
	int i, j;

	if (!a || !b || a->count != b->count)
		return 0;
	for (i = 0; i < a->count; i++) {
		j = find_output(b, a->outputs[i].name);
		if (j < 0 || a->outputs[i].connected != b->outputs[j].connected)
			return 0;
	}
	return 1;
}

static void reload_from_server(Bool keepEdits)
{
	DConfig *cfg;
	char name[64] = "";

	if (app->cfg && app->cfg->fake)
		return;

	cfg = display_query(app->dpy);
	if (!cfg)
		return;

	if (sel_output())
		snprintf(name, sizeof(name), "%s", sel_output()->name);

	if (keepEdits && app->dirty && same_outputs(cfg, app->cfg)) {
		/* only the running state changed; the user's edits stay */
		display_free(app->orig);
		app->orig = cfg;
		return;
	}

	display_free(app->orig);
	display_free(app->cfg);
	app->orig = cfg;
	app->cfg = display_copy(cfg);
	app->selected = find_output(app->cfg, name);
	if (app->selected < 0)
		app->selected = app->cfg->count > 0 ? 0 : -1;
	app->dirty = 0;
	compute_transform();
	update_panel();
}

static void reload_timeout(void *data)
{
	(void)data;
	app->reloadTimer = NULL;
	if (!app->busy)
		reload_from_server(True);
}

static void x_event_hook(XEvent *event)
{
	if (event->type != app->rr_event_base + RRScreenChangeNotify)
		return;
	XRRUpdateConfiguration(event);
	if (app->busy)
		return;
	/* several of these arrive at once, so wait for things to settle */
	if (app->reloadTimer)
		WMDeleteTimerHandler(app->reloadTimer);
	app->reloadTimer = WMAddTimerHandler(500, reload_timeout, NULL);
}

/* ------------------------------------------------------------------ */
/* building the window                                                */

static void close_action(WMWidget *w, void *data)
{
	(void)w;
	(void)data;
	exit(0);
}

static WMLabel *make_label(WMWidget *parent, const char *text, int x, int y, int w)
{
	WMLabel *l = WMCreateLabel(parent);

	WMResizeWidget(l, w, 20);
	WMMoveWidget(l, x, y);
	WMSetLabelText(l, text);
	WMSetLabelTextAlignment(l, WARight);
	return l;
}

static void create_window(void)
{
	WMScreen *scr = app->scr;
	WMPixmap *icon;

	app->win = WMCreateWindow(scr, "WDisplay");
	WMSetWindowTitle(app->win, "Display Preferences");
	WMSetWindowMiniwindowTitle(app->win, "Displays");
	WMSetWindowCloseAction(app->win, close_action, NULL);
	WMResizeWidget(app->win, WIN_WIDTH, WIN_HEIGHT);
	WMSetWindowMinSize(app->win, WIN_WIDTH, WIN_HEIGHT);
	WMSetWindowMaxSize(app->win, WIN_WIDTH, WIN_HEIGHT);

	icon = WMCreatePixmapFromXPMData(scr, wdisplay_icon_xpm);
	if (icon) {
		WMSetApplicationIconPixmap(scr, icon);
		WMReleasePixmap(icon);
	}

	/* arrangement */
	app->arrangeF = WMCreateFrame(app->win);
	WMResizeWidget(app->arrangeF, WIN_WIDTH - 20, 262);
	WMMoveWidget(app->arrangeF, 10, 8);
	WMSetFrameTitle(app->arrangeF, "Arrangement");

	app->canvas = W_CreateView(WMWidgetView(app->arrangeF));
	W_ResizeView(app->canvas, CANVAS_WIDTH, CANVAS_HEIGHT);
	W_MoveView(app->canvas, (WIN_WIDTH - 20 - CANVAS_WIDTH) / 2, 20);
	WMCreateEventHandler(app->canvas, ExposureMask | ButtonPressMask | ButtonReleaseMask
			     | Button1MotionMask, canvas_events, NULL);

	app->hintL = WMCreateLabel(app->arrangeF);
	WMResizeWidget(app->hintL, 340, 30);
	WMMoveWidget(app->hintL, 10, 226);
	WMSetLabelWraps(app->hintL, True);
	WMSetLabelText(app->hintL, "Drag the displays to match how they are placed on "
		       "your desk. Click one to change its settings.");

	app->identifyB = WMCreateCommandButton(app->arrangeF);
	WMResizeWidget(app->identifyB, 100, 24);
	WMMoveWidget(app->identifyB, WIN_WIDTH - 20 - 110, 228);
	WMSetButtonText(app->identifyB, "Identify");
	WMSetButtonAction(app->identifyB, identify_action, NULL);
	WMSetBalloonTextForView("Show the number of each display on the display itself.",
				WMWidgetView(app->identifyB));

	/* settings of the selected display */
	app->displayF = WMCreateFrame(app->win);
	WMResizeWidget(app->displayF, WIN_WIDTH - 20, 120);
	WMMoveWidget(app->displayF, 10, 274);
	WMSetFrameTitle(app->displayF, "Display");

	app->outputL = make_label(app->displayF, "Display:", 6, 20, 80);
	app->outputP = WMCreatePopUpButton(app->displayF);
	WMResizeWidget(app->outputP, 384, 20);
	WMMoveWidget(app->outputP, 90, 20);
	WMSetPopUpButtonAction(app->outputP, output_changed, NULL);

	app->resL = make_label(app->displayF, "Resolution:", 6, 50, 80);
	app->resP = WMCreatePopUpButton(app->displayF);
	WMResizeWidget(app->resP, 190, 20);
	WMMoveWidget(app->resP, 90, 50);
	WMSetPopUpButtonAction(app->resP, resolution_changed, NULL);

	app->rateL = make_label(app->displayF, "Refresh rate:", 284, 50, 80);
	app->rateP = WMCreatePopUpButton(app->displayF);
	WMResizeWidget(app->rateP, 106, 20);
	WMMoveWidget(app->rateP, 368, 50);
	WMSetPopUpButtonAction(app->rateP, rate_changed, NULL);

	app->rotL = make_label(app->displayF, "Rotation:", 6, 80, 80);
	app->rotP = WMCreatePopUpButton(app->displayF);
	WMResizeWidget(app->rotP, 120, 20);
	WMMoveWidget(app->rotP, 90, 80);
	WMAddPopUpButtonItem(app->rotP, "Normal");
	WMAddPopUpButtonItem(app->rotP, "Left");
	WMAddPopUpButtonItem(app->rotP, "Upside down");
	WMAddPopUpButtonItem(app->rotP, "Right");
	WMSetPopUpButtonAction(app->rotP, rotation_changed, NULL);

	app->enableB = WMCreateSwitchButton(app->displayF);
	WMResizeWidget(app->enableB, 110, 20);
	WMMoveWidget(app->enableB, 236, 80);
	WMSetButtonText(app->enableB, "Enabled");
	WMSetButtonAction(app->enableB, enable_changed, NULL);

	app->primaryB = WMCreateSwitchButton(app->displayF);
	WMResizeWidget(app->primaryB, 124, 20);
	WMMoveWidget(app->primaryB, 350, 80);
	WMSetButtonText(app->primaryB, "Primary display");
	WMSetButtonAction(app->primaryB, primary_changed, NULL);
	WMSetBalloonTextForView("The main display, for programs that ask for the primary monitor.",
				WMWidgetView(app->primaryB));

	/* bottom row */
	app->statusL = WMCreateLabel(app->win);
	WMResizeWidget(app->statusL, 300, 20);
	WMMoveWidget(app->statusL, 12, WIN_HEIGHT - 36);

	app->revertB = WMCreateCommandButton(app->win);
	WMResizeWidget(app->revertB, 90, 24);
	WMMoveWidget(app->revertB, WIN_WIDTH - 200, WIN_HEIGHT - 38);
	WMSetButtonText(app->revertB, "Revert");
	WMSetButtonAction(app->revertB, revert_action, NULL);

	app->applyB = WMCreateCommandButton(app->win);
	WMResizeWidget(app->applyB, 90, 24);
	WMMoveWidget(app->applyB, WIN_WIDTH - 100, WIN_HEIGHT - 38);
	WMSetButtonText(app->applyB, "Apply");
	WMSetButtonAction(app->applyB, apply_action, NULL);
	WMSetBalloonTextForView("Use these settings now, and every time you log in.",
				WMWidgetView(app->applyB));

	app->bigFont = WMBoldSystemFontOfSize(scr, 24);
	app->smallFont = WMSystemFontOfSize(scr, 10);
	app->deskColor = WMCreateRGBColor(scr, 0x5050, 0x5050, 0x7575, True);
	app->bezelColor = WMGrayColor(scr);
	app->screenColor = WMCreateRGBColor(scr, 0x3b3b, 0x4b4b, 0x6666, True);
	app->selScreenColor = WMCreateRGBColor(scr, 0x5b5b, 0x8686, 0xc4c4, True);

	WMRealizeWidget(app->win);
	W_MapView(app->canvas);
	WMMapSubwidgets(app->arrangeF);
	WMMapSubwidgets(app->displayF);
	WMMapSubwidgets(app->win);
	WMMapWidget(app->win);
}

int ui_run(Display *dpy, DConfig *cfg)
{
	WMScreen *scr;
	int error_base;

	scr = WMCreateScreen(dpy, DefaultScreen(dpy));
	if (!scr) {
		fprintf(stderr, "wdisplay: could not initialize the screen\n");
		return 1;
	}

	app = wmalloc(sizeof(App));
	app->scr = scr;
	app->dpy = dpy;
	app->dragIdx = -1;
	app->orig = cfg;
	app->cfg = display_copy(cfg);
	app->selected = -1;
	for (int i = 0; i < cfg->count && app->selected < 0; i++)
		if (cfg->outputs[i].primary)
			app->selected = i;
	if (app->selected < 0 && cfg->count > 0)
		app->selected = 0;

	if (!cfg->fake && XRRQueryExtension(dpy, &app->rr_event_base, &error_base)) {
		XRRSelectInput(dpy, DefaultRootWindow(dpy), RRScreenChangeNotifyMask);
		WMHookEventHandler(x_event_hook);
	}

	create_window();
	compute_transform();
	update_panel();

	WMScreenMainLoop(scr);
	return 0;
}
