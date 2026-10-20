/*
 * wdisplay - monitor configuration for Window Maker
 *
 * profile.c: remembers layouts per set of connected monitors, and hooks the
 * restore into Window Maker's autostart script.
 *
 * The layouts are stored in ~/GNUstep/Defaults/WDisplay like this:
 *
 * {
 *   Profiles = {
 *     "DP-1+HDMI-1" = {
 *       "DP-1" = { Enabled = Yes; Width = 2560; Height = 1440; Rate = "143.96";
 *                  X = 1920; Y = 0; Rotation = normal; Primary = Yes; };
 *       "HDMI-1" = { ... };
 *     };
 *   };
 * }
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <WINGs/WUtil.h>

#include "profile.h"

#define HOOK_BEGIN "# >>> wdisplay: restore the saved monitor layout >>>"
#define HOOK_END   "# <<< wdisplay <<<"

static const char autostart_header[] =
	"#!/bin/sh\n"
	"#\n"
	"# Place applications to be executed when WindowMaker is started here.\n"
	"# WindowMaker will wait until this script finishes, so if you run any\n"
	"# commands that take long to execute (like a xterm), put a ``&'' in the\n"
	"# end of the command line.\n"
	"#\n"
	"# This file must be executable.\n"
	"#\n";

char *profile_path(void)
{
	return wdefaultspathfordomain("WDisplay");
}

char *profile_autostart_path(void)
{
	return wstrconcat((char *)wusergnusteppath(), "/Library/WindowMaker/autostart");
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* "DP-1+HDMI-1": the sorted names of all connected outputs */
static char *profile_key(const DConfig *cfg)
{
	const char **names = wmalloc(sizeof(char *) * (cfg->count + 1));
	char *key = wstrdup("");
	int i, n = 0;

	for (i = 0; i < cfg->count; i++)
		if (cfg->outputs[i].connected)
			names[n++] = cfg->outputs[i].name;
	qsort(names, n, sizeof(char *), cmp_str);
	for (i = 0; i < n; i++) {
		if (i)
			key = wstrappend(key, "+");
		key = wstrappend(key, names[i]);
	}
	wfree(names);
	return key;
}

static void put_string(WMPropList *dict, const char *key, const char *value)
{
	WMPropList *k = WMCreatePLString(key);
	WMPropList *v = WMCreatePLString(value);

	WMPutInPLDictionary(dict, k, v);
	WMReleasePropList(k);
	WMReleasePropList(v);
}

static void put_int(WMPropList *dict, const char *key, int value)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%d", value);
	put_string(dict, key, buf);
}

static const char *get_string(WMPropList *dict, const char *key)
{
	WMPropList *k = WMCreatePLString(key);
	WMPropList *v = WMGetFromPLDictionary(dict, k);

	WMReleasePropList(k);
	if (v && WMIsPLString(v))
		return WMGetFromPLString(v);
	return NULL;
}

static int get_int(WMPropList *dict, const char *key, int def)
{
	const char *s = get_string(dict, key);

	return s ? atoi(s) : def;
}

static int get_bool(WMPropList *dict, const char *key)
{
	const char *s = get_string(dict, key);

	return s && (strcasecmp(s, "yes") == 0 || strcmp(s, "1") == 0);
}

static WMPropList *get_dict(WMPropList *dict, const char *key)
{
	WMPropList *k = WMCreatePLString(key);
	WMPropList *v = WMGetFromPLDictionary(dict, k);

	WMReleasePropList(k);
	if (v && WMIsPLDictionary(v))
		return v;
	return NULL;
}

/* mkdir -p for the directory part of path */
static int make_parent_dirs(const char *path)
{
	char *dir = wstrdup(path);
	char *p;

	for (p = dir + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = 0;
		if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
			wfree(dir);
			return 0;
		}
		*p = '/';
	}
	wfree(dir);
	return 1;
}

static void set_error(char **errmsg, const char *what, const char *path)
{
	char buf[PATH_MAX + 256];

	if (!errmsg)
		return;
	snprintf(buf, sizeof(buf), "%s %s: %s", what, path, strerror(errno));
	*errmsg = wstrdup(buf);
}

int profile_save(const DConfig *cfg, char **errmsg)
{
	char *path = profile_path();
	char *key = profile_key(cfg);
	WMPropList *root, *profiles, *profile, *k;
	char buf[32];
	int i, ok;

	if (errmsg)
		*errmsg = NULL;

	root = WMReadPropListFromFile(path);
	if (!root || !WMIsPLDictionary(root)) {
		if (root)
			WMReleasePropList(root);
		root = WMCreatePLDictionary(NULL, NULL);
	}

	profiles = get_dict(root, "Profiles");
	if (!profiles) {
		profiles = WMCreatePLDictionary(NULL, NULL);
		k = WMCreatePLString("Profiles");
		WMPutInPLDictionary(root, k, profiles);
		WMReleasePropList(k);
		WMReleasePropList(profiles);
	}

	profile = WMCreatePLDictionary(NULL, NULL);
	for (i = 0; i < cfg->count; i++) {
		const DOutput *o = &cfg->outputs[i];
		WMPropList *out;

		if (!o->connected)
			continue;
		out = WMCreatePLDictionary(NULL, NULL);
		put_string(out, "Enabled", o->enabled && o->mode >= 0 ? "Yes" : "No");
		if (o->enabled && o->mode >= 0) {
			put_int(out, "Width", o->modes[o->mode].width);
			put_int(out, "Height", o->modes[o->mode].height);
			snprintf(buf, sizeof(buf), "%.2f", o->modes[o->mode].rate);
			put_string(out, "Rate", buf);
			put_int(out, "X", o->x);
			put_int(out, "Y", o->y);
			put_string(out, "Rotation", rotation_names[o->rotation]);
			put_string(out, "Primary", o->primary ? "Yes" : "No");
		}
		k = WMCreatePLString(o->name);
		WMPutInPLDictionary(profile, k, out);
		WMReleasePropList(k);
		WMReleasePropList(out);
	}
	k = WMCreatePLString(key);
	WMPutInPLDictionary(profiles, k, profile);
	WMReleasePropList(k);
	WMReleasePropList(profile);

	ok = make_parent_dirs(path) && WMWritePropListToFile(root, path);
	if (!ok)
		set_error(errmsg, "Could not write", path);

	WMReleasePropList(root);
	wfree(key);
	wfree(path);
	return ok;
}

