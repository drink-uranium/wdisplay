/*
 * wdisplay - monitor configuration for Window Maker
 *
 * display.h: in-memory model of the RandR outputs and their settings.
 */

#ifndef WDISPLAY_DISPLAY_H
#define WDISPLAY_DISPLAY_H

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

enum {
	ROT_NORMAL = 0,
	ROT_LEFT,
	ROT_INVERTED,
	ROT_RIGHT,
	ROT_COUNT
};

typedef struct {
	RRMode id;
	int width, height;
	double rate;
	int interlaced;
	int preferred;
} DMode;

typedef struct {
	char name[64];          /* RandR output name, e.g. "DP-1" */
	char monitor[64];       /* monitor model from EDID, may be empty */
	RROutput id;
	int connected;
	unsigned int rotations; /* supported rotations, RR_Rotate_* mask */

	DMode *modes;
	int nmodes;

	/* settings (current, or pending when edited) */
	int enabled;
	int mode;               /* index into modes, -1 when disabled */
	int x, y;
	int rotation;           /* ROT_* */
	int primary;
} DOutput;

typedef struct {
	DOutput *outputs;
	int count;
	int fake;               /* built-in test layout; never touches the server */
} DConfig;

extern const char *rotation_names[ROT_COUNT];

/* Reads the current state from the X server. Returns NULL on failure. */
DConfig *display_query(Display *dpy);

/* A fake two-monitor layout used for testing without real hardware. */
DConfig *display_fake(void);

DConfig *display_copy(const DConfig *cfg);
void display_free(DConfig *cfg);

/* Width/height of an output's mode, taking rotation into account. */
int output_width(const DOutput *o);
int output_height(const DOutput *o);

/* Index of the mode matching size/rate most closely, or -1. */
int output_find_mode(const DOutput *o, int width, int height, double rate);
int output_preferred_mode(const DOutput *o);

/* Number of enabled outputs. */
int display_enabled_count(const DConfig *cfg);

/*
 * Builds the xrandr command line that applies cfg. The returned array is
 * NULL terminated; free it with display_free_argv().
 */
char **display_build_argv(const DConfig *cfg);
void display_free_argv(char **argv);
char *display_argv_string(char **argv);

/*
 * Applies cfg by running xrandr. On failure returns 0 and, if errmsg is
 * not NULL, stores a malloc'ed error description in it.
 */
int display_apply(const DConfig *cfg, char **errmsg);

#endif
