/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cumulative frame times. render is the gap since the previous
 * present returned (the game and D_Display, up to I_FinishUpdate).
 * palette and present are inside that finish. Off unless every > 0.
 * The 486 has no rdtsc. The caller passes clock_gettime values.
 */
#ifndef WD24X_DOOM_TIME_H
#define WD24X_DOOM_TIME_H

#include <stdint.h>

struct wd24x_doom_time {
	unsigned frames;
	unsigned every;
	uint64_t render_ns;
	uint64_t present_ns;
	uint64_t palette_ns;
	uint64_t left_ns;
};

static inline void wd24x_doom_time_init(struct wd24x_doom_time *t,
					unsigned every)
{
	t->frames = 0;
	t->every = every;
	t->render_ns = 0;
	t->present_ns = 0;
	t->palette_ns = 0;
	t->left_ns = 0;
}

/* 1 when this frame should be printed. */
static inline int wd24x_doom_time_frame(struct wd24x_doom_time *t,
					uint64_t entered,
					uint64_t after_palette,
					uint64_t left)
{
	if (!t || !t->every)
		return 0;
	if (t->left_ns && entered >= t->left_ns)
		t->render_ns += entered - t->left_ns;
	if (after_palette >= entered)
		t->palette_ns += after_palette - entered;
	if (left >= after_palette)
		t->present_ns += left - after_palette;
	t->left_ns = left;
	t->frames++;
	return (t->frames % t->every) == 0;
}

static inline uint64_t wd24x_doom_time_ms(uint64_t ns)
{
	return ns / 1000000ull;
}

#endif
