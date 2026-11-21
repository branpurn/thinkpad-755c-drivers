/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include <string.h>
#include "../wd24adv_cmap_cache.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

int main(void)
{
	u16 r[256], g[256], b[256];
	char buf[8192];
	int n, i;

	for (i = 0; i < 256; i++) {
		r[i] = (u16)((i & 63) << 10);
		g[i] = (u16)(1u << 10);
		b[i] = 0;
	}
	n = wd24_cmap_cache_render(buf, sizeof buf, r, g, b);
	T(n > 0, "render");
	T(strncmp(buf, "WD24X-CMAP-CACHE entries 256 shift10 1\n", 39) == 0,
	  "header");
	T(strstr(buf, "\n0 0 1 0\n") != NULL, "index 0");
	T(strstr(buf, "\n7 7 1 0\n") != NULL, "index 7");
	T(strstr(buf, "\n255 63 1 0\n") != NULL, "index 255");
	r[3] = 1;
	T(wd24_cmap_cache_render(buf, sizeof buf, r, g, b) < 0, "low bit");
	r[3] = 0;
	T(wd24_cmap_cache_render(buf, 32, r, g, b) < 0, "short");
	printf("test_cmap_cache: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
