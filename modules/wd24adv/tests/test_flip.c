/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include "../wd24adv_flip.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

int main(void)
{
	struct wd24_flip_step s;

	T(64000u * 2u > 65536u, "two frames do not fit in 64K");
	wd24_flip_step(0x42, 0, &s);
	T(s.show == 0 && s.cpu == 1 && s.pr0a == 0x10 && s.pr16 == 0x42,
	  "draw page 0: show it, bank the CPU at +64K");
	wd24_flip_step(0x42, 1, &s);
	T(s.show == 1 && s.cpu == 0 && s.pr0a == 0x00 && s.pr16 == 0x4a,
	  "draw page 1: PR16 bit 3, CPU bank 0");
	wd24_flip_step(0x5a, 0, &s);
	T(s.pr16 == 0x42 && (s.pr16 & 0x42) == 0x42, "offset bits cleared, width kept");
	wd24_flip_step(s.pr16, s.cpu, &s);
	T(s.show == 1 && s.pr16 == 0x4a, "second step shows page 1");
	printf("test_flip: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
