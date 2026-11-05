/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wd24adv S3: the first BITBLT operations, off screen only.  Design doc
 * docs/WD24ADV-DESKTOP-DESIGN.md section 7, stage S3.  Runs once, inside
 * insmod, right after the PROVEN 640x480-8 trial mode set
 * (dryrun=0 live=1 mode=640x480-8 s3=1); the 15 s trial timer is already
 * queued, so the text state comes back on its own whatever S3 does.
 *
 * Operations (exactly wd90c24fb's wd_blt_go() programming, which its
 * load-time self-test runs on this machine at the same addresses):
 *   1 FILL  8 x  8 at 60000h, colour 3Ch     CNTRL1 0908h
 *   2 FILL 32 x 32 at 60200h, colour C3h     CNTRL1 0908h
 *   3 COPY 32 x 32 60200h -> 60100h forward  CNTRL1 0900h (dst < src:
 *     wd90c24fb's forward case; the rectangles do not overlap)
 * pitch 640, all inside 60000h-64F9Fh.  Scratch = 60000h-6FFFFh (one
 * 64 KiB PR0A bank, 60h): wd90c24fb's BITBLT/colour-expansion self-test
 * area (rewritten at every wd90c24fb load), above its cursor pattern
 * (50000h-500FFh) and the 300 KiB frame (0-4AFFFh, CRTC start 0), and at or
 * above 256 KiB, so outside anything text mode or wd24adv's 4 x 64 KiB
 * plane save/restore reaches under either plane mapping.  Its contents
 * do not matter and are not restored.  (The design's 4C000h slot is 4 KiB:
 * a 32 x 32 rectangle at pitch 640 needs 19872 bytes.)
 *
 * Sequence:
 *  entry: as S2 (23C0h read twice == 1002h, alias check), snapshot of all
 *    25 words twice (agree, echo, CNTRL1 ACT clear, cursor enable clear);
 *    VRAM: banks 0-5 (00000h-5FFFFh) checksummed, bank 6 read twice
 *    (must agree) -> pre; refuse unless every op changes >= 1 byte.
 *    All reads; the only writes so far are selects and PR0A (bank).
 *  per op: wait idle (W 23C0h 1001h, R; poll 23C2h ACT clear, <= SPINS
 *    reads); each parameter: W sel, R; W 23C2h word, R == word; then W
 *    23C0h 1001h, R; R 23C2h ACT still clear; W 23C2h CNTRL1 (the only new
 *    write, last); poll ACT clear (<= SPINS); bank 6 read back, compared
 *    byte by byte with the model (pre + ops so far).
 *  end: every parameter written is restored to the snapshot (W, R);
 *    post-pass: 24 words == snapshot, CNTRL1 ACT clear (its other bits
 *    read status, recorded); banks 0-5 checksums unchanged; PR0A 00h;
 *    W 23C0h 1002h, R == 1002h.
 *  TIMEOUT (ACT still set after SPINS reads): stop at once, NO further
 *    write of any kind (no abort, no restore, no PR0A, no 23C0h) and no
 *    VRAM access; latched (dead); the trial expiry restores the text state.
 * Guard: 23C0h only the 25 selects and the final 1002h; 23C2h only block 1
 * (never block 2, never 23C4h), only the current op's listed word for that
 * register, or the snapshot word of a register S3 writes; CNTRL1 only the
 * current op's word, once, right after the idle check.
 */
#ifndef WD24ADV_S3_H
#define WD24ADV_S3_H
#include "wd24adv_s2.h"

#define WD24S3_BASE	0x60000u
#define WD24S3_BANK	0x60		/* PR0A, 4 KiB units */
#define WD24S3_WIN	0x10000u	/* bank 6, compared in full */
#define WD24S3_PITCH	640
#define WD24S3_NOPS	3
#define WD24S3_NPAR	12	/* S5 MONO: + BG */
#define WD24S3_SPINS	200000		/* as wd90c24fb WD_BLT_SPINS, ~0.2 s */
#define WD24S3_NCRC	6		/* banks 0-5 */
#define WD24S3_NLOG	2400
#define WD24S3_MAXOPS	12		/* S3: 3, S4: 11 (per-op arrays) */
#define WD24S3_BACKC	0x0d00		/* ACT | PACKED | BACK (S4 only) */
#define WD24S3_FILL	0x0908		/* ACT | PACKED | SRC_FCOL */
#define WD24S3_COPY	0x0900		/* ACT | PACKED */
#define WD24S3_MONO	0x090e		/* ACT | PACKED | SRCF mono host | SRC I/O (S5) */
#define WD24S3_HCOL	0x0902		/* ACT | PACKED | SRC I/O (S5 HOSTCOL) */
#define WD24S5_P_HOST	0x23c4		/* BITBLT I/O port (DS 21.12), S5 only */
#define WD24S5_HLOG	4		/* 23C4h writes logged at each end of an op */

struct wd24s3_par {
	u8 idx;
	u16 val;		/* 12 bits; word = idx << 12 | val */
};

struct wd24s3_op {
	const char *name;
	bool fill;
	u32 src, dst;
	u16 w, h;
	u8 fg;
	u16 cntrl1;
	unsigned int npar;
	struct wd24s3_par par[WD24S3_NPAR];
	u16 rop;		/* ROP word (idx 9) value, for the model */
	bool back;		/* CNTRL1 BACK: par src/dst are bottom-right */
	/* S5 host-data ops (0 for S3/S4) */
	u8 host;		/* 0 none, 1 MONO (colour expanded), 2 HOSTCOL */
	bool transp;		/* MONO: 0 bits leave the destination */
	u8 bg;			/* MONO opaque: colour of 0 bits */
	u8 pat;			/* pattern id, wd24s5_bit / wd24s5_px */
};

extern const struct wd24s3_op wd24s3_ops[WD24S3_NOPS];

/*
 * A stage program: the op list and the one 64 KiB PR0A bank all its
 * rectangles lie in (checked by tests/test_s3.c).  S3 = wd24s3_prog3
 * (60000h, off screen); S4 = wd24s3_prog4 (10000h, ON SCREEN: frame rows
 * 103-203, inside wd24adv's 256 KiB plane save, so the trial revert
 * restores it like any other frame byte).  Every other bank 0-6 is
 * checksummed before and after and must not change.
 */
struct wd24s3_prog {
	const char *stage;		/* "S3" / "S4" */
	const struct wd24s3_op *ops;
	unsigned int nops;
	u32 base;			/* bank start, = bank << 12 */
	u8 bank;			/* PR0A value */
	unsigned int max_ix, max_data;	/* guard caps on 23C0h / 23C2h writes */
	const char *param;		/* module parameter: "s3" / "s4" / "s5" */
	bool host;			/* S5: BG and 23C4h (owed words only) allowed */
	unsigned int max_host;		/* guard cap on 23C4h writes */
};
extern const struct wd24s3_prog wd24s3_prog3, wd24s3_prog4;
#define WD24S4_NOPS	11
extern const struct wd24s3_op wd24s4_ops[WD24S4_NOPS];
/* S5: s5=1 MONO (opaque + transparent), s5=2 HOSTCOL; both bank 1 */
#define WD24S5_NOPS	8
#define WD24S5H_NOPS	3
extern const struct wd24s3_op wd24s5_ops[WD24S5_NOPS], wd24s5h_ops[WD24S5H_NOPS];
extern const struct wd24s3_prog wd24s3_prog5, wd24s3_prog5h;
/* host data: words owed after the start (DS 21.14 mono: 4 px in bits 3:0,
 * msb = leftmost, as wd90c24fb cexp var 0, OBS-FB; DS 21.12 colour: 4 px
 * per two words, low word first, low byte = left pixel: UNPROVEN); no
 * word straddles rows; pad pixels 0 */
u32 wd24s5_words(const struct wd24s3_op *o);
u32 wd24s5_wpr(const struct wd24s3_op *o);
u16 wd24s5_word(const struct wd24s3_op *o, u32 n);
bool wd24s5_bit(const struct wd24s3_op *o, u32 r, u32 c);
u8 wd24s5_px(const struct wd24s3_op *o, u32 r, u32 c);

struct wd24s3_mem {
	int (*bank)(void *priv, u8 pr0a);	/* PR0A write + readback; 0 ok */
	void (*vread)(void *priv, u32 off, u8 *buf, u32 n);	/* A0000h window */
	void *priv;
};

enum wd24s3_res {
	WD24S3_NOT_RUN = 0,
	WD24S3_OK,
	WD24S3_REFUSED,		/* reads only */
	WD24S3_SNAP_BAD,	/* selects only */
	WD24S3_PRE_BAD,		/* VRAM pre-check: no 23C2h write */
	WD24S3_MISMATCH,	/* a register readback differed: stopped, restored */
	WD24S3_VRAM_BAD,	/* VRAM differs from the model: stopped, restored */
	WD24S3_TIMEOUT,		/* engine busy: stopped, nothing more written */
	WD24S3_RESTORE_BAD,
	WD24S3_HOST_SHORT,	/* S5: engine idle before all host words: stopped, restored */
};

struct wd24s3 {
	const struct wd24s1_io *io;
	const struct wd24s3_mem *mem;
	const struct wd24s3_prog *prog;	/* wd24s3_init: S3; then set_prog */
	u8 *pre, *model, *buf;		/* WD24S3_WIN bytes each (caller) */
	bool have_saved, snap_ok, armed, go, restored, dead;
	/* after a timeout: idle re-check (reads only) */
	bool idle_seen;
	unsigned int idle_checks;
	u32 idle_last_spins;
	u16 idle_last_ix, idle_last_c1;
	u16 saved, cur;
	u16 snap[WD24S1_NSEL], snap2[WD24S1_NSEL], post[WD24S1_NSEL];
	unsigned int nsnap, nsnap2, npost;
	u32 crc0[WD24S3_NCRC], crc1[WD24S3_NCRC];
	int op;				/* op being programmed, -1 none */
	unsigned int done;		/* ops run and verified */
	u32 changes[WD24S3_MAXOPS];	/* bytes each op changes (model) */
	u32 spins[WD24S3_MAXOPS], idle_spins[WD24S3_MAXOPS];
	u16 c1_after[WD24S3_MAXOPS];
	u32 hwords[WD24S3_MAXOPS];	/* 23C4h words written per op */
	bool hgo;			/* a host op is started: words owed */
	u32 host_n;			/* next word index of the running op */
	unsigned int whost;
	bool busy_seen[WD24S3_MAXOPS];
	u32 ndiff, diff_off;
	u8 diff_got, diff_want;
	bool dirty[WD24S1_N_BLT];
	u32 restore_fail;
	u16 restore_rb;
	u16 alias;
	enum wd24s3_res res;
	const char *why;
	unsigned int wix, wdata, wgo, wbank, reads, polls, refused;
	struct wd24s1_acc log[WD24S3_NLOG];
	unsigned int nlog;
};

int wd24s3_guard_out(const struct wd24s3 *s, u16 port, u16 val);
void wd24s3_init(struct wd24s3 *s, const struct wd24s1_io *io,
		 const struct wd24s3_mem *mem, u8 *pre, u8 *model, u8 *buf);
/* before wd24s3_run only: select the stage program (S3 is the default) */
void wd24s3_set_prog(struct wd24s3 *s, const struct wd24s3_prog *p);
int wd24s3_run(struct wd24s3 *s);	/* 0 = OK */
/* after TIMEOUT: reads only (23C0h must still be 1001h, then <= SPINS
 * 23C2h reads); 1 = CNTRL1 ACT seen clear (sets idle_seen), 0 = busy or
 * unknown.  Not dead or already idle: 1, no access. */
int wd24s3_idle_poll(struct wd24s3 *s);
const char *wd24s3_res_name(enum wd24s3_res r);
void wd24s3_apply(const struct wd24s3_op *o, u8 *win);	/* model, bank 6 */
/* model for a window starting at base: ROP (fill: fg op dst), copy in
 * the engine's order (BACK: bottom row first, right to left) */
void wd24s3_apply_at(const struct wd24s3_op *o, u8 *win, u32 base);
u8 wd24s3_rop(u16 rop, u8 src, u8 dst);
u32 wd24s3_sum(const u8 *p, u32 n, u32 h);		/* FNV-1a */
#ifdef __KERNEL__
extern const struct wd24s1_io wd24s3_kernel_io;
#endif
#endif
