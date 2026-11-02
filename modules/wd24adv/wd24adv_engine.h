/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wd24adv S0: guard, command validator and op sequencer for the WD90C24A
 * BITBLT engine (23C0h block 1) and hardware cursor (block 2).
 *
 * HOST MODEL ONLY.  This file and wd24adv_engine.c are not in Kbuild, have
 * no caller in the module, and refuse every access when the io is live
 * (WD24X_LIVE_ALLOWED is 0).  tests/test_engine.c checks all three.  Live
 * use needs stages S1..S5 of docs/WD24ADV-DESKTOP-DESIGN.md first.
 *
 * Every rule below cites its source:
 *   DS     WD90C24A/A2 data sheet, Rev D 10/14/94 (public), section numbers;
 *   OBS-FB behaviour of wd90c24fb on this ThinkPad 755C ("Real 755C
 *          results", docs/WD90C24.md): its load-time fill/copy self-test
 *          passes, X fills/copies look right, and only one colour
 *          expansion word format works (4 px per 16-bit word, msb first,
 *          no row padding);
 *   OBS-W  the Windows 98 driver's observed behaviour, described in the
 *          design doc section 2 (not copied): waits for idle before
 *          programming, keeps the 23C0h index around cursor access, mono
 *          from host in packed mode fed with 16-bit writes, fixed-colour
 *          fills;
 *   UNPROVEN  inferred; S1 (read-only) or a later stage must settle it.
 */
#ifndef WD24ADV_ENGINE_H
#define WD24ADV_ENGINE_H
#include "wd24adv_core.h"

/* S0 gate: no live path may touch 23C0h-23C5h.  Changing this needs an
 * approved stage; tests/test_engine.c fails the build if it is not 0. */
#define WD24X_LIVE_ALLOWED	0

/* ports, DS 17.1 "Global port map" */
#define WD24X_P_INDEX	0x23c0	/* index control, 16 bit (DS 17.2) */
#define WD24X_P_DATA	0x23c2	/* register access, (index << 12) | data12 */
#define WD24X_P_HOST	0x23c4	/* BITBLT I/O port, 16 bit (DS 21.12) */

/* index control 23C0h, DS 17.2 */
#define WD24X_IX_BLK_MASK	0x00ff	/* register block pointer */
#define WD24X_IX_RIDX_SHIFT	8	/* bits 11:8: index for reads */
#define WD24X_IX_NOINC		0x1000	/* auto-increment disable */
#define WD24X_IX_INVALID	0x2000	/* read-only: block does not exist */
#define WD24X_IX_WRITABLE	0x1fff	/* 15:14 reserved, 13 read-only */

enum wd24x_blk {
	WD24X_BLK_SYS = 0,	/* interrupt status: never written (DS 17.3) */
	WD24X_BLK_BLT = 1,	/* DS 19, Table 19-1 / 21.15 */
	WD24X_BLK_CUR = 2,	/* DS 18, Table 18-1 */
};

/* BITBLT block 1 indices, DS Table 19-1 */
enum {
	WD24X_B_CNTRL1 = 0, WD24X_B_CNTRL2, WD24X_B_SRC_LO, WD24X_B_SRC_HI,
	WD24X_B_DST_LO, WD24X_B_DST_HI, WD24X_B_DIM_X, WD24X_B_DIM_Y,
	WD24X_B_PITCH, WD24X_B_ROP, WD24X_B_FG, WD24X_B_BG,
	WD24X_B_TCOLOR, WD24X_B_TMASK, WD24X_B_MASK, WD24X_B_NREG
};

/* CNTRL1 bits, DS 19.1.1 / 21.15.1 */
#define WD24X_C1_ACT		0x0800	/* write 1 = start; reads 1 while busy */
#define WD24X_C1_BACK		0x0400	/* bottom-to-top, right-to-left */
#define WD24X_C1_PACKED		0x0100	/* address mode 01 */
#define WD24X_C1_DST_LIN	0x0080	/* never used */
#define WD24X_C1_SRC_LIN	0x0040	/* never used */
#define WD24X_C1_SRCF_MONO_CMP	0x0004	/* not used in S0 */
#define WD24X_C1_SRCF_FIXED	0x0008	/* fixed colour (filled rectangle) */
#define WD24X_C1_SRCF_MONO_HOST	0x000c
#define WD24X_C1_SRC_IO		0x0002	/* source is the 23C4h port */

/* CNTRL2 bits, DS 19.1.2 / 21.15.2 */
#define WD24X_C2_LINE		0x0800	/* never */
#define WD24X_C2_IRQ		0x0400	/* never (design Q7) */
#define WD24X_C2_QUICKSTART	0x0080	/* never: a DST/SRC write would start */
#define WD24X_C2_UPDDST		0x0040	/* never */
#define WD24X_C2_MTRANS		0x0008	/* mono source 0 = leave destination */
#define WD24X_C2_TENABLE	0x0001	/* never (colour transparency) */

/* ROP codes, DS 19.7 / Table 19-3: abcd in bits 11:8, bits 7:0 reserved */
#define WD24X_ROP_CLEAR		0x0000
#define WD24X_ROP_AND		0x0100
#define WD24X_ROP_SRC		0x0300
#define WD24X_ROP_XOR		0x0600
#define WD24X_ROP_OR		0x0700
#define WD24X_ROP_INVDST	0x0a00
#define WD24X_ROP_SET		0x0f00

/* cursor block 2 indices, DS Table 18-1 */
enum {
	WD24X_C_CTRL = 0, WD24X_C_PAT_LO, WD24X_C_PAT_HI, WD24X_C_PRIMARY,
	WD24X_C_SECONDARY, WD24X_C_ORIGIN, WD24X_C_POS_X, WD24X_C_POS_Y,
	WD24X_C_AUX, WD24X_C_NREG
};
#define WD24X_CC_ENABLE		0x0800	/* DS 18.1 bit 11 */
#define WD24X_CC_2COL_INV	0x0020	/* bits 7:5 = 001, two colours + invert */

/* VRAM map at 640x480-8, pitch 640 (design section 4).  1 MB: PR1 = C5h,
 * memory size bits 7:6 = 11 (POST trace; DS 13.2).  Only these surfaces
 * are legal BITBLT targets. */
#define WD24X_PITCH		640u
#define WD24X_VRAM		0x100000u
#define WD24X_FRAME		0x00000u	/* 640x480 = 4B000h bytes */
#define WD24X_FRAME_END		0x4b000u
#define WD24X_GUARD		0x4b000u	/* never a BITBLT target */
#define WD24X_SCRATCH		0x4c000u
#define WD24X_SCRATCH_END	0x4d000u
#define WD24X_CURSOR		0x4d000u	/* 256 bytes, CPU-written only */
#define WD24X_CURSOR_END	0x4d100u
#define WD24X_POOL		0x4e000u
#define WD24X_POOL_END		0x100000u

#define WD24X_DIM_MAX		2048u	/* DS 19.4.1/19.4.2: 1..2K */
#define WD24X_HOST_MAX_WORDS	65536u	/* our cap on one host-data op */
#define WD24X_SPIN_BASE		2000u	/* bounded waits, see wd24x_spins() */
#define WD24X_SPIN_MAX		200000u

enum wd24x_op {
	WD24X_FILL = 1,		/* fixed colour (DS 19.9, 21.11) */
	WD24X_COPY,		/* screen to screen, direction chosen here */
	WD24X_MONO,		/* mono from host, colour expanded (DS 21.14) */
	WD24X_HOSTCOL,		/* 8 bpp from host (DS 21.12) */
};

struct wd24x_cmd {
	u8 op;			/* enum wd24x_op */
	u8 fg, bg;
	bool transparent;	/* MONO only: 0 bits leave the destination */
	u16 rop;		/* one of WD24X_ROP_* */
	u32 dst, src;		/* byte offsets of the top-left pixel */
	u32 w, h;		/* pixels */
};

/* register plan for one op: what wd24x_run() writes, in order */
struct wd24x_plan {
	u8 n;
	u8 idx[WD24X_B_NREG];
	u16 val[WD24X_B_NREG];
	u16 cntrl1;		/* written last, starts the op */
	u32 host_words;		/* 16-bit words owed on 23C4h after start */
	u32 words_per_row;
	u32 spins;		/* poll bound for the idle wait after the op */
};

/* 16-bit port access; live = the real chip (always refused in S0) */
struct wd24x_io {
	u16 (*inw)(void *priv, u16 port);
	void (*outw)(void *priv, u16 val, u16 port);
	void *priv;
	bool live;
};

enum wd24x_evt { WD24X_EV_OUT, WD24X_EV_IN, WD24X_EV_REFUSED, WD24X_EV_DEAD };

struct wd24x_event {
	u8 type;
	u16 port, val;
	int err;
	const char *why;
};

#define WD24X_MAXEV	512

struct wd24x {
	const struct wd24x_io *io;
	bool enabled;		/* acceleration wanted (off by default) */
	bool dead;		/* latched off: timeout / mismatch / bad state */
	const char *dead_why;
	bool readback;		/* verify every 23C2h register write (default on) */
	bool async;		/* FILL/COPY return busy (default off: wait after
				 * every op, design 3.4 mini-gate) */
	/* state the guard relies on */
	bool have_saved;	/* saved_ix holds this op's 23C0h value */
	u16 saved_ix;
	u8 cur_blk;		/* block selected by our last 23C0h write */
	bool idle_seen;		/* CNTRL1 b11 read 0 since our last start */
	bool busy;		/* we started an op and have not seen it idle */
	bool armed;		/* a validated plan is being written */
	bool host_op;		/* the running op takes host data */
	u32 host_left;		/* words still owed */
	u16 plan_cntrl1;	/* the only CNTRL1 value the armed plan may write */
	u32 spins;		/* poll bound of the op last started */
	u16 shadow[3][16];	/* last value written per block/index */
	/* counters */
	unsigned long nout, nin, nhost, nrefused, nops, nfallback;
	unsigned int ntimeout, nmismatch;
	struct wd24x_event ev[WD24X_MAXEV];
	unsigned int nev, dropped;
};

/* closed list entry: block, index, how values are checked, proof status */
struct wd24x_rule {
	u8 blk, idx;
	u16 mask;		/* bits that may be 1 (outside: refused) */
	u16 lo, hi;		/* value range (after mask check) */
	const u16 *set;		/* if non-NULL: value must be in set[0..nset) */
	u8 nset;
	u16 set_unproven;	/* bit i: set[i] itself is UNPROVEN */
	u8 status;		/* WD24_PROVEN (observed) / WD24_UNPROVEN */
	const char *name, *cite;
};

extern const struct wd24x_rule wd24x_rules[];
extern const unsigned int wd24x_nrules;
const struct wd24x_rule *wd24x_rule_find(u8 blk, u8 idx);

void wd24x_init(struct wd24x *x, const struct wd24x_io *io);
/* port-level guard: 0 or -EPERM (never allowed) / -EINVAL (bad value) /
 * -EBUSY (state: engine busy, no host op, ...) */
int wd24x_guard(const struct wd24x *x, u16 port, u16 val);
/* reads: only 23C0h and 23C2h, never 23C4h (DS 21.13: a read with no data
 * holds the host off) */
int wd24x_guard_in(const struct wd24x *x, u16 port);

/* pure: validate and build the plan; no port access */
int wd24x_check(const struct wd24x_cmd *cmd, const char **why);
int wd24x_plan(const struct wd24x_cmd *cmd, struct wd24x_plan *p, const char **why);
u32 wd24x_host_words(const struct wd24x_cmd *cmd);	/* 0 for FILL/COPY */
bool wd24x_backward(const struct wd24x_cmd *cmd);
bool wd24x_surface(u32 off, u32 w, u32 h, u32 *base);

/* sequencer.  All return 0, or <0 with nothing (more) written; on <0 the
 * caller draws with the CPU.  host: MONO/HOSTCOL data, host_words words. */
int wd24x_run(struct wd24x *x, const struct wd24x_cmd *cmd, const u16 *host,
	      u32 nhost);
int wd24x_sync(struct wd24x *x);	/* wait idle (bounded) */
/* before any CPU access to VRAM: 0 = safe, else do not touch VRAM */
int wd24x_cpu_begin(struct wd24x *x);
int wd24x_cursor(struct wd24x *x, bool on, u16 px, u16 py, u8 hotx, u8 hoty,
		 u8 c0, u8 c1);
void wd24x_kill(struct wd24x *x, const char *why);
#endif
