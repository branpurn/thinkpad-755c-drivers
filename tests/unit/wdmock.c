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
	M.seq[0x00] = 0x03;		/* sequencer running */
	M.crtc[0x11] = 0x8e;		/* CRTC 0-7 write-protected (BIOS) */
	M.gfx[0x0f] = 0x00;		/* PR0-4 locked */
	M.gfx[0x0b] = 0xc5;		/* PR1: 1 MiB */
	M.crtc[0x29] = 0x00;
	M.seq[0x06] = 0x00;
	M.seq[0x31] = 0x0d;		/* PR68: VCLK sel 01, MCLK 101 */
	M.seq[0x32] = 0x8b;
	M.seq[0x19] = 0x04;		/* PR57: panel power on */
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

static void plog(char c)
{
	if (M.panel_log_n < sizeof(M.panel_log) - 1)
		M.panel_log[M.panel_log_n++] = c;
}

static void wr_seq(u8 i, u8 v)
{
	u8 mask;

	if (i == 0x19 && paradise() && pr2x_open() && v != M.seq[i])
		plog('7');
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
	if (i == 0x0e && v != M.gfx[i])
		plog('4');
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
		if (fp_open()) {
			if (i == 0x32 && v != M.fp[i])
				plog('P');
			M.fp[i] = v;
		}
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
	case 0x3da: M.attr_ff = 0; return 0x00;
	case 0x3c1: return M.attr[M.attr_i & 0x1f];
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
	case 0x3c0:
		if (!M.attr_ff) M.attr_i = v; else M.attr[M.attr_i & 0x1f] = v;
		M.attr_ff ^= 1;
		break;
	case 0x3c8: M.dac_i = v; M.dac_c = 0; break;
	case 0x3c9:
		M.dac[M.dac_i][M.dac_c] = v & 0x3f;
		if (++M.dac_c == 3) { M.dac_c = 0; M.dac_i++; }
		break;
	default: break;
	}
}

/* Execute a packed-mode BITBLT the way data sheet section 19 describes. */
static void mock_blt_run(void)
{
	u16 c1 = M.blt[0];
	u32 src = M.blt[2] | (u32)M.blt[3] << 12, dst = M.blt[4] | (u32)M.blt[5] << 12;
	u32 w = M.blt[6], h = M.blt[7], pitch = M.blt[8], r, x;
	u8 rop = M.blt[9] >> 8, fg = M.blt[10] & 0xff, mask = M.blt[14] & 0xff;
	int dir = (c1 & 0x0400) ? -1 : 1;

	if (M.blt_busy_left)
		M.blt_started_while_busy++;
	M.blt_ops++;
	if ((c1 & 0x000f) == 0x000e) {	/* mono from host: wait for data */
		M.hm_on = true;
		M.hm_r = M.hm_x = M.hm_words = 0;
		return;
	}
	for (r = 0; r < h; r++)
		for (x = 0; x < w; x++) {
			u32 so = (src + dir * (int)(r * pitch + x)) & 0xfffff;
			u32 d = (dst + dir * (int)(r * pitch + x)) & 0xfffff;
			u8 sv = (c1 & 0x000c) == 0x0008 ? fg : M.vram[so], nv;

			nv = rop == 3 ? sv : rop == 6 ? (u8)(M.vram[d] ^ sv) : M.vram[d];
			M.vram[d] = (M.vram[d] & ~mask) | (nv & mask);
		}
	M.blt_busy_left = M.blt_busy_polls;
	if (!M.blt_busy_left && !M.blt_stuck)
		M.blt[0] &= ~0x0800;
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
		if (blk == 2) {
			u8 idx = (M.ext_index >> 8) & 0x0f;

			return idx ? M.curs[idx] : M.cursor;
		}
		if (blk == 1) {
			u8 idx = (M.ext_index >> 8) & 0x0f;

			if (idx == 0 && M.hm_on)
				return M.blt[0] | 0x0800;
			if (idx == 0 && (M.blt[0] & 0x0800) && !M.blt_stuck) {
				if (M.blt_busy_left)
					M.blt_busy_left--;
				else
					M.blt[0] &= ~0x0800;
				return M.blt[0] | 0x0800;
			}
			return M.blt[idx];
		}
	}
	return 0xffff;
}

