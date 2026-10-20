/*
 * wdisplay - monitor configuration for Window Maker
 *
 * display.c: RandR queries and applying a configuration through xrandr.
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/Xatom.h>

#include "display.h"

const char *rotation_names[ROT_COUNT] = { "normal", "left", "inverted", "right" };

static const Rotation rotation_bits[ROT_COUNT] = {
	RR_Rotate_0, RR_Rotate_90, RR_Rotate_180, RR_Rotate_270
};

static void *xmalloc(size_t size)
{
	void *p = calloc(1, size ? size : 1);

	if (!p) {
		perror("wdisplay");
		exit(1);
	}
	return p;
}

static double mode_rate(const XRRModeInfo *m)
{
	double v = m->vTotal;

	if (m->modeFlags & RR_DoubleScan)
		v *= 2;
	if (m->modeFlags & RR_Interlace)
		v /= 2;
	if (m->hTotal == 0 || v == 0)
		return 0;
	return (double)m->dotClock / ((double)m->hTotal * v);
}

/* Extracts the monitor name (or at least the vendor) from the EDID block. */
static void read_edid_name(Display *dpy, RROutput output, char *buf, size_t size)
{
	Atom edid_atom = XInternAtom(dpy, RR_PROPERTY_RANDR_EDID, True);
	unsigned char *data = NULL;
	unsigned long nitems = 0, after;
	Atom type;
	int format, i;

	buf[0] = 0;
	if (edid_atom == None)
		return;
	if (XRRGetOutputProperty(dpy, output, edid_atom, 0, 128, False, False,
				 AnyPropertyType, &type, &format, &nitems, &after,
				 &data) != Success || !data)
		return;

	if (format == 8 && nitems >= 128) {
		for (i = 54; i <= 108; i += 18) {
			const unsigned char *d = data + i;

			if (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0xfc) {
				int j, n = 0;

				for (j = 5; j < 18 && d[j] != 0x0a && n + 1 < (int)size; j++)
					buf[n++] = d[j];
				while (n > 0 && buf[n - 1] == ' ')
					n--;
				buf[n] = 0;
				break;
			}
		}
		if (!buf[0] && size >= 4) {
			/* fall back to the three letter manufacturer id */
			unsigned id = (data[8] << 8) | data[9];

			buf[0] = '@' + ((id >> 10) & 0x1f);
			buf[1] = '@' + ((id >> 5) & 0x1f);
			buf[2] = '@' + (id & 0x1f);
			buf[3] = 0;
		}
	}
	XFree(data);
}

static int rotation_index(Rotation r)
{
	int i;

	for (i = 0; i < ROT_COUNT; i++)
		if (r & rotation_bits[i])
			return i;
	return ROT_NORMAL;
}

DConfig *display_query(Display *dpy)
{
	Window root = DefaultRootWindow(dpy);
	XRRScreenResources *res;
	RROutput primary;
	DConfig *cfg;
	int i, j, k, pass;
	int event_base, error_base;

	if (!XRRQueryExtension(dpy, &event_base, &error_base))
		return NULL;

	res = XRRGetScreenResources(dpy, root);
	if (!res)
		return NULL;
	primary = XRRGetOutputPrimary(dpy, root);

	cfg = xmalloc(sizeof(DConfig));
	cfg->outputs = xmalloc(sizeof(DOutput) * (res->noutput + 1));

	/* connected outputs first, then ones that are lit but disconnected */
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < res->noutput; i++) {
			XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
			DOutput *o;

			if (!oi)
				continue;
			if ((pass == 0 && oi->connection != RR_Connected) ||
			    (pass == 1 && (oi->connection == RR_Connected || !oi->crtc))) {
				XRRFreeOutputInfo(oi);
				continue;
			}

			o = &cfg->outputs[cfg->count++];
			snprintf(o->name, sizeof(o->name), "%s", oi->name);
			o->id = res->outputs[i];
			o->connected = oi->connection == RR_Connected;
			o->primary = res->outputs[i] == primary;
			o->mode = -1;
			o->rotations = RR_Rotate_0;
			read_edid_name(dpy, o->id, o->monitor, sizeof(o->monitor));

			o->modes = xmalloc(sizeof(DMode) * (oi->nmode + 1));
			for (j = 0; j < oi->nmode; j++) {
				for (k = 0; k < res->nmode; k++) {
					XRRModeInfo *mi = &res->modes[k];
					DMode *m;

					if (mi->id != oi->modes[j])
						continue;
					m = &o->modes[o->nmodes++];
					m->id = mi->id;
					m->width = mi->width;
					m->height = mi->height;
					m->rate = mode_rate(mi);
					m->interlaced = (mi->modeFlags & RR_Interlace) != 0;
					m->preferred = j < oi->npreferred;
					break;
				}
			}

			if (oi->crtc) {
				XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtc);

				if (ci) {
					for (j = 0; j < o->nmodes; j++)
						if (o->modes[j].id == ci->mode)
							o->mode = j;
					o->enabled = o->mode >= 0;
					o->x = ci->x;
					o->y = ci->y;
					o->rotation = rotation_index(ci->rotation);
					o->rotations = ci->rotations;
					XRRFreeCrtcInfo(ci);
				}
			} else if (oi->ncrtc > 0) {
				XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtcs[0]);

				if (ci) {
					o->rotations = ci->rotations;
					XRRFreeCrtcInfo(ci);
				}
			}
			XRRFreeOutputInfo(oi);
		}
	}
	XRRFreeScreenResources(res);
	return cfg;
}

