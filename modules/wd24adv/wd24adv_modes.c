// SPDX-License-Identifier: GPL-2.0
/*
 * wd24adv mode table skeleton (design: REPORT.md section 4/6).
 *
 * Standard VGA register values below are the public IBM VGA / Mode X values
 * (IBM VGA technical reference; M. Abrash, "Mode X").  WD extended values
 * (PR19 b3:2 scaling, PR41/PR64 stretch, PR68/PR69 clocks, panel shadow
 * CRTC) are deliberately absent: they come from the clean-room Stage 0
 * trace of the 755C's own video BIOS.  Every mode still waiting for that
 * data carries WD24M_STAGE0 and is refused for real hardware.
 */
#include "wd24adv_core.h"

#ifndef __KERNEL__
#include <string.h>
#endif

#define S(i, v)	{ WD24_SR, (i), (v) }
#define G(i, v)	{ WD24_GR, (i), (v) }
#define C(i, v)	{ WD24_CR, (i), (v) }
#define A(i, v)	{ WD24_AR, (i), (v) }
#define M(v)	{ WD24_MISC, 0, (v) }
#define N(a)	(sizeof(a) / sizeof((a)[0]))

/* INT 10h mode 13h: 320x200x8 chained (standard VGA) */
static const struct wd24_reg regs_13h[] = {
	M(0x63),
	S(0x00, 0x01), S(0x01, 0x01), S(0x02, 0x0f), S(0x03, 0x00),
	S(0x04, 0x0e), S(0x00, 0x03),
	C(0x11, 0x0e),			/* clear CR11 b7 (CR0-7 write protect) */
	C(0x00, 0x5f), C(0x01, 0x4f), C(0x02, 0x50), C(0x03, 0x82),
	C(0x04, 0x54), C(0x05, 0x80), C(0x06, 0xbf), C(0x07, 0x1f),
	C(0x08, 0x00), C(0x09, 0x41), C(0x0a, 0x00), C(0x0b, 0x00),
	C(0x0c, 0x00), C(0x0d, 0x00), C(0x10, 0x9c), C(0x12, 0x8f),
	C(0x13, 0x28), C(0x14, 0x40), C(0x15, 0x96), C(0x16, 0xb9),
	C(0x17, 0xa3), C(0x18, 0xff), C(0x11, 0x8e),
	G(0x00, 0x00), G(0x01, 0x00), G(0x02, 0x00), G(0x03, 0x00),
	G(0x04, 0x00), G(0x05, 0x40), G(0x06, 0x05), G(0x07, 0x0f),
	G(0x08, 0xff),
	A(0x10, 0x41), A(0x11, 0x00), A(0x12, 0x0f), A(0x13, 0x00),
	A(0x14, 0x00),
};

/* Mode X: 320x240x8 unchained, 4 planes (mode 13h + these) */
static const struct wd24_reg regs_modex[] = {
	M(0xe3),
	S(0x00, 0x01), S(0x01, 0x01), S(0x02, 0x0f), S(0x03, 0x00),
	S(0x04, 0x06), S(0x00, 0x03),
	C(0x11, 0x2c),
	C(0x00, 0x5f), C(0x01, 0x4f), C(0x02, 0x50), C(0x03, 0x82),
	C(0x04, 0x54), C(0x05, 0x80), C(0x06, 0x0d), C(0x07, 0x3e),
	C(0x08, 0x00), C(0x09, 0x41), C(0x0c, 0x00), C(0x0d, 0x00),
	C(0x10, 0xea), C(0x12, 0xdf), C(0x13, 0x28), C(0x14, 0x00),
	C(0x15, 0xe7), C(0x16, 0x06), C(0x17, 0xe3), C(0x11, 0xac),
	G(0x05, 0x40), G(0x06, 0x05),
	A(0x10, 0x41), A(0x11, 0x00), A(0x12, 0x0f), A(0x13, 0x00),
	A(0x14, 0x00),
};

const struct wd24_mode wd24_modes[] = {
	{ .name = "640x480-8", .xres = 640, .yres = 480, .vxres = 640,
	  .vyres = 480, .bpp = 8, .bios_mode = 0x5f, .stage = 2,
	  .frame_bytes = 640 * 480,
	  .flags = WD24M_PANEL_OK | WD24M_BANKED | WD24M_STAGE0 },
	{ .name = "640x400-8", .xres = 640, .yres = 400, .vxres = 640,
	  .vyres = 400, .bpp = 8, .bios_mode = 0x5e, .stage = 3,
	  .frame_bytes = 640 * 400,
	  .flags = WD24M_PANEL_OK | WD24M_BANKED | WD24M_STAGE0 },
	{ .name = "320x200-8", .xres = 320, .yres = 200, .vxres = 320,
	  .vyres = 200, .bpp = 8, .bios_mode = 0x13, .stage = 2,
	  .frame_bytes = 320 * 200,
	  .flags = WD24M_PANEL_OK | WD24M_DIRECT | WD24M_DOUBLESCAN |
		   WD24M_STAGE0,
	  .regs = regs_13h, .nregs = N(regs_13h) },
	{ .name = "320x240-8", .xres = 320, .yres = 240, .vxres = 320,
	  .vyres = 240, .bpp = 8, .stage = 3,
	  .frame_bytes = 320 * 240,
	  .flags = WD24M_PANEL_OK | WD24M_BANKED | WD24M_DOUBLESCAN |
		   WD24M_STAGE0 },
	{ .name = "320x240-8x", .xres = 320, .yres = 240, .vxres = 320,
	  .vyres = 240 * 3, .bpp = 8, .stage = 3,	/* 3 pages for flipping */
	  .frame_bytes = 320 * 240 * 3,
	  .flags = WD24M_PANEL_OK | WD24M_DIRECT | WD24M_PLANAR |
		   WD24M_DOUBLESCAN | WD24M_STAGE0,
	  .regs = regs_modex, .nregs = N(regs_modex) },
	{ .name = "800x600-8-crt", .xres = 800, .yres = 600, .vxres = 800,
	  .vyres = 600, .bpp = 8, .stage = 6, .pixclock_khz = 40000,
	  .frame_bytes = 800 * 600,
	  .flags = WD24M_CRT_ONLY | WD24M_BANKED | WD24M_STAGE0 },
	{ .name = "640x480-8-v800", .xres = 640, .yres = 480, .vxres = 800,
	  .vyres = 600, .bpp = 8, .stage = 3,
	  .frame_bytes = 800 * 600,
	  .flags = WD24M_PANEL_OK | WD24M_BANKED | WD24M_STAGE0 },
	{ .name = "text-03h", .xres = 720, .yres = 400, .vxres = 720,
	  .vyres = 400, .bpp = 4, .bios_mode = 0x03, .stage = 2,
	  .frame_bytes = 0,
	  .flags = WD24M_PANEL_OK | WD24M_TEXT | WD24M_STAGE0 },
};

