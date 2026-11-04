// SPDX-License-Identifier: GPL-2.0
/*
 * Host test of wd24adv S2 (wd24adv_s2.c) against a 23C0h/23C2h register
 * file preset to several bases: the S1 live reference, the 2026-11-04
 * live reading (SRC_LO 2800h) and pseudo-random safe snapshots.  The expected access sequence is
 * restated here independently of the module and compared access by
 * access; fault knobs drive every stop path.
 */
#include <stdio.h>
#include <string.h>
#include "../wd24adv_s2.h"
#include "../wd24adv_engine.h"

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

struct acc { char w; u16 port, val; };
static struct {
	u16 ix, reg[WD24S1_NSEL];
	u8 ar, arr;
	/* faults */
	int stuck_k;		/* writes to select k ignored */
	int flip_k;		/* readback of select k XOR 1 after its first data write */
	int norestore_k;	/* select k keeps the first value written */
	int poke_after;		/* after data write #n, set reg[poke_k] = poke_v */
	int poke_k; u16 poke_v;
	int ixbad_at;		/* 23C0h read #n returns ix ^ 0x0100 */
	int unstable_k;		/* 2nd read of select k returns value ^ 1 */
	int rd_k[WD24S1_NSEL];
	u16 final_ix;		/* if set: a write of 1002h reads back this */
	int ndata, nixr, wrote_k[WD24S1_NSEL];
	struct acc tr[2048];
	int n;
} m;

static int kof(u16 ix)
{
	return wd24s2_k(ix & 0xff, (ix >> 8) & 0xf);
}

static u16 t_inw(void *p __attribute__((unused)), u16 port)
{
	u16 v = 0xffff;
	int k = kof(m.ix);

	if (port == WD24S1_P_INDEX) {
		v = m.ix;
		if (++m.nixr == m.ixbad_at)
			v ^= 0x0100;
	} else if (port == WD24S1_P_DATA && k >= 0) {
		v = m.reg[k];
		if (k == m.flip_k && m.wrote_k[k])
			v ^= 1;
		if (++m.rd_k[k] == 2 && k == m.unstable_k)
			v ^= 1;
	}
	m.tr[m.n++] = (struct acc){ 'R', port, v };
	return v;
}

static void t_outw(void *p __attribute__((unused)), u16 v, u16 port)
{
	int k = kof(m.ix);

	m.tr[m.n++] = (struct acc){ 'W', port, v };
	if (port == WD24S1_P_INDEX) {
		m.ix = (v == WD24S2_S && m.final_ix) ? m.final_ix : (v & 0x1fff);
		return;
	}
	if (port != WD24S1_P_DATA || k < 0)
		return;
	m.ndata++;
	if (k != m.stuck_k && !(k == m.norestore_k && m.wrote_k[k]))
		m.reg[k] = v;
	m.wrote_k[k]++;
	if (m.ndata == m.poke_after)
		m.reg[m.poke_k] = m.poke_v;
}

static u8 t_inb(void *p __attribute__((unused)), u16 port)
{
	u8 v = port == 0x3c0 ? m.ar : m.arr;

	m.tr[m.n++] = (struct acc){ 'r', port, v };
	return v;
}

static const struct wd24s1_io tio = { t_inw, t_outw, t_inb, NULL };
static struct wd24s2 s;
static u16 base[WD24S1_NSEL];	/* register file at load = the expected snapshot */

static void setup(void)
{
	memset(&m, 0, sizeof(m));
	m.ix = WD24S2_S;
	memcpy(m.reg, base, sizeof(m.reg));
	m.ar = 0x20;
	m.arr = 0x3f;
	m.stuck_k = m.flip_k = m.norestore_k = m.poke_k = m.unstable_k = -1;
	wd24s2_init(&s, &tio);
}

static u8 idx_of(int k) { return k < 16 ? k : k - 16; }
static u8 blk_of(int k) { return k < 16 ? 1 : 2; }

static void base_random(unsigned int seed)
{
	int k;

	for (k = 0; k < WD24S1_NSEL; k++) {
		seed = seed * 1103515245u + 12345u;
		base[k] = (u16)(idx_of(k) << 12) | ((seed >> 16) & 0xfff);
	}
	base[0] &= ~WD24S1_ACT;
	base[16] &= ~WD24S2_CC_EN;
}

