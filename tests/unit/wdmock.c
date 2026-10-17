// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <string.h>
#include "wdmock.h"

struct mock M;

static bool paradise(void) { return M.kind != M_VGA; }
static bool has_fp(void) { return M.kind == M_C24 || M.kind == M_C24A || M.kind == M_C20; }
static bool has_ext(void) { return M.kind == M_C24A || M.kind == M_C31; }
static bool pr04_open(void) { return (M.gfx[0x0f] & 0x07) == 0x05; }
static bool pr1x_open(void) { return (M.crtc[0x29] & 0x8f) == 0x85; }
static bool pr2x_open(void) { return (M.seq[0x06] & 0x58) == 0x48; }
static bool fp_open(void) { return (M.pr1b & 0xe0) == 0xa0; }

void mock_reset(enum mock_kind k)
{
	memset(&M, 0, sizeof(M));
	M.kind = k;
	M.misc = 0x67;			/* colour, 28 MHz text clock */
	M.crtc[0x11] = 0x8e;		/* CRTC 0-7 write-protected (BIOS) */
	M.gfx[0x0f] = 0x00;		/* PR0-4 locked */
	M.gfx[0x0b] = 0xc5;		/* PR1: 1 MiB */
	M.crtc[0x29] = 0x00;
	M.seq[0x06] = 0x00;
	M.seq[0x31] = 0x0d;		/* PR68: VCLK sel 01, MCLK 101 */
	M.seq[0x32] = 0x8b;
	M.fp[0x31] = 0x0c;		/* PR18: single-scan TFT */
	M.fp[0x32] = 0x30;		/* PR19: LCD + CRT on */
	M.pr1b = 0x00;
	M.cursor = 0x0000;
}

static int seq_writable(u8 i, u8 *mask)
{
	*mask = 0xff;
	if (i <= 0x06)
		return 1;
	if (!paradise() || !pr2x_open())
		return 0;
	if (i == 0x07 && M.kind == M_C00)
		*mask = 0x0f;
	if (i == 0x10 && (M.kind == M_C00 || M.kind == M_C20 || M.kind == M_PVGA1))
		return 0;
	if (i == 0x14 && (M.kind == M_C1X || M.kind == M_C00 || M.kind == M_PVGA1))
		return 0;
	if ((i == 0x31 || i == 0x32) && (M.kind == M_C24 || M.kind == M_C24A))
		return M.seq[0x35] == 0x50;
	return i < 0x40;
}

static void wr_seq(u8 i, u8 v)
{
	u8 mask;

	if (i <= 4)
		M.timing_writes++;
	if (!paradise() && i > 4)
		return;
	if (seq_writable(i, &mask))
		M.seq[i] = (M.seq[i] & ~mask) | (v & mask);
}

static u8 rd_seq(u8 i)
{
	if (!paradise() && i > 4)
		return 0;
	return M.seq[i];
}

static void wr_gfx(u8 i, u8 v)
{
	if (!paradise() && i > 8)
		return;
	if (i >= 0x09 && i <= 0x0e && !pr04_open())
		return;
	M.gfx[i] = v;
}

static u8 rd_gfx(u8 i)
{
	if (!paradise() && i > 8)
		return 0;
	return M.gfx[i];
}

static void wr_crtc(u8 i, u8 v)
{
	if (i <= 0x18 && i != 0x11) {
		M.timing_writes++;
		if (i <= 7 && (M.crtc[0x11] & 0x80))
			return;
	}
	if (!paradise() && i > 0x18)
		return;
	if (i == 0x34 && has_fp()) {
		M.pr1b = v;
		return;
	}
	if ((i == 0x31 || i == 0x32) && has_fp()) {
		if (fp_open())
			M.fp[i] = v;
		return;
	}
	if (i >= 0x2a && i <= 0x30) {
		if (!pr1x_open())
			return;
		if (i == 0x2b && M.kind == M_PVGA1)
			return;
	}
	if (i >= 0x36 && i <= 0x37)
		return;
	M.crtc[i] = v;
}

