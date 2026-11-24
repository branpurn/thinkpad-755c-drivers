/* SPDX-License-Identifier: GPL-2.0 */
/* Publish-only close must not cancel the trial. No mmap, faults stay 0. */
#include <stdio.h>
#include "wd24adv_trial.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

struct clock {
	unsigned now;
	unsigned deadline;
	int pending;
	int fired;
	int in_text;
};

static void arm(struct clock *c, unsigned trial)
{
	c->deadline = c->now + trial;
	c->pending = 1;
	c->fired = 0;
	c->in_text = 0;
}

static void publish_release(struct clock *c, int has_vma, int faults)
{
	struct wd24_trial_gate g;

	g.pending = c->pending;
	g.faults = faults;
	g.has_vma = has_vma;
	g.stopping = 0;
	g.active = 1;
	wd24_trial_note_release(&g);
	c->pending = g.pending;
}

static void tick(struct clock *c, unsigned now, int faults)
{
	struct wd24_trial_gate g;

	c->now = now;
	g.pending = c->pending;
	g.faults = faults;
	g.has_vma = 0;
	g.stopping = 0;
	g.active = 1;
	if (c->now < c->deadline)
		return;
	if (!wd24_trial_expire_now(&g, 0, 0))
		return;
	c->fired = 1;
	c->pending = 0;
	c->in_text = 1;
}

int main(void)
{
	struct clock c;
	struct wd24_trial_gate g;

	c.now = 0;
	arm(&c, 180);
	publish_release(&c, 0, 0);
	T(c.pending == 1, "nomap release keeps pending");
	tick(&c, 179, 0);
	T(!c.fired && c.pending == 1 && !c.in_text, "before window");
	tick(&c, 180, 0);
	T(c.fired && !c.pending && c.in_text, "nomap expiry at trial");

	c.now = 0;
	arm(&c, 180);
	publish_release(&c, 0, 0);
	tick(&c, 190, 0);
	T(c.fired && c.in_text, "still due after the window");

	g.pending = 1;
	g.faults = 0;
	g.has_vma = 1;
	g.stopping = 0;
	g.active = 1;
	wd24_trial_note_release(&g);
	T(g.pending == 1 && g.stopping == 1 && g.active == 0, "mapped release");
	T(wd24_trial_expire_now(&g, 0, 0) == 1, "mapped still expires");

	g.faults = 0;
	g.pending = 1;
	T(wd24_trial_expire_now(&g, 0, 0) == 1, "zero faults expire");
	g.faults = 15;
	T(wd24_trial_expire_now(&g, 0, 0) == 1, "faults are not a gate");
	T(wd24_trial_expire_now(&g, 1, 0) == 0, "wait out registration");
	T(wd24_trial_expire_now(&g, 1, 1) == 1, "exit does not wait");
	g.pending = 0;
	T(wd24_trial_expire_now(&g, 0, 0) == 0, "idle");

	printf("test_trial_nomap: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
