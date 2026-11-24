/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Citations for the behavior that changes the picture:
 *   CR0C/CR0D load at vertical retrace: WD90C24 datasheet 10.12.
 *   3DA bit 3 is that retrace: datasheet 15.8.
 *   Start shift in doubleword mode (CR14 bit 6): wd24adv_flip.h,
 *   the same shift vga16fb_pan_var uses.
 *   PR0A (GR09) bits 0-7 add to CPU address bits 12-19: VGADOC
 *   paradise.txt, and wd24adv_flip.h.
 *   PR5 (GR0F) = 05h unlocks PR0-PR4: wd24adv_core.h / hw.c 187.
 *   std_13h bytes and the 25.175 MHz clock: wd24adv_modes.c.
 *   320-wide pixels are doubled by AR10 bit 6. Fewer than 480 lines
 *   are centred, not stretched: wd24adv_modes.c (PR19 b3:b2, live 54h).
 *   DAC channels are 6 bits: wd24adv_cmap_cache.h.
 *   Chain-4 A0000 window is GR06 = 05h in that same mode table.
 * Display-enable (3DA bit 0) follows CR01 and CR12. The blank-start
 * registers are not applied until a probe measures the duty. That
 * gap is counted as an assumption in the fidelity table, not a guess
 * that moves the latch.
 */
#include "wd24_chip.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VRAM_BYTES (1u << 20)
#define PANEL_W 640u
#define PANEL_H 480u
#define CLOCK_25 25175000u

/* wd24adv_modes.c std_13h */
static const unsigned char STD_MISC = 0x63;
static const unsigned char STD_SEQ[5] = { 0x03, 0x01, 0x0f, 0x00, 0x0e };
static const unsigned char STD_CR[25] = {
	0x5f, 0x4f, 0x50, 0x82, 0x54, 0x80, 0xbf, 0x1f,
	0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x9c, 0x8e, 0x8f, 0x28, 0x40, 0x96, 0xb9, 0xa3, 0xff
};
static const unsigned char STD_GX[9] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0f, 0xff
};
static const unsigned char STD_AR[21] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	0x41, 0x00, 0x0f, 0x00, 0x00
};

struct wd24_chip {
	unsigned char cr[256];
	unsigned char gr[256];
	unsigned char sr[256];
	unsigned char ar[32];
	unsigned char misc;
	unsigned char cr_i, gr_i, sr_i;
	int ar_flip;
	int ar_idx;
	unsigned char dac[256][3];
	int dac_idx;
	int dac_comp;
	unsigned char *vram;
	unsigned char lat_c, lat_d;
	int owed;
	unsigned dot;
	int last_b3;
	unsigned unknown, refused, unmodeled, latch_to, latches;
	unsigned stall_ns;
	char log[8192];
	size_t logn;
};

static void note(struct wd24_chip *c, const char *fmt, ...)
{
	va_list ap;
	int n;
	char line[160];

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(line))
		n = (int)sizeof(line) - 1;
	if (c->logn + (size_t)n + 1 >= sizeof(c->log)) {
		c->unknown++;
		return;
	}
	memcpy(c->log + c->logn, line, (size_t)n);
	c->logn += (size_t)n;
	c->log[c->logn++] = '\n';
	c->log[c->logn] = 0;
}

struct wd24_chip *wd24_chip_new(void)
{
	struct wd24_chip *c = calloc(1, sizeof(*c));

	if (!c)
		return NULL;
	c->vram = calloc(1, VRAM_BYTES);
	if (!c->vram) {
		free(c);
		return NULL;
	}
	c->last_b3 = -1;
	return c;
}

void wd24_chip_free(struct wd24_chip *c)
{
	if (!c)
		return;
	free(c->vram);
	free(c);
}

