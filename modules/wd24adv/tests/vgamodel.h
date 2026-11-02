/* SPDX-License-Identifier: GPL-2.0 */
/* Port-level model of the 755C's VGA (WD90C24A2) for host tests of the
 * wd24adv port backend: registers, attribute flip-flop, DAC, four 64 KiB
 * planes with planar write mode 0 / read mode 0 and chain-4, the SR06
 * latch / 3C4 index width (wd24probe3), the CR34 gate on PR18/PR19, and
 * a "qemu" profile that behaves like QEMU's std VGA (no WD registers). */
#ifndef VGAMODEL_H
#define VGAMODEL_H
#include "../wd24adv_core.h"
#include "../wd24adv_engine.h"

enum vm_profile { VM_WD24, VM_QEMU };

/* Optional model of the 23C0h register blocks (DS 17-19, 21): BITBLT
 * block 1 executing fill / copy (both directions) / mono-from-host /
 * colour-from-host byte by byte into a 1 MB linear VRAM, and cursor
 * block 2 (registers only).  Anything the driver must never do is a
 * violation.  Where the data sheet is silent the model is strict (an
 * UNPROVEN-hazard counts as a violation). */
struct vm_eng {
	u16 ix;				/* 23C0h */
	u16 reg[3][16];			/* 12-bit data */
	bool busy;
	unsigned int op_polls;		/* busy CNTRL1 reads after the work */
	unsigned int polls_left;
	u32 host_need, host_got;	/* words */
	u32 row, col;			/* host op progress */
	u16 lo;				/* HOSTCOL: low word of a dword */
	bool have_lo;
	/* fault injection */
	bool stuck_busy;		/* never goes idle */
	int bad_blk, bad_idx;		/* this register reads back ^ 1 */
	bool bad_ix;			/* 23C0h reads back ^ 0x0100 */
	bool bad_echo;			/* 23C2h index nibble reads ^ 1 */
	int host_skew;			/* the op wants host_need + skew words */
	unsigned int host_tail;		/* busy reads after the last host word */
	u8 vram[WD24X_VRAM];
	/* counters */
	unsigned long ops, host_words, ix_writes, data_writes, reads, cpu_busy;
	unsigned int violations;
	char violation[96];
};

struct vgamodel {
	enum vm_profile prof;
	u8 misc, sr[256], gr[256], cr[256], ar[32];
	u8 sri, gri, cri, ari;
	bool ar_ff;		/* false: next 3C0 write is an index */
	u8 pel, dac[768], dac_wi, dac_ri, dac_sub;
	u8 vram[4][WD24_PLANE];
	/* plane offsets 64K-128K (linear 256-512 KiB): only reachable through
	 * PR0A banking in chain-4; not part of vm_cmp (never saved/restored) */
	u8 vram_hi[4][WD24_PLANE];
	u8 latch[4];
	/* fault injection: register that ignores writes and reads 'stuck' */
	int stuck_rf, stuck_idx;
	u8 stuck;
	int ro_plane;		/* plane whose memory ignores writes, -1 none */
	/* counters */
	unsigned long data_writes, index_writes, mem_writes;
	unsigned int violations;	/* forbidden register / port written */
	char violation[96];
	struct vm_eng *eng;	/* NULL: any 23Cxh access is a violation */
};

void vm_init(struct vgamodel *m, enum vm_profile p, unsigned int seed);
void vm_io(struct wd24_io *io, struct vgamodel *m);
/* 0 if the whole model (registers, DAC, VRAM) equals; else prints first diff */
void vm_eng_init(struct vm_eng *e, unsigned int seed);
/* 16-bit io for wd24x_* against m (m->eng may be NULL) */
void vm_xio(struct wd24x_io *io, struct vgamodel *m);
int vm_cmp(const struct vgamodel *a, const struct vgamodel *b, const char *what);
#endif
