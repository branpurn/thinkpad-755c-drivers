// SPDX-License-Identifier: GPL-2.0
/*
 * wd90c24fb: native framebuffer for the WD90C24/WD90C24A(2) in the IBM
 * ThinkPad 755C: 640x480, 8 bpp (256 colours), through Paradise banking.
 * See docs/WD90C24.md.
 *
 * The chip has no linear aperture on the 755C, only the 64 KiB window at
 * A0000 offset by PR0(A). fbcon and user space draw into a 300 KiB shadow
 * in system RAM (sys_* helpers); every drawing operation copies its
 * rectangle to video memory, bank by bank. On the WD90C24A/A2 the BITBLT
 * engine (stage 3) also does fillrect and copyarea (scrolling) in video
 * memory, so those need no copy; it is self-tested at load and dropped on
 * the first timeout.
 *
 * mmap() clients get the shadow. A client that says what it drew (the
 * WDFB_IOC_OPS ioctl, wd90c24fb_ioctl.h; tc32's Xfbdev does) has only
 * those rectangles copied, and its screen-to-screen copies and big fills
 * done by the engine. Any other client gets the whole frame copied at
 * 10 Hz while the device is open (also the fallback if the ioctl fails).
 * stats= shows what reached video memory and how.
 *
 * Fail-safe:
 *  - drives only a chip with the WD90C24 signature (force=1 also accepts
 *    other WD90C2x/3x, for the 86Box C30/C31 bench), enable=0 disables;
 *  - the text-mode state (standard registers, fonts, text, palette via
 *    vgastate, Paradise registers via wd90c24_hw.c) is saved before
 *    anything changes and restored on any failure, when a trial period
 *    ends without confirmation (trial=, keep), on "echo 1 > .../text"
 *    (fbcon holds a module reference, so this comes before rmmod), and
 *    around an APM suspend (stage 5);
 *  - after the mode set a banking self-test must pass, or the text mode is
 *    restored and the module returns -ENODEV (the .tcz then falls back to
 *    vga16fb);
 *  - blank level 1 (FB_BLANK_NORMAL) only blacks the picture; levels 2-4
 *    also power the panel down (PNLOFF, i.e. the backlight inverter, then
 *    the panel signals, then the LCD interface) and, if the kernel has
 *    CONFIG_APM_DISPLAY_BLANK, ask the BIOS for APM display standby;
 *    unblank undoes all of it and redraws. Restore always brings the
 *    outputs back as the BIOS had them, with the 100 ms power-up steps.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/console.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/mutex.h>
#include <linux/kstrtox.h>
#include <linux/uaccess.h>
#include <linux/timekeeping.h>
#include <video/vga.h>
#include "wd90c24_hw.h"
#include "wd90c24fb_ioctl.h"

/*
 * drivers/tty/vt/vt.c, set by the APM driver with CONFIG_APM_DISPLAY_BLANK.
 * Weak: a kernel that does not export it (TRIM_UNUSED_KSYMS) still loads
 * the module, which then blanks without the APM call.
 */
extern int (*console_blank_hook)(int) __weak;

/*
 * Only symbols the tc755x kernel exports already (its Module.symvers):
 * this module can be updated without a new kernel.
 */
static u64 wd_now_ns(void)
{
	struct timespec64 t;

	ktime_get_ts64(&t);
	return timespec64_to_ns(&t);
}

static int (*wd_blank_hook(void))(int)
{
	return &console_blank_hook ? READ_ONCE(console_blank_hook) : NULL;
}

static bool enable = true;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Drive the WD90C24 (0 = do nothing, keep vga16fb/text)");
static bool probe_only;
module_param(probe_only, bool, 0444);
MODULE_PARM_DESC(probe_only, "Only identify the chip and log its state");
static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "Also drive other WD90C2x/3x (e.g. C30/C31 on the 86Box bench)");
static unsigned int fake;
module_param(fake, uint, 0444);
MODULE_PARM_DESC(fake, "TESTING ONLY: 1 = treat any VGA as a WD90C24 (fail-safe path); 2 = also skip the self-tests (QEMU suspend/resume test); 3 = also emulate the BITBLT engine on a RAM model of video memory (see verify); 4 = like 3 but the engine takes no CPU time and the model is not kept (timing: the real engine runs alongside the CPU)");
static unsigned int fake_kbps;
module_param(fake_kbps, uint, 0444);
MODULE_PARM_DESC(fake_kbps, "TESTING ONLY, with fake=: model a video memory bus of this many KiB/s (CPU busy-waits per row copied)");
static unsigned int trial;
module_param(trial, uint, 0444);
MODULE_PARM_DESC(trial, "Seconds until text mode is restored unless keep=1 is written (0 = off)");
static bool accel = true;
module_param(accel, bool, 0444);
MODULE_PARM_DESC(accel, "Use the WD90C24A BITBLT engine for fills and scrolling (if its self-test passes)");
static char *output = "auto";
module_param(output, charp, 0444);
MODULE_PARM_DESC(output, "auto (as the BIOS/Fn+F7 set it), lcd, crt or both (WD90C24 only)");
static bool keep;
module_param(keep, bool, 0644);
MODULE_PARM_DESC(keep, "Write 1 during the trial period to keep the framebuffer");
static bool xaccel = true;
module_param(xaccel, bool, 0644);
MODULE_PARM_DESC(xaccel, "Offer mmap clients (Xfbdev) the damage/BITBLT ioctls (0 = they get the 10 Hz full-frame flush)");
static bool hwcursor = true;
module_param(hwcursor, bool, 0644);
MODULE_PARM_DESC(hwcursor, "Offer Xfbdev the WD90C24 hardware cursor (default 1; 0: X draws it in software)");
static int hwc_xoff, hwc_yoff;
module_param(hwc_xoff, int, 0644);
MODULE_PARM_DESC(hwc_xoff, "Hardware cursor X calibration in pixels (if the chip shows it off by some pixels)");
module_param(hwc_yoff, int, 0644);
MODULE_PARM_DESC(hwc_yoff, "Hardware cursor Y calibration in pixels");
static int cexp = 1;
module_param(cexp, int, 0444);
MODULE_PARM_DESC(cexp, "BITBLT colour expansion for two-colour damage (text): 0 off, 1 (default) on if the self-test passes and it measures faster, 2 on if the self-test passes");
static int cexp_var;
module_param(cexp_var, int, 0444);
MODULE_PARM_DESC(cexp_var, "Colour expansion word format 0-11 (default 0: the one the 755C's WD90C24A takes; see cexp_test), -1 = probe them all");
static unsigned int fake_outw_ns = 500;
module_param(fake_outw_ns, uint, 0644);
MODULE_PARM_DESC(fake_outw_ns, "TESTING ONLY, with fake_kbps: modelled cost of one 16-bit port write");
static bool apm_blank = true;
module_param(apm_blank, bool, 0644);
MODULE_PARM_DESC(apm_blank, "Blank levels 2-4 (suspend/powerdown) also put the display in APM standby: the BIOS switches the backlight off (needs CONFIG_APM_DISPLAY_BLANK)");

struct wd_stats {
	u64 bytes;		/* copied from the shadow to video memory */
	u64 rects;		/* ... in this many rectangles (rows of a rect: 1) */
	u64 fills, copies;	/* BITBLT operations */
	u64 ioctls;		/* WDFB_IOC_OPS calls */
	u64 full;		/* whole-frame flushes (10 Hz mmap poll, redraws) */
	u64 ns;			/* time spent copying to video memory */
	u64 model_ns;		/* fake=3: CPU time of the emulated BITBLT */
	u64 ioctl_ns;		/* time in WDFB_IOC_OPS (includes the above) */
	u64 cursor;		/* WDFB_IOC_CURSOR calls */
	u64 cexp, cexp_px, cexp_words;	/* colour-expanded bands, pixels, port words */
};