static int timing_ok(const struct wd24_chip *c, unsigned *hd, unsigned *vl,
		     unsigned *vd, unsigned *vs, unsigned *vw)
{
	unsigned cr7, hchars, vtotal, vde, vre, vend;

	if ((c->misc & 0x0c) != 0)
		return 0;
	hchars = (unsigned)c->cr[0x00] + 5u;
	if (hchars < 2 || hchars > 256)
		return 0;
	cr7 = c->cr[0x07];
	vtotal = (unsigned)c->cr[0x06] + ((cr7 & 0x01) << 8) +
		 ((cr7 & 0x20) << 4) + 2u;
	vde = (unsigned)c->cr[0x12] + ((cr7 & 0x02) << 7) +
	      ((cr7 & 0x40) << 3);
	vre = (unsigned)c->cr[0x10] + ((cr7 & 0x04) << 6) +
	      ((cr7 & 0x80) << 2);
	vend = c->cr[0x11] & 0x0f;
	if (vtotal < 2 || vtotal > 2048 || vde >= vtotal || vre >= vtotal)
		return 0;
	*hd = hchars * 8u;
	*vl = vtotal;
	*vd = vde + 1u;
	*vs = vre;
	if (vend > (vre & 0x0f))
		*vw = vend - (vre & 0x0f);
	else
		*vw = 16u - (vre & 0x0f) + vend;
	if (*vs + *vw > vtotal || *vw == 0)
		return 0;
	return 1;
}

unsigned wd24_chip_h_dots(const struct wd24_chip *c)
{
	unsigned hd, vl, vd, vs, vw;

	if (!timing_ok(c, &hd, &vl, &vd, &vs, &vw))
		return 0;
	return hd;
}

unsigned wd24_chip_v_lines(const struct wd24_chip *c)
{
	unsigned hd, vl, vd, vs, vw;

	if (!timing_ok(c, &hd, &vl, &vd, &vs, &vw))
		return 0;
	return vl;
}

unsigned wd24_chip_v_disp(const struct wd24_chip *c)
{
	unsigned hd, vl, vd, vs, vw;

	if (!timing_ok(c, &hd, &vl, &vd, &vs, &vw))
		return 0;
	return vd;
}

unsigned wd24_chip_field_millihz(const struct wd24_chip *c)
{
	unsigned hd, vl, vd, vs, vw;
	unsigned long long den;

	if (!timing_ok(c, &hd, &vl, &vd, &vs, &vw))
		return 0;
	den = (unsigned long long)hd * vl;
	if (!den)
		return 0;
	return (unsigned)(((unsigned long long)CLOCK_25 * 1000ull) / den);
}

static int in_vsync(const struct wd24_chip *c)
{
	unsigned hd, vl, vd, vs, vw, line;

	if (!timing_ok(c, &hd, &vl, &vd, &vs, &vw))
		return 0;
	line = c->dot / hd;
	return line >= vs && line < vs + vw;
}

static int in_active(const struct wd24_chip *c)
{
	unsigned hd, vl, vd, vs, vw, line, col;

	if (!timing_ok(c, &hd, &vl, &vd, &vs, &vw))
		return 0;
	line = c->dot / hd;
	col = c->dot % hd;
	return line < vd && col < ((unsigned)c->cr[0x01] + 1u) * 8u;
}

static void latch_edge(struct wd24_chip *c, int b3)
{
	if (c->last_b3 == 0 && b3 && c->owed) {
		c->lat_c = c->cr[0x0c];
		c->lat_d = c->cr[0x0d];
		c->owed = 0;
		c->latches++;
	}
	c->last_b3 = b3;
}

int wd24_chip_inb(struct wd24_chip *c, unsigned port)
{
	int b3, b0;

	if (!c)
		return -1;
	switch (port) {
	case 0x3cc:
		return c->misc;
	case 0x3c5:
		return c->sr[c->sr_i];
	case 0x3cf:
		return c->gr[c->gr_i];
	case 0x3d5:
		return c->cr[c->cr_i];
	case 0x3c1:
		return c->ar[c->ar_idx & 31];
	case 0x3c9:
		if (c->dac_comp < 0 || c->dac_comp > 2)
			return 0;
		return c->dac[c->dac_idx & 255][c->dac_comp++];
	case 0x3da:
		b3 = in_vsync(c);
		b0 = !in_active(c);
		latch_edge(c, b3);
		c->ar_flip = 0;
		return (b3 ? 0x08 : 0) | (b0 ? 0x01 : 0);
	default:
		c->unknown++;
		note(c, "inb %03x unknown", port);
		return -1;
	}
}

