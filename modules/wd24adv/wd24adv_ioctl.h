/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * wd24adv user API - placeholder.  This header is independent of
 * wd90c24fb_ioctl.h and does not include or alter it.  No ioctl is
 * implemented yet; numbers are reserved here so they cannot collide with
 * wd90c24fb ('W' 0x24-0x26).  Mode switching will use the standard
 * FBIOPUT_VSCREENINFO / FBIOPAN_DISPLAY.
 */
#ifndef WD24ADV_IOCTL_H
#define WD24ADV_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* capability bits (own namespace) */
#define WD24ADV_CAP_DIRECT	0x10	/* the mmap is VRAM, no damage needed */
#define WD24ADV_CAP_FLIP	0x20	/* FBIOPAN_DISPLAY flips pages */

#define WD24ADV_STATUS_PROVEN	1
#define WD24ADV_STATUS_UNPROVEN	2	/* listed, never programmed */

struct wd24adv_modeinfo {
	__u32 index;		/* in: table index */
	__u16 xres, yres, vxres, vyres;
	__u8 bpp, bios_mode, status, pad;
	__u32 flags;		/* WD24M_* */
	char name[24];
};

/* reserved, not implemented yet */
#define WD24ADV_IOC_MODEINFO	_IOWR('W', 0x30, struct wd24adv_modeinfo)
/*
 * 'W' 0x31 was a panel/CRT routing ioctl in the scaffold.  Runtime routing
 * was removed for good: 0x31 stays reserved and must never be assigned.
 */

#endif
