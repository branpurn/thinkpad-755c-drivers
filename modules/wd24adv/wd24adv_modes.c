// SPDX-License-Identifier: GPL-2.0
/*
 * wd24adv mode table and the dry-run boot model.
 *
 * Standard VGA values are the public IBM VGA mode values (mode 03h text,
 * mode 13h) and, for 640x480x8, the exact values wd90c24fb programs
 * (wd90c24_hw.c 299-349).  WD extended values come only from wd90c24fb
 * (wd24_mode_ext() in wd24adv_core.c) or from the BIOS state saved at load.
 * No mode writes PR19, PR18 or any panel/shadow register: the panel route
 * and its centring stay exactly as the BIOS set them at POST.
 *
 * PR19 on the 755C is 54h (live): b4 LCD on, b5 CRT off, b3:2 = 01 =
 * centring enabled, expansion off (data sheet PR19 b3/b2).  So a mode with
 * fewer than 480 lines is shown centred with black bands, never stretched;
 * a 320-wide mode is pixel-doubled by the 8-bit PEL clock (AR10 b6), not by
 * the panel, which cannot stretch horizontally.
 */
#include "wd24adv_core.h"

#ifndef __KERNEL__
#include <string.h>
#endif

#define ATTR_8BPP { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, \
		    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, \
		    0x41, 0x00, 0x0f, 0x00, 0x00 }
#define SEQ_CHAIN4 { 0x03, 0x01, 0x0f, 0x00, 0x0e }
#define GFX_8BPP { 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0f, 0xff }

/* wd90c24_hw.c 299-308 (CRTC) and 316-330 (misc/seq/gfx/attr) */
static const struct wd24_std std_640x480x8 = {
	.misc = 0xe3,
	.seq = SEQ_CHAIN4,
	.crtc = { 0x5f, 0x4f, 0x50, 0x82, 0x54, 0x80, 0x0b, 0x3e,
		  0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0xea, 0x8c, 0xdf, 0x50, 0x40, 0xe7, 0x04, 0xe3, 0xff },
	.gfx = GFX_8BPP,
	.attr = ATTR_8BPP,
};

/*
 * 640x400x8: horizontal timing, offset (CR13=50h, byte mode CR14=40h,
 * CR17=E3h) and the extended set exactly as wd90c24fb's 640x480x8; vertical
 * timing is the standard VGA 400-line set (as modes 03h/13h: 70 Hz,
 * MISC sync polarity -/+ = 63h, CR06/07/10/11/12/15/16 = BF 1F 9C 8E 8F 96
 * B9), with one scan line per row (CR09 = 40h).
 */
static const struct wd24_std std_640x400x8 = {
	.misc = 0x63,
	.seq = SEQ_CHAIN4,
	.crtc = { 0x5f, 0x4f, 0x50, 0x82, 0x54, 0x80, 0xbf, 0x1f,
		  0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x9c, 0x8e, 0x8f, 0x50, 0x40, 0x96, 0xb9, 0xe3, 0xff },
	.gfx = GFX_8BPP,
	.attr = ATTR_8BPP,
};

/* Standard IBM VGA mode 13h (320x200x8, chain-4, double-scanned) */
static const struct wd24_std std_13h = {
	.misc = 0x63,
	.seq = SEQ_CHAIN4,
	.crtc = { 0x5f, 0x4f, 0x50, 0x82, 0x54, 0x80, 0xbf, 0x1f,
		  0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x9c, 0x8e, 0x8f, 0x28, 0x40, 0x96, 0xb9, 0xa3, 0xff },
	.gfx = GFX_8BPP,
	.attr = ATTR_8BPP,
};

