// SPDX-License-Identifier: GPL-2.0
/* S6d fault-bank helpers (kernel). Session + fbdev live in wd24adv_main.c. */
#include <linux/mm.h>
#include <linux/string.h>
#include "wd24adv_s6d.h"

/*
 * Map the 64 KiB aperture covering `off` into `vma` at the matching
 * virtual offsets. Zaps the previously mapped window first. Caller holds
 * the hardware lock and has already run wait_idle / SETWIN for (w0,w1).
 * Returns 0 or a vm_fault_t-style negative errno.
 */
int wd24s6d_map_win(struct wd24s6d *s, struct vm_area_struct *vma, u32 off)
{
	u32 w0, w1, base, i;
	unsigned long vbase;
	int err;

	if (!s || !vma || off >= WD24S6D_MMAP_LEN)
		return -EINVAL;
	if (s->thrash.latched)
		return -EIO;

	wd24s6d_win_for_off(off, &w0, &w1);
	base = wd24s6d_win_base_pg(off);
	if (wd24s6d_thrash_note(&s->thrash, base)) {
		s->nthrash++;
		s->why = "thrash latch";
		return -EIO;
	}

	s->nfault++;

	/* bank switch: zap previous window, SETWIN new halves */
	if (s->mapped_base != base) {
		if (s->mapped_base != ~0u) {
			unsigned long old = vma->vm_start +
				((unsigned long)s->mapped_base << 12);
			unsigned long len = WD24S6D_WIN;

			if (old >= vma->vm_start && old + len <= vma->vm_end)
				zap_vma_ptes(vma, old, len);
		}
		err = wd24s6b_setwin(s->b, w0, w1);
		if (err) {
			s->why = "SETWIN";
			s->mapped_base = ~0u;
			return err;
		}
		s->nswitch++;
	}

	vbase = vma->vm_start + ((unsigned long)base << 12);
	if (vbase < vma->vm_start || vbase + WD24S6D_WIN > vma->vm_end) {
		s->why = "VMA range";
		return -EINVAL;
	}
	for (i = 0; i < (WD24S6D_WIN / WD24S6D_PAGE); i++) {
		unsigned long addr = vbase + ((unsigned long)i << 12);
		vm_fault_t vt = vmf_insert_pfn(vma, addr,
					      WD24S6D_APERTURE_PFN + i);

		if (vt & VM_FAULT_ERROR) {
			s->why = "vmf_insert_pfn";
			s->mapped_base = ~0u;
			return -EIO;
		}
	}
	s->mapped_base = base;
	return 0;
}
