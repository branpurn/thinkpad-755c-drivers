/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Host-testable checks for the palette clients.
 * A value n<<10 is what s6d_cmap_publish stores from a 6-bit DAC byte.
 * FBIOGETCMAP returns that cache. It does not read the DAC.
 */
#ifndef WD24X_CMAP_CHECK_H
#define WD24X_CMAP_CHECK_H

#include <stdint.h>

#define WD24X_CMAP_N		256
#define WD24X_CMAP_SET_INDEX	7
#define WD24X_CMAP_SET_VALUE	0x8000u

static inline int wd24x_cmap_is_shift10(unsigned v)
{
	return v <= 0xfc00u && (v & 0x03ffu) == 0;
}

static inline int wd24x_cmap_all_shift10(const uint16_t *r, const uint16_t *g,
					 const uint16_t *b)
{
	int i;

	if (!r || !g || !b)
		return 0;
	for (i = 0; i < WD24X_CMAP_N; i++) {
		if (!wd24x_cmap_is_shift10(r[i]) ||
		    !wd24x_cmap_is_shift10(g[i]) ||
		    !wd24x_cmap_is_shift10(b[i]))
			return 0;
	}
	return 1;
}

/* Second GET after one successful PUT of index 7. Other entries stay. */
static inline int wd24x_cmap_set_cache_ok(const uint16_t *r0, const uint16_t *g0,
					  const uint16_t *b0, const uint16_t *r1,
					  const uint16_t *g1, const uint16_t *b1)
{
	int i;

	if (!r0 || !g0 || !b0 || !r1 || !g1 || !b1)
		return 0;
	if (r1[WD24X_CMAP_SET_INDEX] != WD24X_CMAP_SET_VALUE ||
	    g1[WD24X_CMAP_SET_INDEX] != WD24X_CMAP_SET_VALUE ||
	    b1[WD24X_CMAP_SET_INDEX] != WD24X_CMAP_SET_VALUE)
		return 0;
	for (i = 0; i < WD24X_CMAP_N; i++) {
		if (i == WD24X_CMAP_SET_INDEX)
			continue;
		if (r0[i] != r1[i] || g0[i] != g1[i] || b0[i] != b1[i])
			return 0;
	}
	return 1;
}

#endif
