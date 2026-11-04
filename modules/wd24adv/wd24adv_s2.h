/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wd24adv S2: write + readback of the WD90C24A block 1 (BITBLT) and
 * block 2 (cursor) registers with the engine start bit never set and the
 * cursor never enabled.  Design doc docs/WD24ADV-DESKTOP-DESIGN.md
 * section 7, stage S2; runs in the text state like S1 (s2=1 dryrun=0),
 * once, inside insmod: everything written is put back and verified before
 * insmod returns, so nothing stays changed.
 *
 * Reference = the S1 live readings (docs/s1-live-2026-11-02-run.txt,
 * commit 3ad7142, two identical runs): S = 23C0h = 1002h and the 25
 * 23C2h words in wd24s2_ref[].  S2 refuses (no 23C2h write) unless the
 * chip reads exactly that reference first.
 *
 * The closed write list wd24s2_list[] (17 registers).  For each one, in
 * order:
 *   W 23C0h sel; R 23C0h == sel
 *   W 23C2h (idx << 12) | test; R 23C0h == sel; R 23C2h == that word
 *   W 23C2h ref;                R 23C2h == ref
 *   W 23C0h 1001h (CNTRL1) / 1002h (cursor control); R 23C0h == it;
 *   R 23C2h == its reference (0108h: ACT = 0 / 0000h: enable = 0)
 * Never written: block 1 idx 0 (CNTRL1, holds ACT), C, D, F; block 2 idx
 * 0 (control, holds enable), 1/2 (pattern address: the reference 000h /
 * 014h is outside the guard's set), 8; 23C4h is never accessed.
 * Values: every test AND reference data value is one the S0 guard
 * (wd24adv_engine.c wd24x_rules) allows; tests/test_s2.c checks that.
 * Any mismatch: stop, restore every register written so far to its
 * reference (one write each, read back), verify, 23C0h = S, report.
 * Sources: DS = WD90C24A/A2 data sheet Rev D; S1 = live readings above.
 */
#ifndef WD24ADV_S2_H
#define WD24ADV_S2_H
#include "wd24adv_s1.h"

#define WD24S2_S	0x1002		/* S1: 23C0h at entry, both runs */
#define WD24S2_NW	17
#define WD24S2_NIX	(WD24S1_NSEL + 2 * WD24S2_NW + WD24S1_NSEL + 1 + \
			 WD24S2_NW + WD24S1_NSEL)	/* 23C0h cap incl. a stop */
#define WD24S2_NDATA	(3 * WD24S2_NW)			/* 23C2h cap incl. a stop */
#define WD24S2_NLOG	600

struct wd24s2_w {
	u8 blk, idx;
	u16 test;		/* 12-bit data written, then the reference */
	const char *name;
};

extern const struct wd24s2_w wd24s2_list[WD24S2_NW];
extern const u16 wd24s2_ref[WD24S1_NSEL];	/* by select k (wd24s1_sel) */

enum wd24s2_res {
	WD24S2_NOT_RUN = 0,
	WD24S2_OK,		/* every write read back, all restored + verified */
	WD24S2_REFUSED,		/* before any 23C0h write */
	WD24S2_NOT_REF,		/* selects only: chip != S1 reference, no 23C2h write */
	WD24S2_MISMATCH,	/* a readback differed: stopped, restored */
	WD24S2_RESTORE_BAD,	/* a restore did not read back */
};

struct wd24s2 {
	const struct wd24s1_io *io;
	bool have_saved, armed, restored;
	u16 saved, cur;		/* S; last 23C0h value written */
	u16 pre[WD24S1_NSEL], post[WD24S1_NSEL];	/* verify passes */
	unsigned int npre, npost;
	u16 rb_test[WD24S2_NW], rb_ref[WD24S2_NW], rb_safe[WD24S2_NW];
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
