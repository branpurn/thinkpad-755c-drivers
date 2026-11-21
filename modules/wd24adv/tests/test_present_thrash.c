/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include <string.h>
#include "../wd24adv_present.h"
#include "../../../tools/wd24-doom/wd24x-doom-present.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

int main(void)
{
	struct wd24_present_win w[8];
	unsigned n, i;
	u32 faults = 0, at = 0;
	int frame;
	u8 src[WD24_PRESENT_SRC];
	u8 a[WD24_PRESENT_FRAME];
	u8 b[WD24_PRESENT_FRAME];
	u32 off;

	n = wd24_present_wins(w, 8);
	T(n == 5, "five windows");
	T(w[0].start == 0 && w[4].end == WD24_PRESENT_FRAME, "span");
	T((w[0].end - w[0].start) % 4u == 0, "dword span");
	for (i = 1; i < n; i++)
		T(w[i].base != w[i - 1].base, "window steps forward");

	frame = wd24_present_mmap_latch(8, 32, &faults, &at);
	T(frame >= 0 && frame < 8, "mmap latches inside eight frames");
	T(faults <= 32, "latch stops the fault count");
	T(at >= 32, "consecutive switches reached the bound");

	frame = wd24_present_mmap_latch(1, 32, &faults, &at);
	T(frame < 0 && faults == 5, "one frame is five faults");

	memset(src, 0, sizeof src);
	src[0] = 15;
	src[3] = 9;
	src[320] = 4;
	wd24_present_fill(a, src);
	wd24x_doom_present(WD24X_DOOM_LETTER, src, b, NULL);
	off = 0;
	while (off < WD24_PRESENT_FRAME && a[off] == b[off])
		off++;
	T(off == WD24_PRESENT_FRAME, "letter pixels match");
	T(wd24_present_letter_dword(src, 40u * 640u) ==
	  (15u | (15u << 8) | (0u << 16) | (0u << 24)), "first dword");

	printf("test_present_thrash: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
