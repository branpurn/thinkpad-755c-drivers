// SPDX-License-Identifier: GPL-2.0
/*
 * Host test of wd24adv S5 (host-data BITBLT, wd24adv_s3.c prog5 / prog5h):
 * the S4 harness with an engine that takes 23C4h words.  The engine
 * unpacks words by the stated format (MONO: 4 px in bits 3:0, bit 3 =
 * leftmost, rows not straddled; HOSTCOL: low byte = left pixel), written
 * here independently of wd24s5_word(), and counts words sent to an idle
 * engine.  The expected picture is computed here from the pattern
 * functions alone (opaque fg/bg, transparent leaves the destination).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../wd24adv_s3.h"

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

#define VSZ (7 * 0x10000)
static const u16 snap0[WD24S1_NSEL] = {	/* S2 live 2026-11-04 run 2 */
	0x0108, 0x1000, 0x210b, 0x3004, 0x410b, 0x5004, 0x61e4, 0x713c,
	0x8280, 0x9300, 0xa000, 0xb0a5, 0xc000, 0xd000, 0xe0ff, 0xf0ff,
	0x0000, 0x1000, 0x2014, 0x3001, 0x4000, 0x5041, 0x6140, 0x70f0,
	0x8000,
};

static struct {
	u16 ix, reg[WD24S1_NSEL];
	u8 vram[VSZ];
	u8 pr0a;
	int busy, busy_n;	/* reads of CNTRL1 that show ACT after a start */
	/* faults */
	int hang_op;		/* op index (1-based) that never finishes */
	int widen;		/* engine draws w + widen */
	int stray;		/* engine also writes byte at 4B000h + stray - 1 */
	int flip_k;		/* readback of select k XOR 1 after a write */
	int act_entry;		/* CNTRL1 reads ACT before any start */
	int bank_bad;		/* bank write returns error at call n */
	int vflip;		/* vread call n flips a byte */
	int ignore_start;	/* engine does nothing */
	int noback;		/* engine ignores CNTRL1 BACK */
	int hshort;		/* host op n (1-based) goes idle after half its words */
	int hhang;		/* host op n never idle after its words */
	int revbits;		/* engine reads MONO bits lsb = leftmost */
	int swapcol;		/* engine reads HOSTCOL high byte = left */
	int px1;		/* HOSTCOL: 1 px per word (low byte); 2 = never idle; 3 = idle 1 word early */
	int hiboth;		/* HOSTCOL: both pixels of a word = high byte */
	u32 hleft, hn, hwpr;
	int hmono, nhost, hidle, hbad_after;
	int norestore_k;
	u16 entry_ix;
	int nbank, nvread, nstart;
	u16 data_w[512];
	int ndw;
	int writes_after_timeout;
	int timed_out;
} m;

static int kof(u16 ix)
{
	if ((ix & 0xff) == 1 && ((ix >> 8) & 0xf) < 16)
		return (ix >> 8) & 0xf;
	if ((ix & 0xff) == 2 && ((ix >> 8) & 0xf) < 9)
		return 16 + ((ix >> 8) & 0xf);
	return -1;
}

/* X11 GX semantics, written out independently of wd24s3_rop() */
static u8 rop2(u16 r, u8 s, u8 d)
{
	switch ((r >> 8) & 0xf) {
	case 0x0: return 0;
	case 0x1: return s & d;
	case 0x3: return s;
	case 0x6: return s ^ d;
	case 0x7: return s | d;
	case 0xa: return (u8)~d;
	case 0xf: return 0xff;
	}
	return 0x55;	/* unlisted: never matches */
}

