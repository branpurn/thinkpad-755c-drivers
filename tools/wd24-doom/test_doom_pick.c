/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include "wd24x-doom-pick.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

int main(void)
{
	T(!wd24x_doom_fb_opt_in(0), "fb null");
	T(!wd24x_doom_fb_opt_in(""), "fb empty");
	T(!wd24x_doom_fb_opt_in("1"), "fb one");
	T(!wd24x_doom_fb_opt_in("MMAP"), "fb case");
	T(wd24x_doom_fb_opt_in("mmap"), "fb mmap");
	T(!wd24x_doom_input_opt_in(0), "in null");
	T(!wd24x_doom_input_opt_in("1"), "in one");
	T(!wd24x_doom_input_opt_in("TTY"), "in case");
	T(wd24x_doom_input_opt_in("tty"), "in tty");
	printf("test_doom_pick: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
