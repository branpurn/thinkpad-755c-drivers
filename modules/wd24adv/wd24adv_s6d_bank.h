/* SPDX-License-Identifier: GPL-2.0 */
/*
 * S6d fault-bank window placement (pure math, host-testable).
 *
 * The aperture is always A0000-AFFFF (64 KiB).  A fault at VRAM offset off
 * must aim that aperture at a dual-window (w0, w1) that covers off, using
 * the S6b half size (8 pages = 32 KiB per half).  Mapping one 64 KiB window
 * at a time avoids the need for a shadow; a single x86 instruction that
 * touches two different 64 KiB windows can still livelock under fault
 * restart — callers must detect thrashing (see WD24ADV-NEXT-STAGE.md).
 *
 * Standalone: no kernel headers.  Keep WD24S6D_HPG in sync with WD24S6B_HPG.
 */
#ifndef WD24ADV_S6D_BANK_H
#define WD24ADV_S6D_BANK_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint32_t u32;
#endif

#define WD24S6D_WIN		0x10000u	/* 64 KiB aperture */
#define WD24S6D_PAGE		0x1000u
#define WD24S6D_HPG		8u		/* = WD24S6B_HPG: pages per half */

/* 4 KiB page index of the 64 KiB window that contains off. */
static inline u32 wd24s6d_win_base_pg(u32 off)
{
	return (off / WD24S6D_WIN) * (WD24S6D_WIN / WD24S6D_PAGE);
}

/* Dual-window halves covering off: w0 = base, w1 = base + HPG (S6b layout). */
static inline void wd24s6d_win_for_off(u32 off, u32 *w0, u32 *w1)
{
	u32 b = wd24s6d_win_base_pg(off);

	*w0 = b;
	*w1 = b + WD24S6D_HPG;
}

/* True if off is visible in the aperture when halves are (w0, w1). */
static inline int wd24s6d_off_in_win(u32 off, u32 w0, u32 w1)
{
	u32 pg = off / WD24S6D_PAGE;

	return (pg >= w0 && pg < w0 + WD24S6D_HPG) ||
	       (pg >= w1 && pg < w1 + WD24S6D_HPG);
}

/*
 * Two offsets in different 64 KiB windows: a single spanning access can
 * livelock if the handler maps only one window at a time.
 */
static inline int wd24s6d_livelock_span(u32 a, u32 b)
{
	return wd24s6d_win_base_pg(a) != wd24s6d_win_base_pg(b);
}

#endif
