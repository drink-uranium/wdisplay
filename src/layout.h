/*
 * wdisplay - monitor configuration for Window Maker
 *
 * layout.h: keeps the displays in a sane arrangement (no gaps, no overlaps).
 */

#ifndef WDISPLAY_LAYOUT_H
#define WDISPLAY_LAYOUT_H

#include "display.h"

/* Moves the layout so that its top-left corner is at 0,0. */
void layout_normalize(DConfig *cfg);

/*
 * Places output idx as close as possible to wantx,wanty while touching at
 * least one other enabled output and overlapping none. Edges closer than
 * snap pixels are aligned.
 */
void layout_place(DConfig *cfg, int idx, int wantx, int wanty, int snap);

/*
 * Repairs the layout after output idx changed size or got enabled, shifting
 * neighbours so that they stay adjacent instead of overlapping or leaving
 * holes. oldw/oldh is the previous size (0 when it was disabled).
 */
void layout_resized(DConfig *cfg, int idx, int oldw, int oldh);

/* Moves stray outputs back so they all form a single connected group. */
void layout_fixup(DConfig *cfg, int anchor);

#endif
