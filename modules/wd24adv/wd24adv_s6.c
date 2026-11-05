// SPDX-License-Identifier: GPL-2.0
/* wd24adv S6 dual-window proof; see wd24adv_s6.h */
#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/string.h>
#else
#include <stdio.h>
#include <string.h>
#endif
#include "wd24adv_s6.h"

#define LOG(s, ...) do { if ((s)->loglen < WD24S6_LOG) (s)->loglen += \
	snprintf((s)->log + (s)->loglen, WD24S6_LOG - (s)->loglen, __VA_ARGS__); } while (0)

static const char *const resn[] = { "NOT RUN", "OK", "SPLIT ONLY", "RW ONLY",
	"NO DUAL WINDOW", "ENTRY CHECK", "REGISTER READBACK", "MEMORY",
	"RESTORE FAIL" };

const char *wd24s6_res_name(enum wd24s6_res r)
{
	return r <= WD24S6_RESTORE ? resn[r] : "?";
}

/* region table: bank (PR0A value in single mode), window offset */
static const struct { u8 bank; u16 off; const char *n; } R[WD24S6_NREG] = {
	{ WD24S6_Y, 0x0000, "Y+0000" }, { WD24S6_Y, 0x1000, "Y+1000" },
	{ WD24S6_Y, 0x2000, "Y+2000" }, { WD24S6_X, 0x8000, "X+8000" },
	{ WD24S6_X, 0x1000, "X+1000" },
	/* not used by a working chip; saved so that a chip which ignores PR1 b3
	 * or PR31 b7 (writes then land in X) is still restored exactly */
	{ WD24S6_X, 0x0000, "X+0000" }, { WD24S6_X, 0x2000, "X+2000" },
};

static void pat(u8 *d, u8 tag)
{
	u32 i;

	for (i = 0; i < WD24S6_N; i++)
		d[i] = (u8)(tag * 37 + i * 7 + (i >> 8) + 1);
}

static u32 cmp_pat(struct wd24s6 *s, u32 off, u8 tag)
{
	u32 i, bad = 0;

	pat(s->a, tag);
	s->io->mr(s->io->priv, off, s->b, WD24S6_N);
	for (i = 0; i < WD24S6_N; i++)
		bad += s->a[i] != s->b[i];
	return bad;
}

static void put_pat(struct wd24s6 *s, u32 off, u8 tag)
{
	pat(s->a, tag);
	s->io->mw(s->io->priv, off, s->a, WD24S6_N);
}

static int wr(struct wd24s6 *s, enum wd24_rf rf, u8 idx, u8 v, const char *n)
{
	int err = s->io->wr(s->io->priv, rf, idx, v);

	s->nwr++;
	LOG(s, "  write %s = %02x -> %d\n", n, v, err);
	return err;
}