int profile_load(DConfig *cfg)
{
	char *path = profile_path();
	char *key = profile_key(cfg);
	WMPropList *root, *profiles, *profile;
	int i, found = 0;

	root = WMReadPropListFromFile(path);
	wfree(path);
	if (!root) {
		wfree(key);
		return 0;
	}
	if (WMIsPLDictionary(root) && (profiles = get_dict(root, "Profiles")) &&
	    (profile = get_dict(profiles, key))) {
		for (i = 0; i < cfg->count; i++) {
			DOutput *o = &cfg->outputs[i];
			WMPropList *out = get_dict(profile, o->name);
			const char *s;
			int r, mode;

			if (!out)
				continue;
			found = 1;
			if (!get_bool(out, "Enabled")) {
				o->enabled = 0;
				o->primary = 0;
				continue;
			}
			s = get_string(out, "Rate");
			mode = output_find_mode(o, get_int(out, "Width", 0),
						get_int(out, "Height", 0),
						s ? atof(s) : 60.0);
			if (mode < 0)
				mode = output_preferred_mode(o);
			if (mode < 0)
				continue;
			o->enabled = 1;
			o->mode = mode;
			o->x = get_int(out, "X", 0);
			o->y = get_int(out, "Y", 0);
			o->rotation = ROT_NORMAL;
			s = get_string(out, "Rotation");
			for (r = 0; s && r < ROT_COUNT; r++)
				if (strcmp(s, rotation_names[r]) == 0)
					o->rotation = r;
			o->primary = get_bool(out, "Primary");
		}
	}
	WMReleasePropList(root);
	wfree(key);
	return found;
}

/* Quotes s for /bin/sh. */
static char *shell_quote(const char *s)
{
	char *q = wstrdup("'");

	for (; *s; s++) {
		if (*s == '\'') {
			q = wstrappend(q, "'\\''");
		} else {
			char c[2] = { *s, 0 };

			q = wstrappend(q, c);
		}
	}
	return wstrappend(q, "'");
}

static char *self_path(void)
{
	char buf[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);

	if (n <= 0)
		return wstrdup("wdisplay");
	buf[n] = 0;
	return wstrdup(buf);
}

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "r");
	char *data = NULL;
	char buf[4096];
	size_t n, len = 0;

	if (!f)
		return NULL;
	data = wmalloc(1);
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		data = wrealloc(data, len + n + 1);
		memcpy(data + len, buf, n);
		len += n;
	}
	data[len] = 0;
	fclose(f);
	return data;
}

int profile_install_autostart(char **errmsg)
{
	char *path = profile_autostart_path();
	char *self = self_path();
	char *quoted = shell_quote(self);
	char *old = read_file(path);
	char *hook, *new;
	struct stat st;
	FILE *f;
	int ok = 1;

	if (errmsg)
		*errmsg = NULL;

	hook = wstrconcat(HOOK_BEGIN "\nif [ -x ", quoted);
	hook = wstrappend(hook, " ]; then ");
	hook = wstrappend(hook, quoted);
	hook = wstrappend(hook, " --restore; fi\n" HOOK_END "\n");

	if (!old) {
		new = wstrconcat((char *)autostart_header, "\n");
		new = wstrappend(new, hook);
	} else {
		char *begin = strstr(old, HOOK_BEGIN);
		char *end = begin ? strstr(begin, HOOK_END) : NULL;

		if (begin && end) {
			/* replace the existing block in place */
			end += strlen(HOOK_END);
			if (*end == '\n')
				end++;
			new = wmalloc(begin - old + 1);
			memcpy(new, old, begin - old);
			new[begin - old] = 0;
			new = wstrappend(new, hook);
			new = wstrappend(new, end);
		} else {
			new = wstrdup(old);
			if (*new && new[strlen(new) - 1] != '\n')
				new = wstrappend(new, "\n");
			new = wstrappend(new, "\n");
			new = wstrappend(new, hook);
		}
	}

	if (!old || strcmp(old, new) != 0) {
		if (!make_parent_dirs(path) || !(f = fopen(path, "w"))) {
			set_error(errmsg, "Could not write", path);
			ok = 0;
		} else {
			fputs(new, f);
			if (fclose(f) != 0) {
				set_error(errmsg, "Could not write", path);
				ok = 0;
			}
		}
	}
	/* Window Maker only runs the script when it is executable */
	if (ok && stat(path, &st) == 0 && (st.st_mode & S_IXUSR) == 0)
		chmod(path, st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH);

	wfree(path);
	wfree(self);
	wfree(quoted);
	wfree(hook);
	wfree(new);
	if (old)
		wfree(old);
	return ok;
}