/* Off-screen cursor pattern: above the 300 KiB frame, below the BITBLT
 * self-test area at 384 KiB. */
#define WD_CUR_OFF	0x50000

struct wd_cur {
	bool on;		/* shown */
	bool loaded;		/* pattern + colours are in the chip */
	s16 x, y;
	u8 hotx, hoty, fg, bg;
	u8 pat[WD_CUR_BYTES];
};

struct wd_par {
	struct wd_hw hw;
	struct wd_ext_state bios;
	struct vgastate vstate;
	u8 *shadow;
	u32 pseudo_palette[16];
	spinlock_t lock;	/* bank register + window */
	bool live;		/* hardware is in our mode */
	bool accel;		/* BITBLT engine in use */
	bool blt_pending;	/* a blit may still be running */
	bool panel_off;		/* we powered the panel down (blank) */
	bool registered;
	bool suspended;		/* text mode put back for an APM suspend */
	atomic_t users;
	bool mmapped;
	bool client_ops;	/* an mmap client sends WDFB_IOC_OPS: no polling */
	bool apm_blanked;	/* the display is in APM standby (blank) */
	u8 *model;		/* fake=3: RAM model of video memory */
	struct wd_stats st;
	struct wd_cur cur;
	int cexp_v;		/* colour expansion variant in use, -1 = off */
	struct delayed_work mmap_work;
	struct delayed_work trial_work;
};

static struct fb_info *wd_info;
static struct platform_device *wd_pdev;
static DEFINE_MUTEX(wd_stop_lock);
static void wd_cur_apply(struct wd_par *par, bool shape);
static void wd_cur_off(struct wd_par *par);	/* trial end, text=1, resume failure, rmmod */

/* Called with par->lock held. Returns true if the engine is idle. */
static bool wd_blt_idle(struct wd_par *par)
{
	if (!par->blt_pending)
		return true;
	par->blt_pending = false;
	if (wd_blt_wait(&par->hw)) {
		par->accel = false;
		return false;
	}
	return true;
}

/*
 * Colour expansion: rows of a damaged rectangle that hold at most two
 * colours (text, stipples, glyph runs) go to video memory as one BITBLT
 * fed one bit per pixel through the host port, instead of a byte per
 * pixel over the memory window. Returns the number of rows done (0: the
 * first row has more than two colours, copy it as usual).
 */
#define WD_CEXP_MIN_W	16	/* narrower: the setup costs more than it saves */
#define WD_CEXP_WORDS	1024	/* port words per lock hold */

static int wd_row_colours(const u8 *p, u32 w, u8 c[2], int n)
{
	u32 i;

	for (i = 0; i < w; i++) {
		if ((n > 0 && p[i] == c[0]) || (n > 1 && p[i] == c[1]))
			continue;
		if (n == 2)
			return -1;
		c[n++] = p[i];
	}
	return n;
}

static u32 wd_damage_cexp(struct wd_par *par, u32 x, u32 y, u32 w, u32 h,
			  bool *lost)
{
	const u8 *row = par->shadow + y * WD_PITCH + x;
	u32 per = wd_cexp_words(w, par->cexp_v), n = 0, r, bus_ns;
	unsigned long flags;
	u8 c[2];
	int nc = 0;

	while (n < h && (n + 1) * per <= WD_CEXP_WORDS) {
		int k = wd_row_colours(row + n * WD_PITCH, w, c, nc);

		if (k < 0)
			break;
		nc = k;
		n++;
	}
	if (!n)
		return 0;
	if (nc < 2)
		c[1] = c[0];
	spin_lock_irqsave(&par->lock, flags);
	if (!par->live) {
		spin_unlock_irqrestore(&par->lock, flags);
		return n;
	}
	if (!wd_blt_idle(par))
		*lost = true;
	if (par->model) {
		/* QEMU: no engine; draw it plainly, charge the port words */
		wd_flush_rect(&par->hw, par->shadow, x, y, w, n);
		if (fake == 3)
			for (r = 0; r < n; r++)
				memcpy(par->model + (y + r) * WD_PITCH + x,
				       row + r * WD_PITCH, w);
		if (fake_kbps) {
			bus_ns = n * per * fake_outw_ns;
			udelay(bus_ns / 1000);
		}
	} else if (par->accel &&
		   !wd_cexp_start(&par->hw, y * WD_PITCH + x, WD_PITCH, w, n,
				  c[0], c[1], false)) {
		for (r = 0; r < n; r++)
			wd_cexp_row(&par->hw, row + r * WD_PITCH, w, c[0],
				    par->cexp_v, false);
		par->blt_pending = true;
	} else {
		wd_flush_rect(&par->hw, par->shadow, x, y, w, n);
	}
	par->st.cexp++;
	par->st.cexp_px += w * n;
	par->st.cexp_words += n * per;
	spin_unlock_irqrestore(&par->lock, flags);
	return n;
}

static void wd_damage(struct fb_info *info, u32 x, u32 y, u32 w, u32 h)
{
	struct wd_par *par = info->par;
	unsigned long flags;
	bool lost = false;
	u64 t0 = wd_now_ns();
	u32 r, bus_ns = 0;

	if (x >= WD_XRES || y >= WD_YRES || !w || !h)
		return;
	w = min(w, WD_XRES - x);
	h = min(h, WD_YRES - y);
	/* one row per lock hold: short interrupt-off windows on a 486 */
	for (r = 0; r < h; r++) {
		if (par->cexp_v >= 0 && w >= WD_CEXP_MIN_W) {
			u32 n = wd_damage_cexp(par, x, y + r, w, h - r, &lost);

			if (n) {
				r += n - 1;
				continue;
			}
		}
		spin_lock_irqsave(&par->lock, flags);
		if (par->live) {
			if (!wd_blt_idle(par))
				lost = true;
			wd_flush_rect(&par->hw, par->shadow, x, y + r, w, 1);
			if (fake && fake_kbps) {	/* QEMU maps A0000 as RAM */
				bus_ns += div_u64((u64)w * 1000000000, fake_kbps * 1024U);
				udelay(bus_ns / 1000);
				bus_ns %= 1000;
			}
			if (par->model && fake == 3)
				memcpy(par->model + (y + r) * WD_PITCH + x,
				       par->shadow + (y + r) * WD_PITCH + x, w);
		}
		spin_unlock_irqrestore(&par->lock, flags);
	}
	par->st.bytes += w * h;
	par->st.rects++;
	par->st.ns += wd_now_ns() - t0;
	if (lost) {
		pr_warn("BITBLT engine timed out: acceleration off, redrawing\n");
		wd_damage(info, 0, 0, WD_XRES, WD_YRES);
	}
}

static bool wd_clip(u32 x, u32 y, u32 w, u32 h)
{
	return w && h && x < WD_XRES && y < WD_YRES &&
	       w <= WD_XRES - x && h <= WD_YRES - y;
}