/* tags: Y0=1 Y1(B)=2 X8(X)=3 X1(A)=4 Y0'=5 X8'=6 C=7 */
void wd24s6_run(struct wd24s6 *s, const struct wd24s6_io *io, u8 *save)
{
	const void *p = io->priv;
	bool saved = false, regs = false;
	u32 i;
	int err = 0;

	(void)p;
	memset(s, 0, sizeof(*s));
	s->io = io;
	s->save = save;
	s->res = WD24S6_NOTRUN;

	/* E: read-only snapshot + entry check */
	s->pr0a = io->rd(io->priv, WD24_GR, WD24_GR_PR0A);
	s->pr0b = io->rd(io->priv, WD24_GR, WD24_GR_PR0B);
	s->pr1 = io->rd(io->priv, WD24_GR, WD24_GR_PR1);
	s->pr31 = io->rd(io->priv, WD24_SR, WD24_SR_PR31);
	s->gr6 = io->rd(io->priv, WD24_GR, 0x06);
	LOG(s, "E read-only: PR0A %02x PR0B %02x PR1 %02x PR31 %02x GR06 %02x\n",
	    s->pr0a, s->pr0b, s->pr1, s->pr31, s->gr6);
	if (s->pr1 & WD24_PR1_PR0B)
		s->why = "PR1 b3 already set";
	else if ((s->pr1 & 0xc0) != 0xc0)
		s->why = "PR1 b7:6 not 1 MiB";
	else if (s->pr31 & WD24_PR31_RWSPLIT)
		s->why = "PR31 b7 already set";
	else if ((s->gr6 & 0x0c) != 0x04)
		s->why = "GR06 map is not 64 KiB at A0000";
	else if (s->pr0a != 0)
		s->why = "PR0A not 0 after the mode set";
	if (s->why) {
		s->res = WD24S6_ENTRY;
		LOG(s, "entry check: %s: nothing written\n", s->why);
		return;
	}
	io->open(io->priv, true);

	/* save the regions (single window: PR0A only, proven) */
	for (i = 0; i < WD24S6_NREG && !err; i++) {
		err = wr(s, WD24_GR, WD24_GR_PR0A, R[i].bank, "PR0A");
		if (!err)
			io->mr(io->priv, R[i].off, save + i * WD24S6_N, WD24S6_N);
	}
	if (err)
		goto reg;
	saved = true;
	LOG(s, "saved 7 x 4 KiB: Y+0000 Y+1000 Y+2000 X+8000 X+1000 X+0000 X+2000\n");

	/* prefill (single window) and check memory works */
	if ((err = wr(s, WD24_GR, WD24_GR_PR0A, WD24S6_Y, "PR0A")))
		goto reg;
	put_pat(s, 0x0000, 1);
	put_pat(s, 0x1000, 2);
	i = cmp_pat(s, 0x0000, 1) + cmp_pat(s, 0x1000, 2);
	if ((err = wr(s, WD24_GR, WD24_GR_PR0A, WD24S6_X, "PR0A")))
		goto reg;
	put_pat(s, 0x8000, 3);
	put_pat(s, 0x1000, 4);
	i += cmp_pat(s, 0x8000, 3) + cmp_pat(s, 0x1000, 4);
	if (i) {
		s->res = WD24S6_MEM;
		s->why = "single-window prefill did not read back";
		LOG(s, "prefill: %u bytes differ\n", i);
		goto restore;
	}
	LOG(s, "prefill: 4 x 4 KiB verified (single window)\n");

	/* P1: PR0B */
	if ((err = wr(s, WD24_GR, WD24_GR_PR0B, WD24S6_Y, "PR0B")))
		goto reg;
	s->rb_pr0b = io->rd(io->priv, WD24_GR, WD24_GR_PR0B);
	LOG(s, "P1 PR0B readback %02x\n", s->rb_pr0b);

	/* P2: PR1 b3 -> split */
	regs = true;
	if ((err = wr(s, WD24_GR, WD24_GR_PR1, s->pr1 | WD24_PR1_PR0B, "PR1")))
		goto reg;
	s->rb_pr1_on = io->rd(io->priv, WD24_GR, WD24_GR_PR1);
	LOG(s, "P2 PR1 readback %02x\n", s->rb_pr1_on);
	/* D1: lower half = PR0B (Y), upper half = PR0A (X) + 8000h */
	s->d_lo = cmp_pat(s, 0x0000, 1);
	s->d_hi = cmp_pat(s, 0x8000, 3);
	put_pat(s, 0x0000, 5);
	put_pat(s, 0x8000, 6);
	LOG(s, "D1 split read: A0000 (Y+0000) %u bad, A8000 (X+8000) %u bad; wrote Y+0000', X+8000'\n",
	    s->d_lo, s->d_hi);

	/* P3: PR31 b7 -> read PR0A, write PR0B */
	if ((err = wr(s, WD24_SR, WD24_SR_PR31, s->pr31 | WD24_PR31_RWSPLIT, "PR31")))
		goto reg;
	s->rb_pr31_on = io->rd(io->priv, WD24_SR, WD24_SR_PR31);
	LOG(s, "P3 PR31 readback %02x\n", s->rb_pr31_on);
	/* D2: read X while writing Y */
	s->d_rd = cmp_pat(s, 0x1000, 4);	/* X+1000 */
	put_pat(s, 0x1000, 7);			/* -> Y+1000 */
	s->d_wr_noecho = cmp_pat(s, 0x1000, 4);	/* still X: write did not land in X */
	for (i = 0; i < WD24S6_N; i += 256) {	/* copy X+1000 -> Y+2000 */
		io->mr(io->priv, 0x1000 + i, s->b, 256);
		io->mw(io->priv, 0x2000 + i, s->b, 256);
	}
	LOG(s, "D2 rw: read X+1000 %u bad; after write, read still X %u bad; copied X+1000 -> Y+2000\n",
	    s->d_rd, s->d_wr_noecho);

restore:
	/* P4: registers back (each read back by the guard layer) */
	if (regs) {
		err = wr(s, WD24_SR, WD24_SR_PR31, s->pr31, "PR31");
		s->rb_pr31_off = io->rd(io->priv, WD24_SR, WD24_SR_PR31);
		err |= wr(s, WD24_GR, WD24_GR_PR1, s->pr1, "PR1");
		s->rb_pr1_off = io->rd(io->priv, WD24_GR, WD24_GR_PR1);
		LOG(s, "P4 restore: PR31 %02x PR1 %02x\n", s->rb_pr31_off, s->rb_pr1_off);
		if (err || s->rb_pr31_off != s->pr31 || s->rb_pr1_off != s->pr1) {
			s->res = WD24S6_RESTORE;
			s->why = "PR31/PR1 did not restore";
			goto out;	/* single window not trustworthy: text restore */
		}
	}
	/* V: verify in single window */
	if (regs && s->res == WD24S6_NOTRUN) {
		wr(s, WD24_GR, WD24_GR_PR0A, WD24S6_Y, "PR0A");
		s->v_lo = cmp_pat(s, 0x0000, 5);
		s->v_wr = cmp_pat(s, 0x1000, 7);
		s->v_copy = cmp_pat(s, 0x2000, 4);
		wr(s, WD24_GR, WD24_GR_PR0A, WD24S6_X, "PR0A");
		s->v_hi = cmp_pat(s, 0x8000, 6);
		s->v_x = cmp_pat(s, 0x1000, 4);
		LOG(s, "V single: Y+0000'=%u Y+1000(C)=%u Y+2000(copy)=%u X+8000'=%u X+1000 kept=%u bad\n",
		    s->v_lo, s->v_wr, s->v_copy, s->v_hi, s->v_x);
		s->split_ok = !s->d_lo && !s->d_hi && !s->v_lo && !s->v_hi &&
			      s->rb_pr1_on == (s->pr1 | WD24_PR1_PR0B);
		s->rw_ok = !s->d_rd && !s->d_wr_noecho && !s->v_wr && !s->v_copy &&
			   !s->v_x && s->rb_pr31_on == (s->pr31 | WD24_PR31_RWSPLIT);
		s->res = s->split_ok && s->rw_ok ? WD24S6_OK : s->split_ok ? WD24S6_SPLIT_ONLY :
			 s->rw_ok ? WD24S6_RW_ONLY : WD24S6_NONE;
	}
	/* regions back */
	if (saved) {
		for (i = 0; i < WD24S6_NREG; i++) {
			u32 j;

			if (wr(s, WD24_GR, WD24_GR_PR0A, R[i].bank, "PR0A"))
				break;
			io->mw(io->priv, R[i].off, save + i * WD24S6_N, WD24S6_N);
			io->mr(io->priv, R[i].off, s->b, WD24S6_N);
			for (j = 0; j < WD24S6_N; j++)
				s->rest_bad += s->b[j] != save[i * WD24S6_N + j];
		}
		LOG(s, "regions restored: %u bytes differ\n", s->rest_bad);
		if (s->rest_bad && s->res != WD24S6_RESTORE) {
			s->res = WD24S6_RESTORE;
			s->why = "saved regions did not read back";
		}
	}
	wr(s, WD24_GR, WD24_GR_PR0B, s->pr0b, "PR0B");
	wr(s, WD24_GR, WD24_GR_PR0A, s->pr0a, "PR0A");
out:
	io->open(io->priv, false);
	LOG(s, "result: %s%s%s (register writes %u)\n", wd24s6_res_name(s->res),
	    s->why ? ": " : "", s->why ? s->why : "", s->nwr);
	return;
reg:
	if (s->res == WD24S6_NOTRUN) {
		s->res = WD24S6_REG;
		s->why = "a register write was refused or did not read back";
	}
	LOG(s, "stop: register error %d\n", err);
	goto restore;
}