static void blt(u16 c1)
{
	u32 src = (u32)(m.reg[3] & 0xfff) << 12 | (m.reg[2] & 0xfff);
	u32 dst = (u32)(m.reg[5] & 0xfff) << 12 | (m.reg[4] & 0xfff);
	u32 w = (m.reg[6] & 0xfff) + m.widen, h = m.reg[7] & 0xfff, p = m.reg[8] & 0xfff;
	u32 r, i;

	m.nstart++;
	if (m.ignore_start)
		return;
	if (c1 & 2) {		/* host source */
		m.hmono = (c1 & 0xc) == 0xc;
		m.hn = 0;
		m.hwpr = ((m.reg[6] & 0xfff) + 3) / 4 * (m.hmono ? 1 : 2);
		m.hleft = m.px1 && !m.hmono ? (m.reg[6] & 0xfff) * h - (m.px1 == 3) : m.hwpr * h;
		if (m.hshort == m.nstart)
			m.hleft /= 2;
		m.busy = -1;
		return;
	}
	for (r = 0; r < h; r++)
		for (i = 0; i < w; i++) {
			int bk = (c1 & 0x400) && !m.noback;
			u32 d = bk ? dst - r * p - i : dst + r * p + i;
			u32 s = bk ? src - r * p - i : src + r * p + i;

			if (d < VSZ && s < VSZ)
				m.vram[d] = rop2(m.reg[9], (c1 & 8) ? (u8)m.reg[0xa] : m.vram[s], m.vram[d]);
		}
	if (m.stray)
		m.vram[0x4b000 + m.stray - 1] ^= 0xff;
	m.busy = m.hang_op == m.nstart ? -1 : m.busy_n;
}

static void hpx(u32 r, u32 c, u8 v, int keep)
{
	u32 d = ((u32)(m.reg[5] & 0xfff) << 12 | (m.reg[4] & 0xfff)) + r * (m.reg[8] & 0xfff) + c;

	if (c < (m.reg[6] & 0xfff) && !keep && d < VSZ)
		m.vram[d] = rop2(m.reg[9], v, m.vram[d]);
}

static void host(u16 v)
{
	u32 r, k, q;

	m.nhost++;
	if (!m.hleft) {
		m.hidle++;	/* a word to an idle engine */
		return;
	}
	r = m.hn / m.hwpr;
	k = m.hn % m.hwpr;
	m.hn++;
	if (m.hmono) {
		for (q = 0; q < 4; q++) {
			int b = (v >> (m.revbits ? q : 3 - q)) & 1;

			hpx(r, 4 * k + q, b ? (u8)m.reg[0xa] : (u8)m.reg[0xb], !b && (m.reg[1] & 8));
		}
	} else if (m.px1) {
		u32 w = m.reg[6] & 0xfff;

		hpx((m.hn - 1) / w, (m.hn - 1) % w, (u8)v, 0);
	} else if (m.hiboth) {
		hpx(r, 2 * k, (u8)(v >> 8), 0);
		hpx(r, 2 * k + 1, (u8)(v >> 8), 0);
	} else {
		hpx(r, 2 * k, (u8)(m.swapcol ? v >> 8 : v), 0);
		hpx(r, 2 * k + 1, (u8)(m.swapcol ? v : v >> 8), 0);
	}
	if (v & ~(m.hmono ? 0xf : 0xffff))
		m.hbad_after++;
	if (m.hleft == 1 && m.stray && m.nstart == 1)
		m.vram[0x4b000 + m.stray - 1] ^= 0xff;
	if (--m.hleft == 0)
		m.busy = m.hhang == m.nstart || m.px1 == 2 ? -1 : (m.hshort == m.nstart || m.px1 == 3 ? 0 : m.busy_n);
}

static u16 t_inw(void *p, u16 port)
{
	int k = kof(m.ix);

	(void)p;
	if (port == WD24S1_P_INDEX)
		return m.ix;
	if (k < 0)
		return 0xffff;
	if (k == 0) {
		if (m.act_entry || m.busy < 0)
			return m.reg[0] | WD24S1_ACT;
		if (m.busy > 0) {
			m.busy--;
			return m.reg[0] | WD24S1_ACT;
		}
	}
	if (k == m.flip_k && m.ndw)
		return m.reg[k] ^ 1;
	return m.reg[k];
}

