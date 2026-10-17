// SPDX-License-Identifier: GPL-2.0
/*
 * WD90C24/WD90C24A(2) register layer: probe, unlock, save/restore.
 *
 * Probe order, register masks and the save/restore sequence follow the
 * XFree86 3.3.x pvga1 driver (pvg_driver.c: PVGA1Probe, PVGA1Save,
 * PVGA1Restore); see THIRD-PARTY.md for its notice. This is a port of
 * the logic, written for this driver, not a copy of the code.
 * Deliberate differences from XFree86:
 *  - every lock register is put back as it was found (XFree86 left the
 *    chip unlocked), so vgacon/vga16fb see the BIOS state again;
 *  - the memory-clock bits of PR68 are never changed (XFree86 zeroed them
 *    unless a DRAM-speed option was given).
 */
#include "wd90c24_hw.h"

const char *wd_chip_name(enum wd_chip c)
{
	switch (c) {
	case WD_PVGA1: return "PVGA1A";
	case WD_C00:   return "WD90C00";
	case WD_C1X:   return "WD90C1x";
	case WD_C2X:   return "WD90C20/C22";
	case WD_C24:   return "WD90C24";
	case WD_C26:   return "WD90C26";
	case WD_C30:   return "WD90C30";
	case WD_C31:   return "WD90C31";
	case WD_C33:   return "WD90C33";
	case WD_UNKNOWN_C3X: return "unknown WD90C2x/3x";
	default:       return "none";
	}
}

u8 wd_rdinx(u16 port, u8 idx)
{
	wd_outb(idx, port);
	return wd_inb(port + 1);
}

void wd_wrinx(u16 port, u8 idx, u8 val)
{
	wd_outb(idx, port);
	wd_outb(val, port + 1);
}

void wd_modinx(u16 port, u8 idx, u8 mask, u8 val)
{
	wd_wrinx(port, idx, (wd_rdinx(port, idx) & ~mask) | (val & mask));
}

/* True if every bit in mask can be cleared and set; restores the register. */
bool wd_testinx2(u16 port, u8 idx, u8 mask)
{
	u8 old = wd_rdinx(port, idx), lo, hi;

	wd_wrinx(port, idx, old & ~mask);
	lo = wd_rdinx(port, idx) & mask;
	wd_wrinx(port, idx, old | mask);
	hi = wd_rdinx(port, idx) & mask;
	wd_wrinx(port, idx, old);
	return lo == 0 && hi == mask;
}

static bool wd_testinx(u16 port, u8 idx)
{
	return wd_testinx2(port, idx, 0xff);
}

static void wd_unlock_seq(void)
{
	/* PR20: x1xx1xxx with bit 4 clear unlocks PR21 and up */
	wd_wrinx(WD_SEQ, PR20, (wd_rdinx(WD_SEQ, PR20) & 0xef) | 0x48);
}

static void wd_probe_panel(struct wd_hw *hw)
{
	/* PR1B is write-only; XFree86 reads it back anyway and so do we. */
	u8 lock = wd_rdinx(hw->crtc, PR1B);

	wd_wrinx(hw->crtc, PR1B, (lock & 0x0f) | PR1B_FP);
	hw->pr18 = wd_rdinx(hw->crtc, PR18);
	hw->pr19 = wd_rdinx(hw->crtc, PR19);
	wd_wrinx(hw->crtc, PR1B, lock);
}

enum wd_chip wd_probe(struct wd_hw *hw)
{
	u8 gr0f, cr29, cr11, sr06, cr34;
	enum wd_chip chip;

	hw->crtc = (wd_inb(WD_MISC_R) & 1) ? 0x3d4 : 0x3b4;
	hw->chip = WD_NONE;
	hw->sig[0] = hw->sig[1] = hw->sig[2] = 0;
	hw->vram_kb = 0;
	hw->pr18 = hw->pr19 = 0;
	hw->use_1meg = false;
	hw->ext_blocks = false;
	hw->bank = -1;

	/* Paradise test: PR0A must be read-only when locked, r/w when not. */
	gr0f = wd_rdinx(WD_GFX, PR5);
	wd_wrinx(WD_GFX, PR5, gr0f | 0x17);
	if (wd_testinx2(WD_GFX, PR0A, 0x7f)) {
		wd_wrinx(WD_GFX, PR5, gr0f);
		return WD_NONE;
	}
	wd_wrinx(WD_GFX, PR5, 0x05);
	if (!wd_testinx2(WD_GFX, PR0A, 0x7f)) {
		wd_wrinx(WD_GFX, PR5, gr0f);
		return WD_NONE;
	}

