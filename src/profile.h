/*
 * wdisplay - monitor configuration for Window Maker
 *
 * profile.h: remembers layouts per set of connected monitors, and hooks the
 * restore into Window Maker's autostart script.
 */

#ifndef WDISPLAY_PROFILE_H
#define WDISPLAY_PROFILE_H

#include "display.h"

/* ~/GNUstep/Defaults/WDisplay */
char *profile_path(void);

/* ~/GNUstep/Library/WindowMaker/autostart */
char *profile_autostart_path(void);

/* Saves cfg as the layout for its set of connected outputs. */
int profile_save(const DConfig *cfg, char **errmsg);

/*
 * Loads the layout saved for the outputs connected right now into cfg.
 * Returns 0 if there is none.
 */
int profile_load(DConfig *cfg);

/* Makes sure the autostart script runs "wdisplay --restore". */
int profile_install_autostart(char **errmsg);

#endif