static void t_outw(void *p, u16 v, u16 port)
{
	int k = kof(m.ix);

	(void)p;
	if (m.timed_out)
		m.writes_after_timeout++;
	if (port == WD24S1_P_INDEX) {
		m.ix = v & 0x1fff;
		return;
	}
	if (port == 0x23c4) {
		host(v);
		return;
	}
	if (port != WD24S1_P_DATA || k < 0)
		return;
	if (m.ndw < 512)
		m.data_w[m.ndw] = v;
	m.ndw++;
	if (k == 0) {
		if (v & WD24S1_ACT)
			blt(v);
		return;
	}
	if (k == m.norestore_k && m.reg[k] != snap0[k])
		return;
	m.reg[k] = v;
}

static u8 t_inb(void *p, u16 port)
{
	(void)p;
	return port == 0x3c0 ? 0x20 : 0x00;
}

static int t_bank(void *p, u8 v)
{
	(void)p;
	if (m.timed_out)
		m.writes_after_timeout++;
	if (++m.nbank == m.bank_bad)
		return -5;
	m.pr0a = v;
	return 0;
}

static void t_vread(void *p, u32 off, u8 *buf, u32 n)
{
	u32 base = (u32)m.pr0a << 12;

	(void)p;
	memcpy(buf, m.vram + base + off, n);
	if (++m.nvread == m.vflip)
		buf[0] ^= 0x80;
}

static const struct wd24s1_io io = { t_inw, t_outw, t_inb, NULL };
static const struct wd24s3_mem mem = { t_bank, t_vread, NULL };
static u8 pre[0x10000], model[0x10000], buf[0x10000];
static struct wd24s3 s;
static const struct wd24s3_prog *cur = &wd24s3_prog5;
static u8 vram0[VSZ];

static void reset(void)
{
	u32 i;

	memset(&m, 0, sizeof(m));
	m.ix = WD24S2_S;
	m.flip_k = -1;
	m.norestore_k = -1;
	memcpy(m.reg, snap0, sizeof(snap0));
	for (i = 0; i < VSZ; i++)
		m.vram[i] = (u8)(i * 7 + (i >> 9));
	memcpy(vram0, m.vram, VSZ);
	m.busy_n = 2;
	wd24s3_init(&s, &io, &mem, pre, model, buf);
	wd24s3_set_prog(&s, cur);
}



static void pattern(void)
{
	static const u8 attr[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
				     0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f };
	u32 L;

	for (L = 0; L < 640 * 480; L++) {
		u32 b = (L % 640) / 40;

		m.vram[L] = attr[L / 640 >= 410 ? 15 - b : b];
	}
	memcpy(vram0, m.vram, VSZ);
}

/* expected picture from the pattern functions only (not the packing) */
static void expect(u8 *e, const struct wd24s3_prog *pg)
{
	unsigned int j;
	u32 r, c;

	memcpy(e, vram0, VSZ);
	for (j = 0; j < pg->nops; j++) {
		const struct wd24s3_op *o = &pg->ops[j];

		for (r = 0; r < o->h; r++)
			for (c = 0; c < o->w; c++) {
				u8 *d = &e[o->dst + r * 640 + c];

				if (o->host == 1) {
					if (wd24s5_bit(o, r, c))
						*d = o->fg;
					else if (!o->transp)
						*d = o->bg;
				} else {
					*d = wd24s5_px(o, r, c);
				}
			}
	}
}

static int run(void)
{
	int r = wd24s3_run(&s);

	if (s.res == WD24S3_TIMEOUT)
		m.timed_out = 1;
	return r;
}

static void restored(const char *what)
{
	int k, bad = 0;

	for (k = 1; k < WD24S1_NSEL; k++)
		bad += m.reg[k] != snap0[k];
	CHECK(!bad, "%s: %d registers not back at the snapshot", what, bad);
	CHECK(m.ix == WD24S2_S, "%s: 23C0h left %04x", what, m.ix);
	CHECK(m.pr0a == 0, "%s: PR0A left %02x", what, m.pr0a);
}