static void fake_modes(DOutput *o, const int (*list)[3], int n)
{
	int i;

	o->modes = xmalloc(sizeof(DMode) * n);
	o->nmodes = n;
	for (i = 0; i < n; i++) {
		o->modes[i].id = 0x40 + i + (o->id << 4);
		o->modes[i].width = list[i][0];
		o->modes[i].height = list[i][1];
		o->modes[i].rate = list[i][2] / 100.0;
		o->modes[i].preferred = i == 0;
	}
}

DConfig *display_fake(void)
{
	static const int modes_a[][3] = {
		{ 2560, 1440, 14396 }, { 2560, 1440, 11999 }, { 2560, 1440, 5995 },
		{ 1920, 1080, 14400 }, { 1920, 1080, 6000 }, { 1920, 1080, 5994 },
		{ 1280, 720, 6000 }, { 1024, 768, 6000 }, { 800, 600, 6032 },
	};
	static const int modes_b[][3] = {
		{ 1920, 1080, 6000 }, { 1920, 1080, 5994 }, { 1920, 1080, 5000 },
		{ 1680, 1050, 5995 }, { 1280, 1024, 7502 }, { 1280, 1024, 6002 },
		{ 1280, 720, 6000 }, { 1024, 768, 6000 },
	};
	static const int modes_c[][3] = {
		{ 1366, 768, 6000 }, { 1280, 720, 6000 }, { 1024, 768, 6000 },
	};
	DConfig *cfg = xmalloc(sizeof(DConfig));
	DOutput *o;

	cfg->fake = 1;
	cfg->outputs = xmalloc(sizeof(DOutput) * 3);
	cfg->count = 3;

	o = &cfg->outputs[0];
	strcpy(o->name, "DP-1");
	strcpy(o->monitor, "DELL S2721DGF");
	o->id = 1;
	fake_modes(o, modes_a, sizeof(modes_a) / sizeof(modes_a[0]));
	o->connected = o->enabled = o->primary = 1;
	o->rotations = 0x0f;
	o->mode = 0;
	o->x = 1920;

	o = &cfg->outputs[1];
	strcpy(o->name, "HDMI-1");
	strcpy(o->monitor, "LG FULL HD");
	o->id = 2;
	fake_modes(o, modes_b, sizeof(modes_b) / sizeof(modes_b[0]));
	o->connected = o->enabled = 1;
	o->rotations = 0x0f;
	o->mode = 0;
	o->y = 180;

	o = &cfg->outputs[2];
	strcpy(o->name, "eDP-1");
	strcpy(o->monitor, "");
	o->id = 3;
	fake_modes(o, modes_c, sizeof(modes_c) / sizeof(modes_c[0]));
	o->connected = 1;
	o->rotations = 0x0f;
	o->mode = -1;

	return cfg;
}

DConfig *display_copy(const DConfig *cfg)
{
	DConfig *c = xmalloc(sizeof(DConfig));
	int i;

	*c = *cfg;
	c->outputs = xmalloc(sizeof(DOutput) * (cfg->count + 1));
	for (i = 0; i < cfg->count; i++) {
		c->outputs[i] = cfg->outputs[i];
		c->outputs[i].modes = xmalloc(sizeof(DMode) * (cfg->outputs[i].nmodes + 1));
		memcpy(c->outputs[i].modes, cfg->outputs[i].modes,
		       sizeof(DMode) * cfg->outputs[i].nmodes);
	}
	return c;
}

void display_free(DConfig *cfg)
{
	int i;

	if (!cfg)
		return;
	for (i = 0; i < cfg->count; i++)
		free(cfg->outputs[i].modes);
	free(cfg->outputs);
	free(cfg);
}

int output_width(const DOutput *o)
{
	if (o->mode < 0)
		return 0;
	if (o->rotation == ROT_LEFT || o->rotation == ROT_RIGHT)
		return o->modes[o->mode].height;
	return o->modes[o->mode].width;
}

int output_height(const DOutput *o)
{
	if (o->mode < 0)
		return 0;
	if (o->rotation == ROT_LEFT || o->rotation == ROT_RIGHT)
		return o->modes[o->mode].width;
	return o->modes[o->mode].height;
}

