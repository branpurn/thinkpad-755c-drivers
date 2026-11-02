/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wd24adv S1: read-only probe of the WD90C24A register blocks at 23C0h
 * (block 1 BITBLT, block 2 hardware cursor).  Design doc
 * docs/WD24ADV-DESKTOP-DESIGN.md section 7, stage S1.
 *
 * The ONLY port writes are 16-bit writes to the index control register
 * 23C0h (DS 17.2):
 *   - pass 1 and pass 2, each: 1001h | (i << 8) for i = 0..Fh (block 1,
 *     NOINC), then 1002h | (i << 8) for i = 0..8 (block 2, NOINC);
 *   - one final write of the saved value S & 1FFFh (bits 15:14 reserved,
 *     bit 13 read-only).
 * That is 2 * 25 + 1 = 51 writes when nothing stops it; a stop writes the
 * restore at once (selects so far + 1).  23C2h is only read, 23C4h is never
 * touched, no block 1/2 register is ever written, so the CNTRL1 start bit
 * cannot be set.  wd24s1_guard_out() is the closed list; every write goes
 * through it.
 *
 * Before the first write (reads only), refuse unless:
 *   - two reads of 23C0h agree (stable);
 *   - S != FFFFh and S bits 15:14 are 0 (FFFFh / reserved bits: the port
 *     is probably not decoded, UNPROVEN whether decode needs an unlock);
 *   - S differs from the word read at 3C0h/3C1h (a 10-bit ISA alias of
 *     23C0h would make the selects attribute-controller writes; UNPROVEN
 *     hazard, the check only costs two reads).
 * After every select (reads only): 23C0h must read back the select
 * exactly (b13 clear: the block exists); 23C2h bits 15:12 must equal the
 * index (the register access port format, DS 17.1: index in 15:12, data
 * in 11:0; that a read echoes the index is UNPROVEN on this chip) unless echo checking is off; block 1 CNTRL1 must read ACT
 * (bit 11) = 0 (idle at rest).  Any difference stops, restores, reports.
 * Both passes must read the same 25 words; the restore must read back S.
 *
 * Sources: DS = WD90C24A/A2 data sheet Rev D 10/14/94.  OBS-FB =
 * wd90c24fb on this 755C: it writes 23C0h = 1001h and reads 23C0h b13 at
 * detect (wd90c24_hw.c), reads block 1 idx 0 as the busy bit and block 2
 * registers through 23C2h, all in the unlocked state, never in BIOS text.
 */
#ifndef WD24ADV_S1_H
#define WD24ADV_S1_H
#include "wd24adv_core.h"

#define WD24S1_P_INDEX	0x23c0
#define WD24S1_P_DATA	0x23c2
#define WD24S1_NOINC	0x1000
#define WD24S1_INVALID	0x2000
#define WD24S1_RESV	0xc000
#define WD24S1_ACT	0x0800		/* CNTRL1 bit 11 (DS 19.1.1) */
#define WD24S1_N_BLT	16		/* block 1 idx 0..F */
#define WD24S1_N_CUR	9		/* block 2 idx 0..8 (8 = aux colour) */
#define WD24S1_NSEL	(WD24S1_N_BLT + WD24S1_N_CUR)
#define WD24S1_NPASS	2
#define WD24S1_MAX_WRITES (WD24S1_NPASS * WD24S1_NSEL + 1)
#define WD24S1_NLOG	192

struct wd24s1_io {
	u16 (*rd16)(void *priv, u16 port);
	void (*wr16)(void *priv, u16 val, u16 port);
	u8 (*rd8)(void *priv, u16 port);	/* 3C0h/3C1h alias check only */
	void *priv;
};

enum wd24s1_res {
	WD24S1_NOT_RUN = 0,
	WD24S1_OK,		/* both passes equal, restore read back S */
	WD24S1_REFUSED,		/* before any write: zero 23C0h writes */
	WD24S1_STOPPED,		/* unexpected value: restored, see why */
	WD24S1_UNSTABLE,	/* pass 1 != pass 2: restored */
	WD24S1_RESTORE_BAD,	/* restore did not read back S */
};

struct wd24s1_acc {
	u8 w;			/* 1 write, 0 read */
	u8 width;		/* 8 / 16 */
	u16 port, val;
};

struct wd24s1 {
	const struct wd24s1_io *io;
	bool strict_echo;	/* default true */
	/* guard state */
	bool have_saved, restored;
	u16 saved;		/* S */
	/* results */
	u16 ix[WD24S1_NPASS][WD24S1_NSEL];	/* 23C0h after each select */
	u16 val[WD24S1_NPASS][WD24S1_NSEL];	/* 23C2h */
	unsigned int done[WD24S1_NPASS];	/* selects read per pass */
	u16 alias, restore_rb;
	enum wd24s1_res res;
	const char *why;
	int stop_pass, stop_sel;
	unsigned int writes, reads, refused;
	struct wd24s1_acc log[WD24S1_NLOG];
	unsigned int nlog;
};

/* select k (0..24) of a pass: block 1 idx 0..F, then block 2 idx 0..8 */
u16 wd24s1_sel(unsigned int k);
/* the closed list: 0 or -EPERM.  Only 23C0h; only the 25 selects before
 * the restore, and only S & 1FFFh as the one restore. */
int wd24s1_guard_out(const struct wd24s1 *s, u16 port, u16 val);
/* reads: 23C0h, 23C2h (16 bit), 3C0h/3C1h (8 bit, alias check); 0/-EPERM */
int wd24s1_guard_in(u16 port, int width);
void wd24s1_init(struct wd24s1 *s, const struct wd24s1_io *io);
/* the whole probe; returns 0 for WD24S1_OK, else -ENODEV / -EIO */
int wd24s1_run(struct wd24s1 *s);
const char *wd24s1_res_name(enum wd24s1_res r);
#ifdef __KERNEL__
extern const struct wd24s1_io wd24s1_kernel_io;
#endif
#endif
