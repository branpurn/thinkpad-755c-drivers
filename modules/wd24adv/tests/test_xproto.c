/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include <string.h>
#include "wd24x-xproto.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

int main(void)
{
	uint8_t body[128];
	uint8_t req[12 + 256 * 4];
	uint8_t rep[32 + 2 * 8];
	uint32_t root = 0, cmap = 0;
	uint16_t w = 0, h = 0;
	uint8_t depth = 0;
	unsigned r[2], g[2], b[2], low = 99;
	int n;

	memset(body, 0, sizeof body);
	wd24x_xp_put16(body + 16, 0);
	body[20] = 1;
	body[21] = 1;
	body[32] = 8;
	body[33] = 8;
	wd24x_xp_put32(body + 40, 0x11);
	wd24x_xp_put32(body + 44, 0x22);
	wd24x_xp_put16(body + 60, 640);
	wd24x_xp_put16(body + 62, 480);
	body[78] = 8;
	T(wd24x_xsetup_screen(body, sizeof body, &root, &cmap, &w, &h, &depth) == 0,
	  "setup");
	T(root == 0x11 && cmap == 0x22 && w == 640 && h == 480 && depth == 8,
	  "screen fields");

	n = wd24x_xquery_colors(req, sizeof req, 0x22, 256);
	T(n == 8 + 256 * 4, "query size");
	T(req[0] == 91 && wd24x_xp_get16(req + 2) == (8 + 256 * 4) / 4, "opcode");
	T(wd24x_xp_get32(req + 4) == 0x22, "cmap");
	T(wd24x_xp_get32(req + 8) == 0 && wd24x_xp_get32(req + 8 + 255 * 4) == 255,
	  "pixels");
	{
		unsigned npix = 0;
		uint32_t first = 99, bad = 0;
		uint8_t old[12 + 4];

		T(wd24x_xquery_entries(req, (unsigned)n, 256, &npix, &first,
				       &bad) == 0 && npix == 256 && first == 0,
		  "256-entry map accepts 0..255");
		/*
		 * The old layout stored the count at byte 8. The server
		 * reads that as pixel 0, value 256, which a 256-entry
		 * PseudoColor map rejects with BadValue.
		 */
		memset(old, 0, sizeof(old));
		old[0] = 91;
		wd24x_xp_put16(old + 2, (12 + 4) / 4);
		wd24x_xp_put16(old + 8, 256);
		T(wd24x_xquery_entries(old, sizeof(old), 256, &npix, &first,
				       &bad) < 0 && first == 256 && bad == 256,
		  "count-as-pixel is BadValue");
	}

	memset(rep, 0, sizeof rep);
	rep[0] = 1;
	wd24x_xp_put16(rep + 2, 1);
	wd24x_xp_put32(rep + 4, 4);
	wd24x_xp_put16(rep + 8, 2);
	wd24x_xp_put16(rep + 32, (uint16_t)(7u << 10));
	wd24x_xp_put16(rep + 34, (uint16_t)(1u << 10));
	wd24x_xp_put16(rep + 36, 0);
	wd24x_xp_put16(rep + 40, (uint16_t)((3u << 10) | 5u));
	wd24x_xp_put16(rep + 42, 0);
	wd24x_xp_put16(rep + 44, 0);
	T(wd24x_xcolors_parse(rep, sizeof rep, 2, r, g, b, &low) == 0, "parse");
	T(r[0] == 7 && g[0] == 1 && b[0] == 0, "first");
	T(r[1] == 3 && g[1] == 0 && b[1] == 0 && low == 1, "low bits counted");
	T(wd24x_xcolors_parse(rep, sizeof rep, 256, r, g, b, &low) < 0, "count");

	printf("test_xproto: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
