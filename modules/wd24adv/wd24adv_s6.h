/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wd24adv S6: PR0A/PR0B dual-window register proof + data test.
 *
 * Runs once, right after the PROVEN 640x480-8 trial mode set (s6=1
 * dryrun=0 live=1 mode=640x480-8).  Registers touched: PR0A, PR0B (both
 * already in the restore list), PR1 bit 3 and PR31 bit 7 (opened in the
 * guard only for the duration of this program, c->dual_open).  Video memory
 * touched: 7 x 4 KiB in OFF-SCREEN banks X = 50000h and Y = 70000h, saved
 * first and written back at the end.
 *
 * PR31 (SR11) is a Paradise extended register: DS 14.1 PR20 (SR06) must be
 * x1x01xxx to read or write SR07+ (86Box: exactly 48h).  The trial mode ends
 * with SR06 at the BIOS value 0Fh (locked; live S6 run 2026-11-04: PR31
 * read 00, write ignored), so P3/P4 run inside a guarded SR06 unlock
 * ((BIOS & EFh) | 48h = 4Fh, the value wd24probe3 used live) and relock,
 * each verified by SR06 bits 3:0 and the 3C4 index width test.
 *
 * Semantics under test (data sheet PR1 13.2 / PR31; same as 86Box
 * vid_paradise.c paradise_remap() for a 64 KiB A0000 map, GR6 b3:2 = 01):
 *   PR1 b3 = 1, PR31 b7 = 0: A0000-A7FFF -> PR0B, A8000-AFFFF -> PR0A + 8000h
 *   PR31 b7 = 1:             reads -> PR0A, writes -> PR0B (whole window)
 * Pure logic, host-testable.
 */
#ifndef WD24ADV_S6_H
#define WD24ADV_S6_H

#include "wd24adv_core.h"

/* PR0A/PR0B hold bits 6:0 (DS 13.1): the window reaches 0-7FFFFh.  The
 * 640x480-8 frame ends at 4B000h; both banks are off screen. */
#define WD24S6_X	0x50	/* PR0A value: bank X = 50000h (4 KiB units) */
#define WD24S6_Y	0x70	/* PR0B value: bank Y = 70000h */
#define WD24S6_N	0x1000	/* bytes per test region */
#define WD24S6_NREG	7
#define WD24S6_LOG	8192

enum wd24s6_res {
	WD24S6_NOTRUN, WD24S6_OK, WD24S6_SPLIT_ONLY, WD24S6_RW_ONLY,
	WD24S6_NONE, WD24S6_ENTRY, WD24S6_REG, WD24S6_MEM, WD24S6_RESTORE,
	WD24S6_NOTHELD	/* S6c: PR31 b7 effect not held while SR06 locked */
};

struct wd24s6_io {
	u8 (*rd)(void *priv, enum wd24_rf rf, u8 idx);
	int (*wr)(void *priv, enum wd24_rf rf, u8 idx, u8 val);	/* guarded */
	void (*mr)(void *priv, u32 off, u8 *buf, u32 n);	/* window */
	void (*mw)(void *priv, u32 off, const u8 *buf, u32 n);
	void (*open)(void *priv, bool on);	/* guard: c->s6_active, c->dual_open */
	void (*lock)(void *priv, bool on);	/* guard: c->s6_lock_open (SR06 unlock) */
	int (*unlocked)(void *priv);	/* 3C4 index width: 1 unlocked, 0 locked, <0 ? */
	void *priv;
};

struct wd24s6 {
	const struct wd24s6_io *io;
	u8 *save;		/* WD24S6_NREG * WD24S6_N */
	u8 a[WD24S6_N], b[WD24S6_N];
	/* snapshot */
	u8 pr0a, pr0b, pr1, pr31, gr6, sr06, pr31_locked;
	int lock0;		/* lock state at entry (must be 0 = locked) */
	/* readbacks */
	u8 rb_pr0b, rb_pr1_on, rb_pr31_on, rb_pr1_off, rb_pr31_off;
	u8 rb_sr06_unl, rb_sr06_rel;	/* SR06 low nibble after unlock / relock */
	int lk_unl, lk_rel;		/* index width after unlock / relock */
	/* data results: mismatching bytes per check (0 = verified) */
	u32 d_lo, d_hi, d_rd, d_wr_noecho, v_lo, v_hi, v_wr, v_copy, v_x;
	u32 rest_bad;
	bool split_ok, rw_ok;
	enum wd24s6_res res;
	const char *why;
	char log[WD24S6_LOG];
	u32 loglen;
	u32 nwr;		/* register writes issued */
	/* S6c (hold=1): PR31 b7 set, SR06 relocked, D2 run while LOCKED */
	bool hold;
	u8 rb_sr06_hold, rb_pr31_lk, rb_pr31_reunl;	/* SR06 / PR31 while held locked, PR31 after re-unlock */
	int lk_hold;
	bool cleared;		/* PR31 b7 gone after re-unlock */
};

void wd24s6_run(struct wd24s6 *s, const struct wd24s6_io *io, u8 *save);
/* hold=1: S6c, the copy test runs with SR06 relocked and PR31 b7 held */
void wd24s6_run2(struct wd24s6 *s, const struct wd24s6_io *io, u8 *save, bool hold);
const char *wd24s6_res_name(enum wd24s6_res r);

/* 86Box-semantics model (host tests, dry-run listing, QEMU fake build) */
struct wd24s6_model {
	u8 *vram;		/* 1 MiB */
	u8 gr[16], sr[0x20];
	int ignore_pr1b3, ignore_pr31b7, pr1_stuck, refuse;	/* fault injection */
	/* lock faults: unlock only on exactly 48h (86Box), SR06 writes
	 * ignored, relock ignored (stays unlocked), locked SR07+ reads FFh
	 * (86Box) instead of 00h (755C live) */
	int lock_exact48, lock_ignore, relock_stuck, locked_ff;
	/* S6c faults: relock clears PR31 b7 (lock-clear); b7 kept but no
	 * effect while locked (lock-noeffect); re-unlock refused */
	int relock_clears_b7, locked_noeffect, reunlock_fail;
	u32 nunl;
	bool lock_open;
	u32 nreg;
	bool open;
};
extern const struct wd24s6_io wd24s6_model_io;
void wd24s6_model_init(struct wd24s6_model *m, u8 *vram1m);

#endif
