/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include "../wd24adv_latch.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

int main(void)
{
	int armed = 0;

	T(wd24_latch_arm(0, &armed) == 0 && armed == 0, "quiet draw does not arm");
	T(wd24_latch_arm(1, &armed) == 1 && armed == 1, "first latch arms once");
	T(wd24_latch_arm(1, &armed) == 0 && armed == 1, "later latch does not arm");
	T(wd24_latch_arm(1, 0) == 0, "no flag");
	printf("test_latch: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
