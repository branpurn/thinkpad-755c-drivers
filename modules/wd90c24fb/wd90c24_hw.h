/* SPDX-License-Identifier: GPL-2.0 */
/*
 * WD90C24/WD90C24A(2) register layer, shared by the kernel driver and the
 * host unit tests (tests/unit/). No kernel headers here beyond the I/O
 * accessors, so the same code runs against the register model in
 * tests/unit/wdmock.c.
 *
 * Register sequences are ported from the XFree86 3.3.x pvga1 driver (see
 * THIRD-PARTY.md) and checked against the WD90C24A/A2 data sheet
 * (79-890002-001 Rev D, 1994): PR0(A)/PR0(B) section 13.1, PR1 13.2,
 * PR18/PR19/PR1B section 15, BITBLT section 19.
 */
#ifndef WD90C24_HW_H
#define WD90C24_HW_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/io.h>
#include <linux/delay.h>
#define wd_inb(port)		inb(port)
#define wd_outb(val, port)	outb(val, port)
#define wd_inw(port)		inw(port)
#define wd_outw(val, port)	outw(val, port)
#define wd_msleep(ms)		msleep(ms)
#define wd_udelay(us)		udelay(us)
#define wd_win_write(hw, off, src, n) \
	memcpy_toio((u8 __iomem *)(hw)->win + (off), (src), (n))
#define wd_win_writeb(hw, off, v)	writeb((v), (u8 __iomem *)(hw)->win + (off))
#define wd_win_readb(hw, off)		readb((u8 __iomem *)(hw)->win + (off))
#else
#include "wdmock.h"
#endif

/* Standard VGA ports */
#define WD_MISC_R	0x3cc
#define WD_MISC_W	0x3c2
#define WD_SEQ		0x3c4
#define WD_GFX		0x3ce

/* Extended register access (WD90C31/C24A): data sheet section 17 */
#define WD_EXT_INDEX	0x23c0		/* index control register */
#define WD_EXT_DATA	0x23c2		/* register access port */
#define WD_EXT_INVALID	0x2000		/* index control bit 13: no such block */
#define WD_EXT_NOINC	0x1000		/* bit 12: auto-increment disable */
#define WD_BLK_BLT	0x01
#define WD_BLK_CURSOR	0x02

/* Paradise registers by data-sheet name */
#define PR0A	0x09	/* GFX: address offset A (4 KiB units) */
#define PR0B	0x0a	/* GFX: address offset B */
#define PR1	0x0b	/* GFX: memory size */
#define PR2	0x0c	/* GFX: video select */
#define PR3	0x0d	/* GFX: CRT control */
#define PR4	0x0e	/* GFX: video control */
#define PR5	0x0f	/* GFX: unlock PR0-PR4 (0x05) */
#define PR10	0x29	/* CRTC: unlock PR11-PR17 (0x85) */
#define PR11	0x2a	/* CRTC: EGA switches / CRTC lock */
#define PR12	0x2b	/* CRTC: scratch pad */
#define PR13	0x2c	/* CRTC: interlace H/2 start */
#define PR14	0x2d	/* CRTC: interlace H/2 end */
#define PR15	0x2e	/* CRTC: misc control 1 */
#define PR16	0x2f	/* CRTC: misc control 2 */
#define PR18	0x31	/* CRTC: flat panel status (needs PR1B) */
#define PR19	0x32	/* CRTC: flat panel control I (needs PR1B) */
#define PR1B	0x34	/* CRTC: flat panel unlock, write-only */
#define PR20	0x06	/* SEQ: unlock PR21+ (0x48 pattern) */
#define PR30A	0x10	/* SEQ: memory interface */
#define PR31	0x11	/* SEQ: system interface */
#define PR32	0x12	/* SEQ: misc control 4 */
#define PR57	0x19	/* SEQ: feature register I */
#define PR68	0x31	/* SEQ: programmable clock select (C24) */
#define PR69	0x32	/* SEQ: programmable VCLK frequency (C24) */
#define PR72	0x35	/* SEQ: unlock PR68/PR69 (0x50) */

#define PR19_LCD	0x10	/* flat panel display enable */
#define PR19_CRT	0x20	/* CRT display enable */
#define PR1B_FP		0xa0	/* 101xxxxx: unlock PR18/PR19/PR1A/PR36-41 */
#define PR1B_SHADOW	0x06	/* xxxxx110: unlock shadow registers */

