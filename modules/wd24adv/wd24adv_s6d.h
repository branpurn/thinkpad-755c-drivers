/* SPDX-License-Identifier: GPL-2.0 */
/*
 * S6d: fault-banked fbdev on top of S6b dual-window banking.
 * Thrash latch: wd24adv_s6d_thrash.h. Window math: wd24adv_s6d_bank.h.
 * Live: register_framebuffer after 640x480-8 arm; last close / stop → end.
 */
#ifndef WD24ADV_S6D_H
#define WD24ADV_S6D_H

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#ifdef __KERNEL__
struct vm_area_struct;
int wd24s6d_map_win(struct wd24s6d *s, struct vm_area_struct *vma, u32 off);
#endif

#endif
#include "wd24adv_s6b.h"
#include "wd24adv_s6d_bank.h"
#include "wd24adv_s6d_thrash.h"

#define WD24S6D_XRES		640
#define WD24S6D_YRES		480
#define WD24S6D_BPP		8
#define WD24S6D_LINE		640
#define WD24S6D_FRAME		WD24S6B_FRAME	/* 0x4b000 */
#define WD24S6D_MMAP_LEN	WD24S6D_FRAME	/* first cut: visible frame only */
#define WD24S6D_APERTURE_PFN	(0xa0000ul >> 12)

struct wd24s6d {
	struct wd24s6b *b;		/* dual-window session (owned by caller) */
	struct wd24s6d_thrash thrash;
	u32 mapped_base;		/* window base page currently in PTEs; ~0 = none */
	u32 nfault;
	u32 nswitch;
	u32 nthrash;
	bool active;			/* begin() succeeded; end() owed */
	bool fb_reg;			/* register_framebuffer done */
	const char *why;
};

static inline void wd24s6d_init(struct wd24s6d *s, struct wd24s6b *b, u32 bound)
{
	memset(s, 0, sizeof(*s));
	s->b = b;
	s->mapped_base = ~0u;
	wd24s6d_thrash_init(&s->thrash, bound);
}

#ifdef __KERNEL__
struct vm_area_struct;
int wd24s6d_map_win(struct wd24s6d *s, struct vm_area_struct *vma, u32 off);
#endif

#endif