/* fake=3: the BITBLT engine's job, done on the RAM model of video memory */
static void wd_model_op(struct wd_par *par, bool fill, u32 sx, u32 sy, u32 dx,
			u32 dy, u32 w, u32 h, u8 color, bool xor)
{
	u32 r, i, c4 = color * 0x01010101u;
	u8 *first = NULL;

	/*
	 * Kept cheap (rep movs where rows do not overlap; memmove/memset are
	 * not exported by this kernel), since on real hardware the engine
	 * works in parallel with the CPU: the less CPU the model takes, the
	 * closer QEMU timings are to the 755C's.
	 */
	for (r = 0; r < h; r++) {
		/* the engine copies backwards when the destination is later */
		u32 row = !fill && dy * WD_PITCH + dx > sy * WD_PITCH + sx ? h - 1 - r : r;
		u8 *d = par->model + (dy + row) * WD_PITCH + dx;
		const u8 *src = par->model + (sy + row) * WD_PITCH + sx;

		if (fill && !xor && first) {
			memcpy(d, first, w);
		} else if (fill) {
			for (i = 0; i + 4 <= w; i += 4)
				*(u32 *)(d + i) = xor ? *(u32 *)(d + i) ^ c4 : c4;
			for (; i < w; i++)
				d[i] = xor ? d[i] ^ color : color;
			first = d;
		} else if (d > src && d < src + w) {
			for (i = w; i--; )
				d[i] = src[i];
		} else if (d < src && src < d + w) {
			for (i = 0; i < w; i++)
				d[i] = src[i];
		} else {
			memcpy(d, src, w);
		}
	}
}

/* Start a blit; false if the CPU must copy the rectangle instead. */
static bool wd_hw_op(struct fb_info *info, bool fill, u32 sx, u32 sy, u32 dx,
		     u32 dy, u32 w, u32 h, u8 color, bool xor)
{
	struct wd_par *par = info->par;
	unsigned long flags;
	bool done = false, lost = false;
	int ret;

	spin_lock_irqsave(&par->lock, flags);
	if (par->live && par->accel && par->model) {
		u64 t = wd_now_ns();

		if (fake == 3)
			wd_model_op(par, fill, sx, sy, dx, dy, w, h, color, xor);
		par->st.model_ns += wd_now_ns() - t;
		done = true;
	} else if (par->live && par->accel) {
		ret = fill ? wd_blt_fill(&par->hw, dx, dy, w, h, color, xor) :
			     wd_blt_copy(&par->hw, sx, sy, dx, dy, w, h);
		if (ret) {
			par->accel = false;
			par->blt_pending = false;
			lost = true;
		} else {
			par->blt_pending = true;
			done = true;
		}
	}
	spin_unlock_irqrestore(&par->lock, flags);
	if (lost) {
		pr_warn("BITBLT engine timed out: acceleration off, redrawing\n");
		wd_damage(info, 0, 0, WD_XRES, WD_YRES);
	}
	if (done && fill)
		par->st.fills++;
	else if (done)
		par->st.copies++;
	return done;
}

static void wd_fillrect(struct fb_info *info, const struct fb_fillrect *r)
{
	sys_fillrect(info, r);
	if (!wd_clip(r->dx, r->dy, r->width, r->height) ||
	    !wd_hw_op(info, true, 0, 0, r->dx, r->dy, r->width, r->height,
		      r->color, r->rop == ROP_XOR))
		wd_damage(info, r->dx, r->dy, r->width, r->height);
}

static void wd_copyarea(struct fb_info *info, const struct fb_copyarea *a)
{
	sys_copyarea(info, a);
	if (!wd_clip(a->sx, a->sy, a->width, a->height) ||
	    !wd_clip(a->dx, a->dy, a->width, a->height) ||
	    !wd_hw_op(info, false, a->sx, a->sy, a->dx, a->dy, a->width,
		      a->height, 0, false))
		wd_damage(info, a->dx, a->dy, a->width, a->height);
}

static void wd_imageblit(struct fb_info *info, const struct fb_image *img)
{
	sys_imageblit(info, img);
	wd_damage(info, img->dx, img->dy, img->width, img->height);
}

static ssize_t wd_write(struct fb_info *info, const char __user *buf,
			size_t count, loff_t *ppos)
{
	u32 start = *ppos < WD_FB_SIZE ? (u32)*ppos : WD_FB_SIZE;
	ssize_t ret = fb_sys_write(info, buf, count, ppos);

	if (ret > 0) {
		/* fb_sys_write clamps to smem_len, so the u32 maths is safe */
		u32 y0 = start / WD_PITCH, y1 = (start + (u32)ret - 1) / WD_PITCH;

		wd_damage(info, 0, y0, WD_XRES, y1 - y0 + 1);
	}
	return ret;
}

static void wd_mmap_flush(struct work_struct *work)
{
	struct wd_par *par = container_of(work, struct wd_par, mmap_work.work);

	if (!par->mmapped || !atomic_read(&par->users))
		return;
	wd_damage(wd_info, 0, 0, WD_XRES, WD_YRES);
	par->st.full++;
	/* a client that reports its damage (WDFB_IOC_OPS) is not polled */
	if (!par->client_ops)
		schedule_delayed_work(&par->mmap_work, HZ / 10);
}

static int wd_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct wd_par *par = info->par;
	int ret = remap_vmalloc_range(vma, par->shadow, vma->vm_pgoff);

	if (!ret && !par->mmapped) {
		par->mmapped = true;
		schedule_delayed_work(&par->mmap_work, HZ / 10);
	}
	return ret;
}

static int wd_open(struct fb_info *info, int user)
{
	struct wd_par *par = info->par;

	if (user)
		atomic_inc(&par->users);
	return 0;
}

static int wd_release(struct fb_info *info, int user)
{
	struct wd_par *par = info->par;

	if (user && atomic_dec_and_test(&par->users)) {
		par->client_ops = false;
		wd_cur_off(par);
		if (par->mmapped) {
			par->mmapped = false;
			wd_damage(info, 0, 0, WD_XRES, WD_YRES);
		}
	}
	return 0;
}

/*
 * WDFB_IOC_OPS (wd90c24fb_ioctl.h): the client's damage, fills and copies,
 * in order. Damage is copied to video memory right here, in the client's
 * time slice, so a fast client cannot outrun the bus and nothing is
 * polled. A COPY the engine cannot do (accel lost) means the frame is
 * redrawn shortly, after the client has done its own copy in the shadow.
 */
static int wd_ops_ioctl(struct fb_info *info, const void __user *arg)
{
	struct wd_par *par = info->par;
	const struct wdfb_op __user *uop;
	struct wdfb_op op[16];
	struct wdfb_ops req;
	bool hw = false, redraw = false;
	u32 i, k, n;
	int ret = 0;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (req.n > WDFB_MAX_OPS || req.flags)
		return -EINVAL;
	uop = u64_to_user_ptr(req.ops);
	par->client_ops = true;
	par->st.ioctls++;
	for (i = 0; i < req.n && !ret; i += n) {
		n = min_t(u32, req.n - i, ARRAY_SIZE(op));
		if (copy_from_user(op, uop + i, n * sizeof(*op))) {
			ret = -EFAULT;
			break;
		}
		for (k = 0; k < n && !ret; k++) {
			const struct wdfb_op *o = &op[k];

			if (!wd_clip(o->x, o->y, o->w, o->h)) {
				ret = -EINVAL;
				break;
			}
			switch (o->op) {
			case WDFB_OP_DAMAGE:
				wd_damage(info, o->x, o->y, o->w, o->h);
				break;
			case WDFB_OP_FILL:
				hw = true;
				if (!wd_hw_op(info, true, 0, 0, o->x, o->y, o->w,
					      o->h, o->color, false))
					wd_damage(info, o->x, o->y, o->w, o->h);
				break;
			case WDFB_OP_COPY:
				if (!wd_clip(o->sx, o->sy, o->w, o->h)) {
					ret = -EINVAL;
					break;
				}
				hw = true;
				if (!wd_hw_op(info, false, o->sx, o->sy, o->x,
					      o->y, o->w, o->h, 0, false))
					redraw = true;
				break;
			default:
				ret = -EINVAL;
			}
		}
	}
	if (ret) {
		/* a confused client: back to polling, so the screen never freezes */
		par->client_ops = false;
		redraw = true;
	}
	if (redraw)
		schedule_delayed_work(&par->mmap_work, HZ / 25);
	if (!ret && hw && !par->accel)
		ret = WDFB_LOST;
	return ret;
}

