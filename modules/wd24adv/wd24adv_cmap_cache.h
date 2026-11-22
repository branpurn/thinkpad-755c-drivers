/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Text form of the published fb_cmap cache.
 *
 * FBIOGETCMAP copies this cache. It does not read the DAC. A second
 * open of /dev/fb0 while X is mapped is -EBUSY, and that close would
 * drop X's PTEs, so the desktop check reads this text instead. The
 * bytes exist only after publish, or after fb_setcmap returned 0,
 * and that return is the 3C9 readback. No port is touched here.
 */
#ifndef WD24ADV_CMAP_CACHE_H
#define WD24ADV_CMAP_CACHE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint16_t u16;
#endif

#define WD24_CMAP_ABSENT	"WD24X-CMAP-CACHE absent\n"
#define WD24_CMAP_BUSY		"WD24X-CMAP-CACHE busy\n"
#define WD24_CMAP_BAD		"WD24X-CMAP-CACHE bad\n"

static inline unsigned int wd24_cc_put(char *d, unsigned int cap, unsigned int n,
				   const char *s)
{
	while (*s) {
		if (n + 1 >= cap)
			return cap;
		d[n++] = *s++;
	}
	if (n < cap)
		d[n] = 0;
	return n;
}

static inline unsigned int wd24_cc_u(char *d, unsigned int cap, unsigned int n, unsigned int v)
{
	char tmp[4];
	unsigned int k = 0;

	if (v > 255)
		return cap;
	if (v >= 100)
		tmp[k++] = (char)('0' + v / 100);
	if (v >= 10)
		tmp[k++] = (char)('0' + (v / 10) % 10);
	tmp[k++] = (char)('0' + v % 10);
	tmp[k] = 0;
	return wd24_cc_put(d, cap, n, tmp);
}

/*
 * 0 and a byte count, or -1 if the buffer is short.
 * The printed byte is the 6-bit DAC value, channel >> 10.
 * fbcon publishes v<<10. X's FBIOPUTCMAP publishes v*65535/63
 * (fbdev.c, red.length 6), which sets the low 10 bits. Both
 * shift back to v. A u16 shifted by 10 is already 0..63.
 */
static inline int wd24_cmap_cache_render(char *dst, unsigned int cap,
					 const u16 *r, const u16 *g,
					 const u16 *b)
{
	unsigned int i, n;

	if (!dst || cap < 64 || !r || !g || !b)
		return -1;
	n = wd24_cc_put(dst, cap, 0, "WD24X-CMAP-CACHE entries 256 shift10 1\n");
	if (n >= cap)
		return -1;
	for (i = 0; i < 256; i++) {
		unsigned int rr, gg, bb;

		rr = (unsigned int)(r[i] >> 10);
		gg = (unsigned int)(g[i] >> 10);
		bb = (unsigned int)(b[i] >> 10);
		if (rr > 63 || gg > 63 || bb > 63)
			return -1;
		n = wd24_cc_u(dst, cap, n, i);
		n = wd24_cc_put(dst, cap, n, " ");
		n = wd24_cc_u(dst, cap, n, rr);
		n = wd24_cc_put(dst, cap, n, " ");
		n = wd24_cc_u(dst, cap, n, gg);
		n = wd24_cc_put(dst, cap, n, " ");
		n = wd24_cc_u(dst, cap, n, bb);
		n = wd24_cc_put(dst, cap, n, "\n");
		if (n >= cap)
			return -1;
	}
	return (int)n;
}

#endif
