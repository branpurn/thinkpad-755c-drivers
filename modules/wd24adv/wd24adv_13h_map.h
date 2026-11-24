/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mode 13h chain-4 window. Host-testable. No register write.
 *
 * std_13h leaves A0000-AFFFF as one linear 64 KiB window. The visible
 * frame is 64000 bytes, which is not a page multiple. Userspace maps
 * the whole window (65536 bytes, 16 pages). Bytes 64000..65535 are
 * inside that window and are not the frame. The plan's setwin and
 * regs fields stay 0: the mapper must not call SETWIN and must not
 * write a VGA register. Closing the mapping restores nothing, because
 * nothing was written. Text restore stays on the trial / rmmod path.
 */
#ifndef WD24ADV_13H_MAP_H
#define WD24ADV_13H_MAP_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#else
#include <stdint.h>
#include <string.h>
typedef uint32_t u32;
#endif

#ifndef EINVAL
#define EINVAL 22
#endif

#define WD24_13H_APERTURE	0xa0000u
#define WD24_13H_WIN		0x10000u
#define WD24_13H_FRAME		64000u

struct wd24_13h_map {
	u32 phys;
	u32 map_len;
	u32 frame;
	u32 tail;
	int setwin;
	int regs;
};

/*
 * pgoff is the vma page offset. len is the vma length in bytes.
 * Only the page-sized window is accepted.
 */
static inline int wd24_13h_map_plan(u32 pgoff, u32 len, struct wd24_13h_map *o)
{
	if (!o)
		return -EINVAL;
	memset(o, 0, sizeof(*o));
	if (pgoff || len != WD24_13H_WIN)
		return -EINVAL;
	o->phys = WD24_13H_APERTURE;
	o->map_len = WD24_13H_WIN;
	o->frame = WD24_13H_FRAME;
	o->tail = WD24_13H_WIN - WD24_13H_FRAME;
	o->setwin = 0;
	o->regs = 0;
	return 0;
}

/* A store that starts at off and writes n bytes stays inside the frame. */
static inline int wd24_13h_store_ok(u32 off, u32 n)
{
	if (!n || n > WD24_13H_FRAME)
		return 0;
	if (off > WD24_13H_FRAME - n)
		return 0;
	return 1;
}

#endif