	/* PVGA1EnterLeave(ENTER): unlock bank 2, CRTC 0-7, sequencer ext. */
	cr29 = wd_rdinx(hw->crtc, PR10);
	cr11 = wd_rdinx(hw->crtc, 0x11);
	wd_wrinx(hw->crtc, PR10, 0x85);
	wd_wrinx(hw->crtc, 0x11, cr11 & 0x7f);
	sr06 = wd_rdinx(WD_SEQ, PR20);

	if (!wd_testinx(hw->crtc, PR12)) {
		chip = WD_PVGA1;
	} else {
		wd_wrinx(WD_SEQ, PR20, 0x48);
		if (!wd_testinx2(WD_SEQ, 0x07, 0xf0)) {
			chip = WD_C00;
		} else if (!wd_testinx(WD_SEQ, PR30A)) {
			chip = WD_C2X;
		} else if (wd_testinx2(WD_SEQ, 0x14, 0x0f)) {
			cr34 = wd_rdinx(hw->crtc, PR1B);
			wd_wrinx(hw->crtc, PR1B, 0x00);
			hw->sig[0] = wd_rdinx(hw->crtc, 0x36);
			hw->sig[1] = wd_rdinx(hw->crtc, 0x37);
			wd_wrinx(hw->crtc, PR1B, cr34);
			if (hw->sig[0] == '2' && hw->sig[1] == '4')
				chip = WD_C24;
			else if (hw->sig[0] == '2' && hw->sig[1] == '6')
				chip = WD_C26;
			else if (hw->sig[0] == '3' && hw->sig[1] == '0')
				chip = WD_C30;
			else if (hw->sig[0] == '3' && hw->sig[1] == '1')
				chip = WD_C31;
			else if (hw->sig[0] == '3' && hw->sig[1] == '3')
				chip = WD_C33;
			else
				chip = WD_UNKNOWN_C3X;
		} else {
			chip = WD_C1X;
		}
	}
	hw->chip = chip;

	switch (wd_rdinx(WD_GFX, PR1) & 0xc0) {
	case 0x80:
		hw->vram_kb = 512;
		break;
	case 0xc0:
		hw->vram_kb = 1024;
		break;
	default:
		hw->vram_kb = 256;
	}
	if (chip == WD_C24 || chip == WD_C2X) {
		wd_unlock_seq();
		wd_probe_panel(hw);
	}
	/* C24 at 512K: dual scan (PR18 bits 3,1,0 = 0) and panel off. */
	if (chip == WD_C24 && hw->vram_kb == 512 &&
	    !(hw->pr18 & 0x0b) && !(hw->pr19 & PR19_LCD)) {
		hw->use_1meg = true;
		hw->vram_kb = 1024;
	}
	if (chip == WD_C24 || chip == WD_C31 || chip == WD_C33) {
		wd_outw(WD_EXT_NOINC | WD_BLK_BLT, WD_EXT_INDEX);
		hw->ext_blocks = !(wd_inw(WD_EXT_INDEX) & WD_EXT_INVALID);
	}

	wd_wrinx(WD_SEQ, PR20, sr06);
	wd_wrinx(hw->crtc, 0x11, cr11);
	wd_wrinx(hw->crtc, PR10, cr29);
	wd_wrinx(WD_GFX, PR5, gr0f);
	return chip;
}

/* Unlock PR0-PR4, PR11-PR17, CRTC 0-7 and PR21+ (PVGA1EnterLeave). */
void wd_unlock(struct wd_hw *hw)
{
	wd_wrinx(WD_GFX, PR5, 0x05);
	wd_wrinx(hw->crtc, PR10, 0x85);
	wd_modinx(hw->crtc, 0x11, 0x80, 0x00);
	wd_unlock_seq();
}

/*
 * Lock-relevant registers (PR5, PR10, PR20, PR72, CR11) are part of the
 * standard or extended save so wd_restore_ext() can put them back last.
 */
