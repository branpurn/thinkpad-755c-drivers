/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Two 320x200 pages do not fit in one 64K chain-4 window:
 * 64000 * 2 = 128000 > 65536. WD90C24 PR16 (CR2F) bits 4:3 are the
 * CRTC address-counter offset, added to CA17:CA16, selecting one of
 * four 64K boundaries (datasheet 13.13). Bits 6:5 = 1x keep the
 * counter at 64K. The BIOS value has bit 6 set (boot model CR2F=0x42).
 * PR0A (GR09) = 0x10 is the CPU window at +64KiB (wd90c24_hw.c
 * wd_selftest_banks). CR0C/CR0D stay 0. PR3 is not written.
 * After the CPU finishes a page, that page is shown and the window
 * moves to the other page. Not yet seen on the panel.
 */
#ifndef WD24ADV_FLIP_H
#define WD24ADV_FLIP_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t u8;
#endif

#define WD24_FLIP_PR0A_PAGE1	0x10u
#define WD24_FLIP_PR16_PAGE	0x08u
#define WD24_FLIP_PR16_OFF	0x18u

struct wd24_flip_step {
	u8 pr16;
	u8 pr0a;
	unsigned int show;
	unsigned int cpu;
};

/* cpu_page is the page just drawn. pr16_now is the BIOS PR16 byte. */
static inline void wd24_flip_step(u8 pr16_now, unsigned int cpu_page,
				  struct wd24_flip_step *o)
{
	o->show = cpu_page ? 1u : 0u;
	o->cpu = cpu_page ? 0u : 1u;
	o->pr16 = (u8)((pr16_now & ~WD24_FLIP_PR16_OFF) |
		       (o->show ? WD24_FLIP_PR16_PAGE : 0));
	o->pr0a = o->cpu ? WD24_FLIP_PR0A_PAGE1 : 0;
}

#endif
