/* SPDX-License-Identifier: GPL-2.0 */
/*
 * One packed fill or copy, same parameter order as the S4 operations
 * in wd24adv_s3.c (PARR / FILL4 / COPYF / COPYB). Those eleven ran on
 * the 755C: docs/s4-live-2026-11-05.txt. Pitch 640. ROP SRC is 0x300.
 * A copy whose destination address is above the source is programmed
 * backwards from the bottom-right pixel, CNTRL1 0x0d00. Anything else
 * is refused here and the caller uses CPU stores.
 */
#ifndef WD24ADV_S4FB_H
#define WD24ADV_S4FB_H

#include "wd24adv_s3.h"

static inline int wd24s4_box_ok(u32 x, u32 y, u32 w, u32 h)
{
	if (!w || !h || w > 640 || h > 480)
		return 0;
	if (x >= 640 || y >= 480 || x + w > 640 || y + h > 480)
		return 0;
	return 1;
}

static inline u32 wd24s4_br(u32 a, u16 w, u16 h)
{
	return a + (u32)(h - 1) * WD24S3_PITCH + (w - 1);
}

/*
 * src and dst stay the top-left, as in the S4 table. A backward copy
 * programs the bottom-right pixel, which is what PARR(BR(...)) does.
 */
static inline void wd24s4_load(struct wd24s3_op *o, u32 s, u32 d,
			       u16 w, u16 h, u16 rop, int fill, u8 fg,
			       u16 cntrl, int back)
{
	unsigned int n = 0;
	u32 ps = back ? wd24s4_br(s, w, h) : s;
	u32 pd = back ? wd24s4_br(d, w, h) : d;

	o->fill = fill;
	o->src = s;
	o->dst = d;
	o->w = w;
	o->h = h;
	o->fg = fg;
	o->cntrl1 = cntrl;
	o->rop = rop;
	o->back = back;
	o->host = 0;
	o->par[n++] = (struct wd24s3_par){ 2, ps & 0xfff };
	o->par[n++] = (struct wd24s3_par){ 3, (ps >> 12) & 0xfff };
	o->par[n++] = (struct wd24s3_par){ 4, pd & 0xfff };
	o->par[n++] = (struct wd24s3_par){ 5, (pd >> 12) & 0xfff };
	o->par[n++] = (struct wd24s3_par){ 8, WD24S3_PITCH };
	o->par[n++] = (struct wd24s3_par){ 6, w };
	o->par[n++] = (struct wd24s3_par){ 7, h };
	o->par[n++] = (struct wd24s3_par){ 0xe, 0xff };
	o->par[n++] = (struct wd24s3_par){ 9, rop };
	o->par[n++] = (struct wd24s3_par){ 1, 0 };
	if (fill)
		o->par[n++] = (struct wd24s3_par){ 0xa, fg };
	o->npar = n;
}

static inline int wd24s4_make_fill(struct wd24s3_op *o, u32 x, u32 y,
				   u32 w, u32 h, u8 color)
{
	u32 a;

	if (!o || !wd24s4_box_ok(x, y, w, h))
		return -22;
	{
		unsigned char *p = (unsigned char *)o;
		unsigned int i;

		for (i = 0; i < sizeof(*o); i++)
			p[i] = 0;
	}
	a = y * WD24S3_PITCH + x;
	wd24s4_load(o, a, a, (u16)w, (u16)h, 0x300, 1, color, WD24S3_FILL, 0);
	return 0;
}

static inline int wd24s4_make_copy(struct wd24s3_op *o, u32 sx, u32 sy,
				   u32 dx, u32 dy, u32 w, u32 h)
{
	u32 s, d;
	int back;

	if (!o || !wd24s4_box_ok(sx, sy, w, h) || !wd24s4_box_ok(dx, dy, w, h))
		return -22;
	{
		unsigned char *p = (unsigned char *)o;
		unsigned int i;

		for (i = 0; i < sizeof(*o); i++)
			p[i] = 0;
	}
	s = sy * WD24S3_PITCH + sx;
	d = dy * WD24S3_PITCH + dx;
	back = d > s;
	wd24s4_load(o, s, d, (u16)w, (u16)h, 0x300, 0, 0,
		    back ? WD24S3_BACKC : WD24S3_COPY, back);
	return 0;
}

#endif
