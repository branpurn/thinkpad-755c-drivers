/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Opt-in for opening /dev/fb0. Anything other than the exact string
 * stays on the private buffer. Palette encoding and the 640x480
 * present are in wd24x-doom-present.h.
 */
#ifndef WD24X_DOOM_PICK_H
#define WD24X_DOOM_PICK_H

#include <string.h>

static inline int wd24x_doom_fb_is_mmap(const char *v)
{
	return v && strcmp(v, "mmap") == 0;
}

static inline int wd24x_doom_fb_is_ioctl(const char *v)
{
	return v && strcmp(v, "ioctl") == 0;
}

/* mmap or ioctl. Anything else stays on the private buffer. */
static inline int wd24x_doom_fb_opt_in(const char *v)
{
	return wd24x_doom_fb_is_mmap(v) || wd24x_doom_fb_is_ioctl(v);
}

static inline int wd24x_doom_input_opt_in(const char *v)
{
	return v && strcmp(v, "tty") == 0;
}

#endif
