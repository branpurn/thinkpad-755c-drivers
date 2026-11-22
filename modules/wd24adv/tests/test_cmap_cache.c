/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include <string.h>
#include "../wd24adv_cmap_cache.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

static int has(const char *buf, const char *s)
{
	return strstr(buf, s) ? 1 : 0;
}

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
	n = wd24_cmap_cache_render(buf, sizeof(buf), r, g, b);
	T(n > 0 && has(buf, "\n3 0 1 0\n"), "low bits still shift");
	{
		static const unsigned int lvl[4] = { 0, 21, 42, 63 };
		static const unsigned int grey[9] = {
			7, 14, 21, 28, 35, 42, 49, 56, 63
		};
		unsigned int ri, gi, bi, low = 0;

		for (i = 0; i < 256; i++) {
			r[i] = 0;
			g[i] = 0;
			b[i] = 0;
		}
		for (ri = 0; ri < 4; ri++) {
			for (gi = 0; gi < 4; gi++) {
				for (bi = 0; bi < 4; bi++) {
					unsigned int ix = (ri << 4) | (gi << 2) | bi;

					r[ix] = (u16)((lvl[ri] * 65535u) / 63u);
					g[ix] = (u16)((lvl[gi] * 65535u) / 63u);
					b[ix] = (u16)((lvl[bi] * 65535u) / 63u);
				}
			}
		}
		for (i = 0; i < 9; i++) {
			u16 c = (u16)((grey[i] * 65535u) / 63u);

			r[64 + i] = c;
			g[64 + i] = c;
			b[64 + i] = c;
		}
		for (i = 0; i < 256; i++) {
			if ((r[i] | g[i] | b[i]) & 0x03ff)
				low++;
		}
		T(low == 72, "72 channels carry low bits");
		n = wd24_cmap_cache_render(buf, sizeof(buf), r, g, b);
		T(n > 0, "x scale renders");
		T(strncmp(buf, "WD24X-CMAP-CACHE entries 256 shift10 1\n", 39) == 0,
		  "x header");
		T(has(buf, "\n0 0 0 0\n"), "cube black");
		T(has(buf, "\n1 0 0 21\n"), "cube blue");
		T(has(buf, "\n63 63 63 63\n"), "cube white");
		T(has(buf, "\n64 7 7 7\n"), "first grey");
		T(has(buf, "\n72 63 63 63\n"), "last grey");
		T(has(buf, "\n73 0 0 0\n"), "rest empty");
	}
	T(wd24_cmap_cache_render(buf, 32, r, g, b) < 0, "short");
	printf("test_cmap_cache: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
