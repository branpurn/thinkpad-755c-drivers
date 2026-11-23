/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * wd24adv user API - placeholder.  This header is independent of
 * wd90c24fb_ioctl.h and does not include or alter it.  No ioctl is
 * implemented yet except WD24S6D_IOC_PRESENT; numbers are reserved here so they cannot collide with
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
 * One blit. pixels is a userspace pointer to nbytes index bytes.
 * LETTER2X: 320x200, scale 2, destination 0,0, nbytes 64000. The kernel
 * writes the 640x480 frame through the already proven window program.
 * DIRECT: the same 320x200 bytes, scale 1, nbytes 64000. Accepted only
 * while present_mode=13h (the proven 320x200-8 set). One 64000-byte copy,
 * no expansion and no window program. Anything else is -EINVAL and
 * touches no register. switches and bytes are outputs.
 */
struct wd24s6d_present {
	__u64 pixels;
	__u32 src_w;
	__u32 src_h;
	__u32 dst_x;
	__u32 dst_y;
	__u32 scale;
	__u32 op;
	__u32 nbytes;
	__u32 switches;
	__u32 bytes;
	__u32 reserved;
};
#define WD24ADV_BLIT_LETTER2X	0u
#define WD24ADV_BLIT_DIRECT	1u
#define WD24S6D_PRESENT_LETTER	WD24ADV_BLIT_LETTER2X
#define WD24S6D_PRESENT_SRC	64000u
#define WD24S6D_IOC_PRESENT	_IOWR('W', 0x40, struct wd24s6d_present)

/*
 * Show the 64K page just drawn and point the A0000 window at the other
 * page. cr0c/cr0d are the CRTC start written for that page (0 or
 * 0x4000). pr0a is the CPU bank written for the page drawn next.
 * The CRTC loads cr0c/cr0d at vertical retrace, so this does not
 * poll 3DA. late stays 0. vsync is accepted and ignored.
 * Only valid in the 320x200-8 session.
 */
struct wd24s6d_flip {
	__u32 vsync;
	__u32 show;
	__u32 cpu;
	__u32 pr0a;
	__u32 late;
	__u32 cr0c;
	__u32 cr0d;
};
#define WD24S6D_IOC_FLIP	_IOWR('W', 0x41, struct wd24s6d_flip)
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
