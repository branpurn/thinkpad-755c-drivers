/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Letterbox present for 320x200 indices onto the 640x480x8 frame.
 *
 * The fault path counts every new 64 KiB window as a consecutive
 * switch. A forward sweep never faults twice inside one window, so
 * the counter does not reset, and the bound of 32 latches after a
 * handful of frames. This planner does not call that counter. It
 * names each window once. The kernel programs that window once and
 * stores 32-bit groups. The bound stays 32.
 */
#ifndef WD24ADV_PRESENT_H
#define WD24ADV_PRESENT_H

#include "wd24adv_s6d_bank.h"
#include "wd24adv_s6d_thrash.h"

#define WD24_PRESENT_SRC_W	320u
#define WD24_PRESENT_SRC_H	200u
#define WD24_PRESENT_SRC	(WD24_PRESENT_SRC_W * WD24_PRESENT_SRC_H)
#define WD24_PRESENT_DST_W	640u
#define WD24_PRESENT_FRAME	0x4b000u
#define WD24_PRESENT_LETTER_Y	40u
#define WD24_PRESENT_WINS	8u

/* Jiffy span as ns. HZ is the kernel tick. 100 and 250 divide 1e9. */
static inline unsigned long long wd24_present_ns(unsigned long begin,
						 unsigned long end,
						 unsigned long hz)
{
	unsigned long d = end - begin;

	if (!hz)
		return 0;
	return (unsigned long long)d * (1000000000ul / hz);
}

struct wd24_blit_check {
	u32 src_w;
	u32 src_h;
	u32 dst_x;
	u32 dst_y;
	u32 scale;
	u32 op;
	u32 nbytes;
	u32 have_pixels;
};

/* 0, or -22. No hardware. LETTER2X is the only op. */
static inline int wd24_blit_allow(const struct wd24_blit_check *r)
{
	if (!r || !r->have_pixels)
		return -22;
	if (r->op != 0)
		return -22;
	if (r->src_w != WD24_PRESENT_SRC_W || r->src_h != WD24_PRESENT_SRC_H)
		return -22;
	if (r->dst_x != 0 || r->dst_y != 0 || r->scale != 2)
		return -22;
	if (r->nbytes != WD24_PRESENT_SRC)
		return -22;
	if (r->src_w * r->src_h != r->nbytes)
		return -22;
	return 0;
}

struct wd24_present_win {
	u32 start;
	u32 end;
	u32 base;
	u32 w0;
	u32 w1;
};

static inline u8 wd24_present_letter_pix(const u8 *src, u32 off)
{
	u32 y, x, sy, sx;

	if (!src || off >= WD24_PRESENT_FRAME)
		return 0;
	y = off / WD24_PRESENT_DST_W;
	x = off % WD24_PRESENT_DST_W;
	if (y < WD24_PRESENT_LETTER_Y ||
	    y >= WD24_PRESENT_LETTER_Y + WD24_PRESENT_SRC_H * 2u)
		return 0;
	sy = (y - WD24_PRESENT_LETTER_Y) / 2u;
	sx = x / 2u;
	return src[sy * WD24_PRESENT_SRC_W + sx];
}

/* Little-endian byte order: p0 at the lowest address. */
static inline u32 wd24_present_letter_dword(const u8 *src, u32 off)
{
	u32 p0 = wd24_present_letter_pix(src, off);
	u32 p1 = wd24_present_letter_pix(src, off + 1u);
	u32 p2 = wd24_present_letter_pix(src, off + 2u);
	u32 p3 = wd24_present_letter_pix(src, off + 3u);

	return p0 | (p1 << 8) | (p2 << 16) | (p3 << 24);
}

/* One entry per 64 KiB window that the frame touches. Returns the count. */
static inline unsigned wd24_present_wins(struct wd24_present_win *w, unsigned cap)
{
	unsigned n = 0;
	u32 off = 0;

	if (!w)
		return 0;
	while (off < WD24_PRESENT_FRAME && n < cap) {
		u32 start = (off / WD24S6D_WIN) * WD24S6D_WIN;
		u32 end = start + WD24S6D_WIN;

		if (end > WD24_PRESENT_FRAME)
			end = WD24_PRESENT_FRAME;
		w[n].start = start;
		w[n].end = end;
		w[n].base = wd24s6d_win_base_pg(start);
		wd24s6d_win_for_off(start, &w[n].w0, &w[n].w1);
		n++;
		off = end;
	}
	return n;
}

static inline void wd24_present_fill(u8 *dst, const u8 *src)
{
	u32 off;

	if (!dst || !src)
		return;
	for (off = 0; off < WD24_PRESENT_FRAME; off++)
		dst[off] = wd24_present_letter_pix(src, off);
}

/*
 * What the mmap fault path does with one fault per window and no
 * same-window fault between them. Returns the 0-based frame on which
 * the latch fires, or -1 if it does not. *faults counts notes that
 * were allowed. The latching note is not a successful fault.
 */
static inline int wd24_present_mmap_latch(unsigned frames, u32 bound,
					  u32 *faults, u32 *at_switch)
{
	struct wd24s6d_thrash t;
	struct wd24_present_win w[WD24_PRESENT_WINS];
	unsigned n, f, i;

	n = wd24_present_wins(w, WD24_PRESENT_WINS);
	wd24s6d_thrash_init(&t, bound ? bound : WD24S6D_THRASH_BOUND_DEFAULT);
	if (faults)
		*faults = 0;
	for (f = 0; f < frames; f++) {
		for (i = 0; i < n; i++) {
			if (wd24s6d_thrash_note(&t, w[i].base)) {
				if (at_switch)
					*at_switch = t.switches;
				return (int)f;
			}
			if (faults)
				(*faults)++;
		}
	}
	if (at_switch)
		*at_switch = t.switches;
	return -1;
}

#endif
