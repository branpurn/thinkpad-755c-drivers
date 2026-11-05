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

/*
 * S6b (s6b=1 only): /dev/wd24s6b, explicit dual-window mmap of A0000-AFFFF.
 * Steady state: A0000-A7FFF = VRAM w0*4K.., A8000-AFFFF = VRAM w1*4K..
 * (4 KiB pages, w0 + 8 <= w1 or w1 + 8 <= w0, w1 >= 8, both <= 78h).
 * COPY is a memmove inside VRAM (offsets in bytes, below 80000h) done by
 * the kernel through the PR31 b7 dual window; the user mapping must not be
 * touched during COPY (single-threaded tool).
 */
struct wd24s6b_win { __u32 w0, w1; };
struct wd24s6b_copy { __u32 dst, src, len; };
struct wd24s6b_info {
	__u32 state;		/* 0 off, 1 on, 2 dead */
	__u32 w0, w1, ncopy, nset, nwr, vram, frame;
};
#define WD24S6B_IOC_SETWIN	_IOW('W', 0x37, struct wd24s6b_win)
#define WD24S6B_IOC_COPY	_IOW('W', 0x38, struct wd24s6b_copy)
#define WD24S6B_IOC_INFO	_IOR('W', 0x39, struct wd24s6b_info)

#endif
