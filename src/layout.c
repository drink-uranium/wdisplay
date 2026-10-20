/*
 * wdisplay - monitor configuration for Window Maker
 *
 * layout.c: keeps the displays in a sane arrangement (no gaps, no overlaps).
 */

#include <limits.h>
#include <stdlib.h>

#include "layout.h"

#define ACTIVE(o) ((o)->enabled && (o)->mode >= 0)

typedef struct {
	int x, y, w, h;
} Rect;

static Rect rect_of(const DOutput *o)
{
	Rect r = { o->x, o->y, output_width(o), output_height(o) };

	return r;
}

static int overlaps(Rect a, Rect b)
{
	return a.x < b.x + b.w && b.x < a.x + a.w &&
	       a.y < b.y + b.h && b.y < a.y + a.h;
}

/* True when a and b share a piece of an edge. */
static int touches(Rect a, Rect b)
{
	if (a.x + a.w == b.x || b.x + b.w == a.x)
		return a.y < b.y + b.h && b.y < a.y + a.h;
	if (a.y + a.h == b.y || b.y + b.h == a.y)
		return a.x < b.x + b.w && b.x < a.x + a.w;
	return 0;
}

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* Aligns pos (of a span of length len) with the edges of [start, start+blen). */
static int snap_span(int pos, int len, int start, int blen, int snap)
{
	if (abs(pos - start) <= snap)
		return start;
	if (abs(pos + len - (start + blen)) <= snap)
		return start + blen - len;
	if (abs(pos + len / 2 - (start + blen / 2)) <= snap)
		return start + blen / 2 - len / 2;
	return pos;
}

static int fits(const DConfig *cfg, int idx, Rect r)
{
	int i;

	for (i = 0; i < cfg->count; i++) {
		if (i == idx || !ACTIVE(&cfg->outputs[i]))
			continue;
		if (overlaps(r, rect_of(&cfg->outputs[i])))
			return 0;
	}
	return 1;
}

void layout_normalize(DConfig *cfg)
{
	int i, minx = INT_MAX, miny = INT_MAX;

	for (i = 0; i < cfg->count; i++) {
		DOutput *o = &cfg->outputs[i];

		if (!ACTIVE(o))
			continue;
		if (o->x < minx)
			minx = o->x;
		if (o->y < miny)
			miny = o->y;
	}
	if (minx == INT_MAX)
		return;
	for (i = 0; i < cfg->count; i++) {
		DOutput *o = &cfg->outputs[i];

		if (!ACTIVE(o))
			continue;
		o->x -= minx;
		o->y -= miny;
	}
}

void layout_place(DConfig *cfg, int idx, int wantx, int wanty, int snap)
{
	DOutput *o = &cfg->outputs[idx];
	int w = output_width(o), h = output_height(o);
	long best_dist = -1;
	int bestx = o->x, besty = o->y;
	int i, side, others = 0;

	if (!ACTIVE(o))
		return;

	for (i = 0; i < cfg->count; i++) {
		Rect b;

		if (i == idx || !ACTIVE(&cfg->outputs[i]))
			continue;
		others++;
		b = rect_of(&cfg->outputs[i]);

		for (side = 0; side < 4; side++) {
			Rect c = { 0, 0, w, h };
			long dx, dy, dist;

			switch (side) {
			case 0: /* right of b */
			case 1: /* left of b */
				c.x = side == 0 ? b.x + b.w : b.x - w;
				c.y = snap_span(wanty, h, b.y, b.h, snap);
				c.y = clamp(c.y, b.y - h + 1, b.y + b.h - 1);
				break;
			default: /* below / above b */
				c.y = side == 2 ? b.y + b.h : b.y - h;
				c.x = snap_span(wantx, w, b.x, b.w, snap);
				c.x = clamp(c.x, b.x - w + 1, b.x + b.w - 1);
				break;
			}
			if (!fits(cfg, idx, c))
				continue;
			dx = c.x - wantx;
			dy = c.y - wanty;
			dist = dx * dx + dy * dy;
			if (best_dist < 0 || dist < best_dist) {
				best_dist = dist;
				bestx = c.x;
				besty = c.y;
			}
		}
	}

	if (others == 0) {
		o->x = 0;
		o->y = 0;
	} else if (best_dist >= 0) {
		o->x = bestx;
		o->y = besty;
	}
}

/* Marks every output reachable from start through shared edges. */
static void flood(const DConfig *cfg, int start, char *seen)
{
	int i;

	seen[start] = 1;
	for (i = 0; i < cfg->count; i++) {
		if (seen[i] || !ACTIVE(&cfg->outputs[i]))
			continue;
		if (touches(rect_of(&cfg->outputs[start]), rect_of(&cfg->outputs[i])))
			flood(cfg, i, seen);
	}
}

void layout_fixup(DConfig *cfg, int anchor)
{
	char *seen;
	int i, pass, changed;

	if (anchor < 0 || anchor >= cfg->count || !ACTIVE(&cfg->outputs[anchor])) {
		anchor = -1;
		for (i = 0; i < cfg->count && anchor < 0; i++)
			if (ACTIVE(&cfg->outputs[i]))
				anchor = i;
		if (anchor < 0)
			return;
	}

	seen = calloc(cfg->count, 1);
	for (pass = 0; pass < cfg->count * 2; pass++) {
		changed = 0;

		/* first get rid of overlaps */
		for (i = 0; i < cfg->count; i++) {
			DOutput *o = &cfg->outputs[i];

			if (i == anchor || !ACTIVE(o))
				continue;
			if (!fits(cfg, i, rect_of(o))) {
				layout_place(cfg, i, o->x, o->y, 0);
				changed = 1;
			}
		}

		/* then attach whatever floats around to the main group */
		for (i = 0; i < cfg->count; i++)
			seen[i] = 0;
		flood(cfg, anchor, seen);
		for (i = 0; i < cfg->count; i++) {
			DOutput *o = &cfg->outputs[i];

			if (!seen[i] && ACTIVE(o)) {
				layout_place(cfg, i, o->x, o->y, 0);
				changed = 1;
				break;
			}
		}
		if (!changed)
			break;
	}
	free(seen);
	layout_normalize(cfg);
}

void layout_resized(DConfig *cfg, int idx, int oldw, int oldh)
{
	DOutput *o = &cfg->outputs[idx];
	int neww = output_width(o), newh = output_height(o);
	int i, rightmost = 0, others = 0;

	if (!ACTIVE(o)) {
		layout_fixup(cfg, -1);
		return;
	}

	if (oldw == 0 || oldh == 0) {
		/* newly enabled: put it right of everything else */
		for (i = 0; i < cfg->count; i++) {
			DOutput *b = &cfg->outputs[i];

			if (i == idx || !ACTIVE(b))
				continue;
			others++;
			if (b->x + output_width(b) > rightmost)
				rightmost = b->x + output_width(b);
		}
		o->x = rightmost;
		o->y = 0;
		if (!others)
			o->x = 0;
		layout_fixup(cfg, idx);
		return;
	}

	/* shift whatever was beyond the old right/bottom edge */
	for (i = 0; i < cfg->count; i++) {
		DOutput *b = &cfg->outputs[i];

		if (i == idx || !ACTIVE(b))
			continue;
		if (b->x >= o->x + oldw)
			b->x += neww - oldw;
		if (b->y >= o->y + oldh)
			b->y += newh - oldh;
	}
	layout_fixup(cfg, idx);
}
