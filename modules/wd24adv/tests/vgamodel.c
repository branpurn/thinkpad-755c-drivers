// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <string.h>
#include "vgamodel.h"

static unsigned int rnd_state;
static u8 rnd(void)
{
	rnd_state = rnd_state * 1103515245u + 12345u;
	return rnd_state >> 16;
}

/* the 755C text console after POST (wd24_boot_model) */
void vm_init(struct vgamodel *m, enum vm_profile p, unsigned int seed)
{
	unsigned int i, pl;

	memset(m, 0, sizeof(*m));
	m->prof = p;
	m->stuck_rf = -1;
	m->ro_plane = -1;
	for (i = 0; i < wd24_boot_model_n; i++) {
		const struct wd24_model_reg *r = &wd24_boot_model[i];

		switch (r->rf) {
		case WD24_SR: m->sr[r->idx] = r->val; break;
		case WD24_GR: m->gr[r->idx] = r->val; break;
		case WD24_CR: m->cr[r->idx] = r->val; break;
		case WD24_AR: m->ar[r->idx] = r->val; break;
		case WD24_MISC: m->misc = r->val; break;
		default: break;
		}
	}
	if (p == VM_QEMU) {
		memset(m->sr + 8, 0, sizeof(m->sr) - 8);
		memset(m->gr + 9, 0, sizeof(m->gr) - 9);
		memset(m->cr + 0x19, 0, sizeof(m->cr) - 0x19);
		m->sr[6] = 0;
	}
	m->ari = 0x20;
	m->ar_ff = true;	/* after "3DA; 3C0=20" */
	m->pel = 0xff;
	m->sri = 0x01; m->gri = 0x06; m->cri = 0x0e;
	rnd_state = seed;
	for (i = 0; i < 768; i++)
		m->dac[i] = rnd() & 0x3f;
	for (pl = 0; pl < 4; pl++)
		for (i = 0; i < WD24_PLANE; i++)
			m->vram[pl][i] = rnd();
}

static bool sr_unlocked(const struct vgamodel *m)
{
	return m->prof == VM_WD24 && (m->sr[6] & 0x58) == 0x48;
}

static u8 sr_idx_mask(const struct vgamodel *m)
{
	return sr_unlocked(m) ? 0xff : 0x07;	/* wd24probe3: 11 -> 01 locked */
}

static void violate(struct vgamodel *m, const char *rf, u8 idx, u8 v)
{
	m->violations++;
	snprintf(m->violation, sizeof(m->violation), "%s%02X=%02X", rf, idx, v);
}

static bool stuck(const struct vgamodel *m, int rf, u8 idx)
{
	return m->stuck_rf == rf && m->stuck_idx == idx;
}

static void wr_sr(struct vgamodel *m, u8 v)
{
	u8 i = m->sri;

	if (m->prof == VM_QEMU && i > 7)
		return;
	if (i == 0x19 || (i >= 0x30 && i <= 0x34 && i != 0x31))
		violate(m, "SR", i, v);
	if (stuck(m, WD24_SR, i))
		return;
	m->sr[i] = v;
}

static void wr_gr(struct vgamodel *m, u8 v)
{
	u8 i = m->gri;

	if (i > 8 && (m->prof == VM_QEMU || i > 0x0f))
		return;
	if (i >= 0x09 && i <= 0x0e && m->gr[0x0f] != 0x05 && m->prof == VM_WD24)
		return;			/* PR0-PR4 locked by PR5 */
	if (i == 0x0c)			/* PR2 */
		violate(m, "GR", i, v);
	if (i == 0x0b && ((m->gr[i] ^ v) & 0x30))
		violate(m, "GR", i, v);
	if (i == 0x0e && ((m->gr[i] ^ v) & 0x20))
		violate(m, "GR", i, v);
	if (stuck(m, WD24_GR, i))
		return;
	m->gr[i] = v;
}

static void wr_cr(struct vgamodel *m, u8 v)
{
	u8 i = m->cri;

	if (m->prof == VM_QEMU && i > 0x18)
		return;
	if (i == 0x31 || i == 0x32 || i == 0x33 || i == 0x35 || i == 0x3a ||
	    (i >= 0x3b && i <= 0x3f) || (i == 0x34 && (v & 0x07) == 0x06))
		violate(m, "CR", i, v);
	if (i <= 0x07 && (m->cr[0x11] & 0x80) && i != 0x07)
		return;			/* CR11 b7 protect */
	if (i > 0x29 && i != 0x34 && (m->cr[0x29] & 0x8f) != 0x85)
		return;			/* PR11.. locked by PR10 */
	if (stuck(m, WD24_CR, i))
		return;
	m->cr[i] = v;
}

