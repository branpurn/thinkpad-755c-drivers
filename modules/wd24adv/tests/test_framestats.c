/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include <string.h>
#include "../../../tools/wd24-doom/wd24x-doom-framestats.h"
#include "../wd24adv_ioctl.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

int main(void)
{
	struct wd24x_doom_fs s;
	uint16_t v[5] = { 40, 10, 29, 57, 80 };
	unsigned i;

	T(wd24x_doom_tics_from_ms(0) == 0, "0 ms");
	T(wd24x_doom_tics_from_ms(28) == 0, "28 ms is still inside one tic");
	T(wd24x_doom_tics_from_ms(29) == 1, "29 ms is one tic");
	T(wd24x_doom_tics_from_ms(57) == 1, "57 ms is still one tic");
	T(wd24x_doom_tics_from_ms(58) == 2, "58 ms is two tics");
	T(wd24x_doom_tics_from_ms(1000) == 35, "one second is 35 tics");
	T(wd24x_doom_ms_of(1, 500000000u) == 1500u, "seconds plus ns");
	T(!wd24x_fs_over_tics(28, 1) && wd24x_fs_over_tics(29, 1), "over 1 tic");
	T(!wd24x_fs_over_tics(57, 2) && wd24x_fs_over_tics(58, 2), "over 2 tics");
	T(wd24x_fs_bucket(0) == 0 && wd24x_fs_bucket(4) == 0, "under 5");
	T(wd24x_fs_bucket(5) == 1 && wd24x_fs_bucket(28) == 4, "edges");
	T(wd24x_fs_bucket(29) == 5 && wd24x_fs_bucket(240) == 11, "top bucket");

	wd24x_fs_sort(v, 5);
	T(v[0] == 10 && v[2] == 40 && v[4] == 80, "sorted");
	T(wd24x_fs_pct(v, 5, 0) == 10, "min rank");
	T(wd24x_fs_pct(v, 5, 50) == 40, "median rank");
	T(wd24x_fs_pct(v, 5, 100) == 80, "max rank");
	T(wd24x_fs_pct(v, 0, 95) == 0, "empty");

	wd24x_fs_clear(&s);
	for (i = 0; i < 3; i++)
		wd24x_fs_add(&s, 20, 1, 1000, 2000, 0);
	wd24x_fs_add(&s, 40, 2, 1000, 0, 5000);
	wd24x_fs_add(&s, 80, 9, 0, 0, 0);
	T(s.frames == 5 && s.n == 5, "counted");
	T(s.over1 == 2 && s.over2 == 1, "slow frames");
	T(s.tics[1] == 3 && s.tics[2] == 1 && s.tics[7] == 1, "tic bins");
	T(s.render_ns == 4000 && s.copy_ns == 6000 && s.flip_ns == 5000,
	  "split sums");
	T(s.hist[wd24x_fs_bucket(20)] == 3, "histogram");
	{
		unsigned skip, wait, to;
		const char *line = "flip skip 2 wait 40 latch_to 1\n";

		T(sscanf(line, WD24_FLIP_COUNT_SCAN, &skip, &wait, &to) == 3,
		  "live count line");
		T(skip == 2 && wait == 40 && to == 1, "live count values");
	}
	printf("test_framestats: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
