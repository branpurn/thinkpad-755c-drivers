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

/* 13h chain-4 window. Not the 640x480 fault map. */
static inline int wd24x_doom_fb_is_aperture(const char *v)
{
	return v && strcmp(v, "aperture") == 0;
}

/* Render in RAM, then one dword copy into the 13h window. */
static inline int wd24x_doom_fb_is_aperture_copy(const char *v)
{
	return v && strcmp(v, "aperturecopy") == 0;
}

/* mmap, ioctl, or a 13h window. Anything else stays private. */
static inline int wd24x_doom_fb_opt_in(const char *v)
{
	return wd24x_doom_fb_is_mmap(v) || wd24x_doom_fb_is_ioctl(v) ||
	       wd24x_doom_fb_is_aperture(v) ||
	       wd24x_doom_fb_is_aperture_copy(v);
}

static inline int wd24x_doom_input_opt_in(const char *v)
{
	return v && strcmp(v, "tty") == 0;
}

#endif
