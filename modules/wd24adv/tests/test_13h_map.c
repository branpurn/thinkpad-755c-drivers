/* The 13h map plan. No kernel, no register write. */
#include <stdio.h>
#include "../wd24adv_13h_map.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; \
	printf("FAIL %s\n", w); } } while (0)

int main(void)
{
	struct wd24_13h_map m;

	T(wd24_13h_map_plan(0, WD24_13H_WIN, &m) == 0, "window maps");
	T(m.phys == 0xa0000u, "phys A0000");
	T(m.map_len == 65536u, "64 KiB");
	T(m.frame == 64000u, "frame 64000");
	T(m.tail == 1536u, "tail 1536");
	T(m.setwin == 0, "no setwin");
	T(m.regs == 0, "no register");
	T(wd24_13h_map_plan(0, 64000u, &m) != 0, "frame is not the vma");
	T(m.phys == 0 && m.setwin == 0 && m.regs == 0, "refuse is empty");
	T(wd24_13h_map_plan(1, WD24_13H_WIN, &m) != 0, "offset refused");
	T(wd24_13h_map_plan(0, 0, &m) != 0, "empty refused");
	T(wd24_13h_map_plan(0, 65537u, &m) != 0, "past the window");
	T(wd24_13h_map_plan(0, 4096u, &m) != 0, "one page refused");
	T(wd24_13h_store_ok(0, 64000u) == 1, "frame store");
	T(wd24_13h_store_ok(0, 64001u) == 0, "past the frame");
	T(wd24_13h_store_ok(63999u, 2u) == 0, "crosses the frame");
	T(wd24_13h_store_ok(64000u, 1u) == 0, "tail is not the frame");
	T(wd24_13h_store_ok(0, 0) == 0, "empty store");
	printf("test_13h_map: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