static u8 rd_crtc(u8 i)
{
	const char *sig = NULL;

	if (!paradise() && i > 0x18)
		return 0;
	if (i == 0x2b && M.kind == M_PVGA1)
		return 0xff;
	if (i == 0x34 && has_fp())
		return M.pr1b;
	if ((i == 0x31 || i == 0x32) && has_fp())
		return fp_open() ? M.fp[i] : 0x00;
	switch (M.kind) {
	case M_C24: case M_C24A: sig = "24"; break;
	case M_C30: sig = "30"; break;
	case M_C31: sig = "31"; break;
	default: break;
	}
	if (sig && (i == 0x36 || i == 0x37))
		return (has_fp() && fp_open()) ? M.fp[i] : (u8)sig[i - 0x36];
	return M.crtc[i];
}

u8 wd_inb(u16 p)
{
	switch (p) {
	case 0x3cc: return M.misc;
	case 0x3c4: return M.seq_i;
	case 0x3c5: return rd_seq(M.seq_i);
	case 0x3ce: return M.gfx_i;
	case 0x3cf: return rd_gfx(M.gfx_i);
	case 0x3d4: return M.crtc_i;
	case 0x3d5: return rd_crtc(M.crtc_i);
	default: return 0xff;
	}
}

void wd_outb(u8 v, u16 p)
{
	switch (p) {
	case 0x3c2: M.misc = v; M.timing_writes++; break;
	case 0x3c4: M.seq_i = v; break;
	case 0x3c5: wr_seq(M.seq_i, v); break;
	case 0x3ce: M.gfx_i = v; break;
	case 0x3cf: wr_gfx(M.gfx_i, v); break;
	case 0x3d4: M.crtc_i = v; break;
	case 0x3d5: wr_crtc(M.crtc_i, v); break;
	default: break;
	}
}

u16 wd_inw(u16 p)
{
	if (!has_ext())
		return 0xffff;
	if (p == 0x23c0) {
		u8 blk = M.ext_index & 0xff;
		return (M.ext_index & 0x1fff) | ((blk == 1 || blk == 2) ? 0 : 0x2000);
	}
	if (p == 0x23c2) {
		u8 blk = M.ext_index & 0xff;
		if (blk == 2)
			return M.cursor;
		if (blk == 1)
			return M.blt[(M.ext_index >> 8) & 0x0f];
	}
	return 0xffff;
}

void wd_outw(u16 v, u16 p)
{
	if (p == 0x23c0 || p == 0x23c2)
		M.ext_port_writes++;
	if (!has_ext())
		return;
	if (p == 0x23c0) {
		M.ext_index = v;
	} else if (p == 0x23c2) {
		u8 blk = M.ext_index & 0xff;
		if (blk == 2)
			M.cursor = v;
		else if (blk == 1)
			M.blt[v >> 12] = v & 0x0fff;
	}
}

void wd_msleep(unsigned int ms) { M.sleeps_ms += ms; }
void wd_udelay(unsigned int us) { (void)us; }

void mock_snap(struct mock_snap *s)
{
	s->misc = M.misc;
	memcpy(s->seq, M.seq, 256);
	memcpy(s->gfx, M.gfx, 256);
	memcpy(s->crtc, M.crtc, 256);
	memcpy(s->fp, M.fp, 256);
	s->pr1b = M.pr1b;
	s->cursor = M.cursor;
}

int mock_snap_diff(const struct mock_snap *a, const struct mock_snap *b, char *why, size_t n)
{
	int i;

	if (a->misc != b->misc) { snprintf(why, n, "MISC %02x->%02x", a->misc, b->misc); return 1; }
	for (i = 0; i < 256; i++) {
		if (a->seq[i] != b->seq[i]) { snprintf(why, n, "SR%02X %02x->%02x", i, a->seq[i], b->seq[i]); return 1; }
		if (a->gfx[i] != b->gfx[i]) { snprintf(why, n, "GR%02X %02x->%02x", i, a->gfx[i], b->gfx[i]); return 1; }
		if (a->crtc[i] != b->crtc[i]) { snprintf(why, n, "CR%02X %02x->%02x", i, a->crtc[i], b->crtc[i]); return 1; }
		if (a->fp[i] != b->fp[i]) { snprintf(why, n, "FP%02X %02x->%02x", i, a->fp[i], b->fp[i]); return 1; }
	}
	if (a->pr1b != b->pr1b) { snprintf(why, n, "PR1B %02x->%02x", a->pr1b, b->pr1b); return 1; }
	if (a->cursor != b->cursor) { snprintf(why, n, "cursor %04x->%04x", a->cursor, b->cursor); return 1; }
	return 0;
}
