// SPDX-License-Identifier: GPL-2.0
/*
 * Host test of wd24adv S4 (the S4 program of wd24adv_s3.c): the same
 * register-file/engine harness as test_s3.c, its engine extended with the
 * ROPs and BACK addressing; the expected picture is computed here with
 * memmove semantics (what a correct overlapping copy must give), not with
 * the module's model.  Video memory is filled with the real test pattern
 * (bars of the BIOS palette) so every op is checked on what S4 meets live.
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
	int norestore_k;
	u16 entry_ix;
	int nbank, nvread, nstart;
	u16 data_w[256];
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
	if (port != WD24S1_P_DATA || k < 0)
		return;
	m.data_w[m.ndw++] = v;
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
	wd24s3_set_prog(&s, &wd24s3_prog4);
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

static void expect(u8 *e, unsigned int nops)
{
	static u8 tmp[640 * 32];
	unsigned int j;
	u32 r, i;

	memcpy(e, vram0, VSZ);
	for (j = 0; j < nops; j++) {
		const struct wd24s3_op *o = &wd24s4_ops[j];

		for (r = 0; r < o->h; r++)	/* source first: memmove */
			for (i = 0; i < o->w; i++)
				tmp[r * 640 + i] = e[o->src + r * 640 + i];
		for (r = 0; r < o->h; r++)
			for (i = 0; i < o->w; i++) {
				u8 *d = &e[o->dst + r * 640 + i];

				*d = rop2(o->rop, o->fill ? o->fg : tmp[r * 640 + i], *d);
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

int main(void)
{
	static u8 e[VSZ];
	unsigned int j, p, k, bad;
	u32 i;
	int r, rops = 0, fwd = 0, back = 0;

	/* 1. the table: on screen, bank 1 only, frame rows 110-203, legal
	 * words, direction = dst > src, BACK addresses = bottom-right */
	CHECK(wd24s3_prog4.nops == WD24S4_NOPS && WD24S4_NOPS <= WD24S3_MAXOPS, "op count");
	CHECK(wd24s3_prog4.base == 0x10000 && wd24s3_prog4.bank == 0x10, "bank 1");
	for (j = 0; j < WD24S4_NOPS; j++) {
		const struct wd24s3_op *o = &wd24s4_ops[j];
		u32 ps = 0, pd = 0, last = o->dst + (o->h - 1) * 640 + o->w - 1;
		u32 slast = o->src + (o->h - 1) * 640 + o->w - 1;
		u16 pw = 0, ph = 0, pr = 0xffff;

		for (p = 0; p < o->npar; p++) {
			u16 v = o->par[p].val;

			switch (o->par[p].idx) {
			case 2: ps |= v; break;
			case 3: ps |= (u32)v << 12; break;
			case 4: pd |= v; break;
			case 5: pd |= (u32)v << 12; break;
			case 6: pw = v; break;
			case 7: ph = v; break;
			case 9: pr = v; break;
			case 8: CHECK(v == 640, "op %u pitch", j + 1); break;
			case 1: CHECK(v == 0, "op %u CNTRL2", j + 1); break;
			case 0xe: CHECK(v == 0xff, "op %u mask", j + 1); break;
			case 0xa: CHECK(o->fill && v == o->fg, "op %u FG", j + 1); break;
			default: CHECK(0, "op %u writes idx %x", j + 1, o->par[p].idx);
			}
			CHECK(v <= 0xfff, "op %u 12-bit", j + 1);
		}
		CHECK(pw == o->w && ph == o->h && pr == o->rop, "op %u size/rop words", j + 1);
		CHECK(o->dst >= 110 * 640 && last < 204 * 640 && o->dst % 640 + o->w <= 640,
		      "op %u destination inside rows 110-203", j + 1);
		CHECK(o->dst >= 0x10000 && last < 0x20000 && o->src >= 0x10000 && slast < 0x20000,
		      "op %u inside bank 1", j + 1);
		CHECK(last < 0x4b000, "op %u inside the frame", j + 1);
		if (o->fill) {
			CHECK(o->cntrl1 == 0x0908 && !o->back && ps == o->dst && pd == o->dst, "op %u fill words", j + 1);
			rops |= 1 << ((o->rop >> 8) & 0xf);
		} else {
			CHECK(o->rop == 0x300, "op %u copy ROP", j + 1);
			CHECK(o->back == (o->dst > o->src), "op %u direction = dst > src", j + 1);
			CHECK(o->cntrl1 == (o->back ? 0x0d00 : 0x0900), "op %u CNTRL1", j + 1);
			CHECK(ps == (o->back ? slast : o->src) && pd == (o->back ? last : o->dst),
			      "op %u start corner", j + 1);
			/* the rectangles overlap (the point of the test) */
			CHECK(abs((int)(o->src % 640) - (int)(o->dst % 640)) < o->w &&
			      abs((int)(o->src / 640) - (int)(o->dst / 640)) < o->h, "op %u overlaps", j + 1);
			back += o->back;
			fwd += !o->back;
		}
	}
	CHECK(rops == (1 << 0 | 1 << 1 | 1 << 3 | 1 << 6 | 1 << 7 | 1 << 0xa | 1 << 0xf),
	      "fills cover clear, and, copy, xor, or, invert, set (%x)", rops);
	CHECK(fwd == 2 && back == 2, "two forward, two backward copies");
	for (k = 0; k < 16; k++)
		for (i = 0; i < 4; i++) {
			u8 sv = (u8)(i & 2 ? 0xf0 : 0x3c), dv = (u8)(i & 1 ? 0xaa : 0x0f);

			if (rops & (1 << k))
				CHECK(wd24s3_rop((u16)(k << 8), sv, dv) == rop2((u16)(k << 8), sv, dv),
				      "wd24s3_rop %x", k);
		}

	/* 2. OK run on the pattern */
	reset();
	pattern();
	r = run();
	CHECK(r == 0 && s.res == WD24S3_OK && s.done == WD24S4_NOPS, "OK: res %d done %u why %s",
	      s.res, s.done, s.why ? s.why : "-");
	for (j = 0; j < WD24S4_NOPS; j++)
		CHECK(s.changes[j] > 0, "op %u changes bytes", j + 1);
	expect(e, WD24S4_NOPS);
	for (i = bad = 0; i < VSZ; i++)
		bad += e[i] != m.vram[i];
	CHECK(bad == 0, "OK: %u bytes differ from the memmove expectation", bad);
	restored("OK");
	CHECK(m.nstart == WD24S4_NOPS, "OK: %d starts", m.nstart);

	/* 3. an engine that ignores BACK is caught at the first backward copy */
	reset();
	pattern();
	m.noback = 1;
	r = run();
	CHECK(r && s.res == WD24S3_VRAM_BAD && s.done == 8, "BACK ignored: VRAM_BAD at op 9 (%d, %u)", s.res, s.done);
	restored("noback");

	/* 3b. a widened engine is caught at op 1 */
	reset();
	pattern();
	m.widen = 1;
	r = run();
	CHECK(r && s.res == WD24S3_VRAM_BAD && s.done == 0, "widened engine: VRAM_BAD at op 1 (%d)", s.res);
	restored("widen");

	/* 4. a byte outside bank 1 changes (4B000h+): caught by the checksums */
	reset();
	pattern();
	m.stray = 1;
	r = run();
	CHECK(r && s.res == WD24S3_VRAM_BAD, "stray write outside the bank: VRAM_BAD (%d)", s.res);
	restored("stray");

	/* 5. timeout on the first backward copy: nothing written afterwards */
	reset();
	pattern();
	m.hang_op = 9;
	r = run();
	CHECK(r == -ETIMEDOUT && s.res == WD24S3_TIMEOUT && s.dead && s.done == 8, "timeout op 9 (%d, %u)", s.res, s.done);
	CHECK(m.writes_after_timeout == 0, "timeout: no write after");
	CHECK(wd24s3_idle_poll(&s) == 0 && !s.idle_seen, "timeout: still busy -> locked");
	m.busy = 0;
	CHECK(wd24s3_idle_poll(&s) == 1 && s.idle_seen, "timeout: idle seen later -> unlocked");
	CHECK(m.writes_after_timeout == 0, "idle poll: reads only");

	/* 6. entry refusals: not 1002h, ACT at entry */
	reset();
	m.ix = 0x1001;
	r = run();
	CHECK(r && s.res == WD24S3_REFUSED && m.ndw == 0, "23C0h 1001h at entry: refused, no 23C2h write");
	reset();
	pattern();
	m.act_entry = 1;
	r = run();
	CHECK(r && s.res == WD24S3_SNAP_BAD && m.ndw == 0, "ACT at entry: snapshot refused");

	/* 7. a register readback mismatch: stop, restore */
	reset();
	pattern();
	m.flip_k = 9;
	r = run();
	CHECK(r && (s.res == WD24S3_MISMATCH || s.res == WD24S3_RESTORE_BAD) && s.wgo == 0 && s.why && !strcmp(s.why, "parameter did not read back"), "ROP readback flip: stopped before any start");

	/* 8. set_prog after a run is ignored; S3 default unchanged */
	reset();
	CHECK(s.prog == &wd24s3_prog4, "prog set");
	wd24s3_init(&s, &io, &mem, pre, model, buf);
	CHECK(s.prog == &wd24s3_prog3, "init: S3 default");

	printf("test_s4: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
