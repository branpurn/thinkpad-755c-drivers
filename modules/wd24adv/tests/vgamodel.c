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
	if (m->sr[4] & 0x08) {		/* chain 4 */
		if (m->sr[2] & (1 << (off & 3)))
			m->vram[off & 3][(off >> 2) & 0xffff] = v;
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

	if (m->sr[4] & 0x08)
		return m->vram[off & 3][(off >> 2) & 0xffff];
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