enum wd_chip {
	WD_NONE = 0,
	WD_PVGA1,
	WD_C00,
	WD_C1X,
	WD_C2X,		/* WD90C20/C22: no SR10 */
	WD_C24,
	WD_C26,
	WD_C30,
	WD_C31,
	WD_C33,
	WD_UNKNOWN_C3X,	/* SR14 present, unknown CR36/37 signature */
};

struct wd_hw {
	u16 crtc;		/* 0x3d4 (colour) or 0x3b4 */
	void *win;		/* A0000 64 KiB window (kernel: __iomem) */
	enum wd_chip chip;
	char sig[3];		/* CR36/CR37 with PR1B locked */
	unsigned int vram_kb;	/* from PR1 bits 7:6 */
	u8 pr18, pr19;		/* flat panel status/control at probe */
	bool use_1meg;		/* C24 at 512K: dual scan + panel off */
	bool ext_blocks;	/* 23C0 index control answers */
	int bank;		/* current PR0(A) value, -1 = unknown */
};

/* 640x480, 8 bpp packed, the only mode (the 755C panel is 640x480). */
#define WD_XRES		640
#define WD_YRES		480
#define WD_PITCH	640
#define WD_FB_SIZE	(WD_PITCH * WD_YRES)
#define WD_WIN_SIZE	0x10000

/* Standard VGA register set for one mode (what vgaHWInit computes). */
struct wd_std {
	u8 misc;
	u8 seq[5];
	u8 crtc[25];
	u8 gfx[9];
	u8 attr[21];
};

/* Saved extended state (standard VGA state is saved by vgastate). */
struct wd_ext_state {
	u8 misc, cr11;
	u8 pr0a, pr0b, pr1, pr2, pr3, pr4, pr5;
	u8 pr10, pr11, pr13, pr14, pr15, pr16;
	u8 pr20, pr30a, pr31, pr32;
	u8 pr68, pr69, pr72;
	u8 pr19;
	u16 cursor;
};

const char *wd_chip_name(enum wd_chip c);

/* Indexed register helpers */
u8 wd_rdinx(u16 port, u8 idx);
void wd_wrinx(u16 port, u8 idx, u8 val);
void wd_modinx(u16 port, u8 idx, u8 mask, u8 val);
bool wd_testinx2(u16 port, u8 idx, u8 mask);

/*
 * Probe: XFree86 PVGA1Probe order (GR0F lock test, CR2B, SR07, SR10, SR14,
 * CR36/37 signature), then memory size and panel state. Leaves every lock
 * and register it touched as it found them.
 */
enum wd_chip wd_probe(struct wd_hw *hw);

void wd_unlock(struct wd_hw *hw);
void wd_save_ext(struct wd_hw *hw, struct wd_ext_state *s);
void wd_restore_ext(struct wd_hw *hw, const struct wd_ext_state *s);
void wd_relock(struct wd_hw *hw, const struct wd_ext_state *s);

/*
 * Mode 640x480x8 (stage 2). wd_mode_640x480x8() fills the standard and
 * extended register sets from the saved BIOS state; wd_set_mode() programs
 * them. The caller holds the console lock and has saved the state.
 */
void wd_mode_640x480x8(const struct wd_hw *hw, const struct wd_ext_state *bios,
		       struct wd_std *std, struct wd_ext_state *ext);
void wd_write_std(struct wd_hw *hw, const struct wd_std *std);
void wd_set_mode(struct wd_hw *hw, const struct wd_std *std,
		 const struct wd_ext_state *ext);
void wd_screen_on(struct wd_hw *hw, bool on);
/* Banking self-test after the mode set: 0, or -1 if banking does not work. */
int wd_selftest_banks(struct wd_hw *hw, unsigned int vram_kb);
void wd_set_bank(struct wd_hw *hw, u8 bank);
/* Copy a rectangle of the shadow buffer into video memory. */
void wd_flush_rect(struct wd_hw *hw, const u8 *shadow, unsigned int x,
		   unsigned int y, unsigned int w, unsigned int h);
void wd_set_dac(u8 idx, u8 r, u8 g, u8 b);	/* 6-bit components */

#endif
