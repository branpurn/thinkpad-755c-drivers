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