/* ---- model ------------------------------------------------------------ */

static u32 m_addr(struct wd24s6_model *m, u32 off, bool write)
{
	u32 b;

	if (m->sr[0x11] & 0x80 && !m->ignore_pr31b7)
		b = write ? m->gr[0x0a] : m->gr[0x09];
	else if (m->gr[0x0b] & 0x08 && !m->ignore_pr1b3)
		b = off < 0x8000 ? m->gr[0x0a] : m->gr[0x09];
	else
		b = m->gr[0x09];
	return ((b & 0x7f) << 12) + off;	/* PR0A/B bits 6:0: 512 KiB reach */
}

static u8 m_rd(void *priv, enum wd24_rf rf, u8 idx)
{
	struct wd24s6_model *m = priv;

	if (rf == WD24_GR)
		return idx < 16 ? m->gr[idx] : 0xff;
	return idx < 0x20 ? m->sr[idx] : 0xff;
}

static int m_wr(void *priv, enum wd24_rf rf, u8 idx, u8 v)
{
	struct wd24s6_model *m = priv;

	m->nreg++;
	/* the guard rule, restated: b3 / b7 only while open */
	if (rf == WD24_GR && idx == 0x0b && (v & 0x08) && !m->open)
		return -1;
	if (rf == WD24_SR && idx == 0x11 && (v & 0x80) && !m->open)
		return -1;
	if (m->refuse && rf == WD24_GR && idx == 0x0b && (v & 0x08))
		return -1;
	if (rf == WD24_GR && idx < 16) {
		m->gr[idx] = (idx == 0x0b && m->pr1_stuck) ? v & ~0x08 : v;
		if (idx == 0x09 || idx == 0x0a)		/* DS: bits 6:0 */
			m->gr[idx] &= 0x7f;
		return (m_rd(priv, rf, idx) ^ v) & (idx == 0x09 || idx == 0x0a ? 0x7f : 0xff) ?
			-5 : 0;	/* readback, as wd24_write */
	}
	if (rf == WD24_SR && idx < 0x20) {
		m->sr[idx] = v;
		return 0;
	}
	return -1;
}

