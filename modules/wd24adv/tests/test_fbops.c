/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include "../wd24adv_fbops.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

int main(void)
{
	u8 sr;
	struct wd24_pan p;

	T(wd24_blank_sr01(0, 0x00, &sr) == 0 && sr == 0x00, "unblank stays off the bit");
	T(wd24_blank_sr01(1, 0x01, &sr) == 0 && sr == 0x21, "blank sets SR01 bit 5");
	T(wd24_blank_sr01(2, 0x21, &sr) == 0 && (sr & 0x20), "vsync suspend is still screen off");
	T(wd24_blank_sr01(0, 0x21, &sr) == 0 && sr == 0x01, "unblank clears only bit 5");
	T(wd24_blank_sr01(0, 0, 0) != 0, "no out");

	wd24_pan_start(0, 0, 320, 1, &p);
	T(p.err == 0 && p.cr0c == 0 && p.cr0d == 0, "origin");
	wd24_pan_start(0, 1, 320, 1, &p);
	T(p.err == 0 && ((p.cr0c << 8) | p.cr0d) == (320u / 4u), "one row is 80 units");
	wd24_pan_start(1, 0, 320, 1, &p);
	T(p.err != 0, "x pan refused");
	wd24_pan_start(0, 0, 320, 0, &p);
	T(p.err != 0, "not doubleword");
	T(!wd24_col_dword_ok(320), "column stride is not a dword");
	T(wd24_col_dword_ok(1), "span stride is a dword");
	T(wd24_vsync_seen(0, 5) == 0, "wait");
	T(wd24_vsync_seen(1, 5) == 1, "seen");
	T(wd24_vsync_seen(1, 0) == 1, "late sample still counts");
	T(wd24_vsync_seen(0, 0) == -1, "budget");
	printf("test_fbops: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