/* expected sequence, restated from the header comment */
static struct acc ex[2048];
static int nex;
static void e(char w, u16 port, u16 val) { ex[nex++] = (struct acc){ w, port, val }; }

static void ex_read_all(void)
{
	unsigned int k;

	for (k = 0; k < WD24S1_NSEL; k++) {
		u16 sel = wd24s1_sel(k);

		e('W', 0x23c0, sel); e('R', 0x23c0, sel); e('R', 0x23c2, base[k]);
	}
}

static void ex_ok(void)
{
	int j;

	nex = 0;
	e('R', 0x23c0, 0x1002); e('R', 0x23c0, 0x1002); e('r', 0x3c0, 0x20); e('r', 0x3c1, 0x3f);
	ex_read_all();
	ex_read_all();
	for (j = 0; j < WD24S2_NW; j++) {
		const struct wd24s2_w *w = &wd24s2_list[j];
		u16 sel = 0x1000 | (w->idx << 8) | w->blk, tw = (w->idx << 12) | w->test;
		u16 sv = base[wd24s2_k(w->blk, w->idx)];
		u16 ss = w->blk == 1 ? 0x1001 : 0x1002, sr = w->blk == 1 ? base[0] : base[16];

		e('W', 0x23c0, sel); e('R', 0x23c0, sel);
		e('W', 0x23c2, tw); e('R', 0x23c0, sel); e('R', 0x23c2, tw);
		e('W', 0x23c2, sv); e('R', 0x23c2, sv);
		e('W', 0x23c0, ss); e('R', 0x23c0, ss); e('R', 0x23c2, sr);
	}
	ex_read_all();
	e('W', 0x23c0, 0x1002); e('R', 0x23c0, 0x1002);
}

static int same_trace(void)
{
	int i;

	if (m.n != nex)
		return 0;
	for (i = 0; i < nex; i++)
		if (m.tr[i].w != ex[i].w || m.tr[i].port != ex[i].port || m.tr[i].val != ex[i].val)
			return 0;
	return 1;
}

static int at_base(void)
{
	return !memcmp(m.reg, base, sizeof(m.reg)) && m.ix == WD24S2_S;
}

/* every write is 23C0h or 23C2h; 23C2h only to listed registers and only
 * the test word or that register's value at load (the snapshot) */
static int writes_ok(void)
{
	u16 ix = WD24S2_S;
	int i;

	for (i = 0; i < m.n; i++) {
		int k, j;

		if (m.tr[i].w != 'W')
			continue;
		if (m.tr[i].port == 0x23c0) {
			ix = m.tr[i].val;
			continue;
		}
		if (m.tr[i].port != 0x23c2)
			return 0;
		k = wd24s2_k(ix & 0xff, (ix >> 8) & 0xf);
		j = k < 0 ? -1 : wd24s2_find(blk_of(k), idx_of(k));
		if (j < 0)
			return 0;
		if (m.tr[i].val != ((wd24s2_list[j].idx << 12) | wd24s2_list[j].test) && m.tr[i].val != base[k])
			return 0;
	}
	for (i = 0; i < WD24S1_NSEL; i++)
		if (m.wrote_k[i] && wd24s2_find(blk_of(i), idx_of(i)) < 0)
			return 0;
	return 1;
}

static int rule_ok(u8 blk, u8 idx, u16 data)
{
	const struct wd24x_rule *r = wd24x_rule_find(blk, idx);
	unsigned int i;

	if (!r || (data & ~r->mask))
		return 0;
	if (r->set) {
		for (i = 0; i < r->nset; i++)
			if (r->set[i] == data)
				return 1;
		return 0;
	}
	return data >= r->lo && data <= r->hi;
}

static void run_fault(const char *name, enum wd24s2_res want, int want_data)
{
	int rc = wd24s2_run(&s);

	CHECK(s.res == want, "%s: res %s (%s)", name, wd24s2_res_name(s.res), s.why ? s.why : "");
	CHECK(rc != 0, "%s: rc 0", name);
	CHECK(writes_ok(), "%s: forbidden write", name);
	if (want_data >= 0)
		CHECK(m.ndata == want_data, "%s: %d data writes, want %d", name, m.ndata, want_data);
	CHECK(want == WD24S2_REFUSED ? (!s.restored && s.wix == 0) : (s.restored && m.ix == WD24S2_S), "%s: 23C0h not restored (%04x)", name, m.ix);
}

