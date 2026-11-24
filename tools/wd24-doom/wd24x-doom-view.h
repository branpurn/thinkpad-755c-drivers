/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Opt-in view size. Unset leaves the engine defaults: detailLevel 0
 * and screenblocks 10 (320 by 168 above the status bar). LOW=1 and
 * BLOCKS=3..11 change the picture.
 */
#ifndef WD24X_DOOM_VIEW_H
#define WD24X_DOOM_VIEW_H

struct wd24x_doom_view {
	int low;		/* 0 or 1 */
	int blocks;		/* 0 = engine default, else 3..11 */
};

static inline unsigned wd24x_doom_view_pixels(int blocks)
{
	unsigned int w, h;

	if (blocks >= 11)
		return 320u * 200u;
	if (blocks < 3)
		blocks = 10;
	w = (unsigned int)blocks * 32u;
	h = ((unsigned int)blocks * 168u / 10u) & ~7u;
	return w * h;
}

/* 0 accepted. Empty strings are the defaults. */
static inline int wd24x_doom_view_parse(const char *low_s, const char *blocks_s,
					struct wd24x_doom_view *o)
{
	if (!o)
		return -1;
	o->low = 0;
	o->blocks = 0;
	if (low_s && low_s[0]) {
		if (low_s[0] == '0' && !low_s[1])
			o->low = 0;
		else if (low_s[0] == '1' && !low_s[1])
			o->low = 1;
		else
			return -1;
	}
	if (blocks_s && blocks_s[0]) {
		int n = 0;
		const char *p = blocks_s;

		if (*p < '0' || *p > '9')
			return -1;
		while (*p >= '0' && *p <= '9') {
			n = n * 10 + (*p - '0');
			if (n > 11)
				return -1;
			p++;
		}
		if (*p || n < 3)
			return -1;
		o->blocks = n;
	}
	return 0;
}

#endif