const struct wd24_mode wd24_modes[] = {
	{ "640x480-8", 640, 480, 640, 480, 8, 0x00, WD24_PROVEN,
	  WD24_EXT_PACKED8, 640 * 480, WD24M_BANKED, &std_640x480x8,
	  "exact wd90c24fb 640x480x8 sequence; live: 3CC=E3 GR0F=05 GR0E=01 CR11=8C CR29=85 SR11=5D SR31=0D SR06=0F, panel fine" },
	{ "640x400-8", 640, 400, 640, 400, 8, 0x00, WD24_PROVEN,
	  WD24_EXT_PACKED8, 640 * 400, WD24M_BANKED | WD24M_CENTRED,
	  &std_640x400x8,
	  "wd90c24fb horizontals+ext + std VGA 400-line verticals; PR19=54 (centring) as-is; vendor VESA list has 640x400x8 on panel; not yet seen live" },
	{ "320x200-8", 320, 200, 320, 200, 8, 0x13, WD24_PROVEN,
	  WD24_EXT_BIOS, 320 * 200,
	  WD24M_DIRECT | WD24M_DOUBLESCAN | WD24M_CENTRED, &std_13h,
	  "BIOS mode 13h programs std VGA only (POST trace); std 13h regs + BIOS ext as wd90c24fb text restore; Doom fast path; not yet seen live" },
	{ "320x240-8x", 320, 240, 320, 240, 8, 0x00, WD24_UNPROVEN,
	  WD24_EXT_NONE, 320 * 240, WD24M_PLANAR | WD24M_DOUBLESCAN | WD24M_CENTRED,
	  NULL, "UNPROVEN: Mode X 480-line verticals on the panel never observed" },
	{ "640x480-8-v800", 640, 480, 640, 800, 8, 0x00, WD24_UNPROVEN,
	  WD24_EXT_NONE, 640 * 800, WD24M_BANKED, NULL,
	  "UNPROVEN: page flip via CR0C/0D + PR3 start bits never observed" },
};

const unsigned int wd24_nmodes = sizeof(wd24_modes) / sizeof(wd24_modes[0]);

const struct wd24_mode *wd24_mode_find(const char *name)
{
	unsigned int i;

	if (!name)
		return NULL;
	for (i = 0; i < wd24_nmodes; i++)
		if (!strcmp(wd24_modes[i].name, name))
			return &wd24_modes[i];
	return NULL;
}

int wd24_mode_check(const struct wd24_mode *m, const char **why)
{
	u32 bytes;

	*why = NULL;
	if (!m->name || !m->xres || !m->yres || m->bpp != 8)
		*why = "bad geometry";
	else if (m->vxres < m->xres || m->vyres < m->yres)
		*why = "virtual smaller than visible";
	else if (m->status != WD24_PROVEN && m->status != WD24_UNPROVEN)
		*why = "bad status";
	else if (!m->evidence)
		*why = "no evidence line";
	else if (m->status == WD24_UNPROVEN && (m->std || m->ext != WD24_EXT_NONE))
		*why = "UNPROVEN mode carries registers";
	else if (m->status == WD24_PROVEN && (!m->std || m->ext == WD24_EXT_NONE))
		*why = "PROVEN mode without a sequence";
	else if (!!(m->flags & WD24M_CENTRED) != (m->yres < 480))
		*why = "CENTRED flag must match yres < 480 (PR19 b3:2 = 01)";
	if (*why)
		return -EINVAL;
	bytes = (u32)m->vxres * m->vyres;
	if (m->frame_bytes != bytes)
		*why = "frame_bytes != vxres * vyres";
	else if ((m->flags & WD24M_DIRECT) && bytes > 65536)
		*why = "DIRECT mode larger than the 64 KiB window";
	else if (!(m->flags & (WD24M_DIRECT | WD24M_PLANAR)) &&
		 !(m->flags & WD24M_BANKED))
		*why = "> 64 KiB packed mode must be BANKED";
	if (m->std && !*why) {
		bool c4 = m->std->seq[4] & 0x08;
		bool p8 = m->std->attr[0x10] & 0x40;

		if (!c4 || !p8)
			*why = "8 bpp mode without chain-4 / 8-bit PEL";
		else if ((m->ext == WD24_EXT_PACKED8) != !!(m->flags & WD24M_BANKED))
			*why = "only the wd90c24fb packed ext set may bank";
		else if (!(m->std->misc & 0x01))
			*why = "mono CRTC address";
	}
	return *why ? -EINVAL : 0;
}

int wd24_modes_validate(const char **bad_name, const char **why)
{
	unsigned int i, j;

	*bad_name = NULL;
	for (i = 0; i < wd24_nmodes; i++) {
		if (wd24_mode_check(&wd24_modes[i], why)) {
			*bad_name = wd24_modes[i].name;
			return -EINVAL;
		}
		for (j = 0; j < i; j++)
			if (!strcmp(wd24_modes[i].name, wd24_modes[j].name)) {
				*bad_name = wd24_modes[i].name;
				*why = "duplicate name";
				return -EINVAL;
			}
	}
	return 0;
}