int wd24_chip_outb(struct wd24_chip *c, unsigned port, unsigned val)
{
	unsigned idx;

	if (!c)
		return -1;
	val &= 0xff;
	switch (port) {
	case 0x3c2:
		if ((val & 0x0c) != 0) {
			c->unmodeled++;
			note(c, "misc clock %02x not 25.175 MHz", val);
			c->misc = (unsigned char)val;
			return -1;
		}
		c->misc = (unsigned char)val;
		return 0;
	case 0x3c4:
		c->sr_i = (unsigned char)val;
		return 0;
	case 0x3c5:
		idx = c->sr_i;
		if (idx <= 4) {
			c->sr[idx] = (unsigned char)val;
			return 0;
		}
		if (idx == 0x06) {
			unsigned unlock = (c->sr[6] & 0xef) | 0x48;

			if (val == unlock || val == c->sr[6]) {
				c->sr[6] = (unsigned char)val;
				return 0;
			}
			c->refused++;
			note(c, "SR06 %02x refused", val);
			return -1;
		}
		c->unmodeled++;
		note(c, "SR%02x=%02x unmodeled", idx, val);
		c->sr[idx] = (unsigned char)val;
		return -1;
	case 0x3ce:
		c->gr_i = (unsigned char)val;
		return 0;
	case 0x3cf:
		idx = c->gr_i;
		if (idx <= 8) {
			c->gr[idx] = (unsigned char)val;
			return 0;
		}
		if (idx == 0x09) {
			if (c->gr[0x0f] != 0x05) {
				c->refused++;
				note(c, "PR0A write while PR5=%02x", c->gr[0x0f]);
				return -1;
			}
			c->gr[9] = (unsigned char)val;
			return 0;
		}
		if (idx == 0x0f) {
			if (val == 0x05) {
				c->gr[0x0f] = 0x05;
				return 0;
			}
			c->refused++;
			note(c, "PR5 %02x refused", val);
			return -1;
		}
		c->unmodeled++;
		note(c, "GR%02x=%02x unmodeled", idx, val);
		c->gr[idx] = (unsigned char)val;
		return -1;
	case 0x3d4:
		c->cr_i = (unsigned char)val;
		return 0;
	case 0x3d5:
		idx = c->cr_i;
		if (idx == 0x0c || idx == 0x0d) {
			c->cr[idx] = (unsigned char)val;
			c->owed = 1;
			return 0;
		}
		if (idx <= 0x18) {
			c->cr[idx] = (unsigned char)val;
			return 0;
		}
		if (idx == 0x29 && val == 0x85) {
			c->cr[0x29] = 0x85;
			return 0;
		}
		c->unmodeled++;
		note(c, "CR%02x=%02x unmodeled", idx, val);
		c->cr[idx] = (unsigned char)val;
		return -1;
	case 0x3c0:
		if (!c->ar_flip) {
			c->ar_idx = (int)(val & 0x1f);
			c->ar_flip = 1;
			return 0;
		}
		c->ar_flip = 0;
		if (c->ar_idx >= 0 && c->ar_idx < 32) {
			c->ar[c->ar_idx] = (unsigned char)val;
			return 0;
		}
		c->unknown++;
		note(c, "AR index %d unknown", c->ar_idx);
		return -1;
	case 0x3c8:
		c->dac_idx = (int)val;
		c->dac_comp = 0;
		return 0;
	case 0x3c9:
		if (c->dac_comp < 0 || c->dac_comp > 2) {
			c->unknown++;
			note(c, "DAC write with no index");
			return -1;
		}
		c->dac[c->dac_idx & 255][c->dac_comp++] = (unsigned char)(val & 0x3f);
		if (c->dac_comp == 3) {
			c->dac_comp = 0;
			c->dac_idx = (c->dac_idx + 1) & 255;
		}
		return 0;
	case 0x3c7:
		c->dac_idx = (int)val;
		c->dac_comp = 0;
		return 0;
	default:
		c->unknown++;
		note(c, "outb %03x=%02x unknown", port, val);
		return -1;
	}
}