static u8 rd_cr(const struct vgamodel *m)
{
	u8 i = m->cri;

	if (stuck(m, WD24_CR, i))
		return m->stuck;
	if (m->prof == VM_QEMU && i > 0x18)
		return 0;
	if ((i == 0x31 || i == 0x32) && (m->cr[0x34] & 0xf0) != 0xa0)
		return 0xff;		/* PR18/PR19 closed */
	return m->cr[i];
}

static void eng_violate(struct vm_eng *e, const char *what)
{
	e->violations++;
	snprintf(e->violation, sizeof(e->violation), "%s", what);
}

static u8 rot(u8 v, u8 n)
{
	n &= 7;
	return n ? (u8)((v >> n) | (v << (8 - n))) : v;
}

static void memw(void *priv, u32 off, u8 v)
{
	struct vgamodel *m = priv;
	int p;

	m->mem_writes++;
	if (m->eng && m->eng->busy)
		eng_violate(m->eng, "CPU VRAM write while BITBLT busy"), m->eng->cpu_busy++;
	if (m->sr[4] & 0x08) {		/* chain 4 */
		u32 lin = ((m->gr[0x09] & 0x7f) << 12) + (off & 0xffff);

		if (m->sr[2] & (1 << (lin & 3))) {
			if (lin >> 18)
				m->vram_hi[lin & 3][(lin >> 2) & 0xffff] = v;
			else
				m->vram[lin & 3][lin >> 2] = v;
		}
		return;
	}
	for (p = 0; p < 4; p++) {	/* write mode 0 (all wd24adv uses) */
		u8 d = rot(v, m->gr[3]);

		if (!(m->sr[2] & (1 << p)))
			continue;
		if (m->gr[1] & (1 << p))
			d = (m->gr[0] & (1 << p)) ? 0xff : 0x00;
		switch ((m->gr[3] >> 3) & 3) {
		case 1: d &= m->latch[p]; break;
		case 2: d |= m->latch[p]; break;
		case 3: d ^= m->latch[p]; break;
		}
		d = (d & m->gr[8]) | (m->latch[p] & ~m->gr[8]);
		if (p == m->ro_plane)
			continue;
		m->vram[p][off & 0xffff] = d;
	}
}

static u8 memr(void *priv, u32 off)
{
	struct vgamodel *m = priv;
	int p;

	if (m->eng && m->eng->busy)
		eng_violate(m->eng, "CPU VRAM read while BITBLT busy"), m->eng->cpu_busy++;
	if (m->sr[4] & 0x08) {
		u32 lin = ((m->gr[0x09] & 0x7f) << 12) + (off & 0xffff);

		return lin >> 18 ? m->vram_hi[lin & 3][(lin >> 2) & 0xffff]
				 : m->vram[lin & 3][lin >> 2];
	}
	for (p = 0; p < 4; p++)
		m->latch[p] = m->vram[p][off & 0xffff];
	return m->latch[m->gr[4] & 3];
}

static void outb(void *priv, u8 v, u16 port)
{
	struct vgamodel *m = priv;
	bool idx = port == 0x3c4 || port == 0x3ce || port == 0x3d4 ||
		   port == 0x3c7 || port == 0x3c8 || (port == 0x3c0 && !m->ar_ff);

	if (idx)
		m->index_writes++;
	else
		m->data_writes++;
	switch (port) {
	case 0x3c0:
		if (!m->ar_ff)
			m->ari = v & 0x3f;
		else if (!stuck(m, WD24_AR, m->ari & 0x1f))
			m->ar[m->ari & 0x1f] = v;
		m->ar_ff = !m->ar_ff;
		break;
	case 0x3c2: m->misc = v; break;
	case 0x3c4: m->sri = v & sr_idx_mask(m); break;
	case 0x3c5: wr_sr(m, v); break;
	case 0x3c6: m->pel = v; break;
	case 0x3c7: m->dac_ri = v; m->dac_sub = 0; break;
	case 0x3c8: m->dac_wi = v; m->dac_sub = 0; break;
	case 0x3c9:
		m->dac[m->dac_wi * 3 + m->dac_sub] = v & 0x3f;
		if (++m->dac_sub == 3) { m->dac_sub = 0; m->dac_wi++; }
		break;
	case 0x3ce: m->gri = m->prof == VM_QEMU ? v & 0x0f : v; break;
	case 0x3cf: wr_gr(m, v); break;
	case 0x3d4: m->cri = v; break;
	case 0x3d5: wr_cr(m, v); break;
	default:
		m->violations++;
		snprintf(m->violation, sizeof(m->violation), "port %04X=%02X", port, v);
	}
}