static int wd_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
	struct wd_par *par = info->par;
	u32 caps;

	if (!xaccel)
		return -ENOTTY;
	switch (cmd) {
	case WDFB_IOC_CAPS:
		caps = WDFB_CAP_DAMAGE;
		if (par->accel)
			caps |= WDFB_CAP_FILL | WDFB_CAP_COPY;
		if (hwcursor && (par->hw.ext_blocks || par->model))
			caps |= WDFB_CAP_CURSOR;
		return put_user(caps, (u32 __user *)arg);
	case WDFB_IOC_CURSOR: {
		struct wdfb_cursor c;
		unsigned long flags;

		if (!hwcursor || !(par->hw.ext_blocks || par->model))
			return -ENOTTY;
		if (copy_from_user(&c, (const void __user *)arg, sizeof(c)))
			return -EFAULT;
		if (c.flags & ~(WDFB_CUR_SHOW | WDFB_CUR_SHAPE | WDFB_CUR_POS) ||
		    c.hotx > 31 || c.hoty > 31)
			return -EINVAL;
		spin_lock_irqsave(&par->lock, flags);
		par->st.cursor++;
		if (c.flags & WDFB_CUR_SHAPE) {
			wd_cursor_pattern(c.image, c.mask, par->cur.pat);
			par->cur.hotx = c.hotx;
			par->cur.hoty = c.hoty;
			par->cur.fg = c.fg;
			par->cur.bg = c.bg;
		}
		if (c.flags & WDFB_CUR_POS) {
			par->cur.x = c.x;
			par->cur.y = c.y;
		}
		par->cur.on = !!(c.flags & WDFB_CUR_SHOW);
		wd_cur_apply(par, c.flags & WDFB_CUR_SHAPE);
		spin_unlock_irqrestore(&par->lock, flags);
		return 0;
	}
	case WDFB_IOC_OPS: {
		u64 t = wd_now_ns();
		int ret = wd_ops_ioctl(info, (const void __user *)arg);

		par->st.ioctl_ns += wd_now_ns() - t;
		return ret;
	}
	}
	return -ENOTTY;
}

static int wd_setcolreg(unsigned int regno, unsigned int red, unsigned int green,
			unsigned int blue, unsigned int transp, struct fb_info *info)
{
	struct wd_par *par = info->par;

	if (regno >= 256)
		return 1;
	if (regno < 16)
		par->pseudo_palette[regno] = regno;
	/* the DAC is shared with text mode: only touch it while we own it */
	if (par->live)
		wd_set_dac(regno, red >> 10, green >> 10, blue >> 10);
	return 0;
}

/* Panel power-up while live: each step under the lock, sleeps outside it. */
static void wd_live_panel_on(struct wd_par *par)
{
	unsigned long flags;
	int i;

	for (i = 0; i < WD_PANEL_STEPS; i++) {
		spin_lock_irqsave(&par->lock, flags);
		wd_panel_on_step(&par->hw, i);
		spin_unlock_irqrestore(&par->lock, flags);
		if (i < WD_PANEL_STEPS - 1)
			msleep(WD_PANEL_STEP_MS);
	}
}

static bool wd_live_panel_off(struct wd_par *par)
{
	unsigned long flags;
	bool off;

	spin_lock_irqsave(&par->lock, flags);
	off = wd_panel_off(&par->hw);
	spin_unlock_irqrestore(&par->lock, flags);
	return off;
}

/*
 * APM display standby around a blank: the 755C BIOS, like Windows 98's
 * power management through it, switches the CCFL backlight off. Only the
 * APM driver can make the BIOS call; with CONFIG_APM_DISPLAY_BLANK it
 * offers it as console_blank_hook (the VT blanker calls the same hook).
 * Returns true if the display was put in standby.
 */
static bool wd_apm_standby(struct wd_par *par)
{
	int (*hook)(int) = wd_blank_hook();

	if (!apm_blank || !hook || par->apm_blanked)
		return false;
	par->apm_blanked = hook(1) > 0;
	if (!par->apm_blanked)
		pr_info_once("blank: APM display standby refused by the BIOS\n");
	return par->apm_blanked;
}

/* Back from APM standby: the BIOS may have touched the chip, so the bank
 * cache, the DAC and the picture are all done again. */
static void wd_apm_resume(struct fb_info *info)
{
	struct wd_par *par = info->par;
	int (*hook)(int) = wd_blank_hook();
	unsigned long flags;

	if (!par->apm_blanked)
		return;
	par->apm_blanked = false;
	if (hook)
		hook(0);
	spin_lock_irqsave(&par->lock, flags);
	par->hw.bank = -1;
	spin_unlock_irqrestore(&par->lock, flags);
	fb_set_cmap(&info->cmap, info);
	wd_damage(info, 0, 0, WD_XRES, WD_YRES);
}

/*
 * fb_blank levels (stage 4 + backlight):
 *   UNBLANK          picture on, panel powered, backlight on
 *   NORMAL           black picture (SR01 screen off); panel and backlight on
 *   VSYNC_SUSPEND,
 *   HSYNC_SUSPEND,
 *   POWERDOWN        black picture, panel off (PNLOFF, pins, PR19), and the
 *                    display in APM standby, which turns the backlight off
 * Any level can follow any other; unblank powers everything back up in
 * order (APM first, then the panel with its 100 ms steps, then the
 * picture). Caller holds the console lock (fb_blank); this may sleep.
 */
static int wd_blank(int blank, struct fb_info *info)
{
	struct wd_par *par = info->par;
	bool power_down = blank > FB_BLANK_NORMAL;
	unsigned long flags;

	if (!par->live)
		return 0;
	if (!power_down) {
		wd_apm_resume(info);
		if (par->panel_off) {
			wd_live_panel_on(par);
			par->panel_off = false;
		}
		spin_lock_irqsave(&par->lock, flags);
		wd_screen_on(&par->hw, blank == FB_BLANK_UNBLANK);
		spin_unlock_irqrestore(&par->lock, flags);
		return 0;
	}
	spin_lock_irqsave(&par->lock, flags);
	wd_screen_on(&par->hw, false);
	spin_unlock_irqrestore(&par->lock, flags);
	if (par->hw.chip == WD_C24)
		par->panel_off |= wd_live_panel_off(par);
	wd_apm_standby(par);
	return 0;
}

static void wd_apply_output(struct wd_par *par)
{
	bool lcd, crt;
	unsigned long flags;
	u8 pr19;

	if (par->hw.chip != WD_C24 || !output || !strcmp(output, "auto"))
		return;
	if (!strcmp(output, "lcd")) {
		lcd = true; crt = false;
	} else if (!strcmp(output, "crt")) {
		lcd = false; crt = true;
	} else if (!strcmp(output, "both")) {
		lcd = crt = true;
	} else {
		pr_warn("output=%s: use auto, lcd, crt or both\n", output);
		return;
	}
	spin_lock_irqsave(&par->lock, flags);
	pr19 = wd_read_pr19(&par->hw);
	wd_write_pr19(&par->hw, crt ? pr19 | PR19_CRT : pr19 & ~PR19_CRT);
	spin_unlock_irqrestore(&par->lock, flags);
	if (lcd && !(pr19 & PR19_LCD))
		wd_live_panel_on(par);
	else if (!lcd)
		wd_live_panel_off(par);
	pr_info("output=%s (PR19 0x%02x)\n", output, wd_read_pr19(&par->hw));
}