void wd_save_ext(struct wd_hw *hw, struct wd_ext_state *s)
{
	s->misc = wd_inb(WD_MISC_R);
	s->cr11 = wd_rdinx(hw->crtc, 0x11);
	s->pr5 = wd_rdinx(WD_GFX, PR5);
	s->pr10 = wd_rdinx(hw->crtc, PR10);
	s->pr20 = wd_rdinx(WD_SEQ, PR20);
	wd_unlock(hw);
	s->pr0a = wd_rdinx(WD_GFX, PR0A);
	s->pr0b = wd_rdinx(WD_GFX, PR0B);
	s->pr1 = wd_rdinx(WD_GFX, PR1);
	s->pr2 = wd_rdinx(WD_GFX, PR2);
	s->pr3 = wd_rdinx(WD_GFX, PR3);
	s->pr4 = wd_rdinx(WD_GFX, PR4);
	s->pr11 = wd_rdinx(hw->crtc, PR11);
	s->pr13 = wd_rdinx(hw->crtc, PR13);
	s->pr14 = wd_rdinx(hw->crtc, PR14);
	s->pr15 = wd_rdinx(hw->crtc, PR15);
	s->pr16 = wd_rdinx(hw->crtc, PR16);
	s->pr30a = wd_rdinx(WD_SEQ, PR30A);
	s->pr31 = wd_rdinx(WD_SEQ, PR31);
	s->pr32 = wd_rdinx(WD_SEQ, PR32);
	s->pr72 = wd_rdinx(WD_SEQ, PR72);
	s->pr68 = s->pr69 = 0;
	if (hw->chip == WD_C24) {
		wd_wrinx(WD_SEQ, PR72, 0x50);
		s->pr68 = wd_rdinx(WD_SEQ, PR68);
		s->pr69 = wd_rdinx(WD_SEQ, PR69);
		wd_wrinx(WD_SEQ, PR72, s->pr72);
	}
	s->pr19 = hw->pr19;
	s->cursor = 0;
	if (hw->ext_blocks) {
		wd_outw(WD_EXT_NOINC | WD_BLK_CURSOR, WD_EXT_INDEX);
		s->cursor = wd_inw(WD_EXT_DATA);
	}
}

/*
 * PVGA1Restore order: unlock, banks, PR1, clocks, PR3/PR4, bank-2
 * specials, PR31, PR15 (selected bits), PR30A; the caller then restores
 * the standard VGA registers and finally calls wd_relock().
 */
void wd_restore_ext(struct wd_hw *hw, const struct wd_ext_state *s)
{
	u8 mask;

	wd_unlock(hw);
	wd_modinx(WD_GFX, PR3, 0xff, wd_rdinx(WD_GFX, PR3) & 0x1c);
	wd_modinx(WD_GFX, PR4, 0xff, wd_rdinx(WD_GFX, PR4) & 0xfb);
	/* With the LCD on, PR11's CRTC locks stay as the BIOS set them. */
	mask = (hw->chip == WD_C24 && (hw->pr19 & PR19_LCD)) ? 0xff : 0xf8;
	wd_wrinx(hw->crtc, PR11, wd_rdinx(hw->crtc, PR11) & mask);

	wd_wrinx(WD_GFX, PR0A, s->pr0a);
	wd_wrinx(WD_GFX, PR0B, s->pr0b);
	wd_modinx(WD_GFX, PR1, 0xc8, s->pr1);
	if (hw->chip == WD_C24) {
		wd_wrinx(WD_SEQ, PR72, 0x50);
		wd_modinx(WD_SEQ, PR68, 0x18, s->pr68);	/* not MCLK (2:0) */
		if ((s->misc & 0x0c) == 0x08)
			wd_wrinx(WD_SEQ, PR69, s->pr69);
		wd_wrinx(WD_SEQ, PR72, s->pr72);
	} else {
		wd_wrinx(WD_GFX, PR2, s->pr2);
	}
	wd_wrinx(WD_GFX, PR3, s->pr3);
	wd_wrinx(WD_GFX, PR4, s->pr4);
	wd_wrinx(hw->crtc, PR13, s->pr13);
	wd_wrinx(hw->crtc, PR14, s->pr14);
	wd_wrinx(hw->crtc, PR16, s->pr16);
	wd_wrinx(WD_SEQ, PR31, s->pr31);	/* XFree86 kept bits 1:0; we restore all */
	if (hw->chip != WD_PVGA1)
		wd_modinx(hw->crtc, PR15, 0x50, s->pr15);
	if (hw->chip >= WD_C24)
		wd_modinx(WD_SEQ, PR30A, 0xcf, s->pr30a);
	if (hw->ext_blocks) {
		wd_outw(WD_EXT_NOINC | WD_BLK_CURSOR, WD_EXT_INDEX);
		wd_outw(s->cursor, WD_EXT_DATA);
	}
}

