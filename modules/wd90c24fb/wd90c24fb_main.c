// SPDX-License-Identifier: GPL-2.0
/*
 * wd90c24fb: native framebuffer for the WD90C24/WD90C24A(2) in the IBM
 * ThinkPad 755C: 640x480, 8 bpp (256 colours), through Paradise banking.
 * See docs/WD90C24.md.
 *
 * The chip has no linear aperture on the 755C, only the 64 KiB window at
 * A0000 offset by PR0(A). fbcon and user space draw into a 300 KiB shadow
 * in system RAM (sys_* helpers); every drawing operation copies its
 * rectangle to video memory, bank by bank. mmap() clients get the shadow,
 * flushed at 10 Hz while the device is open. On the WD90C24A/A2 the BITBLT
 * engine (stage 3) also does fillrect and copyarea (scrolling) in video
 * memory, so those need no copy; it is self-tested at load and dropped on
 * the first timeout.
 *
 * Fail-safe:
 *  - drives only a chip with the WD90C24 signature (force=1 also accepts
 *    other WD90C2x/3x, for the 86Box C30/C31 bench), enable=0 disables;
 *  - the text-mode state (standard registers, fonts, text, palette via
 *    vgastate, Paradise registers via wd90c24_hw.c) is saved before
 *    anything changes and restored on any failure, on rmmod, and when a
 *    trial period ends without confirmation (trial=, keep);
 *  - after the mode set a banking self-test must pass, or the text mode is
 *    restored and the module returns -ENODEV (the .tcz then falls back to
 *    vga16fb);
 *  - blanking only uses the sequencer screen-off bit and is always undone
 *    on restore.
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
#include <video/vga.h>
#include "wd90c24_hw.h"

static bool enable = true;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Drive the WD90C24 (0 = do nothing, keep vga16fb/text)");
static bool probe_only;
module_param(probe_only, bool, 0444);
MODULE_PARM_DESC(probe_only, "Only identify the chip and log its state");
static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "Also drive other WD90C2x/3x (e.g. C30/C31 on the 86Box bench)");
static bool fake;
module_param(fake, bool, 0444);
MODULE_PARM_DESC(fake, "TESTING ONLY: treat any VGA as a WD90C24 to exercise the fail-safe path");
static unsigned int trial;
module_param(trial, uint, 0444);
MODULE_PARM_DESC(trial, "Seconds until text mode is restored unless keep=1 is written (0 = off)");
static bool accel = true;
module_param(accel, bool, 0444);
MODULE_PARM_DESC(accel, "Use the WD90C24A BITBLT engine for fills and scrolling (if its self-test passes)");
static bool keep;
module_param(keep, bool, 0644);
MODULE_PARM_DESC(keep, "Write 1 during the trial period to keep the framebuffer");

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
	bool registered;
	atomic_t users;
	bool mmapped;
	struct delayed_work mmap_work;
	struct delayed_work trial_work;
};

static struct fb_info *wd_info;

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

static void wd_damage(struct fb_info *info, u32 x, u32 y, u32 w, u32 h)
{
	struct wd_par *par = info->par;
	unsigned long flags;
	bool lost = false;
	u32 r;

	/* one row per lock hold: short interrupt-off windows on a 486 */
	for (r = 0; r < h; r++) {
		spin_lock_irqsave(&par->lock, flags);
		if (par->live) {
			if (!wd_blt_idle(par))
				lost = true;
			wd_flush_rect(&par->hw, par->shadow, x, y + r, w, 1);
		}
		spin_unlock_irqrestore(&par->lock, flags);
	}
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

/* Start a blit; false if the CPU must copy the rectangle instead. */
static bool wd_hw_op(struct fb_info *info, bool fill, u32 sx, u32 sy, u32 dx,
		     u32 dy, u32 w, u32 h, u8 color, bool xor)
{
	struct wd_par *par = info->par;
	unsigned long flags;
	bool done = false, lost = false;
	int ret;

	spin_lock_irqsave(&par->lock, flags);
	if (par->live && par->accel) {
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

	if (user && atomic_dec_and_test(&par->users) && par->mmapped) {
		par->mmapped = false;
		wd_damage(info, 0, 0, WD_XRES, WD_YRES);
	}
	return 0;
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

static int wd_blank(int blank, struct fb_info *info)
{
	struct wd_par *par = info->par;

	if (par->live)
		wd_screen_on(&par->hw, blank == FB_BLANK_UNBLANK);
	return 0;
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
}

/* Mode set + self-test. Caller holds the console lock. */
static int wd_go_live(struct fb_info *info)
{
	struct wd_par *par = info->par;
	struct wd_ext_state ext;
	struct wd_std std;

	wd_mode_640x480x8(&par->hw, &par->bios, &std, &ext);
	wd_set_mode(&par->hw, &std, &ext);
	if (wd_selftest_banks(&par->hw, par->hw.vram_kb)) {
		wd_restore_text(par);
		pr_warn("banking self-test failed: text mode restored\n");
		return -ENODEV;
	}
	if (par->hw.ext_blocks && accel) {
		par->accel = !wd_blt_selftest(&par->hw);
		pr_info("BITBLT self-test %s\n", par->accel ? "passed: acceleration on" :
			"failed: CPU drawing only");
	}
	par->live = true;
	fb_set_cmap(&info->cmap, info);
	wd_damage(info, 0, 0, WD_XRES, WD_YRES);
	return 0;
}

static void wd_stop(struct fb_info *info)
{
	struct wd_par *par = info->par;

	console_lock();
	wd_restore_text(par);
	console_unlock();
	if (par->registered) {
		/* fbcon hands the consoles back to vgacon, which redraws */
		unregister_framebuffer(info);
		par->registered = false;
	}
}

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
	if (par->hw.ext_blocks && accel)
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
		pr_warn("fake=1: treating this VGA as a WD90C24 (testing only)\n");
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
	atomic_set(&par->users, 0);
	INIT_DELAYED_WORK(&par->mmap_work, wd_mmap_flush);
	INIT_DELAYED_WORK(&par->trial_work, wd_trial_end);
	ret = -ENOMEM;
	par->shadow = vmalloc_user(PAGE_ALIGN(WD_FB_SIZE));
	if (!par->shadow)
		goto err_info;
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

	cancel_delayed_work_sync(&par->trial_work);
	cancel_delayed_work_sync(&par->mmap_work);
	if (par->registered)
		wd_stop(info);
	fb_dealloc_cmap(&info->cmap);
	iounmap((void __iomem __force *)par->hw.win);
	vfree(par->shadow);
	framebuffer_release(info);
}

module_init(wd90c24fb_init);
module_exit(wd90c24fb_exit);
MODULE_DESCRIPTION("WD90C24/WD90C24A(2) framebuffer for the ThinkPad 755C");
MODULE_LICENSE("GPL");
