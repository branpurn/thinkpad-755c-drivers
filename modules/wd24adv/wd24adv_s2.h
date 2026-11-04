/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wd24adv S2: write + readback of the WD90C24A block 1 (BITBLT) and
 * block 2 (cursor) registers with the engine start bit never set and the
 * cursor never enabled.  Design doc docs/WD24ADV-DESKTOP-DESIGN.md
 * section 7, stage S2; runs in the text state like S1 (s2=1 dryrun=0),
 * once, inside insmod: everything written is put back and verified before
 * insmod returns, so nothing stays changed.
 *
 * Snapshot (rework 2026-11-04: the S1 values are not stable across boots,
 * docs/s2-live-2026-11-04-run.txt): after the S1-style entry reads (23C0h
 * must be 1002h), all 25 23C2h words are read twice (selects + reads
 * only) into snap[] and snap2[].  S2 refuses with no 23C2h write unless
 * the two passes agree, every word echoes its index in bits 15:12,
 * CNTRL1 has ACT (b11) clear and cursor control has enable (b11) clear.
 *
 * The closed write list wd24s2_list[] (17 registers).  For each one, in
 * order:
 *   W 23C0h sel; R 23C0h == sel
 *   W 23C2h (idx << 12) | test; R 23C0h == sel; R 23C2h == that word
 *   W 23C2h snap;               R 23C2h == snap
 *   W 23C0h 1001h (CNTRL1) / 1002h (cursor control); R 23C0h == it;
 *   R 23C2h == its snapshot value (unchanged)
 * Post-pass: all 25 words == snap[]; then W 23C0h 1002h, R == 1002h.
 * Never written: block 1 idx 0 (CNTRL1, holds ACT), C, D, F; block 2 idx
 * 0 (control, holds enable), 1/2 (pattern address), 8; 23C4h is never
 * accessed.
 * Guard (wd24s2_guard_out): a 23C2h write must be to a listed register
 * and be either its fixed test word or EXACTLY snap[k] of that register
 * from this same load (only once both snapshot passes agreed).  The
 * test values are unchanged from the S1-reference version; the S1
 * reference word is no longer an allowed value.
 * Any mismatch: stop, restore every register written so far to its
 * snapshot value (one write each, read back), post-pass, 23C0h = 1002h.
 * Sources: DS = WD90C24A/A2 data sheet Rev D; S1 = live readings above.
 */
#ifndef WD24ADV_S2_H
#define WD24ADV_S2_H
#include "wd24adv_s1.h"

#define WD24S2_S	0x1002		/* S1: 23C0h at entry, both runs */
#define WD24S2_NW	17
#define WD24S2_NIX	(2 * WD24S1_NSEL + 2 * WD24S2_NW + WD24S2_NW + \
			 WD24S1_NSEL + 1)		/* 23C0h cap incl. a stop */
#define WD24S2_NDATA	(3 * WD24S2_NW)			/* 23C2h cap incl. a stop */
#define WD24S2_NLOG	700
#define WD24S2_CC_EN	0x0800		/* cursor control enable, DS 18.1 b11 */

struct wd24s2_w {
	u8 blk, idx;
	u16 test;		/* 12-bit data written, then the snapshot value */
	const char *name;
};

extern const struct wd24s2_w wd24s2_list[WD24S2_NW];
/* S1 live readings (commit 3ad7142): documentation + tests only, no
 * longer a precondition or an allowed write value */
extern const u16 wd24s2_ref[WD24S1_NSEL];	/* by select k (wd24s1_sel) */

enum wd24s2_res {
	WD24S2_NOT_RUN = 0,
	WD24S2_OK,		/* every write read back, all restored + verified */
	WD24S2_REFUSED,		/* before any 23C0h write */
	WD24S2_SNAP_BAD,	/* selects only: snapshot unstable/unsafe, no 23C2h write */
	WD24S2_MISMATCH,	/* a readback differed: stopped, restored */
	WD24S2_RESTORE_BAD,	/* a restore did not read back */
};

struct wd24s2 {
	const struct wd24s1_io *io;
	bool have_saved, snap_ok, armed, restored;
	u16 saved, cur;		/* S; last 23C0h value written */
	u16 snap[WD24S1_NSEL], snap2[WD24S1_NSEL], post[WD24S1_NSEL];
	unsigned int nsnap, nsnap2, npost;
	u16 rb_test[WD24S2_NW], rb_snap[WD24S2_NW], rb_safe[WD24S2_NW];
	u8 step[WD24S2_NW];	/* 0 not reached .. 4 done */
	bool dirty[WD24S2_NW];	/* test value may be in the register */
	u32 restore_fail;	/* bit j: entry j restore did not read back */
	u16 restore_rb;
	u16 alias;
	enum wd24s2_res res;
	const char *why;
	int stop_j;
	unsigned int wix, wdata, reads, refused;
	struct wd24s1_acc log[WD24S2_NLOG];
	unsigned int nlog;
};

int wd24s2_find(u8 blk, u8 idx);	/* list entry or -1 */
int wd24s2_k(u8 blk, u8 idx);		/* select number 0..24 or -1 */
int wd24s2_guard_out(const struct wd24s2 *s, u16 port, u16 val);
void wd24s2_init(struct wd24s2 *s, const struct wd24s1_io *io);
int wd24s2_run(struct wd24s2 *s);	/* 0 = OK, -ENODEV refused, -EIO */
const char *wd24s2_res_name(enum wd24s2_res r);
#ifdef __KERNEL__
extern const struct wd24s1_io wd24s2_kernel_io;
#endif
#endif
