/* SPDX-License-Identifier: GPL-2.0 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wd24x-cmap-check.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

int main(void)
{
	uint16_t r0[256], g0[256], b0[256], r1[256], g1[256], b1[256];
	int i;

	T(wd24x_cmap_is_shift10(0), "0");
	T(wd24x_cmap_is_shift10(0x8000), "0x8000");
	T(wd24x_cmap_is_shift10(0xfc00), "max");
	T(!wd24x_cmap_is_shift10(0xfc01), "low bit");
	T(!wd24x_cmap_is_shift10(0xffff), "ffff");
	T(!wd24x_cmap_is_shift10(1), "one");
	for (i = 0; i < 256; i++) {
		r0[i] = g0[i] = b0[i] = (uint16_t)((i & 63) << 10);
		r1[i] = r0[i];
		g1[i] = g0[i];
		b1[i] = b0[i];
	}
	T(wd24x_cmap_all_shift10(r0, g0, b0), "table");
	r0[3] = 1;
	T(!wd24x_cmap_all_shift10(r0, g0, b0), "one raw");
	r0[3] = (uint16_t)(3 << 10);
	for (i = 0; i < 256; i++) {
		r1[i] = r0[i];
		g1[i] = g0[i];
		b1[i] = b0[i];
	}
	r1[7] = g1[7] = b1[7] = WD24X_CMAP_SET_VALUE;
	T(wd24x_cmap_set_cache_ok(r0, g0, b0, r1, g1, b1), "one entry");
	r1[8] = 0;
	T(!wd24x_cmap_set_cache_ok(r0, g0, b0, r1, g1, b1), "neighbour");
	r1[8] = r0[8];
	r1[7] = 0;
	T(!wd24x_cmap_set_cache_ok(r0, g0, b0, r1, g1, b1), "index missed");
	printf("test_cmap_check: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
