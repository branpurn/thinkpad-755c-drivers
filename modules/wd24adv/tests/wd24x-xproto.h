/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Enough of the X11 setup and QueryColors replies to compare X's
 * colormap with the driver's published cache. Little-endian ('l').
 * Channels are reduced with >> 10, the same shift the DAC stores.
 * Low 10 bits are counted and are not a mismatch: the RAMDAC has 6.
 */
#ifndef WD24X_XPROTO_H
#define WD24X_XPROTO_H

#include <stdint.h>
#include <string.h>

static inline uint16_t wd24x_xp_get16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t wd24x_xp_get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wd24x_xp_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void wd24x_xp_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* Setup body (after the 8-byte prefix). One screen, 8-bit format. */
static inline int wd24x_xsetup_screen(const uint8_t *body, unsigned blen,
				      uint32_t *root, uint32_t *cmap,
				      uint16_t *width, uint16_t *height,
				      uint8_t *depth)
{
	unsigned vlen, nfmt, off, i;

	if (!body || blen < 40 || !root || !cmap || !width || !height || !depth)
		return -1;
	vlen = wd24x_xp_get16(body + 16);
	nfmt = body[21];
	off = 32 + ((vlen + 3u) & ~3u);
	for (i = 0; i < nfmt; i++) {
		if (off + 8 > blen)
			return -1;
		off += 8;
	}
	if (off + 40 > blen)
		return -1;
	*root = wd24x_xp_get32(body + off);
	*cmap = wd24x_xp_get32(body + off + 4);
	*width = wd24x_xp_get16(body + off + 20);
	*height = wd24x_xp_get16(body + off + 22);
	*depth = body[off + 38];
	return 0;
}

/*
 * QueryColors request. The protocol has no count field: after the
 * colormap, each pixel is a CARD32. A count stored at byte 8 is read
 * as pixel 0. For n = 256 that value is 256, and a 256-entry map
 * returns BadValue (error 2).
 */
static inline int wd24x_xquery_colors(uint8_t *dst, unsigned cap,
				      uint32_t cmap, unsigned n)
{
	unsigned i, bytes;

	if (!dst || n == 0 || n > 256)
		return -1;
	bytes = 8u + n * 4u;
	if (bytes > cap)
		return -1;
	memset(dst, 0, bytes);
	dst[0] = 91;
	wd24x_xp_put16(dst + 2, (uint16_t)(bytes / 4u));
	wd24x_xp_put32(dst + 4, cmap);
	for (i = 0; i < n; i++)
		wd24x_xp_put32(dst + 8 + i * 4, i);
	return (int)bytes;
}

/* What the server does with the request. 0 if every pixel is in range. */
static inline int wd24x_xquery_entries(const uint8_t *req, unsigned len,
				       unsigned entries, unsigned *npix,
				       uint32_t *first, uint32_t *bad)
{
	unsigned n, i;
	uint16_t words;

	if (!req || !npix || !first || !bad || len < 8 || req[0] != 91)
		return -1;
	words = wd24x_xp_get16(req + 2);
	if ((unsigned)words * 4u > len || words < 2)
		return -1;
	n = ((unsigned)words * 4u - 8u) / 4u;
	*npix = n;
	*first = wd24x_xp_get32(req + 8);
	*bad = 0;
	for (i = 0; i < n; i++) {
		uint32_t px = wd24x_xp_get32(req + 8 + i * 4);

		if (px >= entries) {
			*bad = px;
			return -1;
		}
	}
	return 0;
}

/*
 * QueryColors reply. r/g/b receive the >> 10 values.
 * *lowbits counts channels whose low 10 bits were set.
 */
static inline int wd24x_xcolors_parse(const uint8_t *rep, unsigned len,
				      unsigned expect, unsigned *r,
				      unsigned *g, unsigned *b,
				      unsigned *lowbits)
{
	unsigned n, i, nbytes;

	if (!rep || !r || !g || !b || !lowbits || len < 32 || rep[0] != 1)
		return -1;
	n = wd24x_xp_get16(rep + 8);
	if (n != expect)
		return -1;
	nbytes = wd24x_xp_get32(rep + 4) * 4u;
	if (nbytes < n * 8u || 32u + n * 8u > len)
		return -1;
	*lowbits = 0;
	for (i = 0; i < n; i++) {
		const uint8_t *p = rep + 32 + i * 8;
		unsigned rr = wd24x_xp_get16(p);
		unsigned gg = wd24x_xp_get16(p + 2);
		unsigned bb = wd24x_xp_get16(p + 4);

		if ((rr | gg | bb) & 0x03ffu)
			(*lowbits)++;
		r[i] = rr >> 10;
		g[i] = gg >> 10;
		b[i] = bb >> 10;
		if (r[i] > 63 || g[i] > 63 || b[i] > 63)
			return -1;
	}
	return 0;
}

#endif
