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

static void run(const char *name, int ig1, int ig7, int stuck, int refuse)
{
	struct wd24s6_io io = wd24s6_model_io;
	u8 g0[16], s0[0x20];

	wd24s6_model_init(&m, vram);
	m.ignore_pr1b3 = ig1;
	m.ignore_pr31b7 = ig7;
	m.pr1_stuck = stuck;
	m.refuse = refuse;
	memcpy(ref, vram, sizeof(ref));
	memcpy(g0, m.gr, 16);
	memcpy(s0, m.sr, 0x20);
	io.priv = &m;
	wd24s6_run(&st, &io, save);
	/* always: registers and every byte of video memory as before */
	CHECK(!memcmp(m.gr, g0, 16) && !memcmp(m.sr, s0, 0x20), "%s: registers restored", name);
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

	/* entry checks: nothing written */
	{
		struct wd24s6_io io = wd24s6_model_io;
		static const struct { int r; u8 i, v; const char *n; } E[] = {
			{ 1, 0x0b, 0xcd, "PR1 b3 set" }, { 1, 0x0b, 0x85, "512K" },
			{ 0, 0x11, 0xdd, "PR31 b7" }, { 1, 0x06, 0x01, "GR06 128K" },
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

	/* the core guard: b3 / b7 refused unless dual_open */
	{
		static struct wd24_ctx c;

		memset(&c, 0, sizeof(c));
		CHECK(wd24_guard(&c, WD24_GR, WD24_GR_PR1, 0xc5, 0xcd) != 0, "guard PR1 b3 closed");
		CHECK(wd24_guard(&c, WD24_SR, WD24_SR_PR31, 0x5d, 0xdd) != 0, "guard PR31 b7 closed");
		CHECK(wd24_guard(&c, WD24_GR, WD24_GR_PR1, 0xc5, 0xc5) == 0, "guard PR1 same ok");
		CHECK(wd24_guard(&c, WD24_SR, WD24_SR_PR31, 0x5d, 0x5d) == 0, "guard PR31 5D ok");
		CHECK(wd24_guard(&c, WD24_GR, WD24_GR_PR1, 0xcd, 0xc5) == 0, "guard PR1 b3 clear ok");
		c.dual_open = true;
		CHECK(wd24_guard(&c, WD24_GR, WD24_GR_PR1, 0xc5, 0xcd) == 0, "guard PR1 b3 open");
		CHECK(wd24_guard(&c, WD24_SR, WD24_SR_PR31, 0x5d, 0xdd) == 0, "guard PR31 b7 open");
		CHECK(wd24_guard(&c, WD24_GR, WD24_GR_PR1, 0xc5, 0xf5) != 0, "aperture still refused when open");
	}
	printf("test_s6: %d passed, %d failed\n", checks - fails, fails);
	return fails != 0;
}
