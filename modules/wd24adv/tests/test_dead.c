// SPDX-License-Identifier: GPL-2.0
/* Host test of the S3 timeout lock (wd24_ctx.no_vram) on the port backend
 * against the VGA model: with the lock set, trial expiry / text restore
 * touch registers only (ZERO video memory accesses) and every memory path
 * (mode set, pattern, load/save) is refused; without it the normal full
 * restore runs. */
#include <stdio.h>
#include <string.h>
#include "vgamodel.h"

static int fails, passes;
#define T(cond, what) do { if (cond) passes++; else { fails++; \
	printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

static struct vgamodel m, snap;
static struct wd24_ctx c;
static struct wd24_io io, inner;
static u8 planes[4 * WD24_PLANE];
static unsigned long nmr, nmw;

static u8 cmemr(void *p, u32 off) { nmr++; return inner.memr(p, off); }
static void cmemw(void *p, u32 off, u8 v) { nmw++; inner.memw(p, off, v); }

static void setup(unsigned int seed)
{
	vm_init(&m, VM_WD24, seed);
	vm_io(&inner, &m);
	io = inner;
	io.memr = cmemr;
	io.memw = cmemw;
	wd24_ctx_init(&c, &wd24_port_backend);
	c.io = &io;
	c.planes = planes;
	memset(planes, 0, sizeof(planes));
	snap = m;
}

#define RUN(expr) ({ int _e; c.be->begin(&c); _e = (expr); c.be->end(&c); _e; })
#define MEM() (nmr + nmw)

int main(void)
{
	const struct wd24_mode *mm[3] = { wd24_mode_find("640x480-8"),
		wd24_mode_find("640x400-8"), wd24_mode_find("320x200-8") };
	unsigned long dw, m0;
	int k, err;

	for (k = 0; k < 3; k++) {
		/* timeout, engine still busy: expiry is registers only */
		setup(100 + k);
		T(RUN(wd24_plan_load(&c)) == 0 && RUN(wd24_plan_mode(&c, mm[k])) == 0, "load + mode");
		T(RUN(wd24_pattern(&c, mm[k])) == 0 && MEM() > 0, "pattern writes memory (unlocked)");
		c.no_vram = true;
		m0 = MEM();
		T(RUN(wd24_pattern(&c, mm[k])) != 0 && MEM() == m0, "locked, still in mode: pattern refused, no access");
		dw = m.data_writes;
		err = RUN(wd24_trial_expire(&c));
		T(err == 0 && MEM() == m0, "locked expiry: ZERO video memory accesses");
		T(m.data_writes > dw && c.in_text && !c.trial_pending, "locked expiry: registers restored, in text");
		/* sticky: every later memory path refused, still zero accesses */
		m0 = MEM(); dw = m.data_writes;
		T(RUN(wd24_plan_mode(&c, mm[k])) == -EIO && m.data_writes == dw, "locked: mode set refused, no write");
		T(RUN(wd24_pattern(&c, mm[k])) != 0, "locked: pattern refused");
		T(RUN(wd24_save_planes(&c)) == -EIO, "locked: plane save refused");
		T(RUN(wd24_plan_text(&c)) == 0, "locked: text = registers only");
		T(MEM() == m0, "locked: ZERO memory accesses across mode/pattern/save/text");
		/* reload: a fresh load with the lock refuses before saving memory */
		c.no_vram = true;
		RUN(wd24_plan_load(&c));
		T(MEM() == m0, "locked: load path never reads memory");

		/* timeout, then seen idle: normal restore (lock not set) */
		setup(200 + k);
		RUN(wd24_plan_load(&c));
		RUN(wd24_plan_mode(&c, mm[k]));
		RUN(wd24_pattern(&c, mm[k]));
		m0 = MEM();
		T(RUN(wd24_trial_expire(&c)) == 0 && MEM() > m0, "idle: normal expiry restores planes");
		T(vm_cmp(&m, &snap, "idle expiry") == 0, "idle: model identical to boot (incl. memory)");
	}
	printf("test_dead: %d passed, %d failed\n", passes, fails);
	return fails != 0;
}