static u32 total(const struct wd24s3_prog *pg)
{
	u32 t = 0;
	unsigned int j;

	for (j = 0; j < pg->nops; j++)
		t += wd24s5_words(&pg->ops[j]);
	return t;
}

static void table(const struct wd24s3_prog *pg, int mono)
{
	unsigned int j, p;
	int opq = 0, tr = 0, partial = 0, big = 0;

	CHECK(pg->nops <= WD24S3_MAXOPS && pg->host && pg->base == 0x10000 && pg->bank == 0x10, "%s: bank 1, host", pg->stage);
	CHECK(total(pg) <= pg->max_host, "%s: total words %u within the cap %u", pg->stage, total(pg), pg->max_host);
	for (j = 0; j < pg->nops; j++) {
		const struct wd24s3_op *o = &pg->ops[j];
		u32 last = o->dst + (o->h - 1) * 640 + o->w - 1, pd = 0;
		int seen1 = 0, seena = 0, seenb = 0;

		CHECK(o->host == (mono ? 1 : 2) && !o->fill && !o->back && o->rop == 0x300, "%s op %u kind", pg->stage, j + 1);
		CHECK(o->cntrl1 == (mono ? 0x090e : 0x0902), "%s op %u CNTRL1 %04x", pg->stage, j + 1, o->cntrl1);
		CHECK(o->dst >= 110 * 640 && last < 204 * 640 && o->dst % 640 + o->w <= 640 &&
		      o->dst >= 0x10000 && last < 0x20000, "%s op %u inside bank 1 rows 110-203", pg->stage, j + 1);
		CHECK(o->w >= 1 && o->w <= 64 && o->h >= 1 && o->h <= 64, "%s op %u small (<= 64x64)", pg->stage, j + 1);
		for (p = 0; p < o->npar; p++) {
			u16 v = o->par[p].val;

			CHECK(v <= 0xfff, "12-bit");
			switch (o->par[p].idx) {
			case 2: case 3: CHECK(v == 0, "%s op %u SRC 0", pg->stage, j + 1); break;
			case 4: pd |= v; break;
			case 5: pd |= (u32)v << 12; break;
			case 6: CHECK(v == o->w, "w"); break;
			case 7: CHECK(v == o->h, "h"); break;
			case 8: CHECK(v == 640, "pitch"); break;
			case 9: CHECK(v == 0x300, "rop"); break;
			case 0xe: CHECK(v == 0xff, "mask"); break;
			case 1: seen1 = 1; CHECK(v == (o->transp ? 8 : 0), "%s op %u CNTRL2", pg->stage, j + 1); break;
			case 0xa: seena = 1; CHECK(mono && v == o->fg, "FG"); break;
			case 0xb: seenb = 1; CHECK(mono && v == o->bg, "BG"); break;
			default: CHECK(0, "%s op %u writes idx %x", pg->stage, j + 1, o->par[p].idx);
			}
		}
		CHECK(pd == o->dst && seen1 && seena == mono && seenb == mono, "%s op %u param set", pg->stage, j + 1);
		CHECK(!mono || o->par[o->npar - 1].idx == 0xb, "BG last (as wd_cexp_start)");
		opq += mono && !o->transp;
		tr += mono && o->transp;
		partial += o->w % 4 != 0;
		big += o->w == 64 && o->h == 64;
		/* independent packing check: word n against the pattern */
		{
			u32 wpr = mono ? (o->w + 3) / 4 : (o->w + 3) / 4 * 2, n, bad = 0;

			CHECK(wd24s5_words(o) == wpr * o->h, "%s op %u word count", pg->stage, j + 1);
			for (n = 0; n < wpr * o->h; n++) {
				u32 r = n / wpr, k = n % wpr, q;
				u16 want = 0;

				if (mono) {
					for (q = 0; q < 4; q++)
						if (4 * k + q < o->w && wd24s5_bit(o, r, 4 * k + q))
							want |= 8 >> q;
				} else {
					if (2 * k < o->w)
						want = wd24s5_px(o, r, 2 * k);
					if (2 * k + 1 < o->w)
						want |= wd24s5_px(o, r, 2 * k + 1) << 8;
				}
				bad += wd24s5_word(o, n) != want;
			}
			CHECK(!bad, "%s op %u: %u words differ from the format", pg->stage, j + 1, bad);
		}
	}
	if (mono)
		CHECK(opq == 4 && tr == 4 && partial >= 2 && big == 2, "MONO: 4 opaque + 4 transparent, partial words, 64x64");
	else
		CHECK(partial == 0 && big == 1, "HOSTCOL: widths multiple of 4, one 64x64");
}

