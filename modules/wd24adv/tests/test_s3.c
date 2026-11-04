// SPDX-License-Identifier: GPL-2.0
/*
 * Host test of wd24adv S3 (wd24adv_s3.c): a 23C0h/23C2h register file
 * with an emulated BITBLT engine (fill/copy as wd90c24fb programs it,
 * busy for a set number of CNTRL1 reads) on a 448 KiB RAM model of video
 * memory behind a PR0A-banked 64 KiB window.  The exact 23C2h write list
 * is restated here independently; fault knobs drive every stop path.
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
			u32 d = dst + r * p + i, s = src + r * p + i;

			if (d < VSZ && s < VSZ)
				m.vram[d] = (c1 & 8) ? (u8)m.reg[0xa] : m.vram[s];
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
}

/* the expected 23C2h write list for the OK path, restated */
static const u16 want_ok[] = {
	0x2000, 0x3060, 0x4000, 0x5060, 0x8280, 0x6008, 0x7008, 0xe0ff, 0x9300, 0x1000, 0xa03c, 0x0908,
	0x2200, 0x3060, 0x4200, 0x5060, 0x8280, 0x6020, 0x7020, 0xe0ff, 0x9300, 0x1000, 0xa0c3, 0x0908,
	0x2200, 0x3060, 0x4100, 0x5060, 0x8280, 0x6020, 0x7020, 0xe0ff, 0x9300, 0x1000, 0x0900,
	/* restore, idx 1..A, E, snapshot words */
	0x1000, 0x210b, 0x3004, 0x410b, 0x5004, 0x61e4, 0x713c, 0x8280, 0x9300, 0xa000, 0xe0ff,
};

static void expect_vram_ok(void)
{
	u32 r, i, bad = 0;
	static u8 e[VSZ];

	memcpy(e, vram0, VSZ);
	for (r = 0; r < 8; r++)
		for (i = 0; i < 8; i++)
			e[0x60000 + r * 640 + i] = 0x3c;
	for (r = 0; r < 32; r++)
		for (i = 0; i < 32; i++)
			e[0x60200 + r * 640 + i] = 0xc3;
	for (r = 0; r < 32; r++)
		for (i = 0; i < 32; i++)
			e[0x60100 + r * 640 + i] = 0xc3;
	for (i = 0; i < VSZ; i++)
		bad += e[i] != m.vram[i];
	CHECK(bad == 0, "VRAM after OK run: %u bytes differ from the independent expectation", bad);
}

static int run(void)
{
	int r = wd24s3_run(&s);

	if (s.res == WD24S3_TIMEOUT)
		m.timed_out = 1;
	return r;
}

static void only_listed(const char *what)
{
	int i, j, bad = 0;

	for (i = 0; i < m.ndw; i++) {
		for (j = 0; j < (int)(sizeof(want_ok) / 2); j++)
			if (m.data_w[i] == want_ok[j])
				break;
		bad += j == (int)(sizeof(want_ok) / 2);
	}
	CHECK(!bad, "%s: %d 23C2h writes outside the closed list", what, bad);
}

