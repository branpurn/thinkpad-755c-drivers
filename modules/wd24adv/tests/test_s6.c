/* host tests: S6 dual-window proof (wd24adv_s6.c) on the 86Box-semantics
 * model, plus fault-injected chips that must NOT be reported OK */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../wd24adv_s6.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u8 vram[0x100000], ref[0x100000], save[WD24S6_NREG * WD24S6_N];
static struct wd24s6 st;
static struct wd24s6_model m;

enum { L48 = 1, LIGN = 2, LREL = 4, LFF = 8, LP31 = 16 };
static int lockf;	/* lock faults for the next run() */

static void run(const char *name, int ig1, int ig7, int stuck, int refuse)
{
	struct wd24s6_io io = wd24s6_model_io;
	u8 g0[16], s0[0x20];

	wd24s6_model_init(&m, vram);
	m.ignore_pr1b3 = ig1;
	m.ignore_pr31b7 = ig7;
	m.pr1_stuck = stuck;
	m.refuse = refuse;
	m.lock_exact48 = !!(lockf & L48);
	m.lock_ignore = !!(lockf & LIGN);
	m.relock_stuck = !!(lockf & LREL);
	m.locked_ff = !!(lockf & LFF);
	if (lockf & LP31)
		m.sr[0x11] = 0x5c;
	memcpy(ref, vram, sizeof(ref));
	memcpy(g0, m.gr, 16);
	memcpy(s0, m.sr, 0x20);
	io.priv = &m;
	wd24s6_run(&st, &io, save);
	/* always: registers and every byte of video memory as before */
	if (!(lockf & LREL))
		CHECK(!memcmp(m.gr, g0, 16) && !memcmp(m.sr, s0, 0x20) && m.sr[6] == 0x0f,
		      "%s: registers restored, SR06 relocked", name);
	CHECK(!m.lock_open, "%s: lock step closed", name);
	CHECK(!memcmp(vram, ref, sizeof(ref)), "%s: video memory restored", name);
	CHECK(!m.open, "%s: guard closed", name);
	CHECK(st.nwr < 40, "%s: bounded writes %u", name, st.nwr);
	printf("--- %s: %s\n", name, wd24s6_res_name(st.res));
}

