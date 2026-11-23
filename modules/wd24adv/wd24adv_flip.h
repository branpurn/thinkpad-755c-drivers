/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Two 320x200 pages do not fit in one 64K chain-4 window
 * (64000 * 2 > 65536). The scanout start that moves the picture is
 * CR0C/CR0D. Mode 13h sets CR14 bit 6, so that register counts units
 * of 4 bytes: page 1 at byte 65536 is 65536/4 = 0x4000
 * (CR0C=0x40, CR0D=0x00). vgacon's writes to those two registers
 * moved this panel, and clearing them fixed the wrap. KD_GRAPHICS
 * stays on so vgacon does not write them back.
 *
 * PR3 bits 4:3 are CA17:CA16 (datasheet 13.4; VGADOC paradise.txt
 * calls them display-start bits 16-17, with bits 0-15 in CR0C/CR0D).
 * One of those bits is a +65536 character step. In doubleword mode
 * that is +256KiB, not the next 64K. Linux vga16fb_pan_var writes
 * the 8bpp byte offset >> 2 to those two CRTC registers.
 * PR16 bits 4:3 are added to those same two bits (datasheet 13.13;
 * VGADOC: address-counter offset, not the display start).
 * PR16 bit 6 stays 1 in the BIOS value (CR2F=0x42), which limits the
 * counter to 64K locations. Toggling PR16 bit 3 on 2fe57de left
 * CR0C/CR0D at 0 and the picture did not move. This step does not
 * write PR16 or PR3.
 *
 * PR0A (GR09) = 0x10 is the CPU window at +64KiB (wd90c24_hw.c
 * wd_selftest_banks: 0x00, 0x08, 0x10, 0x40). The CPU draws the page
 * the CRTC is not scanning. There is no framebuffer copy.
 *
 * The CRTC uses CR0C/CR0D for screen refresh (datasheet 10.12) and,
 * as on VGA, loads that start at vertical retrace. The flip writes
 * the two bytes and returns. It does not poll 3DA.
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
#define WD24_FLIP_CR0C_PAGE1	0x40u
#define WD24_FLIP_CR0D_PAGE1	0x00u
#define WD24_FLIP_WRITES_HOT	3u	/* CR0C, CR0D, PR0A */
#define WD24_FLIP_WRITES_COLD	7u	/* those, plus the four unlock writes */

struct wd24_flip_step {
	u8 cr0c;
	u8 cr0d;
	u8 pr0a;
	unsigned int show;
	unsigned int cpu;
};

static inline unsigned int wd24_flip_writes(int already_unlocked)
{
	return already_unlocked ? WD24_FLIP_WRITES_HOT : WD24_FLIP_WRITES_COLD;
}

/* cpu_page is the page just drawn. */
static inline void wd24_flip_step(unsigned int cpu_page, struct wd24_flip_step *o)
{
	o->show = cpu_page ? 1u : 0u;
	o->cpu = cpu_page ? 0u : 1u;
	o->cr0c = o->show ? WD24_FLIP_CR0C_PAGE1 : 0;
	o->cr0d = o->show ? WD24_FLIP_CR0D_PAGE1 : 0;
	o->pr0a = o->cpu ? WD24_FLIP_PR0A_PAGE1 : 0;
}

#endif