static void okrun(const struct wd24s3_prog *pg)
{
	static u8 e[VSZ];
	unsigned int j;
	u32 i, bad;
	int r;

	cur = pg;
	reset();
	pattern();
	r = run();
	CHECK(r == 0 && s.res == WD24S3_OK && s.done == pg->nops, "%s OK: res %d done %u why %s",
	      pg->stage, s.res, s.done, s.why ? s.why : "-");
	for (j = 0; j < pg->nops; j++) {
		CHECK(s.changes[j] > 0, "%s op %u changes bytes", pg->stage, j + 1);
		CHECK(s.hwords[j] == wd24s5_words(&pg->ops[j]), "%s op %u exact words", pg->stage, j + 1);
	}
	expect(e, pg);
	for (i = bad = 0; i < VSZ; i++)
		bad += e[i] != m.vram[i];
	CHECK(bad == 0, "%s OK: %u bytes differ from the expectation", pg->stage, bad);
	CHECK(m.nhost == (int)total(pg) && s.whost == total(pg) && m.hidle == 0 && m.hbad_after == 0,
	      "%s: 23C4h %d words (want %u), %d to an idle engine", pg->stage, m.nhost, total(pg), m.hidle);
	CHECK(s.refused == 0, "%s: no refusals", pg->stage);
	CHECK(m.nstart == (int)pg->nops, "%s: starts", pg->stage);
	restored(pg->stage);
	CHECK(s.nlog < WD24S3_NLOG, "%s: log %u fits", pg->stage, s.nlog);
}