/* the guard, exhaustively, for the current base */
static void guard_checks(const char *bn)
{
	int k;

	setup();
	CHECK(wd24s2_guard_out(&s, 0x23c0, 0x1001) < 0, "%s: write before S read allowed", bn);
	s.have_saved = true; s.saved = 0x1002;
	CHECK(wd24s2_guard_out(&s, 0x23c2, 0x2000) < 0, "%s: 23C2h unarmed allowed", bn);
	s.armed = true;
	memcpy(s.snap, base, sizeof(s.snap));
	for (int pass = 0; pass < 2; pass++) {
		s.snap_ok = pass;	/* 0: snapshot not accepted -> only test words */
		for (k = 0; k < WD24S1_NSEL; k++) {
			unsigned int d;
			int jj = wd24s2_find(blk_of(k), idx_of(k)), nok = 0, bad = 0;
			u16 tw = jj < 0 ? 0 : (u16)((idx_of(k) << 12) | wd24s2_list[jj].test);

			s.cur = 0x1000 | (idx_of(k) << 8) | blk_of(k);
			for (d = 0; d < 0x10000; d++) {
				if (wd24s2_guard_out(&s, 0x23c2, d))
					continue;
				nok++;
				if (d != tw && !(pass && d == base[k]))
					bad++;
			}
			CHECK(bad == 0, "%s pass %d blk %u idx %X: %d non-test non-snapshot values allowed", bn, pass, blk_of(k), idx_of(k), bad);
			if (jj < 0)
				CHECK(nok == 0, "%s: blk %u idx %X: %d values allowed", bn, blk_of(k), idx_of(k), nok);
			else
				CHECK(nok == ((pass && tw != base[k]) ? 2 : 1), "%s pass %d blk %u idx %X: %d values allowed", bn, pass, blk_of(k), idx_of(k), nok);
			/* the S1 reference word is no longer an allowance */
			if (jj >= 0 && wd24s2_ref[k] != base[k] && wd24s2_ref[k] != tw)
				CHECK(wd24s2_guard_out(&s, 0x23c2, wd24s2_ref[k]) < 0, "%s: S1 ref word %04x allowed", bn, wd24s2_ref[k]);
		}
	}
	s.cur = 0x1201;
	CHECK(wd24s2_guard_out(&s, 0x23c4, 0x2000) < 0, "%s: 23C4h allowed", bn);
	CHECK(wd24s2_guard_out(&s, 0x23c4, base[2]) < 0, "%s: 23C4h snapshot allowed", bn);
	CHECK(wd24s2_guard_out(&s, 0x23c0, 0x1003) < 0, "%s: odd select allowed", bn);
	CHECK(wd24s2_guard_out(&s, 0x23c0, 0x0201) < 0, "%s: INC select allowed", bn);
	s.restored = true;
	CHECK(wd24s2_guard_out(&s, 0x23c2, base[2]) < 0, "%s: snapshot after restore allowed", bn);
}

