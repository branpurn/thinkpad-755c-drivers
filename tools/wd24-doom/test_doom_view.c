/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include "wd24x-doom-view.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

int main(void)
{
	struct wd24x_doom_view v;

	T(wd24x_doom_view_parse(0, 0, &v) == 0 && v.low == 0 && v.blocks == 0,
	  "unset is the engine default");
	T(wd24x_doom_view_parse("", "", &v) == 0 && !v.low && !v.blocks, "empty");
	T(wd24x_doom_view_parse("1", 0, &v) == 0 && v.low == 1, "low on");
	T(wd24x_doom_view_parse("0", "10", &v) == 0 && v.low == 0 && v.blocks == 10,
	  "explicit default");
	T(wd24x_doom_view_parse("2", 0, &v) != 0, "low 2 refused");
	T(wd24x_doom_view_parse(0, "2", &v) != 0, "blocks 2 refused");
	T(wd24x_doom_view_parse(0, "12", &v) != 0, "blocks 12 refused");
	T(wd24x_doom_view_parse(0, "11", &v) == 0 && v.blocks == 11, "fullscreen");
	T(wd24x_doom_view_pixels(10) == 320u * 168u, "blocks 10 pixels");
	T(wd24x_doom_view_pixels(11) == 320u * 200u, "blocks 11 pixels");
	T(wd24x_doom_view_pixels(8) == 256u * 128u, "blocks 8 pixels");
	T(wd24x_doom_view_pixels(8) * 100u / wd24x_doom_view_pixels(10) == 60u,
	  "blocks 8 is 60 percent of the default view");
	printf("test_doom_view: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