static void mock_hm_word(u16 v)
{
	u32 dst = M.blt[4] | (u32)M.blt[5] << 12, w = M.blt[6], h = M.blt[7];
	u32 pitch = M.blt[8], ppw = 4u << (M.cexp_var >> 2), k, words;
	bool lsb = M.cexp_var & 2, trans = M.blt[1] & 0x0008;

	if (!M.hm_on) {
		M.hm_stray++;
		return;
	}
	words = (w + ppw - 1) / ppw;
	if (M.cexp_var & 1)
		words = (words + 1) & ~1u;
	for (k = 0; k < ppw; k++, M.hm_x++) {
		bool bit = lsb ? (v >> k) & 1 : (v >> (ppw - 1 - k)) & 1;
		u32 d = (dst + M.hm_r * pitch + M.hm_x) & 0xfffff;

		if (M.hm_x >= w)
			continue;
		if (bit)
			M.vram[d] = M.blt[10] & 0xff;
		else if (!trans)
			M.vram[d] = M.blt[11] & 0xff;
	}
	if (++M.hm_words == words) {
		M.hm_words = M.hm_x = 0;
		if (++M.hm_r == h) {
			M.hm_on = false;
			M.blt[0] &= ~0x0800;
		}
	}
}

void wd_outw(u16 v, u16 p)
{
	if (p == 0x23c4) {
		if (has_ext())
			mock_hm_word(v);
		return;
	}
	if (p == 0x23c0 || p == 0x23c2)
		M.ext_port_writes++;
	if (!has_ext())
		return;
	if (p == 0x23c0) {
		M.ext_index = v;
	} else if (p == 0x23c2) {
		u8 blk = M.ext_index & 0xff;
		if (blk == 2) {
			M.curs[v >> 12] = v & 0x0fff;
			if (!(v >> 12))
				M.cursor = v;
		}
		else if (blk == 1) {
			M.blt[v >> 12] = v & 0x0fff;
			if ((v >> 12) == 0 && (v & 0x0800))
				mock_blt_run();
		}
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

/*
 * Data sheet 13.1: with the 64 KiB map (GR6 bits 3:2 = 01), PR1 bit 3 = 0
 * and SR11 bit 7 = 0, PR0(A) offsets the whole window in 4 KiB units. With
 * PR1 bit 3 set, PR0(B) covers A0000-A7FFF and PR0(A) A8000-AFFFF. Plain
 * VGA ignores the offset registers (no extra memory).
 */
u32 mock_win_addr(u32 off)
{
	u32 a;

	if (off >= 0x10000)
		M.win_oob++;
	off &= 0xffff;
	if (M.kind == M_VGA || (M.gfx[6] & 0x0c) != 0x04)
		return off;
	if (M.gfx[0x0b] & 0x08)
		a = ((off < 0x8000 ? M.gfx[0x0a] : M.gfx[0x09]) << 12) + off;
	else
		a = (M.gfx[0x09] << 12) + off;
	return a & ((1 << 20) - 1);
}

void wd_win_write(struct wd_hw *hw, u32 off, const void *src, size_t n)
{
	const u8 *p = src;
	size_t i;

	(void)hw;
	for (i = 0; i < n; i++)
		M.vram[mock_win_addr(off + i)] = p[i];
}

void wd_win_writeb(struct wd_hw *hw, u32 off, u8 v)
{
	(void)hw;
	M.vram[mock_win_addr(off)] = v;
}

u8 wd_win_readb(struct wd_hw *hw, u32 off)
{
	(void)hw;
	return M.vram[mock_win_addr(off)];
}