static const struct fb_ops wd_ops = {
	.owner		= THIS_MODULE,
	.fb_read	= fb_sys_read,
	.fb_write	= wd_write,
	.fb_mmap	= wd_mmap,
	.fb_open	= wd_open,
	.fb_release	= wd_release,
	.fb_setcolreg	= wd_setcolreg,
	.fb_blank	= wd_blank,
	.fb_fillrect	= wd_fillrect,
	.fb_copyarea	= wd_copyarea,
	.fb_imageblit	= wd_imageblit,
	.fb_ioctl	= wd_ioctl,
};

/* Text mode back, in PVGA1Restore order. Caller holds the console lock. */
static void wd_restore_text(struct wd_par *par)
{
	unsigned long flags;

	spin_lock_irqsave(&par->lock, flags);
	par->live = false;
	wd_blt_idle(par);	/* bounded; a stuck engine must not block restore */
	spin_unlock_irqrestore(&par->lock, flags);
	wd_restore_ext(&par->hw, &par->bios);
	restore_vga(&par->vstate);
	wd_relock(&par->hw, &par->bios);
	wd_screen_on(&par->hw, true);	/* never leave the panel blank */
	wd_restore_panel(&par->hw, &par->bios);	/* outputs + panel power */
	par->panel_off = false;
	if (par->apm_blanked) {		/* out of APM display standby too */
		int (*hook)(int) = wd_blank_hook();

		par->apm_blanked = false;
		if (hook)
			hook(0);
	}
}

/* Mode set + self-test. Caller holds the console lock. */
/* Push the cursor state to the chip. Called with par->lock held. */
static void wd_cur_apply(struct wd_par *par, bool shape)
{
	struct wd_cur *c = &par->cur;
	int x, y;

	if (!par->live || !par->hw.ext_blocks || par->model)
		return;	/* fake modes: QEMU has no cursor block, state only */
	wd_blt_idle(par);
	x = c->x + hwc_xoff;
	y = c->y + hwc_yoff;
	/* the chip cannot place the hot spot left/above the screen edge:
	 * pin it there (the hot spot pixel is still exact at 0) */
	x = clamp(x, 0, 0x7ff);
	y = clamp(y, 0, 0x3ff);
	if (shape || !c->loaded) {
		wd_vram_write(&par->hw, WD_CUR_OFF, c->pat, WD_CUR_BYTES);
		wd_cursor_move(&par->hw, x, y);
		wd_cursor_load(&par->hw, WD_CUR_OFF, c->fg, c->bg, c->hotx,
			       c->hoty, c->on);
		c->loaded = true;
	} else {
		wd_cursor_move(&par->hw, x, y);
		wd_cursor_enable(&par->hw, c->on);
	}
}

static void wd_cur_off(struct wd_par *par)
{
	unsigned long flags;

	spin_lock_irqsave(&par->lock, flags);
	if (par->cur.on) {
		par->cur.on = false;
		wd_cur_apply(par, false);
	}
	spin_unlock_irqrestore(&par->lock, flags);
}

/*
 * Colour expansion self-test: every word format (or just cexp_var) is
 * tried off screen at 0x60000 and compared with what the CPU would draw;
 * then port writes and memory-window writes are timed. The report goes
 * to dmesg and to /sys/module/wd90c24fb/parameters/cexp_test.
 */
static char wd_cexp_report[1024];

static void wd_cexp_time(struct wd_par *par, u32 *outw_ns10, u32 *byte_ns10)
{
	static u8 buf[2048];
	unsigned long flags;
	u64 t;
	int i;

	spin_lock_irqsave(&par->lock, flags);
	wd_blt_idle(par);
	t = wd_now_ns();
	for (i = 0; i < 1000; i++)	/* harmless: re-select block 1 */
		wd_outw(WD_EXT_NOINC | WD_BLK_BLT, WD_EXT_INDEX);
	*outw_ns10 = div_u64((wd_now_ns() - t) * 10, 1000);
	memset(buf, 0, sizeof(buf));
	wd_set_bank(&par->hw, 0x60);	/* off screen */
	t = wd_now_ns();
	for (i = 0; i < 2; i++)
		wd_win_write(&par->hw, 0, buf, sizeof(buf));
	*byte_ns10 = div_u64((wd_now_ns() - t) * 10, 2 * sizeof(buf));
	par->hw.bank = -1;
	spin_unlock_irqrestore(&par->lock, flags);
}

/* Returns the passing variant with the most pixels per word, or -1. */
static int wd_cexp_selftest(struct wd_par *par, int only, u32 *speedup10)
{
	char why[96];
	int v, len = 0, best = -1;
	u32 on, bn;
	unsigned long flags;

	for (v = WD_CEXP_VARIANTS - 1; v >= 0; v--) {
		int bad;

		if (only >= 0 && v != only)
			continue;
		spin_lock_irqsave(&par->lock, flags);
		wd_blt_idle(par);
		bad = wd_cexp_probe(&par->hw, v, why, sizeof(why));
		spin_unlock_irqrestore(&par->lock, flags);
		len += scnprintf(wd_cexp_report + len, sizeof(wd_cexp_report) - len,
				 "var %2d (%2u px/word, %s first, %s rows): %s%s%s\n",
				 v, wd_cexp_ppw(v), v & 2 ? "lsb" : "msb",
				 v & 1 ? "dword" : "word",
				 bad < 0 ? "HUNG" : bad ? "FAIL " : "PASS",
				 bad > 0 ? why : "", "");
		if (bad < 0) {	/* engine still busy: no more probing */
			par->accel = false;
			break;
		}
		if (!bad && best < 0)
			best = v;
	}
	wd_cexp_time(par, &on, &bn);
	*speedup10 = best >= 0 && on ? wd_cexp_ppw(best) * bn * 10 / on : 0;
	len += scnprintf(wd_cexp_report + len, sizeof(wd_cexp_report) - len,
			 "port write %u.%u us, memory byte %u.%u us; best var %d, est. speed-up %u.%ux\n",
			 on / 10000, (on / 1000) % 10, bn / 10000, (bn / 1000) % 10,
			 best, *speedup10 / 10, *speedup10 % 10);
	if (!par->accel)
		scnprintf(wd_cexp_report + len, sizeof(wd_cexp_report) - len,
			  "BITBLT engine stuck: acceleration off until reload\n");
	return best;
}

static void wd_cexp_setup(struct wd_par *par)
{
	u32 sp;
	int v;

	par->cexp_v = -1;
	if (!cexp || !par->accel)
		return;
	if (par->model) {
		par->cexp_v = cexp_var >= 0 && cexp_var < WD_CEXP_VARIANTS ? cexp_var : 0;
		pr_warn("fake=%u: colour expansion emulated (var %d)\n", fake, par->cexp_v);
		return;
	}
	v = wd_cexp_selftest(par, cexp_var, &sp);
	if (v < 0)
		pr_info("colour expansion: self-test failed, CPU copies (cat parameters/cexp_test)\n");
	else if (cexp == 1 && sp < 11)
		pr_info("colour expansion: var %d works but is not faster (%u.%ux), off\n",
			v, sp / 10, sp % 10);
	else
		par->cexp_v = v;
	if (par->cexp_v >= 0)
		pr_info("colour expansion on: var %d, est. %u.%ux\n", v, sp / 10, sp % 10);
}