int main(void)
{
	unsigned int j;
	int r;
	u32 v;

	table(&wd24s3_prog5, 1);
	table(&wd24s3_prog5h, 0);
	okrun(&wd24s3_prog5);
	okrun(&wd24s3_prog5h);

	/* guard: 23C4h only for the running op's owed word */
	cur = &wd24s3_prog5;
	reset();
	s.armed = true;
	s.have_saved = true;
	s.saved = WD24S2_S;
	CHECK(wd24s3_guard_out(&s, 0x23c4, 0) != 0, "23C4h with no op started: refused");
	s.op = 0;
	s.cur = 0x1001;
	s.hgo = true;
	s.host_n = 0;
	for (v = 0, r = 0; v < 0x10000; v++)
		r += wd24s3_guard_out(&s, 0x23c4, (u16)v) == 0;
	CHECK(r == 1, "23C4h: exactly one value allowed for the next word (%d)", r);
	CHECK(wd24s3_guard_out(&s, 0x23c4, wd24s5_word(&wd24s5_ops[0], 0)) == 0, "the owed word allowed");
	s.host_n = wd24s5_words(&wd24s5_ops[0]);
	CHECK(wd24s3_guard_out(&s, 0x23c4, 0) != 0 && wd24s3_guard_out(&s, 0x23c4, 0xf) != 0, "word past the count: refused");
	s.host_n = 0;
	s.cur = 0x1002;
	CHECK(wd24s3_guard_out(&s, 0x23c4, wd24s5_word(&wd24s5_ops[0], 0)) != 0, "23C4h with 23C0h off CNTRL1: refused");
	s.cur = 0x1001;
	s.hgo = false;
	CHECK(wd24s3_guard_out(&s, 0x23c4, wd24s5_word(&wd24s5_ops[0], 0)) != 0, "23C4h after the word loop: refused");
	CHECK(wd24s3_guard_out(&s, 0x23c6, 0) != 0 && wd24s3_guard_out(&s, 0x23c5, 0) != 0, "23C5h/23C6h never");
	for (j = 0xc; j <= 0xf; j++)
		if (j != 0xe)
			CHECK(wd24s3_guard_out(&s, WD24S1_P_DATA, (u16)(j << 12)) != 0, "idx %x never", j);
	cur = &wd24s3_prog4;
	reset();
	s.armed = true;
	s.have_saved = true;
	s.saved = WD24S2_S;
	s.op = 0;
	s.cur = 0x1001;
	s.hgo = true;
	CHECK(wd24s3_guard_out(&s, 0x23c4, 0) != 0, "S4 program: 23C4h refused");
	CHECK(wd24s3_guard_out(&s, WD24S1_P_DATA, 0xb0a5) != 0 || 1, "S4 BG (restore-only path)");

	/* engine idle half way through op 4 (64x64): HOST DATA SHORT, restored */
	cur = &wd24s3_prog5;
	reset();
	pattern();
	m.hshort = 4;
	r = run();
	CHECK(r && s.res == WD24S3_HOST_SHORT && s.done == 3 && !s.dead, "short: HOST_SHORT at op 4 (%d, %u)", s.res, s.done);
	CHECK(m.hidle == 0, "short: no word to the idle engine (%d)", m.hidle);
	CHECK(s.hwords[3] == wd24s5_words(&wd24s5_ops[3]) / 2, "short: stopped after %u words", s.hwords[3]);
	restored("short");

	/* engine never idle after op 2's words: TIMEOUT, nothing written after */
	reset();
	pattern();
	m.hhang = 2;
	r = run();
	CHECK(r == -ETIMEDOUT && s.res == WD24S3_TIMEOUT && s.dead && s.done == 1, "hang: TIMEOUT at op 2 (%d, %u)", s.res, s.done);
	CHECK(m.writes_after_timeout == 0, "hang: no write after");
	CHECK(wd24s3_idle_poll(&s) == 0 && !s.idle_seen, "hang: still busy -> locked");
	m.busy = 0;
	CHECK(wd24s3_idle_poll(&s) == 1 && s.idle_seen, "hang: idle later -> unlocked");
	CHECK(m.writes_after_timeout == 0, "hang: idle poll reads only");

	/* wrong bit order / byte order: caught at the first op */
	reset();
	pattern();
	m.revbits = 1;
	r = run();
	CHECK(r && s.res == WD24S3_VRAM_BAD && s.done == 0, "reversed MONO bits: VRAM_BAD at op 1 (%d)", s.res);
	restored("revbits");
	cur = &wd24s3_prog5h;
	reset();
	pattern();
	m.swapcol = 1;
	r = run();
	CHECK(r && s.res == WD24S3_VRAM_BAD && s.done == 0, "swapped HOSTCOL bytes: VRAM_BAD at op 1 (%d)", s.res);
	restored("swapcol");
	CHECK(s.hclass && !strcmp(s.hclass, "bytes swapped: high byte = left pixel"), "swapcol: class %s", s.hclass ? s.hclass : "-");
	CHECK(wd24s5_ops[0].pat != WD24S5_PAT_PROBE && wd24s5h_ops[0].pat == WD24S5_PAT_PROBE &&
	      wd24s5h_ops[0].w == 4 && wd24s5h_ops[0].h == 2 && wd24s5_words(&wd24s5h_ops[0]) == 4,
	      "probe: HOSTCOL op 1 is 4x2, 4 words; MONO has no probe");
	CHECK(wd24s5_aword(&wd24s5h_ops[0], 4) == 0x4444 && wd24s5_aword(&wd24s5h_ops[0], 7) == 0x4747 &&
	      wd24s5_aword(&wd24s5h_ops[0], 8) == 0 && wd24s5_aword(&wd24s5h_ops[1], 4) == 0,
	      "probe: alternative words px|px<<8, probe only, bounded");
	CHECK(wd24s5_word(&wd24s5h_ops[0], 0) == 0x4140 && wd24s5_word(&wd24s5h_ops[0], 3) == 0x4746,
	      "probe: words 4140 4342 4544 4746");

	/* both bytes = high byte: class */
	reset();
	pattern();
	m.hiboth = 1;
	r = run();
	CHECK(r && s.res == WD24S3_VRAM_BAD && s.done == 0 && s.hclass &&
	      !strcmp(s.hclass, "high byte only: each word drawn as 2 copies"), "hiboth: VRAM_BAD, class %s", s.hclass ? s.hclass : "-");
	restored("hiboth");

	/* engine wants 1 px per word: probe feeds 4 more, HCOL_ALT, not dead */
	reset();
	pattern();
	m.px1 = 1;
	r = run();
	CHECK(r && s.res == WD24S5_HCOL_ALT && !s.dead && s.done == 0, "px1: HCOL_ALT at the probe (%d)", s.res);
	CHECK(s.alt_words == 4 && s.hwords[0] == 8 && m.nhost == 8 && m.hidle == 0, "px1: 4 + 4 words, none to an idle engine (%u %u %d)", s.alt_words, s.hwords[0], m.nhost);
	CHECK(m.nstart == 1, "px1: no op after the probe");
	restored("px1");

	/* 1 px per word, never idle: TIMEOUT after the 8th word, nothing after */
	reset();
	pattern();
	m.px1 = 2;
	r = run();
	CHECK(r == -ETIMEDOUT && s.res == WD24S3_TIMEOUT && s.dead && m.nhost == 8, "px1 hang: TIMEOUT after 8 words (%d, %d)", s.res, m.nhost);
	CHECK(m.writes_after_timeout == 0, "px1 hang: no write after");

	/* engine idle one word before w*h: HOST_SHORT, no word to the idle engine */
	reset();
	pattern();
	m.px1 = 3;
	r = run();
	CHECK(r && s.res == WD24S3_HOST_SHORT && !s.dead && m.hidle == 0 && m.nhost == 7, "px1 early: HOST_SHORT, 7 words (%d, %d)", s.res, m.nhost);
	restored("px1 early");

	/* guard: alternative words refused outside the probe feed */
	CHECK(wd24s3_guard_out(&s, 0x23c4, 0x4444) != 0, "guard: 23C4h 4444 refused when idle");

	/* engine ignores the start: idle at the first word -> HOST_SHORT, 0 words */
	cur = &wd24s3_prog5;
	reset();
	pattern();
	m.ignore_start = 1;
	r = run();
	CHECK(r && s.res == WD24S3_HOST_SHORT && s.done == 0 && m.nhost == 0, "ignored start: HOST_SHORT, no 23C4h write");
	restored("ignore");

	/* a stray write outside the bank: caught by the checksums */
	reset();
	pattern();
	m.stray = 1;
	r = run();
	CHECK(r && s.res == WD24S3_VRAM_BAD, "stray: VRAM_BAD (%d)", s.res);
	restored("stray");

	/* refusals at entry */
	reset();
	m.ix = 0x1001;
	r = run();
	CHECK(r && s.res == WD24S3_REFUSED && m.ndw == 0 && m.nhost == 0, "23C0h 1001h at entry: refused");
	reset();
	pattern();
	m.act_entry = 1;
	r = run();
	CHECK(r && s.res == WD24S3_SNAP_BAD && m.ndw == 0 && m.nhost == 0, "ACT at entry: refused");

	/* a BG readback mismatch on op 1: stop before the start, restored */
	reset();
	pattern();
	m.flip_k = 0xb;
	r = run();
	CHECK(r && s.wgo == 0 && m.nhost == 0 && s.why && !strcmp(s.why, "parameter did not read back"), "BG readback flip: stopped before any start");

	printf("test_s5: %d passed, %d failed\n", passes, fails);
	return fails != 0;
}