static void registers_restored(const char *what)
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
	int r, i;

	/* OK path */
	reset();
	r = run();
	CHECK(r == 0 && s.res == WD24S3_OK, "OK path: %d %s %s", r, wd24s3_res_name(s.res), s.why ? s.why : "");
	CHECK(m.ndw == (int)(sizeof(want_ok) / 2), "OK path: %d 23C2h writes, want %zu", m.ndw, sizeof(want_ok) / 2);
	for (i = 0; i < m.ndw && i < (int)(sizeof(want_ok) / 2); i++)
		CHECK(m.data_w[i] == want_ok[i], "OK path: 23C2h write %d = %04x want %04x", i, m.data_w[i], want_ok[i]);
	CHECK(s.wgo == 3 && m.nstart == 3, "OK: 3 starts (%u %d)", s.wgo, m.nstart);
	CHECK(s.done == 3, "OK: 3 ops verified");
	CHECK(s.busy_seen[0] && s.spins[0] == 2, "OK: busy observed, 2 busy polls (%u)", s.spins[0]);
	CHECK(s.changes[0] == 64 && s.changes[1] > 1000 && s.changes[2] > 1000,
	      "OK: model changes %u %u %u", s.changes[0], s.changes[1], s.changes[2]);
	expect_vram_ok();
	registers_restored("OK");
	CHECK(s.restored && s.restore_rb == WD24S2_S, "OK: 1002h restored");
	CHECK(s.refused == 0, "OK: no guard refusal (%u)", s.refused);

	/* OK with the engine finishing before the first poll */
	reset();
	m.busy_n = 0;
	r = run();
	CHECK(r == 0 && !s.busy_seen[0], "OK, instant engine: %s", wd24s3_res_name(s.res));

	/* refusals: no write at all */
	reset(); m.ix = 0x1001; r = run();
	CHECK(s.res == WD24S3_REFUSED && m.ndw == 0 && m.nbank == 0, "23C0h != 1002h refused, no write");
	reset(); m.act_entry = 1; r = run();
	CHECK(s.res == WD24S3_SNAP_BAD && m.ndw == 0 && m.nbank == 0 && m.ix == WD24S2_S,
	      "ACT at entry: snapshot refused, selects only (%s)", wd24s3_res_name(s.res));
	reset(); m.reg[16] |= WD24S2_CC_EN; r = run();
	CHECK(s.res == WD24S3_SNAP_BAD && m.ndw == 0, "cursor enabled: refused");
	reset(); m.vflip = 7; r = run();	/* 6 crc banks + 16 reads each... flip early */
	CHECK(s.res == WD24S3_VRAM_BAD && s.why && strstr(s.why, "outside"), "vread flip in a checksum bank: caught (%s)", wd24s3_res_name(s.res));
	reset(); m.vflip = 6 * 16 + 16 + 1; r = run();	/* second read of bank 6 */
	CHECK(s.res == WD24S3_PRE_BAD && m.ndw == 0 && m.pr0a == 0 && m.ix == WD24S2_S,
	      "bank 6 unstable: pre-check refused, no 23C2h write (%s)", wd24s3_res_name(s.res));
	reset(); for (i = 0; i < 8; i++) memset(m.vram + 0x60000 + i * 640, 0x3c, 8); memcpy(vram0, m.vram, VSZ);
	r = run();
	CHECK(s.res == WD24S3_PRE_BAD && m.ndw == 0, "inconclusive (scratch already 3Ch): refused");
	reset(); m.bank_bad = 1; r = run();
	CHECK(s.res == WD24S3_PRE_BAD && m.ndw == 0, "PR0A readback failure: refused");

	/* timeout: hangs on op 1, 2, 3 */
	for (i = 1; i <= 3; i++) {
		int nd;

		reset();
		m.hang_op = i;
		r = run();
		nd = m.ndw;
		CHECK(r == -ETIMEDOUT && s.res == WD24S3_TIMEOUT && s.dead, "hang op %d: TIMEOUT (%s)", i, wd24s3_res_name(s.res));
		CHECK(s.wgo == (unsigned)i && nd == 12 * i - (i == 3), "hang op %d: stopped right after its start (%d writes)", i, nd);
		CHECK(m.writes_after_timeout == 0, "hang op %d: no write after the timeout", i);
		CHECK(s.polls >= WD24S3_SPINS, "hang op %d: bounded wait ran %u polls", i, s.polls);
		CHECK(wd24s3_guard_out(&s, WD24S1_P_INDEX, 0x1002) && wd24s3_guard_out(&s, WD24S1_P_DATA, 0x1000),
		      "hang op %d: guard closed after the timeout", i);
		only_listed("timeout");
	}

	/* engine wrong: width + 1 -> VRAM mismatch, stopped after op 1, restored */
	reset(); m.widen = 1; r = run();
	CHECK(s.res == WD24S3_VRAM_BAD && s.wgo == 1 && s.diff_off == 0x60008, "widen: VRAM_BAD at 60008h (%s %x)",
	      wd24s3_res_name(s.res), s.diff_off);
	registers_restored("widen");
	only_listed("widen");
	/* stray write outside scratch (guard gap / frame): caught by the checksum */
	reset(); m.stray = 1; r = run();
	CHECK(s.res == WD24S3_VRAM_BAD && s.why && strstr(s.why, "outside"), "stray: outside change detected (%s)",
	      s.why ? s.why : "");
	registers_restored("stray");
	/* engine does nothing */
	reset(); m.ignore_start = 1; r = run();
	CHECK(s.res == WD24S3_VRAM_BAD && s.wgo == 1, "no-op engine: VRAM_BAD after op 1");
	registers_restored("ignore");
	/* parameter readback flip */
	reset(); m.flip_k = 6; r = run();
	CHECK((s.res == WD24S3_MISMATCH || s.res == WD24S3_RESTORE_BAD) && s.why && !strcmp(s.why, "parameter did not read back") && s.wgo == 0, "DIM_X readback flip: MISMATCH before any start (%s)", wd24s3_res_name(s.res));
	/* restore failure */
	reset(); m.norestore_k = 4; r = run();
	CHECK(s.res == WD24S3_RESTORE_BAD, "restore failure reported (%s)", wd24s3_res_name(s.res));

	/* guard unit checks */
	reset();
	s.have_saved = true; s.saved = WD24S2_S; s.armed = true; s.snap_ok = true;
	memcpy(s.snap, snap0, sizeof(snap0));
	s.cur = 0x1001; s.op = 0;
	CHECK(wd24s3_guard_out(&s, WD24S1_P_DATA, 0x0908), "CNTRL1 refused without go");
	s.go = true;
	CHECK(!wd24s3_guard_out(&s, WD24S1_P_DATA, 0x0908), "CNTRL1 fill allowed with go, op 0");
	CHECK(wd24s3_guard_out(&s, WD24S1_P_DATA, 0x0900), "CNTRL1 copy refused for a fill op");
	CHECK(wd24s3_guard_out(&s, WD24S1_P_DATA, 0x0d08), "CNTRL1 backward refused");
	CHECK(wd24s3_guard_out(&s, WD24S1_P_DATA, 0x0108), "CNTRL1 without ACT refused");
	s.go = false;
	{
		int bad = 0;
		unsigned int v, idx;

		for (idx = 0; idx < 16; idx++) {
			s.cur = 0x1001 | (u16)(idx << 8);
			for (v = 0; v < 0x1000; v++) {
				u16 w = (u16)(idx << 12 | v);
				int ok = !wd24s3_guard_out(&s, WD24S1_P_DATA, w);
				int want = 0, j;

				for (j = 0; j < (int)wd24s3_ops[0].npar; j++)
					want |= wd24s3_ops[0].par[j].idx == idx && wd24s3_ops[0].par[j].val == v;
				want |= idx && idx != 0xb && idx != 0xc && idx != 0xd && idx != 0xf && w == snap0[idx];
				bad += ok != want;
			}
		}
		CHECK(!bad, "guard: block 1 exhaustive (op 0) %d disagreements", bad);
		bad = 0;
		for (idx = 0; idx < 9; idx++) {
			s.cur = 0x1002 | (u16)(idx << 8);
			for (v = 0; v < 0x10000; v += 7)
				bad += !wd24s3_guard_out(&s, WD24S1_P_DATA, (u16)v);
		}
		CHECK(!bad, "guard: block 2 never written (%d)", bad);
		CHECK(wd24s3_guard_out(&s, 0x23c4, 0) && wd24s3_guard_out(&s, 0x23c6, 0), "guard: 23C4h/23C6h never");
		CHECK(wd24s3_guard_out(&s, WD24S1_P_INDEX, 0x1003) && wd24s3_guard_out(&s, WD24S1_P_INDEX, 0x0001),
		      "guard: 23C0h non-select refused");
		s.armed = false; s.cur = 0x1201;
		CHECK(wd24s3_guard_out(&s, WD24S1_P_DATA, 0x2000), "guard: unarmed data refused");
	}
	printf("test_s3: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