int main(void)
{
	u32 i;

	run("86Box semantics", 0, 0, 0, 0);
	CHECK(st.res == WD24S6_OK && st.split_ok && st.rw_ok, "OK");
	CHECK(st.rb_pr1_on == 0xcd && st.rb_pr31_on == 0xdd && st.rb_pr0b == 0x70, "readbacks");
	CHECK(st.sr06 == 0x0f && st.lock0 == 0 && st.pr31_locked == 0 && st.pr31 == 0x5d &&
	      st.rb_sr06_unl == 0x0f && st.lk_unl == 1 && st.rb_sr06_rel == 0x0f && st.lk_rel == 0,
	      "lock readbacks");
	CHECK(st.rb_pr1_off == 0xc5 && st.rb_pr31_off == 0x5d && !st.v_lo && !st.v_hi, "V1 + P4");
	CHECK(!st.rest_bad, "regions back");
	fputs(st.log, stdout);

	/* off-screen only: trace every address the program touches */
	{
		struct wd24s6_io io = wd24s6_model_io;
		u32 lo = 0xfffff, hi = 0;

		wd24s6_model_init(&m, vram);
		memcpy(ref, vram, sizeof(ref));
		io.priv = &m;
		/* break the save step to leave traces: use a chip that ignores both */
		m.ignore_pr1b3 = m.ignore_pr31b7 = 1;
		wd24s6_run(&st, &io, save);
		for (i = 0; i < sizeof(vram); i++)
			if (vram[i] != ref[i]) {
				lo = i < lo ? i : lo;
				hi = i;
			}
		CHECK(st.res == WD24S6_NONE, "chip ignoring both: NONE (%s)", wd24s6_res_name(st.res));
		CHECK(lo == 0xfffff && hi == 0, "ignoring chip: memory restored (%x..%x)", lo, hi);
	}
	run("PR31 b7 ignored", 0, 1, 0, 0);
	CHECK(st.res == WD24S6_SPLIT_ONLY, "split only");
	run("PR1 b3 ignored", 1, 0, 0, 0);
	CHECK(st.res == WD24S6_RW_ONLY, "rw only");
	run("PR1 b3 does not read back", 0, 0, 1, 0);
	CHECK(st.res == WD24S6_REG, "readback stop");
	run("PR1 write refused", 0, 0, 0, 1);
	CHECK(st.res == WD24S6_REG && st.rb_pr31_on == 0, "refused: stops before PR31");

	/* lock variants */
	lockf = LFF;
	run("locked SR07+ read FFh (86Box)", 0, 0, 0, 0);
	CHECK(st.res == WD24S6_OK && st.pr31_locked == 0xff, "FF variant OK");
	lockf = L48;
	run("unlock only on exactly 48h (86Box)", 0, 0, 0, 0);
	CHECK(st.res == WD24S6_REG && st.nwr == 2 && m.nreg == 2, "48h-only: REG after the pair, %u", st.nwr);
	lockf = LIGN;
	run("SR06 writes ignored", 0, 0, 0, 0);
	CHECK(st.res == WD24S6_REG && st.nwr == 2, "ignored unlock: REG, %u writes", st.nwr);
	lockf = LP31;
	run("PR31 not 5d", 0, 0, 0, 0);
	CHECK(st.res == WD24S6_ENTRY && st.nwr == 2 && st.pr31 == 0x5c, "PR31 5c: ENTRY, pair only");
	lockf = LREL;
	run("relock ignored", 0, 0, 0, 0);
	CHECK(st.res == WD24S6_RESTORE && st.nwr == 2 && st.lk_rel == 1, "relock stuck: RESTORE at E2");
	lockf = 0;

	/* entry checks before any write */
	{
		struct wd24s6_io io = wd24s6_model_io;
		static const struct { int r; u8 i, v; const char *n; } E[] = {
			{ 1, 0x0b, 0xcd, "PR1 b3 set" }, { 1, 0x0b, 0x85, "512K" },
			{ 0, 0x06, 0x4f, "SR06 already unlocked" }, { 1, 0x06, 0x01, "GR06 128K" },
			{ 1, 0x09, 0x10, "PR0A" } };
		for (i = 0; i < 5; i++) {
			wd24s6_model_init(&m, vram);
			memcpy(ref, vram, sizeof(ref));
			if (E[i].r)
				m.gr[E[i].i] = E[i].v;
			else
				m.sr[E[i].i] = E[i].v;
			io.priv = &m;
			wd24s6_run(&st, &io, save);
			CHECK(st.res == WD24S6_ENTRY && m.nreg == 0 && st.nwr == 0 &&
			      !memcmp(vram, ref, sizeof(ref)), "entry %s", E[i].n);
		}
	}

	/* the core guard */
	{
		static struct wd24_ctx c;

		memset(&c, 0, sizeof(c));
		c.saved = true;
		c.bios.pr20 = 0x0f;
		c.bios.pr31 = 0x5d;
		CHECK(wd24_guard(&c, WD24_GR, WD24_GR_PR1, 0xc5, 0xcd) != 0, "guard PR1 b3 closed");
		CHECK(wd24_guard(&c, WD24_SR, WD24_SR_PR31, 0x5d, 0xdd) != 0, "guard PR31 b7 closed");
		CHECK(wd24_guard(&c, WD24_GR, WD24_GR_PR1, 0xc5, 0xc5) == 0, "guard PR1 same ok");
		CHECK(wd24_guard(&c, WD24_SR, WD24_SR_PR31, 0x5d, 0x5d) == 0, "guard PR31 5D ok");
		CHECK(wd24_guard(&c, WD24_GR, WD24_GR_PR1, 0xcd, 0xc5) == 0, "guard PR1 b3 clear ok");
		/* outside S6: proven SR06 rule unchanged */
		CHECK(wd24_guard(&c, WD24_SR, WD24_SR_PR20, 0x0f, 0x4f) == 0, "outside S6: unlock ok");
		CHECK(wd24_guard(&c, WD24_SR, WD24_SR_PR20, 0x0f, 0x0f) == 0, "outside S6: relock ok");
		c.dual_open = c.s6_active = true;
		CHECK(wd24_guard(&c, WD24_GR, WD24_GR_PR1, 0xc5, 0xcd) == 0, "guard PR1 b3 open");
		CHECK(wd24_guard(&c, WD24_SR, WD24_SR_PR31, 0x5d, 0xdd) != 0, "PR31 b7 refused outside lock step");
		CHECK(wd24_guard(&c, WD24_SR, WD24_SR_PR20, 0x0f, 0x4f) != 0, "S6: 4F refused outside lock step");
		CHECK(wd24_guard(&c, WD24_SR, WD24_SR_PR20, 0x0f, 0x0f) == 0, "S6: relock always ok");
		c.s6_lock_open = true;
		CHECK(wd24_guard(&c, WD24_SR, WD24_SR_PR20, 0x0f, 0x4f) == 0, "S6 lock step: 4F ok");
		CHECK(wd24_guard(&c, WD24_SR, WD24_SR_PR20, 0x0f, 0x48) != 0, "S6 lock step: 48 refused");
		CHECK(wd24_guard(&c, WD24_SR, WD24_SR_PR20, 0x0f, 0x5f) != 0, "S6 lock step: 5F refused");
		CHECK(wd24_guard(&c, WD24_SR, WD24_SR_PR31, 0x5d, 0xdd) == 0, "guard PR31 b7 in lock step");
		CHECK(wd24_guard(&c, WD24_GR, WD24_GR_PR1, 0xc5, 0xf5) != 0, "aperture still refused when open");
		c.s6_active = false;
		CHECK(wd24_guard(&c, WD24_SR, WD24_SR_PR31, 0x5d, 0xdd) == 0, "dual+lock flags (core rule)");
		c.dual_open = false;
		CHECK(wd24_guard(&c, WD24_SR, WD24_SR_PR31, 0x5d, 0xdd) != 0, "PR31 b7 needs dual_open too");
	}
	printf("test_s6: %d passed, %d failed\n", checks - fails, fails);
	return fails != 0;
}
