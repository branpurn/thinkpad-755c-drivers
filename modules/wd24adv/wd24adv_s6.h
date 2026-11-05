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
	WD24S6_NONE, WD24S6_ENTRY, WD24S6_REG, WD24S6_MEM, WD24S6_RESTORE
};

struct wd24s6_io {
	u8 (*rd)(void *priv, enum wd24_rf rf, u8 idx);
	int (*wr)(void *priv, enum wd24_rf rf, u8 idx, u8 val);	/* guarded */
	void (*mr)(void *priv, u32 off, u8 *buf, u32 n);	/* window */
	void (*mw)(void *priv, u32 off, const u8 *buf, u32 n);
	void (*open)(void *priv, bool on);	/* guard: c->dual_open */
	void *priv;
};

struct wd24s6 {
	const struct wd24s6_io *io;
	u8 *save;		/* WD24S6_NREG * WD24S6_N */
	u8 a[WD24S6_N], b[WD24S6_N];
	/* snapshot */
	u8 pr0a, pr0b, pr1, pr31, gr6;
	/* readbacks */
	u8 rb_pr0b, rb_pr1_on, rb_pr31_on, rb_pr1_off, rb_pr31_off;
	/* data results: mismatching bytes per check (0 = verified) */
	u32 d_lo, d_hi, d_rd, d_wr_noecho, v_lo, v_hi, v_wr, v_copy, v_x;
	u32 rest_bad;
	bool split_ok, rw_ok;
	enum wd24s6_res res;
	const char *why;
	char log[WD24S6_LOG];
	u32 loglen;
	u32 nwr;		/* register writes issued */
};

void wd24s6_run(struct wd24s6 *s, const struct wd24s6_io *io, u8 *save);
const char *wd24s6_res_name(enum wd24s6_res r);

/* 86Box-semantics model (host tests, dry-run listing, QEMU fake build) */
struct wd24s6_model {
	u8 *vram;		/* 1 MiB */
	u8 gr[16], sr[0x20];
	int ignore_pr1b3, ignore_pr31b7, pr1_stuck, refuse;	/* fault injection */
	u32 nreg;
	bool open;
};
extern const struct wd24s6_io wd24s6_model_io;
void wd24s6_model_init(struct wd24s6_model *m, u8 *vram1m);

#endif