static void suite(const char *bn)
{
	int j, k;
	char n[64];

	guard_checks(bn);

	/* OK path: exact sequence, all restored to the snapshot */
	setup();
	ex_ok();
	CHECK(wd24s2_run(&s) == 0 && s.res == WD24S2_OK, "%s OK: %s (%s)", bn, wd24s2_res_name(s.res), s.why ? s.why : "");
	CHECK(same_trace(), "%s OK: trace differs (%d vs %d accesses)", bn, m.n, nex);
	CHECK(at_base(), "%s OK: not at snapshot", bn);
	CHECK(!memcmp(s.snap, base, sizeof(base)) && !memcmp(s.snap2, base, sizeof(base)) && s.nsnap == 25 && s.nsnap2 == 25, "%s OK: snapshot", bn);
	CHECK(!memcmp(s.post, base, sizeof(base)) && s.npost == 25, "%s OK: post-pass", bn);
	CHECK(s.wdata == 2 * WD24S2_NW && m.ndata == 34, "%s OK: data writes %u", bn, s.wdata);
	CHECK(s.wix == 50 + 34 + 25 + 1, "%s OK: 23C0h writes %u", bn, s.wix);
	CHECK(writes_ok(), "%s OK: forbidden write", bn);
	for (k = 0; k < WD24S1_NSEL; k++)
		CHECK(m.wrote_k[k] == (wd24s2_find(blk_of(k), idx_of(k)) >= 0 ? 2 : 0), "%s k %d writes %d", bn, k, m.wrote_k[k]);
	CHECK(wd24s2_run(&s) == -EBUSY, "%s: second run allowed", bn);

	/* refusals before any write */
	setup(); m.ix = 0x1001;
	run_fault("S=1001", WD24S2_REFUSED, 0);
	setup(); m.ar = 0x02; m.arr = 0x10;
	CHECK(wd24s2_run(&s) == -ENODEV && s.res == WD24S2_REFUSED && s.wix == 0, "%s alias refusal", bn);
	setup(); m.ix = 0xffff;
	CHECK(wd24s2_run(&s) == -ENODEV && s.wix == 0, "%s FFFF refusal", bn);

	/* snapshot refusals: selects + reads only, no 23C2h write */
	for (k = 0; k < WD24S1_NSEL; k++) {
		snprintf(n, sizeof(n), "%s unstable k=%d", bn, k);
		setup(); m.unstable_k = k;
		run_fault(n, WD24S2_SNAP_BAD, 0);
		CHECK(s.nsnap == 25 && s.nsnap2 == 25 && s.snap2[k] == (base[k] ^ 1), "%s snap2", n);
		CHECK(s.wix == 50 + 1 && at_base(), "%s: wix %u", n, s.wix);
		CHECK(s.why && strstr(s.why, "unstable"), "%s why", n);

		snprintf(n, sizeof(n), "%s echo k=%d", bn, k);
		setup(); m.reg[k] ^= 0x1000;
		run_fault(n, WD24S2_SNAP_BAD, 0);
	}
	setup(); m.reg[0] |= WD24S1_ACT;
	run_fault("snapshot CNTRL1 ACT", WD24S2_SNAP_BAD, 0);
	CHECK(s.why && strstr(s.why, "ACT"), "ACT why");
	setup(); m.reg[16] |= WD24S2_CC_EN;
	run_fault("snapshot cursor on", WD24S2_SNAP_BAD, 0);
	CHECK(s.why && strstr(s.why, "cursor"), "cursor why");
	setup(); m.reg[16] = 0x0820;
	run_fault("snapshot cursor on 2col", WD24S2_SNAP_BAD, 0);
	setup(); m.ixbad_at = 2 + 5;
	run_fault("snapshot select rb", WD24S2_SNAP_BAD, 0);
	setup(); m.ixbad_at = 2 + 25 + 5;
	run_fault("snapshot 2 select rb", WD24S2_SNAP_BAD, 0);

	/* per entry: stuck (test not read back), flip, restore fails */
	for (j = 0; j < WD24S2_NW; j++) {
		const struct wd24s2_w *w = &wd24s2_list[j];
		int kk = wd24s2_k(w->blk, w->idx);
		int same = ((w->idx << 12) | w->test) == base[kk];

		snprintf(n, sizeof(n), "%s %s stuck", bn, w->name);
		setup(); m.stuck_k = kk;
		if (same) {
			CHECK(wd24s2_run(&s) == 0, "%s: same-value entry", n);
		} else {
			run_fault(n, WD24S2_MISMATCH, 2 * j + 1 + 1);
			CHECK(s.stop_j == j && s.step[j] == 2, "%s stop %d step %d", n, s.stop_j, s.step[j]);
			CHECK(at_base(), "%s: not at snapshot", n);
		}

		snprintf(n, sizeof(n), "%s %s flip", bn, w->name);
		setup(); m.flip_k = kk;
		run_fault(n, WD24S2_RESTORE_BAD, 2 * j + 2);
		CHECK(s.stop_j == j && (s.restore_fail & (1u << j)), "%s stop %d", n, s.stop_j);
		CHECK(!memcmp(m.reg, base, sizeof(m.reg)), "%s: register file not at snapshot", n);

		snprintf(n, sizeof(n), "%s %s norestore", bn, w->name);
		setup(); m.norestore_k = kk;
		if (same) {
			CHECK(wd24s2_run(&s) == 0, "%s: same-value entry", n);
		} else {
			run_fault(n, WD24S2_RESTORE_BAD, 2 * j + 3);
			CHECK(s.stop_j == j && s.step[j] == 3, "%s step %d", n, s.step[j]);
			CHECK(s.restore_fail & (1u << j), "%s restore_fail", n);
		}

		/* safety: CNTRL1 / cursor control change after entry j's restore */
		snprintf(n, sizeof(n), "%s %s safety", bn, w->name);
		setup();
		m.poke_after = 2 * j + 2;
		m.poke_k = w->blk == 1 ? 0 : 16;
		m.poke_v = base[m.poke_k] ^ 0x0800;
		run_fault(n, WD24S2_RESTORE_BAD, 2 * j + 2);
		CHECK(s.stop_j == j && s.step[j] == 3, "%s stop %d step %d", n, s.stop_j, s.step[j]);
		CHECK(s.why && strstr(s.why, w->blk == 1 ? "CNTRL1" : "cursor control"), "%s why %s", n, s.why);
	}
	/* another register drifts: post-pass catches it */
	setup(); m.poke_after = 3; m.poke_k = 2; m.poke_v = base[2] ^ 1;
	run_fault("drift after restore", WD24S2_RESTORE_BAD, -1);
	CHECK(s.npost == 3, "%s drift npost %u", bn, s.npost);

	/* 23C0h readback wrong after a data write */
	setup(); m.ixbad_at = 2 + 50 + 2;
	run_fault("23C0h after data write", WD24S2_MISMATCH, 2);
	CHECK(at_base(), "%s 23C0h after data write: not at snapshot", bn);

	/* final restore of 1002h does not read back */
	setup(); m.final_ix = 0x1001;
	CHECK(wd24s2_run(&s) != 0 && s.res == WD24S2_RESTORE_BAD && s.restore_rb == 0x1001, "%s final restore bad: %s", bn, wd24s2_res_name(s.res));
}

