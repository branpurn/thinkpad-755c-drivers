// SPDX-License-Identifier: GPL-2.0
/*
 * Host test of wd24adv S2 (wd24adv_s2.c) against a 23C0h/23C2h register
 * file preset to the S1 live reference.  The expected access sequence is
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

static void setup(void)
{
	memset(&m, 0, sizeof(m));
	m.ix = WD24S2_S;
	memcpy(m.reg, wd24s2_ref, sizeof(m.reg));
	m.ar = 0x20;
	m.arr = 0x3f;
	m.stuck_k = m.flip_k = m.norestore_k = m.poke_k = -1;
	wd24s2_init(&s, &tio);
}

/* expected sequence, restated from the header comment */
static struct acc ex[2048];
static int nex;
static void e(char w, u16 port, u16 val) { ex[nex++] = (struct acc){ w, port, val }; }

static void ex_verify_all(void)
{
	unsigned int k;

	for (k = 0; k < WD24S1_NSEL; k++) {
		u16 sel = wd24s1_sel(k);

		e('W', 0x23c0, sel); e('R', 0x23c0, sel); e('R', 0x23c2, wd24s2_ref[k]);
	}
}

static void ex_ok(void)
{
	int j;

	nex = 0;
	e('R', 0x23c0, 0x1002); e('R', 0x23c0, 0x1002); e('r', 0x3c0, 0x20); e('r', 0x3c1, 0x3f);
	ex_verify_all();
	for (j = 0; j < WD24S2_NW; j++) {
		const struct wd24s2_w *w = &wd24s2_list[j];
		u16 sel = 0x1000 | (w->idx << 8) | w->blk, tw = (w->idx << 12) | w->test;
		u16 ref = wd24s2_ref[wd24s2_k(w->blk, w->idx)];
		u16 ss = w->blk == 1 ? 0x1001 : 0x1002, sr = w->blk == 1 ? 0x0108 : 0x0000;

		e('W', 0x23c0, sel); e('R', 0x23c0, sel);
		e('W', 0x23c2, tw); e('R', 0x23c0, sel); e('R', 0x23c2, tw);
		e('W', 0x23c2, ref); e('R', 0x23c2, ref);
		e('W', 0x23c0, ss); e('R', 0x23c0, ss); e('R', 0x23c2, sr);
	}
	ex_verify_all();
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

static int at_ref(void)
{
	return !memcmp(m.reg, wd24s2_ref, sizeof(m.reg)) && m.ix == WD24S2_S;
}

static int writes_ok(void)
{
	int i;

	/* every write is 23C0h or 23C2h; 23C2h never to CNTRL1 / cursor ctl / C D F / PAT / 8 */
	for (i = 0; i < m.n; i++)
		if (m.tr[i].w == 'W' && m.tr[i].port != 0x23c0 && m.tr[i].port != 0x23c2)
			return 0;
	for (i = 0; i < WD24S1_NSEL; i++) {
		u8 blk = i < 16 ? 1 : 2, idx = i < 16 ? i : i - 16;

		if (m.wrote_k[i] && wd24s2_find(blk, idx) < 0)
			return 0;
	}
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

int main(void)
{
	int j, k;

	/* static: the list against the S0 guard rules and the forbidden set */
	for (j = 0; j < WD24S2_NW; j++) {
		const struct wd24s2_w *w = &wd24s2_list[j];
		u16 ref = wd24s2_ref[wd24s2_k(w->blk, w->idx)];

		CHECK(rule_ok(w->blk, w->idx, w->test), "%s test %03x not guard-allowed", w->name, w->test);
		CHECK(rule_ok(w->blk, w->idx, ref & 0xfff), "%s ref %03x not guard-allowed", w->name, ref & 0xfff);
		CHECK((ref >> 12) == w->idx, "%s ref echo", w->name);
		CHECK(!(w->blk == 1 && (w->idx == 0 || w->idx == 0xc || w->idx == 0xd || w->idx == 0xf)), "%s forbidden", w->name);
		CHECK(!(w->blk == 2 && (w->idx <= 2 || w->idx == 8)), "%s forbidden", w->name);
		CHECK(!(w->blk == 1 && w->idx == 1 && (w->test & 0x80)), "CNTRL2 quick start");
		for (k = 0; k < j; k++)
			CHECK(wd24s2_list[k].blk != w->blk || wd24s2_list[k].idx != w->idx, "dup %s", w->name);
	}
	CHECK(wd24s2_list[11].idx == 1 && wd24s2_list[11].blk == 1, "CNTRL2 last in block 1");
	CHECK(!(wd24s2_ref[0] & WD24S1_ACT) && wd24s2_ref[0] == 0x0108, "CNTRL1 ref ACT=0");
	CHECK(wd24s2_ref[16] == 0, "cursor ctl ref 0");

	/* guard: unarmed / forbidden registers / values */
	setup();
	CHECK(wd24s2_guard_out(&s, 0x23c0, 0x1001) < 0, "write before S read allowed");
	s.have_saved = true; s.saved = 0x1002;
	CHECK(wd24s2_guard_out(&s, 0x23c2, 0x2000) < 0, "23C2h unarmed allowed");
	s.armed = true;
	for (k = 0; k < WD24S1_NSEL; k++) {
		u8 blk = k < 16 ? 1 : 2, idx = k < 16 ? k : k - 16;
		unsigned int d;
		int jj = wd24s2_find(blk, idx), nok = 0;

		s.cur = 0x1000 | (idx << 8) | blk;
		for (d = 0; d < 0x10000; d++)
			if (!wd24s2_guard_out(&s, 0x23c2, d))
				nok++;
		if (jj < 0)
			CHECK(nok == 0, "blk %u idx %X: %d values allowed", blk, idx, nok);
		else
			CHECK(nok == (wd24s2_list[jj].test == (wd24s2_ref[k] & 0xfff) ? 1 : 2),
			      "blk %u idx %X: %d values allowed", blk, idx, nok);
	}
	s.cur = 0x1201;
	CHECK(wd24s2_guard_out(&s, 0x23c4, 0x2000) < 0, "23C4h allowed");
	CHECK(wd24s2_guard_out(&s, 0x23c0, 0x1003) < 0, "odd select allowed");
	CHECK(wd24s2_guard_out(&s, 0x23c0, 0x0201) < 0, "INC select allowed");

	/* OK path: exact sequence, all restored */
	setup();
	ex_ok();
	CHECK(wd24s2_run(&s) == 0 && s.res == WD24S2_OK, "OK: %s (%s)", wd24s2_res_name(s.res), s.why ? s.why : "");
	CHECK(same_trace(), "OK: trace differs (%d vs %d accesses)", m.n, nex);
	CHECK(at_ref(), "OK: not at reference");
	CHECK(s.wdata == 2 * WD24S2_NW && m.ndata == 34, "OK: data writes %u", s.wdata);
	CHECK(s.wix == 25 + 34 + 25 + 1, "OK: 23C0h writes %u", s.wix);
	CHECK(writes_ok(), "OK: forbidden write");
	for (k = 0; k < WD24S1_NSEL; k++) {
		u8 blk = k < 16 ? 1 : 2, idx = k < 16 ? k : k - 16;

		CHECK(m.wrote_k[k] == (wd24s2_find(blk, idx) >= 0 ? 2 : 0), "k %d writes %d", k, m.wrote_k[k]);
	}
	CHECK(wd24s2_run(&s) == -EBUSY, "second run allowed");

	/* refusals before any write */
	setup(); m.ix = 0x1001; m.reg[0] = 0x0108;
	run_fault("S=1001", WD24S2_REFUSED, 0);
	CHECK(s.wix == 0 && !s.restored, "S=1001 wrote");
	setup(); m.ar = 0x02; m.arr = 0x10;
	{
		int rc = wd24s2_run(&s);
		CHECK(rc == -ENODEV && s.res == WD24S2_REFUSED && s.wix == 0, "alias refusal");
	}
	setup(); m.ix = 0xffff;
	CHECK(wd24s2_run(&s) == -ENODEV && s.wix == 0, "FFFF refusal");

	/* pre-pass mismatch, each register: selects only, no 23C2h write */
	for (k = 0; k < WD24S1_NSEL; k++) {
		char n[48];

		setup(); m.reg[k] ^= 0x0001;
		snprintf(n, sizeof(n), "pre k=%d", k);
		run_fault(n, WD24S2_NOT_REF, 0);
		CHECK(s.npre == (unsigned int)k + 1, "%s npre %u", n, s.npre);
	}
	setup(); m.reg[0] = 0x0908;	/* ACT set */
	run_fault("pre CNTRL1 ACT", WD24S2_NOT_REF, 0);
	setup(); m.reg[16] = 0x0800;	/* cursor enabled */
	run_fault("pre cursor on", WD24S2_NOT_REF, 0);
	setup(); m.ixbad_at = 2 + 5;	/* pre-pass select 5 reads back wrong */
	run_fault("pre select rb", WD24S2_NOT_REF, 0);

	/* per entry: stuck (test not read back), flip, restore fails */
	for (j = 0; j < WD24S2_NW; j++) {
		const struct wd24s2_w *w = &wd24s2_list[j];
		int kk = wd24s2_k(w->blk, w->idx);
		char n[48];

		snprintf(n, sizeof(n), "%s stuck", w->name);
		setup(); m.stuck_k = kk;
		if (w->test == (wd24s2_ref[kk] & 0xfff)) {
			/* PITCH / MASK: test = reference, a stuck register reads right */
			CHECK(wd24s2_run(&s) == 0, "%s: same-value entry", n);
		} else {
			/* j entries done (2 writes each) + test write + 1 restore write */
			run_fault(n, WD24S2_MISMATCH, 2 * j + 1 + 1);
			CHECK(s.stop_j == j && s.step[j] == 2, "%s stop %d step %d", n, s.stop_j, s.step[j]);
			CHECK(at_ref(), "%s: not at reference", n);
		}

		snprintf(n, sizeof(n), "%s flip", w->name);
		setup(); m.flip_k = kk;
		/* readback differs after the test write; restore readback also flips */
		run_fault(n, WD24S2_RESTORE_BAD, 2 * j + 2);
		CHECK(s.stop_j == j && (s.restore_fail & (1u << j)), "%s stop %d", n, s.stop_j);
		CHECK(!memcmp(m.reg, wd24s2_ref, sizeof(m.reg)), "%s: register file not at ref", n);

		snprintf(n, sizeof(n), "%s norestore", w->name);
		setup(); m.norestore_k = kk;
		if (w->test == (wd24s2_ref[kk] & 0xfff)) {
			CHECK(wd24s2_run(&s) == 0, "%s: same-value entry", n);
		} else {
			run_fault(n, WD24S2_RESTORE_BAD, 2 * j + 3);
			CHECK(s.stop_j == j && s.step[j] == 3, "%s step %d", n, s.step[j]);
			CHECK(s.restore_fail & (1u << j), "%s restore_fail", n);
		}
	}

	/* safety re-check: CNTRL1 / cursor control change after entry j's restore */
	for (j = 0; j < WD24S2_NW; j++) {
		const struct wd24s2_w *w = &wd24s2_list[j];
		char n[48];

		snprintf(n, sizeof(n), "%s safety", w->name);
		setup();
		m.poke_after = 2 * j + 2;
		m.poke_k = w->blk == 1 ? 0 : 16;
		m.poke_v = w->blk == 1 ? 0x0908 : 0x0800;
		/* CNTRL1/cursor ctl are never written, so they cannot be restored: post-pass fails */
		run_fault(n, WD24S2_RESTORE_BAD, 2 * j + 2);
		CHECK(s.stop_j == j && s.step[j] == 3, "%s stop %d step %d", n, s.stop_j, s.step[j]);
		CHECK(s.why && strstr(s.why, w->blk == 1 ? "CNTRL1" : "cursor control"), "%s why %s", n, s.why);
	}
	/* safety change that is in the write list's reach: some other register drifts */
	setup(); m.poke_after = 3; m.poke_k = 2; m.poke_v = 0x2001;	/* SRC_LO drifts after DST_LO's... after SRC_HI's test write */
	run_fault("drift after restore", WD24S2_RESTORE_BAD, -1);
	CHECK(s.npost == 3, "drift npost %u", s.npost);

	/* 23C0h readback wrong after a data write */
	setup(); m.ixbad_at = 2 + 25 + 2;	/* entry 0: the R 23C0h after the test write */
	run_fault("23C0h after data write", WD24S2_MISMATCH, 2);
	CHECK(at_ref(), "23C0h after data write: not at reference");

	/* final restore of 1002h does not read back */
	setup(); m.final_ix = 0x1001;
	{
		int rc = wd24s2_run(&s);
		CHECK(rc != 0 && s.res == WD24S2_RESTORE_BAD && s.restore_rb == 0x1001, "final restore bad: %s", wd24s2_res_name(s.res));
	}

	printf("test_s2: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