static int wd_go_live(struct fb_info *info)
{
	struct wd_par *par = info->par;
	struct wd_ext_state ext;
	struct wd_std std;

	wd_mode_640x480x8(&par->hw, &par->bios, &std, &ext);
	wd_set_mode(&par->hw, &std, &ext);
	if (fake >= 2) {
		pr_warn("fake=2: self-tests skipped (testing only)\n");
	} else if (wd_selftest_banks(&par->hw, par->hw.vram_kb)) {
		wd_restore_text(par);
		pr_warn("banking self-test failed: text mode restored\n");
		return -ENODEV;
	}
	if (par->model) {
		par->accel = accel;
		pr_warn("fake=%u: BITBLT emulated%s\n", fake, fake == 3 ?
			" on a RAM model of video memory" : ", taking no time (timing only)");
	} else if (par->hw.ext_blocks && accel) {
		par->accel = !wd_blt_selftest(&par->hw);
		pr_info("BITBLT self-test %s\n", par->accel ? "passed: acceleration on" :
			"failed: CPU drawing only");
	}
	par->cexp_v = -1;
	par->live = true;
	wd_cexp_setup(par);
	fb_set_cmap(&info->cmap, info);
	wd_damage(info, 0, 0, WD_XRES, WD_YRES);
	par->cur.loaded = false;
	if (par->cur.on) {	/* back from a suspend with X running */
		unsigned long flags;

		spin_lock_irqsave(&par->lock, flags);
		wd_cur_apply(par, true);
		spin_unlock_irqrestore(&par->lock, flags);
	}
	return 0;
}

/* /sys/module/wd90c24fb/parameters/stats: what reached video memory, and how */
static int wd_stats_get(char *buf, const struct kernel_param *kp)
{
	const struct wd_par *par;
	const struct wd_stats *st;

	if (!wd_info)
		return scnprintf(buf, PAGE_SIZE, "no framebuffer\n");
	par = wd_info->par;
	st = &par->st;
	return scnprintf(buf, PAGE_SIZE,
			 "bytes=%llu rects=%llu fills=%llu copies=%llu ioctls=%llu full=%llu flush_us=%llu model_us=%llu ioctl_us=%llu accel=%d client=%d cursor=%llu hwcursor=%d cexp=%llu cexp_px=%llu cexp_words=%llu cexp_var=%d\n",
			 st->bytes, st->rects, st->fills, st->copies, st->ioctls,
			 st->full, div_u64(st->ns, 1000), div_u64(st->model_ns, 1000),
			 div_u64(st->ioctl_ns, 1000),
			 par->accel, par->client_ops, st->cursor, par->cur.on,
			 st->cexp, st->cexp_px, st->cexp_words, par->cexp_v);
}
static const struct kernel_param_ops wd_stats_ops = { .get = wd_stats_get };
module_param_cb(stats, &wd_stats_ops, NULL, 0444);
MODULE_PARM_DESC(stats, "Read: bytes/rectangles copied to video memory, BITBLT fills/copies, time spent copying");

/* fake=3 only: bytes where the video memory model differs from the frame */
static int wd_verify_get(char *buf, const struct kernel_param *kp)
{
	struct wd_par *par = wd_info ? wd_info->par : NULL;
	unsigned long flags;
	u32 i, bad = 0, first = 0;

	if (!par || !par->model || fake != 3)
		return scnprintf(buf, PAGE_SIZE, "n/a (fake=3 only)\n");
	spin_lock_irqsave(&par->lock, flags);
	wd_blt_idle(par);
	spin_unlock_irqrestore(&par->lock, flags);
	for (i = 0; i < WD_FB_SIZE; i++)
		if (par->model[i] != par->shadow[i] && !bad++)
			first = i;
	return scnprintf(buf, PAGE_SIZE, "%u differing bytes (first at %u,%u)\n",
			 bad, first % WD_PITCH, first / WD_PITCH);
}
static const struct kernel_param_ops wd_verify_ops = { .get = wd_verify_get };
module_param_cb(verify, &wd_verify_ops, NULL, 0444);
MODULE_PARM_DESC(verify, "TESTING ONLY (fake=3): bytes where the video memory model differs from the frame");

static void wd_unregister(struct fb_info *info)
{
	struct wd_par *par = info->par;

	unregister_framebuffer(info);
	par->registered = false;
}

static void wd_stop(struct fb_info *info)
{
	struct wd_par *par = info->par;

	mutex_lock(&wd_stop_lock);
	if (par->registered) {
		console_lock();
		wd_restore_text(par);
		console_unlock();
		/* fbcon hands the consoles back to vgacon, which redraws */
		wd_unregister(info);
	}
	mutex_unlock(&wd_stop_lock);
}

/* echo 1 > /sys/module/wd90c24fb/parameters/text: back to text now */
static void wd_text_now(struct work_struct *work)
{
	struct wd_par *par = wd_info->par;

	cancel_delayed_work_sync(&par->trial_work);
	cancel_delayed_work_sync(&par->mmap_work);
	if (par->registered)
		pr_info("text=1: back to text mode (now rmmod wd90c24fb can unload)\n");
	wd_stop(wd_info);
}
static DECLARE_WORK(wd_text_work, wd_text_now);

static int wd_text_set(const char *val, const struct kernel_param *kp)
{
	bool v;
	int ret = kstrtobool(val, &v);

	if (ret)
		return ret;
	if (v && wd_info)
		schedule_work(&wd_text_work);
	return 0;
}

static const struct kernel_param_ops wd_text_ops = { .set = wd_text_set };

/*
 * echo N > /sys/module/wd90c24fb/parameters/cursor_test (needs hwcursor=1):
 * 0 hides the cursor, 1/2/3 show a test arrow (white, black outline, tip
 * = hot spot) at the centre / top-left / bottom-right of the screen.
 * Reading it dumps cursor registers 0-8 (block 2) as the chip has them.
 */
static int wd_cursor_test_set(const char *val, const struct kernel_param *kp)
{
	static const s16 pos[4][2] = { {0, 0}, {320, 240}, {0, 0}, {639, 479} };
	struct wd_par *par;
	unsigned long flags;
	u8 img[128], msk[128];
	unsigned int n, r;
	int ret = kstrtouint(val, 0, &n);

	if (ret)
		return ret;
	if (n > 3)
		return -EINVAL;
	if (!wd_info)
		return -ENODEV;
	par = wd_info->par;
	if (!hwcursor || !par->hw.ext_blocks || !par->live)
		return -ENODEV;
	/* the classic 12x19 arrow: B = black outline, W = white, tip = hot spot */
	static const char *const arrow[19] = {
		"B", "BB", "BWB", "BWWB", "BWWWB", "BWWWWB", "BWWWWWB",
		"BWWWWWWB", "BWWWWWWWB", "BWWWWWWWWB", "BWWWWWWWWWB",
		"BWWWWWWBBBBB", "BWWWBWWB", "BWWB BWWB", "BWB  BWWB",
		"BB    BWWB", "B     BWWB", "       BWWB", "        BB",
	};

	memset(img, 0, sizeof(img));
	memset(msk, 0, sizeof(msk));
	for (r = 0; r < 19; r++) {
		const char *p = arrow[r];
		unsigned int c;

		for (c = 0; p[c]; c++) {
			if (p[c] == ' ')
				continue;
			msk[r * 4 + c / 8] |= 0x80 >> (c % 8);
			if (p[c] == 'W')
				img[r * 4 + c / 8] |= 0x80 >> (c % 8);
		}
	}
	spin_lock_irqsave(&par->lock, flags);
	if (n) {
		wd_cursor_pattern(img, msk, par->cur.pat);
		par->cur.hotx = 0;
		par->cur.hoty = 0;
		par->cur.fg = 15;	/* VGA white */
		par->cur.bg = 0;	/* black */
		par->cur.x = pos[n][0];
		par->cur.y = pos[n][1];
	}
	par->cur.on = n != 0;
	wd_cur_apply(par, n != 0);
	spin_unlock_irqrestore(&par->lock, flags);
	return 0;
}