int output_find_mode(const DOutput *o, int width, int height, double rate)
{
	int i, best = -1;
	double best_diff = 0;

	for (i = 0; i < o->nmodes; i++) {
		double diff;

		if (o->modes[i].width != width || o->modes[i].height != height)
			continue;
		diff = fabs(o->modes[i].rate - rate);
		if (best < 0 || diff < best_diff) {
			best = i;
			best_diff = diff;
		}
	}
	return best;
}

int output_preferred_mode(const DOutput *o)
{
	int i, best = -1;

	for (i = 0; i < o->nmodes; i++)
		if (o->modes[i].preferred)
			return i;
	/* no preference: take the biggest one */
	for (i = 0; i < o->nmodes; i++) {
		if (best < 0 ||
		    (long)o->modes[i].width * o->modes[i].height >
		    (long)o->modes[best].width * o->modes[best].height)
			best = i;
	}
	return best;
}

int display_enabled_count(const DConfig *cfg)
{
	int i, n = 0;

	for (i = 0; i < cfg->count; i++)
		if (cfg->outputs[i].enabled && cfg->outputs[i].mode >= 0)
			n++;
	return n;
}

char **display_build_argv(const DConfig *cfg)
{
	char **argv = xmalloc(sizeof(char *) * (cfg->count * 10 + 4));
	int i, argc = 0, have_primary = 0;
	char buf[64];

	argv[argc++] = strdup("xrandr");

	for (i = 0; i < cfg->count; i++)
		if (cfg->outputs[i].enabled && cfg->outputs[i].primary)
			have_primary = 1;
	if (!have_primary)
		argv[argc++] = strdup("--noprimary");

	for (i = 0; i < cfg->count; i++) {
		const DOutput *o = &cfg->outputs[i];

		argv[argc++] = strdup("--output");
		argv[argc++] = strdup(o->name);
		if (o->enabled && o->mode >= 0) {
			argv[argc++] = strdup("--mode");
			snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)o->modes[o->mode].id);
			argv[argc++] = strdup(buf);
			argv[argc++] = strdup("--pos");
			snprintf(buf, sizeof(buf), "%dx%d", o->x, o->y);
			argv[argc++] = strdup(buf);
			argv[argc++] = strdup("--rotate");
			argv[argc++] = strdup(rotation_names[o->rotation]);
			if (o->primary)
				argv[argc++] = strdup("--primary");
		} else {
			argv[argc++] = strdup("--off");
		}
	}
	argv[argc] = NULL;
	return argv;
}

void display_free_argv(char **argv)
{
	int i;

	for (i = 0; argv[i]; i++)
		free(argv[i]);
	free(argv);
}

char *display_argv_string(char **argv)
{
	size_t len = 1;
	char *s;
	int i;

	for (i = 0; argv[i]; i++)
		len += strlen(argv[i]) + 1;
	s = xmalloc(len);
	for (i = 0; argv[i]; i++) {
		if (i)
			strcat(s, " ");
		strcat(s, argv[i]);
	}
	return s;
}

int display_apply(const DConfig *cfg, char **errmsg)
{
	char **argv = display_build_argv(cfg);
	char output[4096];
	size_t len = 0;
	int fds[2], status, ok;
	pid_t pid;
	ssize_t n;

	if (errmsg)
		*errmsg = NULL;

	if (cfg->fake || getenv("WDISPLAY_DRY_RUN")) {
		char *cmd = display_argv_string(argv);

		printf("%s\n", cmd);
		fflush(stdout);
		free(cmd);
		display_free_argv(argv);
		return 1;
	}

	if (pipe(fds) < 0) {
		if (errmsg)
			*errmsg = strdup(strerror(errno));
		display_free_argv(argv);
		return 0;
	}

	pid = fork();
	if (pid < 0) {
		if (errmsg)
			*errmsg = strdup(strerror(errno));
		close(fds[0]);
		close(fds[1]);
		display_free_argv(argv);
		return 0;
	}
	if (pid == 0) {
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		dup2(fds[1], STDERR_FILENO);
		close(fds[1]);
		execvp(argv[0], argv);
		fprintf(stderr, "could not run xrandr: %s\n", strerror(errno));
		_exit(127);
	}

	close(fds[1]);
	while ((n = read(fds[0], output + len, sizeof(output) - 1 - len)) > 0
	       || (n < 0 && errno == EINTR))
		if (n > 0)
			len += n;
	output[len] = 0;
	close(fds[0]);

	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

	if (!ok && errmsg) {
		while (len > 0 && (output[len - 1] == '\n' || output[len - 1] == ' '))
			output[--len] = 0;
		*errmsg = strdup(len ? output : "xrandr failed");
	}
	display_free_argv(argv);
	return ok;
}
