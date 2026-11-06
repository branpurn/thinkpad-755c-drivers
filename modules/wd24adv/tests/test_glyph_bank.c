/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Offline bound for the glyph cells. Uses the driver's window and thrash
 * helpers. Not a kernel run and not the 755C.
 *
 * shadowUpdatePacked walks one damage box from top to bottom. These five
 * boxes do not share a row, so a region keeps them separate. Each box is
 * inside one 64 KiB window. A forward walk of that box switches at most
 * once, at the boundary with the previous box, then stays. The latch is 32.
 */
#include <stdio.h>
#include <string.h>
#include "../wd24adv_s6d_bank.h"
#include "../wd24adv_s6d_thrash.h"
#include "wd24x-glyph-bits.h"

static int fails, passes;

#define T(cond, what) do { \
	if (cond) \
		passes++; \
	else { \
		fails++; \
		printf("FAIL %s (line %d)\n", what, __LINE__); \
	} \
} while (0)

int main(void)
{
	struct wd24s6d_thrash th;
	int n, row, col, prev_win = -1;
	unsigned max_run = 0;
	unsigned bytes = 0;
	unsigned ink[WD24X_GLYPH_N];

	printf("test_glyph_bank: window math only, not the panel\n");
	memset(ink, 0, sizeof ink);
	wd24s6d_thrash_init(&th, WD24S6D_THRASH_BOUND_DEFAULT);
	T(th.bound == 32, "bound stays 32");

	for (n = 0; n < WD24X_GLYPH_N; n++) {
		uint16_t x = wd24x_glyph_xy[n][0];
		uint16_t y = wd24x_glyph_xy[n][1];
		uint32_t first = (uint32_t)y * WD24X_GLYPH_PITCH + x;
		uint32_t win = wd24s6d_win_base_pg(first);
		int y2;

		T(x + WD24X_GLYPH_W <= WD24X_GLYPH_PITCH, "x inside the line");
		T(y + WD24X_GLYPH_H <= 480, "y inside the frame");
		for (row = 0; row < WD24X_GLYPH_H; row++) {
			for (col = 0; col < WD24X_GLYPH_W; col++) {
				uint32_t off = ((uint32_t)y + (uint32_t)row) *
					       WD24X_GLYPH_PITCH + x + (uint32_t)col;

				T(!wd24s6d_livelock_span(first, off),
				  "cell does not cross a 64 KiB window");
				T(wd24s6d_win_base_pg(off) == win, "same window");
				if (wd24s6d_thrash_note(&th, wd24s6d_win_base_pg(off)))
					T(0, "thrash note refused");
				if (th.switches > max_run)
					max_run = th.switches;
				bytes++;
				if (wd24x_glyph_ink(n, row, col))
					ink[n]++;
			}
		}
		if (prev_win >= 0)
			T((int)win != prev_win, "next cell is a later window");
		prev_win = (int)win;
		if (n + 1 < WD24X_GLYPH_N) {
			uint16_t ny = wd24x_glyph_xy[n + 1][1];

			T(ny >= y + WD24X_GLYPH_H + 1, "cells do not touch");
		}
		for (y2 = 0; y2 < n; y2++) {
			int r, c, diff = 0;

			for (r = 0; r < 7; r++)
				for (c = 0; c < 5; c++)
					if (wd24x_glyph_ink(n, r, c) !=
					    wd24x_glyph_ink(y2, r, c))
						diff = 1;
			T(diff, "digit masks differ");
		}
		T(ink[n] >= 8 && ink[n] <= 27, "digit is neither empty nor solid");
	}
	T(!th.latched && th.trips == 0 && max_run <= 1,
	  "forward cell walk stays under the latch");
	T(bytes == (unsigned)WD24X_GLYPH_N * WD24X_GLYPH_W * WD24X_GLYPH_H,
	  "byte budget is five cells");
	T(bytes < 640u * 480u, "not a full-frame paint");
	{
		uint32_t a = (uint32_t)WD24X_GLYPH_CTRL_Y * WD24X_GLYPH_PITCH +
			     WD24X_GLYPH_CTRL_X;
		int hit = 0;

		for (n = 0; n < WD24X_GLYPH_N; n++) {
			uint16_t x = wd24x_glyph_xy[n][0];
			uint16_t y = wd24x_glyph_xy[n][1];

			if (WD24X_GLYPH_CTRL_X + 8 > x && WD24X_GLYPH_CTRL_X < x + 8 &&
			    WD24X_GLYPH_CTRL_Y + 8 > y && WD24X_GLYPH_CTRL_Y < y + 8)
				hit = 1;
		}
		T(!hit && a < 640u * 480u, "control cell is inside the frame and outside the digits");
		(void)a;
	}
	printf("test_glyph_bank: bytes %u max consecutive switches %u bound %u\n",
	       bytes, max_run, th.bound);
	printf("test_glyph_bank: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