/* Last step after the standard registers: the BIOS's lock state. */
void wd_relock(struct wd_hw *hw, const struct wd_ext_state *s)
{
	wd_wrinx(hw->crtc, PR11, s->pr11);
	wd_modinx(hw->crtc, 0x11, 0x80, s->cr11);	/* CRTC 0-7 protect */
	wd_wrinx(WD_SEQ, PR20, s->pr20);
	wd_wrinx(hw->crtc, PR10, s->pr10);
	wd_wrinx(WD_GFX, PR5, s->pr5);
}

/*
 * 640x480x8, from XFree86 PVGA1Init + vgaHWInit for 256 colours, at the
 * standard 25.175 MHz (clock 0) VGA timing: 800x525 total, both syncs
 * negative. Differences from XFree86: PR1 bit 3 stays clear, so PR0(A)
 * alone offsets the whole 64 KiB window (data sheet 13.1); PR68's MCLK
 * bits are left as the BIOS set them.
 */
static const u8 wd_crtc_640x480x8[25] = {
	0x5f, 0x4f, 0x50, 0x82, 0x54, 0x80, 0x0b, 0x3e,
	0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xea, 0x8c, 0xdf,
	0x50,	/* offset: 640 / 8 (doubleword mode, as XFree86 sets) */
	0x40,	/* underline: doubleword mode */
	0xe7, 0x04,
	0xe3,	/* mode control: byte mode */
	0xff,
};

void wd_mode_640x480x8(const struct wd_hw *hw, const struct wd_ext_state *bios,
		       struct wd_std *std, struct wd_ext_state *ext)
{
	static const u8 seq[5] = { 0x03, 0x01, 0x0f, 0x00, 0x0e };
	static const u8 gfx[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0f, 0xff };
	int i;

	std->misc = 0xe3;
	for (i = 0; i < 5; i++)
		std->seq[i] = seq[i];
	for (i = 0; i < 25; i++)
		std->crtc[i] = wd_crtc_640x480x8[i];
	for (i = 0; i < 9; i++)
		std->gfx[i] = gfx[i];
	for (i = 0; i < 16; i++)
		std->attr[i] = i;
	std->attr[16] = 0x41;	/* graphics, 8-bit colour */
	std->attr[17] = 0x00;
	std->attr[18] = 0x0f;
	std->attr[19] = 0x00;
	std->attr[20] = 0x00;

	*ext = *bios;
	ext->misc = std->misc;
	ext->pr0a = 0;
	ext->pr0b = 0;
	ext->pr1 = bios->pr1 & ~0x08;
	if (hw->use_1meg)
		ext->pr1 |= 0xc0;
	ext->pr3 = 0x00;		/* CRTCCtrl */
	ext->pr4 = 0x01;		/* VideoCtrl: 256-colour path */
	ext->pr13 = 0x00;		/* no interlace */
	ext->pr14 = 0x00;
	ext->pr15 = bios->pr15 & ~0x50;	/* MiscCtrl1: clock 0 is not MCLK */
	ext->pr16 = 0x00;		/* MiscCtrl2 */
	ext->pr31 = (bios->pr31 & 0x03) | 0x5c;	/* InterfaceCtrl */
	ext->pr30a = (bios->pr30a & ~0xcf) | 0xc1;	/* MemoryInterface */
	if (hw->chip == WD_C24)		/* VideoSelect for clock 0 */
		ext->pr68 = (bios->pr68 & ~0x18) | 0x08;
}

void wd_screen_on(struct wd_hw *hw, bool on)
{
	(void)hw;
	wd_modinx(WD_SEQ, 0x01, 0x20, on ? 0x00 : 0x20);
}

void wd_write_std(struct wd_hw *hw, const struct wd_std *std)
{
	u16 is1 = hw->crtc + 6;		/* input status 1 resets the ATC flip-flop */
	int i;

	wd_wrinx(WD_SEQ, 0x00, 0x01);	/* synchronous reset */
	wd_outb(std->misc, WD_MISC_W);
	for (i = 1; i < 5; i++)
		wd_wrinx(WD_SEQ, i, i == 1 ? std->seq[i] | 0x20 : std->seq[i]);
	wd_wrinx(WD_SEQ, 0x00, 0x03);
	wd_modinx(hw->crtc, 0x11, 0x80, 0x00);
	for (i = 0; i < 25; i++)
		if (i != 0x11)
			wd_wrinx(hw->crtc, i, std->crtc[i]);
	wd_wrinx(hw->crtc, 0x11, std->crtc[0x11]);
	for (i = 0; i < 9; i++)
		wd_wrinx(WD_GFX, i, std->gfx[i]);
	wd_inb(is1);
	for (i = 0; i < 21; i++) {
		wd_outb(i, 0x3c0);
		wd_outb(std->attr[i], 0x3c0);
	}
	wd_inb(is1);
	wd_outb(0x20, 0x3c0);		/* palette address source: video on */
}

