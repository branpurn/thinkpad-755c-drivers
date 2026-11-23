/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include "../wd24adv_flip.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

int main(void)
{
	struct wd24_flip_step s;

	T(64000u * 2u > 65536u, "two frames do not fit in 64K");
	T(((unsigned)WD24_FLIP_CR0C_PAGE1 << 8 | WD24_FLIP_CR0D_PAGE1) << 2 == 65536u,
	  "0x4000 doubleword units are byte 65536");
	T((0x10000u << 2) == 262144u, "PR3 CA16 is 256KiB in doubleword mode");
	wd24_flip_step(0, &s);
	T(s.show == 0 && s.cpu == 1 && s.cr0c == 0 && s.cr0d == 0 &&
	  s.pr0a == 0x10, "after page 0: start stays 0, CPU at +64K");
	wd24_flip_step(1, &s);
	T(s.show == 1 && s.cpu == 0 && s.cr0c == 0x40 && s.cr0d == 0 &&
	  s.pr0a == 0x00, "after page 1: CR0C=40, CPU bank 0");
	wd24_flip_step(s.cpu, &s);
	T(s.show == 0 && s.cr0c == 0, "next step returns the start to 0");
	printf("test_flip: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