/*
 * Boot model: the registers as the BIOS leaves them in text mode 03h on
 * the 755C.  L = read live on the 755C (2026-11-02), P = the clean-room
 * POST register trace (vbios-trace notes), S = standard IBM VGA mode 03h.
 * Only the dry-run backend uses this.
 */
#define L(rf, i, v)	{ WD24_##rf, (i), (v), WD24_SRC_LIVE }
#define P(rf, i, v)	{ WD24_##rf, (i), (v), WD24_SRC_POST }
#define S(rf, i, v)	{ WD24_##rf, (i), (v), WD24_SRC_STD }
const struct wd24_model_reg wd24_boot_model[] = {
	S(MISC, 0, 0x67),
	S(SR, 0x00, 0x03), S(SR, 0x01, 0x00), S(SR, 0x02, 0x03),
	S(SR, 0x03, 0x00), S(SR, 0x04, 0x02),
	L(SR, 0x06, 0x0f), P(SR, 0x10, 0xc1), P(SR, 0x11, 0x25),
	P(SR, 0x12, 0x00), L(SR, 0x19, 0x35), L(SR, 0x21, 0x15),
	L(SR, 0x31, 0x0d), L(SR, 0x35, 0x00),
	S(GR, 0x00, 0x00), S(GR, 0x01, 0x00), S(GR, 0x02, 0x00),
	S(GR, 0x03, 0x00), S(GR, 0x04, 0x00), S(GR, 0x05, 0x10),
	S(GR, 0x06, 0x0e), S(GR, 0x07, 0x00), S(GR, 0x08, 0xff),
	P(GR, 0x09, 0x00), P(GR, 0x0a, 0x00), P(GR, 0x0b, 0xc5),
	P(GR, 0x0c, 0x01), P(GR, 0x0d, 0x00), P(GR, 0x0e, 0x40),
	P(GR, 0x0f, 0x00),
	S(CR, 0x00, 0x5f), S(CR, 0x01, 0x4f), S(CR, 0x02, 0x50),
	S(CR, 0x03, 0x82), S(CR, 0x04, 0x55), S(CR, 0x05, 0x81),
	S(CR, 0x06, 0xbf), S(CR, 0x07, 0x1f), S(CR, 0x08, 0x00),
	S(CR, 0x09, 0x4f), S(CR, 0x0a, 0x0d), S(CR, 0x0b, 0x0e),
	S(CR, 0x0c, 0x00), S(CR, 0x0d, 0x00), S(CR, 0x0e, 0x00),
	S(CR, 0x0f, 0x00), S(CR, 0x10, 0x9c), S(CR, 0x11, 0x8e),
	S(CR, 0x12, 0x8f), S(CR, 0x13, 0x28), S(CR, 0x14, 0x1f),
	S(CR, 0x15, 0x96), S(CR, 0x16, 0xb9), S(CR, 0x17, 0xa3),
	S(CR, 0x18, 0xff),
	P(CR, 0x29, 0x00), P(CR, 0x2a, 0x80), P(CR, 0x2b, 0x00),
	P(CR, 0x2c, 0x00), P(CR, 0x2d, 0x00), P(CR, 0x2e, 0x00),
	P(CR, 0x2f, 0x42),
	L(CR, 0x31, 0xc7), L(CR, 0x32, 0x54), L(CR, 0x33, 0x00),
	L(CR, 0x34, 0xa0), L(CR, 0x3b, 0xef), L(CR, 0x3c, 0x9a),
	L(CR, 0x3e, 0x20),
	S(AR, 0x00, 0x00), S(AR, 0x01, 0x01), S(AR, 0x02, 0x02),
	S(AR, 0x03, 0x03), S(AR, 0x04, 0x04), S(AR, 0x05, 0x05),
	S(AR, 0x06, 0x14), S(AR, 0x07, 0x07), S(AR, 0x08, 0x38),
	S(AR, 0x09, 0x39), S(AR, 0x0a, 0x3a), S(AR, 0x0b, 0x3b),
	S(AR, 0x0c, 0x3c), S(AR, 0x0d, 0x3d), S(AR, 0x0e, 0x3e),
	S(AR, 0x0f, 0x3f), S(AR, 0x10, 0x0c), S(AR, 0x11, 0x00),
	S(AR, 0x12, 0x0f), S(AR, 0x13, 0x08), S(AR, 0x14, 0x00),
};
const unsigned int wd24_boot_model_n =
	sizeof(wd24_boot_model) / sizeof(wd24_boot_model[0]);
