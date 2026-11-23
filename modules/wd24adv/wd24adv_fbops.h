/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Decisions for the fb_ops this session can honour without a new
 * register. SR01 bit 5 is screen off (wd24_screen_on, the VGA
 * sequencer clocking-mode bit vga16fb also uses). CR0C/CR0D are the
 * refresh start (datasheet 10.12). In this doubleword set the byte
 * address is that register times 4 (vga16fb_pan_var for 8bpp).
 * 3DA bit 3 is vertical retrace (datasheet 15.8).
 */
#ifndef WD24ADV_FBOPS_H
#define WD24ADV_FBOPS_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t u8;
#endif

#define WD24_SR01_SCREEN_OFF	0x20u
#define WD24_BLANK_UNBLANK	0

/* 0 = unblank (bit clear), 1 = screen off (bit set). */
static inline int wd24_blank_sr01(int blank, u8 sr01, u8 *out)
{
	if (!out)
		return -1;
	if (blank == WD24_BLANK_UNBLANK)
		*out = (u8)(sr01 & ~WD24_SR01_SCREEN_OFF);
	else
		*out = (u8)(sr01 | WD24_SR01_SCREEN_OFF);
	return 0;
}

struct wd24_pan {
	u8 cr0c;
	u8 cr0d;
	int err;
};

/* doubleword != 0: CRTC unit is byte/4. x must be 0. */
static inline void wd24_pan_start(unsigned int x, unsigned int y,
				  unsigned int line, int doubleword,
				  struct wd24_pan *o)
{
	unsigned long byte, unit;

	if (!o)
		return;
	o->cr0c = 0;
	o->cr0d = 0;
	o->err = -22;
	if (x || !line || !doubleword)
		return;
	byte = (unsigned long)y * line;
	if (byte & 3ul)
		return;
	unit = byte >> 2;
	if (unit > 0xfffful)
		return;
	o->cr0c = (u8)(unit >> 8);
	o->cr0d = (u8)unit;
	o->err = 0;
}

/* Column pixels are `stride` apart. A dword store is four consecutive
 * bytes, so it is the same pixels only when stride is 1 (a span).
 */
static inline int wd24_col_dword_ok(unsigned int stride)
{
	return stride == 1u;
}

/* 3DA bit 3. 1 = retrace is active, 0 = keep waiting, -1 = budget spent. */
static inline int wd24_vsync_seen(int status_bit3, int budget_left)
{
	if (status_bit3)
		return 1;
	if (budget_left <= 0)
		return -1;
	return 0;
}

#endif
