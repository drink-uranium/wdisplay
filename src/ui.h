/*
 * wdisplay - monitor configuration for Window Maker
 */

#ifndef WDISPLAY_UI_H
#define WDISPLAY_UI_H

#include "display.h"

/* Shows the preferences window. Takes ownership of cfg. */
int ui_run(Display *dpy, DConfig *cfg);

#endif