static int wd_cursor_test_get(char *buf, const struct kernel_param *kp)
{
	struct wd_par *par;
	unsigned long flags;
	int len = 0;
	u8 i;

	if (!wd_info)
		return scnprintf(buf, PAGE_SIZE, "no framebuffer\n");
	par = wd_info->par;
	if (!par->hw.ext_blocks || !par->live || par->model)
		return scnprintf(buf, PAGE_SIZE, "no cursor block\n");
	spin_lock_irqsave(&par->lock, flags);
	wd_blt_idle(par);
	for (i = 0; i <= 8; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%u=%03x", i ? " " : "",
				 i, wd_cursor_read(&par->hw, i));
	spin_unlock_irqrestore(&par->lock, flags);
	return len + scnprintf(buf + len, PAGE_SIZE - len, "\n");
}

/*
 * echo 1 > /sys/module/wd90c24fb/parameters/cexp_test: probe all colour
 * expansion formats off screen (works with cexp=0; nothing on screen
 * changes), report in dmesg and here.
 */
static int wd_cexp_test_set(const char *val, const struct kernel_param *kp)
{
	struct wd_par *par;
	u32 sp;
	unsigned int v;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;
	if (v != 1)
		return -EINVAL;
	if (!wd_info)
		return -ENODEV;
	par = wd_info->par;
	if (!par->live || !par->accel || par->model || !par->hw.ext_blocks)
		return -ENODEV;
	wd_cexp_selftest(par, -1, &sp);
	pr_info("cexp_test:\n%s", wd_cexp_report);
	return 0;
}

static int wd_cexp_test_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%s", wd_cexp_report[0] ?
			 wd_cexp_report : "not run (echo 1 > cexp_test)\n");
}

static const struct kernel_param_ops wd_cexp_test_ops = {
	.set = wd_cexp_test_set, .get = wd_cexp_test_get };
module_param_cb(cexp_test, &wd_cexp_test_ops, NULL, 0644);
MODULE_PARM_DESC(cexp_test, "Write 1: probe the BITBLT colour expansion formats off screen; read: the report");

static const struct kernel_param_ops wd_cursor_test_ops = {
	.set = wd_cursor_test_set, .get = wd_cursor_test_get };
module_param_cb(cursor_test, &wd_cursor_test_ops, NULL, 0644);
MODULE_PARM_DESC(cursor_test, "Hardware cursor probe (hwcursor=1): 0 off, 1 centre, 2 top-left, 3 bottom-right; read = registers");
module_param_cb(text, &wd_text_ops, NULL, 0200);
MODULE_PARM_DESC(text, "Write 1 to restore text mode and release the console (needed before rmmod)");

static void wd_trial_end(struct work_struct *work)
{
	struct wd_par *par = container_of(work, struct wd_par, trial_work.work);

	if (keep) {
		pr_info("trial over, keep=1: staying in 640x480x8\n");
		return;
	}
	pr_warn("trial over without keep=1: back to text mode (rmmod wd90c24fb to unload)\n");
	cancel_delayed_work_sync(&par->mmap_work);
	wd_stop(wd_info);
}

/*
 * APM suspend/resume (the kernel APM driver runs the device PM callbacks
 * around the BIOS call). Before suspend the BIOS gets back the text mode it
 * set up, with its own outputs and panel power, so whatever the 755C BIOS
 * saves and restores is a state it knows. On resume the whole mode set runs
 * again from the saved BIOS state (clocks included: PR68 bits 4:3 only,
 * MCLK is never touched) and the shadow is redrawn; if the self-test fails
 * after resume the framebuffer is dropped and text mode stays.
 */
static void wd_resume_failed(struct work_struct *work)
{
	struct fb_info *info = wd_info;
	struct wd_par *par = info->par;

	cancel_delayed_work_sync(&par->trial_work);
	cancel_delayed_work_sync(&par->mmap_work);
	mutex_lock(&wd_stop_lock);
	if (par->registered) {
		/* go_live already restored text mode */
		wd_unregister(info);
	}
	mutex_unlock(&wd_stop_lock);
}
static DECLARE_WORK(wd_resume_fail_work, wd_resume_failed);

static int wd_pm_suspend(struct device *dev)
{
	struct fb_info *info = wd_info;
	struct wd_par *par;
	int ret;

	if (!info)
		return 0;
	par = info->par;
	console_lock();
	if (par->live) {
		wd_restore_text(par);	/* frees the saved VGA state ... */
		/* ... so save it again: rmmod/trial end after resume needs it */
		par->vstate.flags = VGA_SAVE_MODE | VGA_SAVE_FONTS | VGA_SAVE_CMAP;
		ret = save_vga(&par->vstate);
		wd_relock(&par->hw, &par->bios);
		if (ret)
			pr_warn("suspend: cannot re-save the text state; text mode stays after resume\n");
		par->suspended = !ret;
		pr_info("suspend: text mode handed back to the BIOS\n");
	}
	console_unlock();
	return 0;
}

