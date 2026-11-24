/* SPDX-License-Identifier: GPL-2.0 */
/*
 * When the trial timer is allowed to restore text.
 * A cmap publish that never maps the frame is not a confirm and does
 * not cancel the timer. Fault count is not an input.
 */
#ifndef WD24ADV_TRIAL_H
#define WD24ADV_TRIAL_H

struct wd24_trial_gate {
	int pending;
	int faults;
	int has_vma;
	int stopping;
	int active;
};

/* User fd went away. A mapped fd ends the banking session. A publish-only
  * fd does not: the trial timer still owns the mode. pending is not cleared.
  */
static inline void wd24_trial_note_release(struct wd24_trial_gate *g)
{
	if (!g)
		return;
	if (g->has_vma) {
		g->stopping = 1;
		g->active = 0;
	}
}

/* 1 = run text restore now. 0 = not yet (still registering, or nothing pending).
  * faults, stopping, and active are deliberately unused.
  */
static inline int wd24_trial_expire_now(const struct wd24_trial_gate *g,
					int registering, int exiting)
{
	if (!g || !g->pending)
		return 0;
	if (registering && !exiting)
		return 0;
	(void)g->faults;
	(void)g->stopping;
	(void)g->active;
	return 1;
}

#endif
