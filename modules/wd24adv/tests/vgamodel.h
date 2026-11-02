/* SPDX-License-Identifier: GPL-2.0 */
/* Port-level model of the 755C's VGA (WD90C24A2) for host tests of the
 * wd24adv port backend: registers, attribute flip-flop, DAC, four 64 KiB
 * planes with planar write mode 0 / read mode 0 and chain-4, the SR06
 * latch / 3C4 index width (wd24probe3), the CR34 gate on PR18/PR19, and
 * a "qemu" profile that behaves like QEMU's std VGA (no WD registers). */
#ifndef VGAMODEL_H
#define VGAMODEL_H
#include "../wd24adv_core.h"

enum vm_profile { VM_WD24, VM_QEMU };

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
};

void vm_init(struct vgamodel *m, enum vm_profile p, unsigned int seed);
void vm_io(struct wd24_io *io, struct vgamodel *m);
/* 0 if the whole model (registers, DAC, VRAM) equals; else prints first diff */
int vm_cmp(const struct vgamodel *a, const struct vgamodel *b, const char *what);
#endif
