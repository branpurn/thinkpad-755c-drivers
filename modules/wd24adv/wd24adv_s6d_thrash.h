/* SPDX-License-Identifier: GPL-2.0 */
/*
 * S6d thrash latch (pure math, host-testable).
 *
 * Symptom of the fault-livelock hole: faults alternate between two 64 KiB
 * windows without a quiet same-window stretch (one x86 insn spanning banks
 * restart-faults forever). Count consecutive window *switches*; latch when
 * the count reaches the bound. A fault that stays in the current window
 * resets the counter. See WD24ADV-NEXT-STAGE.md / DESKTOP-DESIGN §3.3.
 */
#ifndef WD24ADV_S6D_THRASH_H
#define WD24ADV_S6D_THRASH_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint32_t u32;
typedef uint8_t u8;
typedef int32_t s32;
#endif

#define WD24S6D_THRASH_BOUND_DEFAULT	32u

struct wd24s6d_thrash {
	u32 cur;		/* current window base page; ~0u = none */
	u32 switches;		/* consecutive switches since last quiet */
	u32 bound;		/* latch when switches reaches bound */
	u32 trips;		/* how many times the latch fired */
	u8 latched;		/* sticky until reset */
};

static inline void wd24s6d_thrash_init(struct wd24s6d_thrash *t, u32 bound)
{
	t->cur = ~0u;
	t->switches = 0;
	t->bound = bound ? bound : WD24S6D_THRASH_BOUND_DEFAULT;
	t->trips = 0;
	t->latched = 0;
}

static inline void wd24s6d_thrash_reset(struct wd24s6d_thrash *t)
{
	u32 bound = t->bound;
	u32 trips = t->trips;

	wd24s6d_thrash_init(t, bound);
	t->trips = trips;
}

/*
 * Note a fault targeting window base page `win`. Returns 0 if the mapping
 * may proceed, -1 if the latch is (now) set — caller must refuse the fault.
 */
static inline int wd24s6d_thrash_note(struct wd24s6d_thrash *t, u32 win)
{
	if (t->latched)
		return -1;
	if (t->cur == ~0u) {
		t->cur = win;
		t->switches = 0;
		return 0;
	}
	if (win == t->cur) {
		t->switches = 0;
		return 0;
	}
	t->cur = win;
	t->switches++;
	if (t->switches >= t->bound) {
		t->latched = 1;
		t->trips++;
		return -1;
	}
	return 0;
}

#endif