void wd_set_mode(struct wd_hw *hw, const struct wd_std *std,
		 const struct wd_ext_state *ext)
{
	wd_screen_on(hw, false);
	wd_restore_ext(hw, ext);	/* same writer as restore, mode values */
	wd_write_std(hw, std);
	hw->bank = -1;
	wd_set_bank(hw, 0);
	wd_screen_on(hw, true);
}

void wd_set_bank(struct wd_hw *hw, u8 bank)
{
	if (hw->bank != bank) {
		wd_wrinx(WD_GFX, PR0A, bank);
		hw->bank = bank;
	}
}

static u8 wd_marker(int i)
{
	return 0x5a ^ (u8)(i * 0x33);
}

int wd_selftest_banks(struct wd_hw *hw, unsigned int vram_kb)
{
	/* 0, 32 KiB, 64 KiB and (512K+ parts) 256 KiB */
	static const u8 banks[4] = { 0x00, 0x08, 0x10, 0x40 };
	int i, n = vram_kb >= 512 ? 4 : 3, ret = 0;

	wd_set_bank(hw, 0x10);
	if (wd_rdinx(WD_GFX, PR0A) != 0x10)
		ret = -1;
	for (i = 0; i < n && !ret; i++) {
		wd_set_bank(hw, banks[i]);
		wd_win_writeb(hw, 0, wd_marker(i));
	}
	for (i = 0; i < n && !ret; i++) {
		wd_set_bank(hw, banks[i]);
		if (wd_win_readb(hw, 0) != wd_marker(i))
			ret = -1;
	}
	/* the window is linear: bank 0 + 32 KiB is bank 8 + 0 */
	if (!ret) {
		wd_set_bank(hw, 0);
		if (wd_win_readb(hw, 0x8000) != wd_marker(1))
			ret = -1;
	}
	hw->bank = -1;
	wd_set_bank(hw, 0);
	return ret;
}

void wd_flush_rect(struct wd_hw *hw, const u8 *shadow, unsigned int x,
		   unsigned int y, unsigned int w, unsigned int h)
{
	unsigned int r;
	u32 off, base;

	if (x >= WD_XRES || y >= WD_YRES || !w || !h)
		return;
	if (w > WD_XRES - x)
		w = WD_XRES - x;
	if (h > WD_YRES - y)
		h = WD_YRES - y;
	for (r = 0; r < h; r++) {
		off = (y + r) * WD_PITCH + x;
		base = hw->bank < 0 ? 0 : (u32)hw->bank << 12;
		if (hw->bank < 0 || off < base || off + w > base + WD_WIN_SIZE) {
			wd_set_bank(hw, off >> 12);
			base = (off >> 12) << 12;
		}
		wd_win_write(hw, off - base, shadow + off, w);
	}
}

void wd_set_dac(u8 idx, u8 r, u8 g, u8 b)
{
	wd_outb(idx, 0x3c8);
	wd_outb(r, 0x3c9);
	wd_outb(g, 0x3c9);
	wd_outb(b, 0x3c9);
}

/* ---- BITBLT (port of XFree86 wd90c31BitBlt and WAIT_BLIT) ---- */

static void wd_blt_reg(u16 reg, u32 val)
{
	wd_outw(reg | (val & 0x0fff), WD_EXT_DATA);
}

int wd_blt_wait(struct wd_hw *hw)
{
	unsigned int n;

	(void)hw;
	wd_outw(WD_EXT_NOINC | WD_BLK_BLT, WD_EXT_INDEX);	/* CNTRL1 */
	for (n = 0; n < WD_BLT_SPINS; n++)
		if (!(wd_inw(WD_EXT_DATA) & BLT_ACT))
			return 0;
	return -1;
}

