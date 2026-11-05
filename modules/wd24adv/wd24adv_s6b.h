/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wd24adv S6b: explicit dual-window mmap + dual-window (PR31 b7) copy.
 *
 * Session = one open of /dev/wd24s6b while the PROVEN 640x480-8 mode is up
 * (s6b=1 dryrun=0 live=1 mode=640x480-8).
 *
 * Steady state (SR06 LOCKED, PR31 b7 = 0, PR1 b3 = 1, all proven live S6):
 *   A0000-A7FFF -> VRAM w0 * 4K .. +32K   (PR0B = w0)
 *   A8000-AFFFF -> VRAM w1 * 4K .. +32K   (PR0A = w1 - 8)
 * The 64 KiB window is mapped ONCE at mmap time (vm_iomap_memory); userspace
 * moves the two halves with SETWIN.  There is no page fault on the window,
 * so the fault-banking livelock (one instruction touching two banks) cannot
 * happen: a cross-bank memmove points w0 at the source and w1 at the
 * destination and both are mapped at the same time.  (Fault banking itself
 * is not buildable for the tc755 kernel: vmf_insert_pfn, zap_vma_ptes and
 * unmap_mapping_range are not exported.)
 *
 * COPY (memmove in VRAM, kernel side): PR1 b3 off (PR1 = BIOS value, the
 * state S6c proved b7 in) -> SR06 unlock -> PR31 |= 80h -> SR06 relock ->
 * copy in 4 KiB chunks, reads through PR0A (source bank), writes through
 * PR0B (destination bank), SR06 LOCKED (S6c) -> unlock -> PR31 = base ->
 * relock -> PR1 b3 on, PR0B/PR0A back to w0/w1.  SR06 is unlocked only
 * to set and to clear PR31 b7.
 *
 * Every write goes through the core guard (open = c->dual_open +
 * c->s6_active; lock = c->s6_lock_open), reads back, and any failure makes
 * the session DEAD after a best-effort restore.  end() restores PR31 base
 * (if b7 may be set), PR1, PR0A, PR0B to the entry values and closes the
 * guard.  Pure logic, host-testable.
 */
#ifndef WD24ADV_S6B_H
#define WD24ADV_S6B_H

#include "wd24adv_s6.h"

#define WD24S6B_VRAM	0x80000		/* PR0A/PR0B bits 6:0: 512 KiB reach */
#define WD24S6B_FRAME	0x4b000		/* 640x480-8 visible frame */
#define WD24S6B_HPG	8		/* 4 KiB pages per window half */
#define WD24S6B_MAXW	(WD24S6B_VRAM / 0x1000 - WD24S6B_HPG)	/* 78h */
#define WD24S6B_CHUNK	0x1000
#define WD24S6B_LOG	4096

enum wd24s6b_state { WD24S6B_OFF, WD24S6B_ON, WD24S6B_DEAD };

struct wd24s6b {
	const struct wd24s6_io *io;
	/* entry snapshot */
	u8 pr0a, pr0b, pr1, gr6, sr06;
	u8 pr31;		/* base, read unlocked at the first COPY */
	bool pr31_known, b7;	/* b7: PR31 b7 may be set (cleanup needed) */
	bool pr1set, unl;	/* unl: SR06 may be unlocked */
	u8 w0, w1;		/* window halves, 4 KiB page numbers */
	u8 cur_a, cur_b;	/* PR0A / PR0B as written */
	enum wd24s6b_state state;
	const char *why;
	u32 nwr, ncopy, nset, rest_bad;
	u8 buf[WD24S6B_CHUNK];
	char log[WD24S6B_LOG];
	u32 loglen;
};

int wd24s6b_begin(struct wd24s6b *b, const struct wd24s6_io *io);
int wd24s6b_setwin(struct wd24s6b *b, u32 w0, u32 w1);
int wd24s6b_copy(struct wd24s6b *b, u32 dst, u32 src, u32 len);
int wd24s6b_end(struct wd24s6b *b);
const char *wd24s6b_state_name(enum wd24s6b_state s);

#endif