static void m_mr(void *priv, u32 off, u8 *buf, u32 n)
{
	struct wd24s6_model *m = priv;
	u32 i;

	for (i = 0; i < n; i++)
		buf[i] = m->vram[m_addr(m, (off + i) & 0xffff, false)];
}

static void m_mw(void *priv, u32 off, const u8 *buf, u32 n)
{
	struct wd24s6_model *m = priv;
	u32 i;

	for (i = 0; i < n; i++)
		m->vram[m_addr(m, (off + i) & 0xffff, true)] = buf[i];
}

static void m_open(void *priv, bool on)
{
	((struct wd24s6_model *)priv)->open = on;
}

const struct wd24s6_io wd24s6_model_io = { m_rd, m_wr, m_mr, m_mw, m_open, NULL };

void wd24s6_model_init(struct wd24s6_model *m, u8 *vram1m)
{
	u32 i;

	memset(m, 0, sizeof(*m));
	m->vram = vram1m;
	for (i = 0; i < 0x100000; i++)
		vram1m[i] = (u8)(i * 13 + (i >> 12));
	/* the 640x480-8 trial state: PR1 C5h (POST trace), PR31 5Dh, GR06 05h */
	m->gr[0x06] = 0x05;
	m->gr[0x0b] = 0xc5;
	m->sr[0x11] = 0x5d;	/* live: SR11=5D */
}