static u8 inb(void *priv, u16 port)
{
	struct vgamodel *m = priv;
	u8 v;

	switch (port) {
	case 0x3c0: return m->ari;
	case 0x3c1: return stuck(m, WD24_AR, m->ari & 0x1f) ? m->stuck : m->ar[m->ari & 0x1f];
	case 0x3c4: return m->sri;
	case 0x3c5:
		if (stuck(m, WD24_SR, m->sri))
			return m->stuck;
		if (m->sri == 6)
			return m->prof == VM_QEMU ? 0 : m->sr[6] & 0x0f;
		return m->sr[m->sri];
	case 0x3c6:
		m->violations++;	/* hidden DAC register sequence */
		snprintf(m->violation, sizeof(m->violation), "read 3C6");
		return m->pel;
	case 0x3c9:
		v = m->dac[m->dac_ri * 3 + m->dac_sub];
		if (++m->dac_sub == 3) { m->dac_sub = 0; m->dac_ri++; }
		return v;
	case 0x3cc: return m->misc;
	case 0x3ce: return m->gri;
	case 0x3cf:
		if (stuck(m, WD24_GR, m->gri))
			return m->stuck;
		if (m->gri == 0x0f)
			return m->gr[0x0f] & 0x07;
		if (m->gri == 0x09 || m->gri == 0x0a)
			return m->gr[m->gri] & 0x7f;
		return m->gr[m->gri];
	case 0x3d4: return m->cri;
	case 0x3d5: return rd_cr(m);
	case 0x3da: m->ar_ff = false; return 0;
	default:
		m->violations++;
		snprintf(m->violation, sizeof(m->violation), "read port %04X", port);
		return 0xff;
	}
}

void vm_io(struct wd24_io *io, struct vgamodel *m)
{
	io->inb = inb;
	io->outb = outb;
	io->memr = memr;
	io->memw = memw;
	io->priv = m;
}

int vm_cmp(const struct vgamodel *a, const struct vgamodel *b, const char *what)
{
	static const struct { const char *n; size_t o, len; } f[] = {
#define F(x) { #x, offsetof(struct vgamodel, x), sizeof(((struct vgamodel *)0)->x) }
		F(misc), F(sr), F(gr), F(cr), F(ar), F(sri), F(gri), F(cri),
		F(ari), F(ar_ff), F(pel), F(dac), F(vram),
#undef F
	};
	unsigned int i;
	size_t k;

	for (i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
		const u8 *x = (const u8 *)a + f[i].o, *y = (const u8 *)b + f[i].o;

		for (k = 0; k < f[i].len; k++)
			if (x[k] != y[k]) {
				printf("  %s: %s[%zu] %02X != %02X\n", what, f[i].n, k, x[k], y[k]);
				return 1;
			}
	}
	return 0;
}

/* ---- 23C0h blocks ------------------------------------------------------- */

void vm_eng_init(struct vm_eng *e, unsigned int seed)
{
	unsigned int i;

	memset(e, 0, sizeof(*e));
	e->bad_blk = -1;
	e->bad_idx = -1;
	e->op_polls = 3;
	e->ix = 0x0000;		/* UNPROVEN: the BIOS value (S1 reads it) */
	rnd_state = seed;
	for (i = 0; i < WD24X_VRAM; i++)
		e->vram[i] = rnd();
}

/* DS 19.7: result bit = code bit (3 - (2s + d)), code = ROP bits 11:8 */
static u8 rop(u16 r, u8 s, u8 d)
{
	u8 code = (r >> 8) & 0xf, out = 0;
	int b;

	for (b = 0; b < 8; b++) {
		int si = (s >> b) & 1, di = (d >> b) & 1;

		out |= ((code >> (3 - (2 * si + di))) & 1) << b;
	}
	return out;
}

