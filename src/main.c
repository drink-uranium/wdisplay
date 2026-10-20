/*
 * wdisplay - monitor configuration for Window Maker
 *
 * Copyright (c) 2026 the wdisplay authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <WINGs/WINGs.h>

#include "display.h"
#include "layout.h"
#include "profile.h"
#include "ui.h"

#define VERSION "0.1"

static void usage(const char *prog)
{
	printf("Usage: %s [options]\n"
	       "Monitor configuration for Window Maker.\n\n"
	       "  --restore        apply the layout saved for the connected monitors and exit\n"
	       "  --print          print the xrandr command for the saved layout and exit\n"
	       "  --display NAME   X display to use\n"
	       "  --version        print the version\n"
	       "  --help           show this help\n\n"
	       "Layouts are saved in %s\n"
	       "and restored at login from %s\n",
	       prog, profile_path(), profile_autostart_path());
}

/* Applies the saved layout for whatever monitors are plugged in. */
static int restore(Display *dpy, int print_only)
{
	DConfig *cfg = display_query(dpy);
	char *err = NULL;

	if (!cfg) {
		fprintf(stderr, "wdisplay: the X server does not support RandR\n");
		return 1;
	}
	if (!profile_load(cfg)) {
		if (print_only)
			fprintf(stderr, "wdisplay: no saved layout for these monitors\n");
		display_free(cfg);
		return print_only ? 1 : 0;
	}
	if (display_enabled_count(cfg) == 0) {
		fprintf(stderr, "wdisplay: the saved layout has no enabled display\n");
		display_free(cfg);
		return 1;
	}
	layout_normalize(cfg);

	if (print_only) {
		char **argv = display_build_argv(cfg);
		char *cmd = display_argv_string(argv);

		printf("%s\n", cmd);
		free(cmd);
		display_free_argv(argv);
	} else if (!display_apply(cfg, &err)) {
		fprintf(stderr, "wdisplay: %s\n", err ? err : "xrandr failed");
		free(err);
		display_free(cfg);
		return 1;
	}
	display_free(cfg);
	return 0;
}

int main(int argc, char **argv)
{
	const char *display_name = NULL;
	int do_restore = 0, do_print = 0, i;
	Display *dpy;
	DConfig *cfg;

	WMInitializeApplication("WDisplay", &argc, argv);

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--restore") == 0 || strcmp(argv[i], "-r") == 0) {
			do_restore = 1;
		} else if (strcmp(argv[i], "--print") == 0) {
			do_print = 1;
		} else if ((strcmp(argv[i], "--display") == 0 || strcmp(argv[i], "-display") == 0)
			   && i + 1 < argc) {
			display_name = argv[++i];
		} else if (strcmp(argv[i], "--version") == 0) {
			printf("wdisplay %s\n", VERSION);
			return 0;
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "wdisplay: unknown option %s\n", argv[i]);
			usage(argv[0]);
			return 1;
		}
	}

	dpy = XOpenDisplay(display_name);
	if (!dpy) {
		fprintf(stderr, "wdisplay: could not open display %s\n", XDisplayName(display_name));
		return 1;
	}

	if (do_restore || do_print)
		return restore(dpy, do_print);

	if (getenv("WDISPLAY_FAKE"))
		cfg = display_fake();
	else
		cfg = display_query(dpy);
	if (!cfg) {
		fprintf(stderr, "wdisplay: the X server does not support RandR\n");
		return 1;
	}
	return ui_run(dpy, cfg);
}
