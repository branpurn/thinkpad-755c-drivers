/* SPDX-License-Identifier: GPL-2.0 */
/*
 * One cursor show, the program wd90c24fb's cursor_test = 1 ran on this
 * 755C. Data book section 8 (wd90c24_hw.c wd_cursor_pattern / _move /
 * _load / _enable). Pattern at 50000h. Address 50000h >> 2 is registers
 * 1=000 2=014. Colours 15 and 0, hotspot 0, position 320,240. Control
 * 820h while shown, 020h to clear the enable bit (two colours + invert
 * stays, which is what wd_cursor_enable(false) writes). The 755C read
 * back 0=820 1=000 2=014 3=00f 4=000 5=000 6=140 7=0f0.
 *
 * The pattern bytes are the unit-test image in tests/unit/test_wd90c24.c:
 * column 0 of row 0 is primary, column 1 is secondary, the rest of that
 * row is transparent, and column 31 of row 31 is primary.
 */
#ifndef WD24ADV_CUR_H
#define WD24ADV_CUR_H

#include "wd24adv_s3.h"

#define WD24CUR_PAT		0x50000u
#define WD24CUR_BYTES		256
#define WD24CUR_HOLD_MS		3000
#define WD24CUR_NSHOW		8
#define WD24CUR_HIDE		0x020u

struct wd24cur_step {
	u8 idx;
	u16 val;
};

/*
 * wd_cursor_move(320, 240) then wd_cursor_load(0x50000, 15, 0, 0, 0, 1).
 * Control is last: the address and origin take effect on that write.
 */
static const struct wd24cur_step wd24cur_show[WD24CUR_NSHOW] = {
	{ 6, 0x140 },
	{ 7, 0x0f0 },
	{ 1, 0x000 },
	{ 2, 0x014 },
	{ 3, 0x00f },
	{ 4, 0x000 },
	{ 5, 0x000 },
	{ 0, 0x820 },
};

/* regs 0..7 after the show, same order as the 755C line */
static const u16 wd24cur_expect[8] = {
	0x820, 0x000, 0x014, 0x00f, 0x000, 0x000, 0x140, 0x0f0
};

static inline void wd24cur_pattern(u8 out[WD24CUR_BYTES])
{
	u8 img[128], msk[128];
	unsigned int r, k, i;

	for (i = 0; i < 128; i++) {
		img[i] = 0;
		msk[i] = 0;
	}
	msk[0] = 0xc0;
	img[0] = 0x80;
	msk[127] = 0x01;
	img[127] = 0x01;
	for (r = 0; r < 32; r++)
		for (k = 0; k < 4; k++) {
			u8 m = msk[r * 4 + k], im = img[r * 4 + k];

			out[r * 8 + 2 * k] = (u8)~m;
			out[r * 8 + 2 * k + 1] = im & m;
		}
}

struct wd24cur_mem {
	int (*read)(void *p, u32 off, u8 *buf, u32 n);
	int (*write)(void *p, u32 off, const u8 *buf, u32 n);
	void (*hold)(void *p);
	void *priv;
};

struct wd24cur_res {
	unsigned int show;	/* 1: the eight regs matched */
	unsigned int regs;	/* how many of regs 0..7 matched */
	unsigned int vram;	/* pattern bytes that read back */
	unsigned int restored;	/* 1: block 2 and the pattern bytes match the save */
	const char *why;
};

int wd24cur_run(struct wd24s3 *s, const struct wd24cur_mem *m,
		struct wd24cur_res *out);

#endif