int main(void)
{
	int j, k;
	unsigned int seed;

	/* static: the list against the S0 guard rules and the forbidden set */
	for (j = 0; j < WD24S2_NW; j++) {
		const struct wd24s2_w *w = &wd24s2_list[j];

		CHECK(rule_ok(w->blk, w->idx, w->test), "%s test %03x not guard-allowed", w->name, w->test);
		CHECK(!(w->blk == 1 && (w->idx == 0 || w->idx == 0xc || w->idx == 0xd || w->idx == 0xf)), "%s forbidden", w->name);
		CHECK(!(w->blk == 2 && (w->idx <= 2 || w->idx == 8)), "%s forbidden", w->name);
		CHECK(!(w->blk == 1 && w->idx == 1 && (w->test & 0x80)), "CNTRL2 quick start");
		for (k = 0; k < j; k++)
			CHECK(wd24s2_list[k].blk != w->blk || wd24s2_list[k].idx != w->idx, "dup %s", w->name);
	}
	CHECK(wd24s2_list[11].idx == 1 && wd24s2_list[11].blk == 1, "CNTRL2 last in block 1");
	{	/* test values unchanged from 45d2f32 */
		static const u16 t[WD24S2_NW] = { 0x000, 0x04c, 0x000, 0x04c, 0x008, 0x008, 0x280, 0x600,
			0x05a, 0x05a, 0x0ff, 0x008, 0x05a, 0x0a5, 0x000, 0x010, 0x010 };
		for (j = 0; j < WD24S2_NW; j++)
			CHECK(wd24s2_list[j].test == t[j], "test value %d changed", j);
	}

	memcpy(base, wd24s2_ref, sizeof(base));
	suite("S1ref");
	memcpy(base, wd24s2_ref, sizeof(base)); base[2] = 0x2800;
	suite("live1104");
	for (seed = 1; seed <= 4; seed++) {
		char bn[16];

		base_random(seed * 7919u);
		snprintf(bn, sizeof(bn), "rand%u", seed);
		suite(bn);
	}

	printf("test_s2: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
