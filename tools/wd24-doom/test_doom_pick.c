/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include "wd24x-doom-pick.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

int main(void)
{
	T(!wd24x_doom_fb_opt_in(0), "null");
	T(!wd24x_doom_fb_opt_in(""), "empty");
	T(!wd24x_doom_fb_opt_in("1"), "one");
	T(!wd24x_doom_fb_opt_in("MMAP"), "case");
	T(wd24x_doom_fb_opt_in("mmap"), "mmap");
	T(!wd24x_doom_palette_refused(0), "pal null");
	T(!wd24x_doom_palette_refused("0"), "pal zero");
	T(wd24x_doom_palette_refused("1"), "pal one");
	T(wd24x_doom_luma(0) == 0, "black");
	T(wd24x_doom_luma(0x00ffffffu) == 255 || wd24x_doom_luma(0x00ffffffu) == 254,
	  "white");
	T(wd24x_doom_luma(0x000000ffu) == (29u * 255u) >> 8, "blue");
	printf("test_doom_pick: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