static int chain4_window(const struct wd24_chip *c)
{
	return (c->sr[4] & 0x08) && c->gr[6] == 0x05;
}

int wd24_chip_mem_write(struct wd24_chip *c, unsigned off,
			const unsigned char *p, unsigned n)
{
	unsigned i, addr;

	if (!c || !p)
		return -1;
	if (!chain4_window(c)) {
		c->unmodeled++;
		note(c, "VRAM write outside chain-4 GR06=05");
		return -1;
	}
	if (off >= 65536u || n > 65536u - off) {
		c->unknown++;
		note(c, "VRAM write off %u n %u", off, n);
		return -1;
	}
	for (i = 0; i < n; i++) {
		addr = ((unsigned)c->gr[9] << 12) + off + i;
		if (addr >= VRAM_BYTES) {
			c->unknown++;
			note(c, "VRAM address %u past 1 MiB", addr);
			return -1;
		}
		c->vram[addr] = p[i];
	}
	return 0;
}

int wd24_chip_mem_read(struct wd24_chip *c, unsigned off,
		       unsigned char *p, unsigned n)
{
	unsigned i, addr;

	if (!c || !p)
		return -1;
	if (!chain4_window(c)) {
		c->unmodeled++;
		note(c, "VRAM read outside chain-4 GR06=05");
		return -1;
	}
	if (off >= 65536u || n > 65536u - off) {
		c->unknown++;
		note(c, "VRAM read off %u n %u", off, n);
		return -1;
	}
	for (i = 0; i < n; i++) {
		addr = ((unsigned)c->gr[9] << 12) + off + i;
		if (addr >= VRAM_BYTES) {
			c->unknown++;
			note(c, "VRAM address %u past 1 MiB", addr);
			return -1;
		}
		p[i] = c->vram[addr];
	}
	return 0;
}

int wd24_chip_program_13h(struct wd24_chip *c)
{
	unsigned i;

	if (wd24_chip_outb(c, 0x3c2, STD_MISC))
		return -1;
	for (i = 0; i < 5; i++) {
		if (wd24_chip_outb(c, 0x3c4, i) ||
		    wd24_chip_outb(c, 0x3c5, STD_SEQ[i]))
			return -1;
	}
	for (i = 0; i < 25; i++) {
		if (wd24_chip_outb(c, 0x3d4, i) ||
		    wd24_chip_outb(c, 0x3d5, STD_CR[i]))
			return -1;
	}
	for (i = 0; i < 9; i++) {
		if (wd24_chip_outb(c, 0x3ce, i) ||
		    wd24_chip_outb(c, 0x3cf, STD_GX[i]))
			return -1;
	}
	if (wd24_chip_inb(c, 0x3da) < 0)
		return -1;
	for (i = 0; i < 21; i++) {
		if (wd24_chip_outb(c, 0x3c0, i) ||
		    wd24_chip_outb(c, 0x3c0, STD_AR[i]))
			return -1;
	}
	if (wd24_chip_outb(c, 0x3c0, 0x20))
		return -1;
	return wd24_chip_field_millihz(c) ? 0 : -1;
}

int wd24_chip_wait_latch(struct wd24_chip *c)
{
	unsigned hd, vl, vd, vs, vw, n, span;

	if (!c)
		return 0;
	if (!c->owed)
		return 1;
	if (!timing_ok(c, &hd, &vl, &vd, &vs, &vw)) {
		c->latch_to++;
		note(c, "latch with no CRTC timing");
		return 0;
	}
	span = hd * vl;
	for (n = 0; n < vl * 2u; n++) {
		c->dot = (c->dot + hd) % span;
		if (wd24_chip_inb(c, 0x3da) < 0) {
			c->latch_to++;
			return 0;
		}
		if (!c->owed)
			return 1;
	}
	c->latch_to++;
	note(c, "latch timeout");
	return 0;
}