static int wd_blt_go(struct wd_hw *hw, u32 src, u32 dst, u32 pitch, u32 w,
		     u32 h, u16 rop, u16 cntrl1, u8 color)
{
	if (!w || !h || w > 2048 || h > 2048 || pitch > 0x0fff)
		return 0;
	if (wd_blt_wait(hw))
		return -1;
	wd_blt_reg(BLT_SRC_LO, src & 0xfff);
	wd_blt_reg(BLT_SRC_HI, src >> 12);
	wd_blt_reg(BLT_DST_LO, dst & 0xfff);
	wd_blt_reg(BLT_DST_HI, dst >> 12);
	wd_blt_reg(BLT_PITCH, pitch);
	wd_blt_reg(BLT_DIM_X, w);	/* pixels, 1..2048 */
	wd_blt_reg(BLT_DIM_Y, h);
	wd_blt_reg(BLT_MASK, 0xff);
	wd_blt_reg(BLT_ROP, rop);
	wd_blt_reg(BLT_CNTRL2, 0);
	if (cntrl1 & BLT_SRC_FCOL)
		wd_blt_reg(BLT_FGCOL, color);
	wd_blt_reg(BLT_CNTRL1, BLT_ACT | BLT_PACKED | cntrl1);
	return 0;
}

int wd_blt_fill_lin(struct wd_hw *hw, u32 dst, u32 pitch, u32 w, u32 h,
		    u8 color, bool xor)
{
	return wd_blt_go(hw, dst, dst, pitch, w, h,
			 xor ? BLT_ROP_XOR : BLT_ROP_SRC, BLT_SRC_FCOL, color);
}

int wd_blt_copy_lin(struct wd_hw *hw, u32 src, u32 dst, u32 pitch,
		    u32 w, u32 h)
{
	/* Overlap: a destination after the source is copied from the end. */
	if (dst > src)
		return wd_blt_go(hw, src + (h - 1) * pitch + w - 1,
				 dst + (h - 1) * pitch + w - 1, pitch, w, h,
				 BLT_ROP_SRC, BLT_BACKWARD, 0);
	return wd_blt_go(hw, src, dst, pitch, w, h, BLT_ROP_SRC, 0, 0);
}

int wd_blt_fill(struct wd_hw *hw, u32 x, u32 y, u32 w, u32 h, u8 color, bool xor)
{
	return wd_blt_fill_lin(hw, y * WD_PITCH + x, WD_PITCH, w, h, color, xor);
}

int wd_blt_copy(struct wd_hw *hw, u32 sx, u32 sy, u32 dx, u32 dy, u32 w, u32 h)
{
	return wd_blt_copy_lin(hw, sy * WD_PITCH + sx, dy * WD_PITCH + dx,
			       WD_PITCH, w, h);
}

static int wd_check_lin(struct wd_hw *hw, u32 off, u32 n, u8 want)
{
	u32 i;

	for (i = 0; i < n; i++) {
		u32 a = off + i;

		wd_set_bank(hw, a >> 12);
		if (wd_win_readb(hw, a & 0xfff) != want)
			return -1;
	}
	return 0;
}

int wd_blt_selftest(struct wd_hw *hw)
{
	/* 384 KiB: past the 300 KiB frame, inside 512 KiB parts */
	const u32 a = 0x60000, b = 0x61000, pitch = WD_PITCH;
	u32 r;
	int ret = 0;

	/* clear both areas (rows of 64 + 1 guard byte) with the CPU */
	for (r = 0; r < 4; r++) {
		u32 i;

		for (i = 0; i <= 64; i++) {
			wd_set_bank(hw, (a + r * pitch + i) >> 12);
			wd_win_writeb(hw, (a + r * pitch + i) & 0xfff, 0x00);
			wd_set_bank(hw, (b + r * pitch + i) >> 12);
			wd_win_writeb(hw, (b + r * pitch + i) & 0xfff, 0x00);
		}
	}
	if (wd_blt_fill_lin(hw, a, pitch, 64, 4, 0xa5, false) || wd_blt_wait(hw) ||
	    wd_blt_copy_lin(hw, a, b, pitch, 64, 4) || wd_blt_wait(hw))
		ret = -1;
	for (r = 0; r < 4 && !ret; r++) {
		if (wd_check_lin(hw, a + r * pitch, 64, 0xa5) ||
		    wd_check_lin(hw, a + r * pitch + 64, 1, 0x00) ||
		    wd_check_lin(hw, b + r * pitch, 64, 0xa5) ||
		    wd_check_lin(hw, b + r * pitch + 64, 1, 0x00))
			ret = -1;
	}
	hw->bank = -1;
	wd_set_bank(hw, 0);
	return ret;
}
