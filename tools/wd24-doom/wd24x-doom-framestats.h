/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Per-present frame intervals. The caller passes CLOCK_MONOTONIC
 * deltas. Off means the caller does not sample the clock.
 * 1 tic is 1000/35 ms. A stored interval is whole milliseconds.
 */
#ifndef WD24X_DOOM_FRAMESTATS_H
#define WD24X_DOOM_FRAMESTATS_H

#include <stdint.h>

#define WD24X_FS_CAP		8192u
#define WD24X_FS_BUCKETS	12u
#define WD24X_FS_TICS		8u

/* Upper edge of buckets 0..10. Bucket 11 is everything at or above
 * the last edge.
 */
static const unsigned wd24x_fs_edge[WD24X_FS_BUCKETS - 1u] = {
	5u, 10u, 15u, 20u, 29u, 40u, 58u, 80u, 114u, 160u, 240u
};

struct wd24x_doom_fs {
	unsigned frames;
	unsigned n;
	uint16_t ms[WD24X_FS_CAP];
	unsigned hist[WD24X_FS_BUCKETS];
	unsigned over1;
	unsigned over2;
	unsigned tics[WD24X_FS_TICS];
	uint64_t render_ns;
	uint64_t copy_ns;
	uint64_t flip_ns;
};

static inline unsigned wd24x_fs_bucket(unsigned ms)
{
	unsigned i;

	for (i = 0; i < WD24X_FS_BUCKETS - 1u; i++) {
		if (ms < wd24x_fs_edge[i])
			return i;
	}
	return WD24X_FS_BUCKETS - 1u;
}

/* ms is greater than `tics` Doom tics (1000/35 ms each). */
static inline int wd24x_fs_over_tics(unsigned ms, unsigned tics)
{
	return ms * 35u > tics * 1000u;
}

static inline uint32_t wd24x_doom_ms_of(uint64_t sec, uint32_t nsec)
{
	return (uint32_t)(sec * 1000u + nsec / 1000000u);
}

/* Vanilla I_GetTime: whole tics from a millisecond count. */
static inline int wd24x_doom_tics_from_ms(uint32_t ms)
{
	return (int)((ms * 35u) / 1000u);
}

static inline void wd24x_fs_clear(struct wd24x_doom_fs *s)
{
	unsigned i;

	s->frames = 0;
	s->n = 0;
	s->over1 = 0;
	s->over2 = 0;
	s->render_ns = 0;
	s->copy_ns = 0;
	s->flip_ns = 0;
	for (i = 0; i < WD24X_FS_BUCKETS; i++)
		s->hist[i] = 0;
	for (i = 0; i < WD24X_FS_TICS; i++)
		s->tics[i] = 0;
}

static inline void wd24x_fs_add(struct wd24x_doom_fs *s, unsigned interval_ms,
				unsigned tics, uint64_t render_ns,
				uint64_t copy_ns, uint64_t flip_ns)
{
	unsigned t;

	if (interval_ms > 65535u)
		interval_ms = 65535u;
	if (s->n < WD24X_FS_CAP)
		s->ms[s->n++] = (uint16_t)interval_ms;
	s->frames++;
	s->hist[wd24x_fs_bucket(interval_ms)]++;
	if (wd24x_fs_over_tics(interval_ms, 1))
		s->over1++;
	if (wd24x_fs_over_tics(interval_ms, 2))
		s->over2++;
	t = tics;
	if (t >= WD24X_FS_TICS)
		t = WD24X_FS_TICS - 1u;
	s->tics[t]++;
	s->render_ns += render_ns;
	s->copy_ns += copy_ns;
	s->flip_ns += flip_ns;
}

static inline void wd24x_fs_sort(uint16_t *v, unsigned n)
{
	unsigned i, j;

	for (i = 1; i < n; i++) {
		uint16_t x = v[i];

		j = i;
		while (j > 0 && v[j - 1] > x) {
			v[j] = v[j - 1];
			j--;
		}
		v[j] = x;
	}
}

/* Nearest rank. p is 0..100. n is the stored count. */
static inline unsigned wd24x_fs_pct(const uint16_t *sorted, unsigned n,
				    unsigned p)
{
	unsigned index;

	if (!n)
		return 0;
	if (p > 100u)
		p = 100u;
	index = (n * p + 99u) / 100u;
	if (index == 0)
		return sorted[0];
	index--;
	if (index >= n)
		index = n - 1u;
	return sorted[index];
}

#endif