static unsigned start_byte(const struct wd24_chip *c)
{
	unsigned unit = ((unsigned)c->lat_c << 8) | c->lat_d;

	if (c->cr[0x14] & 0x40)
		return unit << 2;
	return unit << 1;
}

int wd24_chip_render(struct wd24_chip *c, unsigned char *rgb)
{
	unsigned rows, rep, pitch, base, y0, row, sub, col, x, y, idx;
	unsigned char r, g, b;

	if (!c || !rgb)
		return -1;
	memset(rgb, 0, PANEL_W * PANEL_H * 3u);
	if (!(c->cr[0x14] & 0x40) || c->cr[0x13] != 0x28) {
		c->unmodeled++;
		note(c, "render pitch is not mode 13h");
		return -1;
	}
	rep = (c->cr[0x09] & 0x1f) + 1u;
	rows = 200;
	if (rows * rep > PANEL_H) {
		c->unmodeled++;
		note(c, "active lines %u exceed the panel", rows * rep);
		return -1;
	}
	if (!(c->ar[0x10] & 0x40)) {
		c->unmodeled++;
		note(c, "AR10 bit 6 clear, no pixel double");
		return -1;
	}
	pitch = 320;
	base = start_byte(c);
	y0 = (PANEL_H - rows * rep) / 2u;
	for (row = 0; row < rows; row++) {
		for (sub = 0; sub < rep; sub++) {
			y = y0 + row * rep + sub;
			for (col = 0; col < pitch; col++) {
				unsigned addr = base + row * pitch + col;

				if (addr >= VRAM_BYTES) {
					c->unknown++;
					note(c, "render address %u", addr);
					return -1;
				}
				idx = c->vram[addr];
				r = (unsigned char)((c->dac[idx][0] * 255u) / 63u);
				g = (unsigned char)((c->dac[idx][1] * 255u) / 63u);
				b = (unsigned char)((c->dac[idx][2] * 255u) / 63u);
				for (x = 0; x < 2u; x++) {
					unsigned o = (y * PANEL_W + col * 2u + x) * 3u;

					rgb[o] = r;
					rgb[o + 1] = g;
					rgb[o + 2] = b;
				}
			}
		}
	}
	return 0;
}

unsigned wd24_chip_unknown(const struct wd24_chip *c) { return c ? c->unknown : 0; }
unsigned wd24_chip_refused(const struct wd24_chip *c) { return c ? c->refused : 0; }
unsigned wd24_chip_unmodeled(const struct wd24_chip *c) { return c ? c->unmodeled : 0; }
unsigned wd24_chip_latch_to(const struct wd24_chip *c) { return c ? c->latch_to : 0; }
unsigned wd24_chip_latches(const struct wd24_chip *c) { return c ? c->latches : 0; }
unsigned char wd24_chip_lat_cr0c(const struct wd24_chip *c) { return c ? c->lat_c : 0; }
unsigned char wd24_chip_lat_cr0d(const struct wd24_chip *c) { return c ? c->lat_d : 0; }
unsigned char wd24_chip_pr0a(const struct wd24_chip *c) { return c ? c->gr[9] : 0; }
const char *wd24_chip_log(const struct wd24_chip *c) { return c ? c->log : ""; }

void wd24_chip_set_stall_ns(struct wd24_chip *c, unsigned ns)
{
	if (!c)
		return;
	c->stall_ns = ns;
	if (ns)
		note(c, "VRAM stall %u ns/byte is an assumption", ns);
}

unsigned wd24_chip_stall_ns(const struct wd24_chip *c)
{
	return c ? c->stall_ns : 0;
}
