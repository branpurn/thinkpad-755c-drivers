// SPDX-License-Identifier: GPL-2.0
/* S6d fault-bank helpers (kernel). Session + fbdev live in wd24adv_main.c. */
#include <linux/mm.h>
#include <linux/string.h>
#include "wd24adv_s6d.h"
#include "wd24adv_s6d_fault.h"

/*
 * Map the 64 KiB aperture covering `off` into `vma`. Zaps only the
 * previously mapped window, and only when that range sits inside the VMA.
 * Caller holds the hardware lock and, on the live fault path, the console
 * lock. Returns 0 or a negative errno. A non-zero return does not move
 * the hardware window.
 *
 * zap_vma_ptes (Linux 6.18 mm/memory.c) does not take mmap_lock; the fault
 * path holds it for read. Teardown of every VMA uses unmap_mapping_range
 * from s6d_stop, not from here.
 */
int wd24s6d_map_win(struct wd24s6d *s, struct vm_area_struct *vma, u32 off)
{
	struct wd24s6d_mapreq q;
	struct wd24s6d_mapact a;
	unsigned long vbase;
	u32 i;
	int err;

	if (!s || !vma)
		return -EINVAL;

	q.off = off;
	q.mapped_base = s->mapped_base;
	q.mmap_len = WD24S6D_MMAP_LEN;
	q.vma_len = vma->vm_end - vma->vm_start;
	wd24s6d_plan_fault(&s->thrash, &q, &s->nfault, &s->nthrash, &a);
	if (a.err) {
		s->why = a.why;
		return a.err;
	}

	if (a.zap && a.zap_pages) {
		unsigned long old = vma->vm_start +
			((unsigned long)a.zap_base << 12);

		zap_vma_ptes(vma, old, (unsigned long)a.zap_pages << 12);
	}
	if (a.setwin) {
		err = wd24s6b_setwin(s->b, a.w0, a.w1);
		if (err) {
			s->why = "SETWIN";
			s->mapped_base = ~0u;
			return err;
		}
		s->nswitch++;
	}

	if (!a.insert || !a.npages)
		return 0;
	vbase = vma->vm_start + ((unsigned long)a.base << 12);
	for (i = 0; i < a.npages; i++) {
		unsigned long addr = vbase + ((unsigned long)i << 12);
		vm_fault_t vt = vmf_insert_pfn(vma, addr,
					      WD24S6D_APERTURE_PFN + i);

		if (vt & VM_FAULT_ERROR) {
			s->why = "vmf_insert_pfn";
			s->mapped_base = ~0u;
			return -EIO;
		}
	}
	s->mapped_base = a.base;
	return 0;
}