static u32 eng_addr(const struct vm_eng *e, int lo)
{
	return ((u32)(e->reg[1][lo + 1] & 0x1ff) << 12) | e->reg[1][lo];
}

static void pix(struct vm_eng *e, u32 a, u8 s)
{
	u8 mask = e->reg[1][WD24X_B_MASK] & 0xff;

	if (a >= WD24X_VRAM) {
		eng_violate(e, "BITBLT beyond 1 MB");
		return;
	}
	e->vram[a] = (rop(e->reg[1][WD24X_B_ROP], s, e->vram[a]) & mask) |
		     (e->vram[a] & ~mask);
}

static u32 dst_at(const struct vm_eng *e, u32 r, u32 c)
{
	u32 d = eng_addr(e, WD24X_B_DST_LO), p = e->reg[1][WD24X_B_PITCH];

	return (e->reg[1][0] & WD24X_C1_BACK) ? d - r * p - c : d + r * p + c;
}

/* one host pixel at the current position */
static void host_pix(struct vm_eng *e, u8 s, bool skip)
{
	u32 w = e->reg[1][WD24X_B_DIM_X];

	if (e->col < w && e->row < e->reg[1][WD24X_B_DIM_Y] && !skip)
		pix(e, dst_at(e, e->row, e->col), s);
	e->col++;
}

static void host_word(struct vm_eng *e, u16 v)
{
	u16 c1 = e->reg[1][0];
	u32 w = e->reg[1][WD24X_B_DIM_X], per = (w + 3) / 4, k;

	if (!e->busy || e->host_got >= e->host_need) {
		eng_violate(e, "23C4h write with no host data owed");
		return;
	}
	e->host_words++;
	if ((c1 & 0x0c) == WD24X_C1_SRCF_MONO_HOST) {
		/* DS 21.14 low 4 bits; OBS-FB: msb = leftmost */
		for (k = 0; k < 4; k++) {
			bool one = (v >> (3 - k)) & 1;

			host_pix(e, one ? e->reg[1][WD24X_B_FG] : e->reg[1][WD24X_B_BG],
				 !one && (e->reg[1][1] & WD24X_C2_MTRANS));
		}
	} else if (!e->have_lo) {	/* DS 21.12: low word first */
		e->lo = v;
		e->have_lo = true;
	} else {			/* byte order UNPROVEN: little endian */
		u32 dw = ((u32)v << 16) | e->lo;

		e->have_lo = false;
		for (k = 0; k < 4; k++)
			host_pix(e, dw >> (8 * k), false);
	}
	if (e->col >= per * 4) {
		e->col = 0;
		e->row++;
	}
	e->host_got++;
	if (e->host_got == e->host_need) {
		e->polls_left = e->host_tail;
		if (!e->polls_left && !e->stuck_busy)
			e->busy = false;
	}
}

static void start(struct vm_eng *e)
{
	u16 c1 = e->reg[1][0], c2 = e->reg[1][1];
	u32 w = e->reg[1][WD24X_B_DIM_X], h = e->reg[1][WD24X_B_DIM_Y];
	u32 s = eng_addr(e, WD24X_B_SRC_LO), r, c;
	bool back = c1 & WD24X_C1_BACK;

	if ((c1 & 0x0300) != WD24X_C1_PACKED || (c1 & 0x00f0) || (c1 & 0x01) ||
	    (c1 & 0x0c) == WD24X_C1_SRCF_MONO_CMP)
		eng_violate(e, "CNTRL1 mode not modelled");
	if (c2 & 0x0ff7)
		eng_violate(e, "CNTRL2 bit not modelled");
	if ((e->reg[1][3] | e->reg[1][5]) & 0x0e00)
		eng_violate(e, "SRC/DST hi reserved bits");
	if (e->reg[1][WD24X_B_ROP] & 0x00ff)
		eng_violate(e, "ROP reserved bits");
	if (!w || !h || w > 2048 || h > 2048)
		eng_violate(e, "dimension");
	e->ops++;
	e->busy = true;
	e->row = e->col = 0;
	e->have_lo = false;
	e->host_got = 0;
	e->polls_left = e->op_polls;
	if (c1 & WD24X_C1_SRC_IO) {
		u32 per = (w + 3) / 4;

		e->host_need = h * per * ((c1 & 0x0c) ? 1 : 2) + e->host_skew;
		return;
	}
	e->host_need = 0;
	/* hardware order: a wrong direction corrupts overlapping copies */
	for (r = 0; r < h; r++)
		for (c = 0; c < w; c++) {
			u32 sa = back ? s - r * e->reg[1][8] - c : s + r * e->reg[1][8] + c;
			u8 v = (c1 & 0x0c) == WD24X_C1_SRCF_FIXED ? e->reg[1][WD24X_B_FG]
				: sa < WD24X_VRAM ? e->vram[sa] : 0;

			if ((c1 & 0x0c) != WD24X_C1_SRCF_FIXED && sa >= WD24X_VRAM)
				eng_violate(e, "source beyond 1 MB");
			pix(e, dst_at(e, r, c), v);
		}
}