static int wd_pm_resume(struct device *dev)
{
	struct fb_info *info = wd_info;
	struct wd_par *par;
	int ret;

	if (!info)
		return 0;
	par = info->par;
	if (!par->suspended)
		return 0;
	par->suspended = false;
	if (!par->registered)
		return 0;
	console_lock();
	ret = wd_go_live(info);
	console_unlock();
	if (ret) {
		pr_warn("resume: mode set failed, staying in text mode\n");
		schedule_work(&wd_resume_fail_work);
		return 0;
	}
	wd_apply_output(par);
	pr_info("resume: 640x480x8 restored%s\n", par->accel ? ", BITBLT" : "");
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(wd_pm_ops, wd_pm_suspend, wd_pm_resume);

static struct platform_driver wd_pdrv = {
	.driver = {
		.name	= "wd90c24fb",
		.pm	= pm_sleep_ptr(&wd_pm_ops),
	},
};

static void wd_log_probe(const struct wd_hw *hw)
{
	if (hw->chip == WD_NONE) {
		pr_info("no Paradise/WD chip found\n");
		return;
	}
	pr_info("found %s (CR36/37 \"%c%c\"), %u KiB video memory%s\n",
		wd_chip_name(hw->chip), hw->sig[0] ? hw->sig[0] : '-',
		hw->sig[1] ? hw->sig[1] : '-', hw->vram_kb,
		hw->use_1meg ? " (C24 dual-scan rule)" : "");
	if (hw->chip == WD_C24 || hw->chip == WD_C2X)
		pr_info("panel: PR18=0x%02x PR19=0x%02x: LCD %s, CRT %s%s\n",
			hw->pr18, hw->pr19, hw->pr19 & PR19_LCD ? "on" : "off",
			hw->pr19 & PR19_CRT ? "on" : "off",
			hw->pr18 & 0x0b ? "" : ", dual scan");
	if (hw->chip == WD_C24)
		pr_info("extended register blocks (BITBLT, cursor) %s: %s\n",
			hw->ext_blocks ? "present" : "absent",
			hw->ext_blocks ? "WD90C24A/A2" : "plain WD90C24");
}

static void wd_init_info(struct fb_info *info, struct wd_par *par)
{
	struct fb_var_screeninfo *v = &info->var;
	struct fb_fix_screeninfo *f = &info->fix;

	strscpy(f->id, "WD90C24", sizeof(f->id));
	f->smem_len = WD_FB_SIZE;
	f->type = FB_TYPE_PACKED_PIXELS;
	f->visual = FB_VISUAL_PSEUDOCOLOR;
	f->line_length = WD_PITCH;
	f->accel = FB_ACCEL_NONE;

	v->xres = v->xres_virtual = WD_XRES;
	v->yres = v->yres_virtual = WD_YRES;
	v->bits_per_pixel = 8;
	v->red.length = v->green.length = v->blue.length = 6;
	v->activate = FB_ACTIVATE_NOW;
	v->height = v->width = -1;
	v->pixclock = 39722;		/* 25.175 MHz */
	v->left_margin = 48;
	v->right_margin = 16;
	v->upper_margin = 33;
	v->lower_margin = 10;
	v->hsync_len = 96;
	v->vsync_len = 2;
	v->vmode = FB_VMODE_NONINTERLACED;

	info->fbops = &wd_ops;
	info->flags = FBINFO_VIRTFB;
	/* tells fbcon to scroll with copyarea; harmless if the self-test fails */
	if ((par->hw.ext_blocks || par->model) && accel)
		info->flags |= FBINFO_HWACCEL_COPYAREA | FBINFO_HWACCEL_FILLRECT;
	info->screen_buffer = par->shadow;
	info->screen_size = WD_FB_SIZE;
	info->pseudo_palette = par->pseudo_palette;
}

static int __init wd90c24fb_init(void)
{
	struct wd_hw hw = { 0 };
	struct fb_info *info;
	struct wd_par *par;
	int ret;

	if (!enable) {
		pr_info("enable=0: not touching the chip\n");
		return -ENODEV;
	}
	wd_probe(&hw);	/* register-neutral; vgacon shares 0x3c0-0x3df */
	wd_log_probe(&hw);
	if (probe_only)
		return -ENODEV;
	if (fake) {
		pr_warn("fake=%u: treating this VGA as a WD90C24 (testing only)\n", fake);
		hw.chip = WD_C24;
		hw.vram_kb = 1024;
		hw.ext_blocks = false;
	} else if (hw.chip != WD_C24) {
		if (!force || hw.chip < WD_C24) {
			if (hw.chip != WD_NONE)
				pr_info("%s is not a WD90C24: not driving it%s\n",
					wd_chip_name(hw.chip),
					hw.chip >= WD_C24 ? " (force=1 to try)" : "");
			return -ENODEV;
		}
		pr_warn("force=1: driving a %s as a WD90C24\n", wd_chip_name(hw.chip));
	}
	if (hw.vram_kb < 512) {
		pr_info("%u KiB video memory: 640x480x8 needs 512 KiB\n", hw.vram_kb);
		return -ENODEV;
	}

	info = framebuffer_alloc(sizeof(*par), NULL);
	if (!info)
		return -ENOMEM;
	par = info->par;
	par->hw = hw;
	spin_lock_init(&par->lock);
	par->cexp_v = -1;
	atomic_set(&par->users, 0);
	INIT_DELAYED_WORK(&par->mmap_work, wd_mmap_flush);
	INIT_DELAYED_WORK(&par->trial_work, wd_trial_end);
	ret = -ENOMEM;
	par->shadow = vmalloc_user(PAGE_ALIGN(WD_FB_SIZE));
	if (!par->shadow)
		goto err_info;
	if (fake >= 3) {
		par->model = vmalloc_user(WD_FB_SIZE);	/* zeroed */
		if (!par->model)
			goto err_shadow;
	}
	par->hw.win = (void __force *)ioremap(0xa0000, WD_WIN_SIZE);
	if (!par->hw.win)
		goto err_shadow;
	if (fb_alloc_cmap(&info->cmap, 256, 0))
		goto err_win;
	wd_init_info(info, par);
	wd_info = info;

	/* Save the text state first; nothing has changed yet. */
	par->vstate.flags = VGA_SAVE_MODE | VGA_SAVE_FONTS | VGA_SAVE_CMAP;
	console_lock();
	wd_save_ext(&par->hw, &par->bios);
	ret = save_vga(&par->vstate);
	wd_relock(&par->hw, &par->bios);
	console_unlock();
	if (ret) {
		ret = -ENOMEM;
		goto err_cmap;
	}

	/*
	 * fbcon takes the consoles over now, drawing into the shadow only, so
	 * vgacon no longer writes the CRTC behind our back. Then the mode set.
	 */
	ret = register_framebuffer(info);
	if (ret)
		goto err_vga;
	par->registered = true;

	console_lock();
	ret = wd_go_live(info);
	console_unlock();
	if (ret) {
		unregister_framebuffer(info);
		par->registered = false;
		goto err_vga;
	}
	wd_apply_output(par);
	if (!platform_driver_register(&wd_pdrv)) {
		wd_pdev = platform_device_register_simple("wd90c24fb", -1, NULL, 0);
		if (IS_ERR(wd_pdev)) {
			wd_pdev = NULL;
			platform_driver_unregister(&wd_pdrv);
		}
	}
	if (!wd_pdev)
		pr_warn("no platform device: suspend/resume will not restore the mode\n");
	fb_info(info, "%s, 640x480x8 via PR0(A) banking, %u KiB shadow%s\n",
		wd_chip_name(par->hw.chip), WD_FB_SIZE / 1024,
		par->accel ? ", BITBLT" : "");
	if (trial) {
		pr_warn("trial: text mode returns in %u s unless: echo 1 > /sys/module/wd90c24fb/parameters/keep\n",
			trial);
		schedule_delayed_work(&par->trial_work, trial * HZ);
	}
	return 0;

err_vga:
	/* frees the saved state; a no-op if wd_restore_text() already ran */
	console_lock();
	restore_vga(&par->vstate);
	console_unlock();
err_cmap:
	fb_dealloc_cmap(&info->cmap);
err_win:
	iounmap((void __iomem __force *)par->hw.win);
err_shadow:
	vfree(par->model);
	vfree(par->shadow);
err_info:
	framebuffer_release(info);
	wd_info = NULL;
	return ret;
}

static void __exit wd90c24fb_exit(void)
{
	struct fb_info *info = wd_info;
	struct wd_par *par = info->par;

	if (wd_pdev) {
		platform_device_unregister(wd_pdev);
		platform_driver_unregister(&wd_pdrv);
		wd_pdev = NULL;
	}
	cancel_work_sync(&wd_resume_fail_work);
	cancel_work_sync(&wd_text_work);
	cancel_delayed_work_sync(&par->trial_work);
	cancel_delayed_work_sync(&par->mmap_work);
	if (par->registered)
		wd_stop(info);
	fb_dealloc_cmap(&info->cmap);
	iounmap((void __iomem __force *)par->hw.win);
	vfree(par->model);
	vfree(par->shadow);
	framebuffer_release(info);
}

module_init(wd90c24fb_init);
module_exit(wd90c24fb_exit);
MODULE_DESCRIPTION("WD90C24/WD90C24A(2) framebuffer for the ThinkPad 755C");
MODULE_LICENSE("GPL");
