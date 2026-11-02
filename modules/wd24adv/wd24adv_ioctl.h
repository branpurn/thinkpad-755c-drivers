/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * wd24adv user API - placeholder.  This header is independent of
 * wd90c24fb_ioctl.h and does not include or alter it.  No ioctl is
 * implemented in the scaffold; numbers are reserved here so they cannot
 * collide with wd90c24fb ('W' 0x24-0x26).
 *
 * Plan (REPORT.md section 6): mode switching uses the standard
 * FBIOPUT_VSCREENINFO / FBIOPAN_DISPLAY.  Whether wd24adv also answers the
 * wd90c24fb CAPS/OPS/CURSOR ioctls (binary-compatible for the SDL backend)
 * is decided at Stage 2; if so it will define its own copies of those
 * structs here rather than share the header.
 */
#ifndef WD24ADV_IOCTL_H
#define WD24ADV_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* capability bits (superset of the wd90c24fb meaning, own namespace) */
#define WD24ADV_CAP_DIRECT	0x10	/* the mmap is VRAM, no damage needed */
#define WD24ADV_CAP_FLIP	0x20	/* FBIOPAN_DISPLAY flips pages */

struct wd24adv_modeinfo {
	__u32 index;		/* in: table index */
	__u16 xres, yres, vxres, vyres;
	__u8 bpp, bios_mode, stage, pad;
	__u32 flags;		/* WD24M_* */
	char name[24];
};

/* reserved, not implemented yet */
#define WD24ADV_IOC_MODEINFO	_IOWR('W', 0x30, struct wd24adv_modeinfo)
#define WD24ADV_IOC_ROUTE	_IOW('W', 0x31, __u32)	/* allow_routing=1 only */

#endif