const unsigned int wd24_nmodes = N(wd24_modes);

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

/* registers a mode list must never contain: routing/power/clock/aperture */
static bool reg_forbidden_in_mode(const struct wd24_reg *r)
{
	switch (r->rf) {
	case WD24_CR:
		return r->idx == WD24_CR_PR18 || r->idx == WD24_CR_PR19 ||
		       r->idx == WD24_CR_PR1A || r->idx == WD24_CR_PR1B ||
		       r->idx == WD24_CR_PR36 || r->idx == WD24_CR_PR10;
	case WD24_SR:
		return r->idx == WD24_SR_PR57 || r->idx == WD24_SR_PR68 ||
		       r->idx == WD24_SR_PR69 || r->idx == WD24_SR_PR68UL ||
		       r->idx == WD24_SR_PR20;
	case WD24_GR:
		return r->idx == WD24_GR_PR1 || r->idx == WD24_GR_PR4 ||
		       r->idx == WD24_GR_PR5;
	default:
		return false;
	}
}

int wd24_mode_check(const struct wd24_mode *m, const char **why)
{
	u32 bytes, window;
	unsigned int i;

#define BAD(s) do { *why = (s); return -EINVAL; } while (0)
	if (!m || !m->name)
		BAD("no mode");
	if (!m->xres || !m->yres || m->vxres < m->xres || m->vyres < m->yres)
		BAD("bad geometry");
	if (!(m->flags & WD24M_PANEL_OK) == !(m->flags & WD24M_CRT_ONLY))
		BAD("exactly one of PANEL_OK / CRT_ONLY");
	/* text 03h is 720x400 nominal; the panel logic fits 9-dot text */
	if ((m->flags & WD24M_PANEL_OK) && !(m->flags & WD24M_TEXT) &&
	    (m->xres > 640 || m->yres > 480))
		BAD("visible size exceeds the 640x480 panel");
	if (m->pr19_scale & ~WD24_PR19_SCALE)
		BAD("pr19_scale outside PR19 b3:2");
	if (m->flags & WD24M_TEXT) {
		if (m->flags & (WD24M_DIRECT | WD24M_BANKED | WD24M_PLANAR))
			BAD("text mode with a framebuffer mapping flag");
	} else {
		bytes = (u32)m->vxres * m->vyres * m->bpp / 8;
		if (bytes != m->frame_bytes)
			BAD("frame_bytes != vxres*vyres*bpp/8");
		if (!(m->flags & WD24M_DIRECT) == !(m->flags & WD24M_BANKED))
			BAD("exactly one of DIRECT / BANKED");
		window = (m->flags & WD24M_PLANAR) ? bytes / 4 : bytes;
		if ((m->flags & WD24M_DIRECT) && window > 0x10000)
			BAD("DIRECT mode larger than the 64 KiB window");
		if (bytes > 1024 * 1024)
			BAD("larger than 1 MiB VRAM");
	}
	if (!m->nregs && !(m->flags & WD24M_STAGE0))
		BAD("no register list and not marked STAGE0");
	if (m->nregs && !m->regs)
		BAD("nregs without regs");
	for (i = 0; i < m->nregs; i++) {
		if (m->regs[i].rf >= WD24_NRF || m->regs[i].idx >= WD24_NIDX)
			BAD("register out of range");
		if (reg_forbidden_in_mode(&m->regs[i]))
			BAD("mode list touches a routing/power/clock/unlock register");
	}
#undef BAD
	return 0;
}

int wd24_modes_validate(const char **bad_name, const char **why)
{
	unsigned int i, j;
	int err;

	for (i = 0; i < wd24_nmodes; i++) {
		*bad_name = wd24_modes[i].name;
		err = wd24_mode_check(&wd24_modes[i], why);
		if (err)
			return err;
		for (j = 0; j < i; j++)
			if (!strcmp(wd24_modes[i].name, wd24_modes[j].name)) {
				*why = "duplicate name";
				return -EINVAL;
			}
	}
	*bad_name = NULL;
	return 0;
}
