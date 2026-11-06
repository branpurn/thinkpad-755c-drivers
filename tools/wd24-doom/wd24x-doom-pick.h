/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Decisions for the Doom backend. The engine is GPL-2 and is fetched,
 * not stored in this tree. See tools/wd24-doom/README.
 */
#ifndef WD24X_DOOM_PICK_H
#define WD24X_DOOM_PICK_H

#include <string.h>

/* Only the exact string opens /dev/fb0. Anything else stays on the file path. */
static inline int wd24x_doom_fb_opt_in(const char *v)
{
	return v && strcmp(v, "mmap") == 0;
}

/* Palette programming is not in this backend. The flag refuses the run. */
static inline int wd24x_doom_palette_refused(const char *v)
{
	return v && strcmp(v, "1") == 0;
}

/* 32-bit pixel, blue in the low byte, as i_video.c stores rgba8888. */
static inline unsigned wd24x_doom_luma(unsigned pix)
{
	unsigned b = pix & 255u;
	unsigned g = (pix >> 8) & 255u;
	unsigned r = (pix >> 16) & 255u;

	return (r * 77u + g * 150u + b * 29u) >> 8;
}

#endif
