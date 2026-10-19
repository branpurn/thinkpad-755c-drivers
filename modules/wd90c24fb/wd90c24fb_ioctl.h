/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * wd90c24fb private ioctls: damage and acceleration for a client that
 * mmap()s the frame (TinyX Xfbdev, tinycore-32-ram gfx/patches/tinyx).
 *
 * The mapped frame is a shadow in system RAM. A client that draws into it
 * says what changed with WDFB_IOC_OPS, and the driver copies exactly those
 * rectangles to video memory, at once, instead of the whole frame 10 times
 * a second. With the WD90C24A BITBLT engine (WDFB_IOC_CAPS) a client can
 * also have video memory filled (WDFB_OP_FILL: after it filled the shadow
 * itself) or copied inside video memory (WDFB_OP_COPY: BEFORE it copies the
 * shadow, so the source in video memory is still the old picture), so
 * scrolling and window moves cost no copy over the bus at all. Ops run in
 * order. Coordinates are pixels in the 640x480 frame; a rectangle that
 * does not fit is -EINVAL.
 *
 * WDFB_IOC_OPS returns 0, or WDFB_LOST if a FILL/COPY could not use the
 * engine (it was turned off after a timeout): the driver then redraws the
 * whole frame shortly, and the client should re-read the caps.
 */
#ifndef WD90C24FB_IOCTL_H
#define WD90C24FB_IOCTL_H
#include <linux/types.h>
#include <linux/ioctl.h>

#define WDFB_CAP_DAMAGE	0x01	/* WDFB_OP_DAMAGE; mmap is no longer polled */
#define WDFB_CAP_FILL	0x02	/* WDFB_OP_FILL by the BITBLT engine */
#define WDFB_CAP_COPY	0x04	/* WDFB_OP_COPY by the BITBLT engine */

#define WDFB_OP_DAMAGE	0	/* copy x,y,w,h from the shadow */
#define WDFB_OP_FILL	1	/* fill x,y,w,h with color */
#define WDFB_OP_COPY	2	/* copy sx,sy,w,h to x,y (overlap is fine) */

struct wdfb_op {
	__u8 op;
	__u8 color;
	__u16 x, y, w, h;
	__u16 sx, sy;
	__u16 pad;
};

struct wdfb_ops {
	__u32 n;		/* number of ops, at most WDFB_MAX_OPS */
	__u32 flags;		/* 0 */
	__u64 ops;		/* user pointer to struct wdfb_op[n] */
};

#define WDFB_MAX_OPS	256
#define WDFB_LOST	1

#define WDFB_IOC_CAPS	_IOR('W', 0x24, __u32)
#define WDFB_IOC_OPS	_IOW('W', 0x25, struct wdfb_ops)

#endif