static void eng_out(struct vgamodel *m, u16 v, u16 port)
{
	struct vm_eng *e = m->eng;
	u8 blk = e->ix & 0xff, idx = v >> 12;

	switch (port) {
	case WD24X_P_INDEX:
		e->ix_writes++;
		if (v & 0xe000)
			eng_violate(e, "23C0h reserved/read-only bits");
		e->ix = v & 0x1fff;
		return;
	case WD24X_P_DATA:
		e->data_writes++;
		if (blk == 0 || blk > 2) {
			eng_violate(e, "23C2h write to block 0 / missing block");
			return;
		}
		if (blk == 2 && idx > WD24X_C_AUX) {
			eng_violate(e, "cursor index past aux");
			return;
		}
		if (blk == 1 && e->busy) {
			eng_violate(e, idx ? "BITBLT register write while busy"
					   : "CNTRL1 write while busy (abort)");
			return;
		}
		e->reg[blk][idx] = v & 0x0fff;
		if (blk == 1 && idx == 0 && (v & WD24X_C1_ACT))
			start(e);
		if (blk == 1 && (e->reg[1][1] & WD24X_C2_QUICKSTART))
			eng_violate(e, "quick start armed");
		return;
	case WD24X_P_HOST:
		host_word(e, v);
		return;
	}
	eng_violate(e, "unknown 16-bit port");
}

static u16 eng_in(struct vgamodel *m, u16 port)
{
	struct vm_eng *e = m->eng;
	u8 blk = e->ix & 0xff, idx = (e->ix >> 8) & 0xf;
	u16 v;

	e->reads++;
	if (port == WD24X_P_INDEX)
		return (e->ix ^ (e->bad_ix ? 0x0100 : 0)) | (blk > 2 ? WD24X_IX_INVALID : 0);
	if (port != WD24X_P_DATA) {
		eng_violate(e, port == WD24X_P_HOST ? "23C4h read" : "unknown 16-bit read");
		return 0xffff;
	}
	v = (blk >= 1 && blk <= 2) ? e->reg[blk][idx] : 0;
	if (blk == 1 && idx == 0) {
		v &= ~WD24X_C1_ACT;
		if (e->busy) {
			v |= WD24X_C1_ACT;
			if (!e->stuck_busy && e->host_got >= e->host_need &&
			    e->polls_left && !--e->polls_left)
				e->busy = false;
		}
	}
	if (blk == e->bad_blk && idx == e->bad_idx)
		v ^= 1;
	v |= (u16)((idx ^ (e->bad_echo ? 1 : 0)) << 12);
	if (!(e->ix & WD24X_IX_NOINC))
		e->ix = (e->ix & 0xf0ff) | (u16)(((idx + 1) & 0xf) << 8);
	return v;
}

static void xoutw(void *priv, u16 v, u16 port)
{
	struct vgamodel *m = priv;

	if (!m->eng) {
		m->violations++;
		snprintf(m->violation, sizeof(m->violation), "port %04X=%04X", port, v);
		return;
	}
	eng_out(m, v, port);
}

static u16 xinw(void *priv, u16 port)
{
	struct vgamodel *m = priv;

	if (!m->eng) {
		m->violations++;
		snprintf(m->violation, sizeof(m->violation), "read port %04X", port);
		return 0xffff;
	}
	return eng_in(m, port);
}

void vm_xio(struct wd24x_io *io, struct vgamodel *m)
{
	io->inw = xinw;
	io->outw = xoutw;
	io->priv = m;
	io->live = false;
}
