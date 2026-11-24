/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include "../wd24adv_flip.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

int main(void)
{
	struct wd24_flip_step s;
	unsigned int old;

	T(64000u * 2u > 65536u, "two frames do not fit in 64K");
	T(wd24_flip_cpu_byte(0x10) == 65536u, "PR0A 0x10 is byte 65536");
	T(wd24_flip_crtc_byte(0x40, 0x00) == 65536u, "CRTC 0x4000 is byte 65536");
	T(wd24_flip_cpu_byte(0x10) == wd24_flip_crtc_byte(0x40, 0x00),
	  "CPU +64K is the same VRAM as CRTC 0x4000");
	T(wd24_flip_cpu_byte(0x20) == 131072u, "PR0A 0x20 is byte 131072");
	T(wd24_flip_crtc_byte(0x80, 0x00) == 131072u, "CRTC 0x8000 is byte 131072");
	T(wd24_flip_cpu_byte(0x20) == wd24_flip_crtc_byte(0x80, 0x00),
	  "CPU +128K is the same VRAM as CRTC 0x8000");
	T((0x10000u << 2) == 262144u, "PR3 CA16 is 256KiB in doubleword mode");

	/* go5: two pages, start written, CPU aimed at the page that was
	 * showing, ioctl returned before 3DA bit 3. That draw is the ghost.
	 */
	T(!wd24_flip_draw_ok(1, 0, 0, 0), "go5 draw is before the sample");
	T(!wd24_flip_draw_ok(1, 0, 0, 1), "two pages draw the previous page");
	T(!wd24_flip_draw_ok(0, 0, 1, 1), "draw on the page just published");

	old = 0;
	wd24_flip_step(0, &s);
	T(s.show == 0 && s.cpu == 1 && s.cr0c == 0 && s.cr0d == 0 &&
	  s.pr0a == 0x10, "after page 0: start stays 0, CPU at +64K");
	T(!wd24_flip_draw_ok(s.show, s.cpu, old, 0), "page 0 not released yet");
	T(wd24_flip_draw_ok(s.show, s.cpu, old, 1), "page 1 is hidden once sampled");
	old = s.show;
	wd24_flip_step(s.cpu, &s);
	T(s.show == 1 && s.cpu == 2 && s.cr0c == 0x40 && s.cr0d == 0 &&
	  s.pr0a == 0x20, "after page 1: CR0C=40, CPU at +128K");
	T(wd24_flip_draw_ok(s.show, s.cpu, old, 1), "page 2 is not the beam");
	T(wd24_flip_cpu_byte(s.pr0a) == wd24_flip_crtc_byte(0x80, 0),
	  "the CPU bank for page 2 is CRTC 0x8000");
	old = s.show;
	wd24_flip_step(s.cpu, &s);
	T(s.show == 2 && s.cpu == 0 && s.cr0c == 0x80 && s.cr0d == 0 &&
	  s.pr0a == 0x00, "after page 2: CR0C=80, CPU bank 0");
	T(wd24_flip_draw_ok(s.show, s.cpu, old, 1), "page 0 is free two steps later");
	T(s.cpu != old && s.cpu != s.show, "draw is neither published page");
	wd24_flip_step(s.cpu, &s);
	T(s.show == 0 && s.cr0c == 0, "the step after that returns the start to 0");
	T(wd24_flip_writes(1) == 3, "hot flip is CR0C, CR0D, PR0A");
	T(wd24_flip_writes(0) == 7, "cold flip adds the four unlock writes");
	printf("test_flip: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
