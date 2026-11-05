/* SPDX-License-Identifier: GPL-2.0 */
/*
 * S6d fault decision (pure, host-testable).
 *
 * The kernel fault handler (wd24s6d_map_win) performs zap / SETWIN /
 * vmf_insert_pfn only as this plan says. A refused plan must not change
 * the hardware window. nswitch is incremented by the caller only after
 * SETWIN succeeds — a failed SETWIN is not counted.
 *
 * Probe touches are the bounded mmap script: one faulting byte per step.
 * An 8x8 stamp is stored only while that window is already mapped, so it
 * adds no faults. Do not touch two 64 KiB windows in one instruction.
 */
#ifndef WD24ADV_S6D_FAULT_H
#define WD24ADV_S6D_FAULT_H

#include "wd24adv_s6d_bank.h"
#include "wd24adv_s6d_thrash.h"

#ifndef __KERNEL__
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EIO
#define EIO 5
#endif
#endif

struct wd24s6d_mapreq {
	u32 off;
	u32 mapped_base;	/* ~0u = no window installed */
	u32 mmap_len;		/* frame length (WD24S6D_MMAP_LEN) */
	u32 vma_len;		/* bytes the VMA covers; 0 is refuse */
};

struct wd24s6d_mapact {
	int err;		/* 0, or a negative errno; no hardware change */
	const char *why;
	u32 base;
	u32 w0, w1;
	int zap;		/* zap the previous window before SETWIN */
	u32 zap_base;
	u32 zap_pages;		/* pages of that window that lie in the VMA */
	int setwin;		/* hardware window must move */
	int insert;		/* install aperture PFNs that lie in the VMA */
	u32 npages;		/* <= 16; the frame tail is a short window */
};

/* Pages of the 64 KiB window at base_pg that are fully inside vma_len.
 * The 640x480 frame is page-aligned, so the last window is short. */
static inline u32 wd24s6d_pages_in_vma(u32 base_pg, u32 vma_len)
{
	u32 off = base_pg << 12;
	u32 avail;

	if (off >= vma_len)
		return 0;
	avail = vma_len - off;
	if (avail > WD24S6D_WIN)
		avail = WD24S6D_WIN;
	return avail / WD24S6D_PAGE;
}

static inline void wd24s6d_plan_fault(struct wd24s6d_thrash *t,
				      const struct wd24s6d_mapreq *q,
				      u32 *nfault, u32 *nthrash,
				      struct wd24s6d_mapact *a)
{
	u32 w0, w1, base;

	a->err = 0;
	a->why = "";
	a->base = 0;
	a->w0 = 0;
	a->w1 = 0;
	a->zap = 0;
	a->zap_base = ~0u;
	a->zap_pages = 0;
	a->setwin = 0;
	a->insert = 0;
	a->npages = 0;

	if (!t || !q || !q->vma_len || q->off >= q->mmap_len) {
		a->err = -EINVAL;
		a->why = "range";
		return;
	}
	if (t->latched) {
		if (nthrash)
			(*nthrash)++;
		a->err = -EIO;
		a->why = "thrash latch";
		return;
	}

	wd24s6d_win_for_off(q->off, &w0, &w1);
	base = wd24s6d_win_base_pg(q->off);
	if (wd24s6d_thrash_note(t, base)) {
		if (nthrash)
			(*nthrash)++;
		a->err = -EIO;
		a->why = "thrash latch";
		return;
	}
	if (nfault)
		(*nfault)++;

	a->base = base;
	a->w0 = w0;
	a->w1 = w1;
	a->npages = wd24s6d_pages_in_vma(base, q->vma_len);
	if (!a->npages) {
		a->err = -EINVAL;
		a->why = "VMA range";
		return;
	}
	if (q->mapped_base != ~0u && q->mapped_base != base) {
		a->zap_pages = wd24s6d_pages_in_vma(q->mapped_base, q->vma_len);
		if (a->zap_pages) {
			a->zap = 1;
			a->zap_base = q->mapped_base;
		}
	}
	if (q->mapped_base != base)
		a->setwin = 1;
	a->insert = 1;
}

/* Bounded mmap script. 8x8 stamps use BIOS text-DAC indices; hue is not
 * claimed here. Tail byte then a return to window 0 are single bytes. */
#define WD24S6D_STAMP		8u
#define WD24S6D_STAMP_N		5u
#define WD24S6D_TOUCH_N		7u
#define WD24S6D_FRAME_BYTES	0x4b000u

static inline u32 wd24s6d_stamp_off(unsigned i)
{
	static const u32 off[WD24S6D_STAMP_N] = {
		0x00000u, 0x10000u, 0x20000u, 0x30000u, 0x40000u
	};

	return i < WD24S6D_STAMP_N ? off[i] : 0;
}

static inline u8 wd24s6d_stamp_pix(unsigned i)
{
	static const u8 pix[WD24S6D_STAMP_N] = { 1, 2, 4, 14, 15 };

	return i < WD24S6D_STAMP_N ? pix[i] : 0;
}

/* First-byte touches, in order. Stamps are filled only after touch i<5. */
static inline u32 wd24s6d_touch_off(unsigned i)
{
	static const u32 off[WD24S6D_TOUCH_N] = {
		0x00000u, 0x10000u, 0x20000u, 0x30000u, 0x40000u,
		0x4afffu,	/* last frame byte; same window as 0x40000 */
		0x00010u	/* back to window 0 */
	};

	return i < WD24S6D_TOUCH_N ? off[i] : 0;
}

/* Faults and successful switches for wd24s6d_touch_off[] on a full-frame VMA
 * with the thrash bound left at its default (no latch). */
#define WD24S6D_PROBE_FAULTS	6u
#define WD24S6D_PROBE_SWITCHES	6u

/* Stop / teardown order. unregister is skipped when no fb was registered.
 * text-restore is the caller's next step (trial expiry, "text", rmmod),
 * not inside the fb stop itself. */
#define WD24S6D_STOP_N		5
static inline const char *wd24s6d_stop_step(unsigned i)
{
	static const char *const name[WD24S6D_STOP_N] = {
		"quiesce", "unmap", "end-session", "unregister", "text-restore"
	};

	return i < WD24S6D_STOP_N ? name[i] : "";
}

#endif
