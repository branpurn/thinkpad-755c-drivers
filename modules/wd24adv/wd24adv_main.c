// SPDX-License-Identifier: GPL-2.0
/*
 * wd24adv - experimental advanced driver for the WD90C24/WD90C24A(2)
 * (ThinkPad 755C).  DRY-RUN BY DEFAULT.
 *
 * This is a separate module from wd90c24fb.  It refuses to load while
 * wd90c24fb (or vga16fb) is loaded.
 *
 * Default (dryrun=1): no hardware register is touched.  Every plan runs
 * against a model of the 755C boot state and the writes it WOULD perform
 * are listed in /proc/wd24adv/plan.
 *
 * Live (dryrun=0 live=1, both required): the same plans run on the port
 * backend (wd24adv_port.c).  At load, after a read-only entry check, the
 * full text state is saved (Paradise extended registers as wd90c24fb does,
 * standard VGA registers, DAC and all four 64 KiB planes as Linux vgastate
 * does) and restored on unload.  A mode is set only on request (mode= at
 * load or "mode NAME" written to /proc/wd24adv/control) and is reverted to
 * text after trial= seconds (5..600, default 15, cannot be disabled) unless
 * "confirm" is written first.  Every write is read back; a difference stops
 * and restores.  Requires the VGA text console (vgacon) with no
 * framebuffer console and no X server: see README.md "Live test".
 *
 * There is no panel/CRT routing at all: allow_routing=1 and allow_crt=1 are
 * refused at load.  Every write is a standard VGA register or one that
 * wd90c24fb already performs (see wd24adv_core.c).
 *
 *   /proc/wd24adv/plan     - dry-run ordered writes: load, every mode, text
 *   /proc/wd24adv/modes    - mode table, PROVEN/UNPROVEN, evidence
 *   /proc/wd24adv/selftest - shared guard/plan self-test (same as the host)
 *   /proc/wd24adv/live     - live only: log of the last hardware operation
 *   /proc/wd24adv/control  - live only (root, write): mode NAME | text | confirm
 *   /proc/wd24adv/s1       - s1=1 only: the S1 probe plan (dryrun=1) or result
 *   /proc/wd24adv/s2       - s2=1 only: the S2 write list (dryrun=1) or result
 *   /proc/wd24adv/s3       - s3=1 only: the S3 operation list (dryrun=1) or result
 *   /proc/wd24adv/s4       - s4=1 only: the S4 operation list (dryrun=1) or result
 *   /proc/wd24adv/s5       - s5=1|2 only: the S5 host-data list (dryrun=1) or result
 *   /proc/wd24adv/s6       - s6=1 only: the S6 dual-window proof (dryrun=1: run on the model) or result
 *   /proc/wd24adv/s6b      - s6b=1 only: S6b session log (/dev/wd24s6b, dual-window mmap + copy)
 *   /proc/wd24adv/live     - live log; read uses trylock
 *   /proc/wd24adv/s6d      - s6d=1 only: fault/switch counters (trylock)
 *
 * S6d: s6d=1 arms after 640x480-8.
 *   s6d_fbcon=0 — do not register_framebuffer (bars stay; no fbdev node).
 *   s6d_fbcon=1 s6d_bind=1 (defaults) — register and allow fbcon open.
 *   s6d_fbcon=1 s6d_bind=0 — register a real fb, but fb_open(info, 0)
 *     returns -EBUSY so fbcon_startup fails and the VT is not taken over.
 *     This is not skip_vt_switch and not DEFERRED_TAKEOVER (that needs FB=y).
 *   s6d_cmap=0 (default) — no fb_setcmap. FBIOGETCMAP stays -EINVAL.
 *   s6d_cmap=1 — refused unless s6d=1 s6d_fbcon=1 s6d_bind=0 dryrun=0
 *     live=1 mode=640x480-8 and no other stage. Userspace open reads the
 *     DAC with the vgastate sequence and publishes it only when it
 *     matches the load snapshot. FBIOPUTCMAP then writes 3C8/3C9 and
 *     returns 0 only after a 3C9 readback. fbcon cannot call it: bind
 *     is 0, and the hook also refuses while busy is 0.
 * pattern is a bool (1=bars, 0=skip), not a pattern id.
 * Do not printk while holding hw_lock unless the fb draw path can nest:
 * after fbcon binds, printk on console=tty0 re-enters imageblit.
 *
 * S1 (s1=1 dryrun=0, never with live=1 or mode=): after the same read-only
 * entry check, a read-only probe of the 23C0h register blocks
 * (wd24adv_s1.c: its only writes are 23C0h selects and one restore).  It
 * runs once during load; the module then stays loaded holding only the
 * result, with no port region claimed, and rmmod touches nothing.
 *
 * S2 (s2=1 dryrun=0, never with s1=1, live=1 or mode=): after the same
 * entry check, write + readback of the closed list in wd24adv_s2.h (block
 * 1 without CNTRL1, block 2 without the cursor control), each value put
 * back to its snapshot (read twice at load) and verified, 23C0h back to 1002h, all inside
 * this insmod.  Same load/unload behaviour as S1.
 *
 * S3 (s3=1 with dryrun=0 live=1 mode=640x480-8 only): right after that
 * PROVEN trial mode set (trial timer already queued), three BITBLT
 * operations in the off-screen scratch bank 60000h-6FFFFh, each verified
 * against a model by reading video memory back (wd24adv_s3.h).  Once,
 * inside insmod; the trial expiry (or rmmod) restores the text state.
 *
 * S4 (s4=1, same conditions as s3=1, never with s1/s2/s3): the same
 * sequence, guard, stop rules and timeout lock as S3 with the S4 program
 * (wd24adv_s3.c wd24s4_ops): ON SCREEN in bank 1 (frame rows 110-203) -
 * seven 64x16 fills, one per ROP X uses (copy, xor, and, or, invert,
 * clear, set), and four overlapping copies (two forward, two backward,
 * direction chosen in the table as dst > src -> BACK), each verified
 * against the model; banks 0 and 2-6 must not change.  The picture stays
 * until the trial expiry restores text (no confirm in the S4 sequence).
 */
#define pr_fmt(fmt) "wd24adv: " fmt

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/rculist.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/console.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>

#include "wd24adv_core.h"
#include "wd24adv_s2.h"
#include "wd24adv_s3.h"
#include "wd24adv_s1.h"
#include "wd24adv_s6.h"
#include "wd24adv_s6b.h"
#include "wd24adv_s6d.h"
#include "wd24adv_ioctl.h"
#include "wd24adv_dac_entry.h"
#include <linux/fb.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <asm/video.h>

static bool dryrun = true;
module_param(dryrun, bool, 0444);
MODULE_PARM_DESC(dryrun, "record register writes only (default 1); 0 needs live=1");

static bool live;
module_param(live, bool, 0444);
MODULE_PARM_DESC(live, "with dryrun=0: write the hardware (save at load, restore on unload)");

static char *mode = "";
module_param(mode, charp, 0444);
MODULE_PARM_DESC(mode, "live: set this PROVEN mode right after load (trial applies)");

static bool pattern = true;
module_param(pattern, bool, 0444);
MODULE_PARM_DESC(pattern, "live: bool — 1 (default) draw 16 roll-up colour bars after packed/320x200 mode set; 0 skip bars (NOT a pattern-id). With s6d=1+fbcon, bars are brief then wiped");

static bool allow_routing;
module_param(allow_routing, bool, 0444);
MODULE_PARM_DESC(allow_routing, "REMOVED: runtime panel/CRT routing does not exist; 1 is refused");

static bool allow_crt;
module_param(allow_crt, bool, 0444);
MODULE_PARM_DESC(allow_crt, "REMOVED: no CRT modes; 1 is refused");

static bool allow_clock;
module_param(allow_clock, bool, 0444);
MODULE_PARM_DESC(allow_clock, "permit PR68 b4:3 / PR69 clock writes; no table mode needs them (default 0)");

static bool fix_sr06;
module_param(fix_sr06, bool, 0444);
MODULE_PARM_DESC(fix_sr06, "load: if 3C4 width unlocked and SR06 data==0x0F, write SR06=0x0F once (S6 L relock) then re-check; else refuse as usual (default 0)");

static unsigned int trial = 15;
module_param(trial, uint, 0444);
MODULE_PARM_DESC(trial, "live: revert a mode to text after N s unless confirmed (5..600, default 15; 0 refused)");

static char *also_conflict = "";
module_param(also_conflict, charp, 0444);
MODULE_PARM_DESC(also_conflict, "test aid: also refuse to load if this module is loaded (wd90c24fb is always checked)");

static bool s1;
module_param(s1, bool, 0444);
MODULE_PARM_DESC(s1, "S1 read-only 23C0h probe: dryrun=1 lists the plan, dryrun=0 runs it once at load (not with live=1)");

static bool s1_echo = true;
module_param(s1_echo, bool, 0444);
MODULE_PARM_DESC(s1_echo, "S1: stop if 23C2h bits 15:12 do not echo the index (default 1; 0 = record only)");

static bool s2;
module_param(s2, bool, 0444);
MODULE_PARM_DESC(s2, "S2 write+readback of block 1/2 (start bit and cursor never set; all restored inside insmod): dryrun=1 lists, dryrun=0 runs once");

static bool s3;
module_param(s3, bool, 0444);
MODULE_PARM_DESC(s3, "S3 off-screen BITBLT fill/copy at 60000h after the 640x480-8 trial mode set: dryrun=1 lists, dryrun=0 live=1 mode=640x480-8 runs once");

static bool s4;
module_param(s4, bool, 0444);
MODULE_PARM_DESC(s4, "S4 ON-SCREEN BITBLT fills (all 7 X ROPs) and overlapping fwd/back copies in frame bank 1 after the 640x480-8 trial mode set: dryrun=1 lists, dryrun=0 live=1 mode=640x480-8 runs once");

static int s5;
module_param(s5, int, 0444);
MODULE_PARM_DESC(s5, "S5 ON-SCREEN host-data BITBLT (23C4h) in frame bank 1 after the 640x480-8 trial mode set: 1 = MONO colour expansion (opaque + transparent), 2 = HOSTCOL (UNPROVEN); dryrun=1 lists, dryrun=0 live=1 mode=640x480-8 runs once");

static bool s6;
module_param(s6, bool, 0444);
MODULE_PARM_DESC(s6, "S6 dual window: PR0B + PR1 b3 (split) + PR31 b7 (read PR0A / write PR0B) readback-restore and data test in off-screen 50000h/70000h after the 640x480-8 trial mode set: dryrun=1 runs it on the model, dryrun=0 live=1 mode=640x480-8 runs once");

/* s3=1, s4=1 or s5=1|2: the program run by s3_live() */
static const struct wd24s3_prog *sprog = &wd24s3_prog3;

#define TXT_MAX	(160 * 1024)

#define TRIAL_MIN	5
#define TRIAL_MAX	600
#define VGA_PORTS	0x3c0
#define VGA_NPORTS	0x20

static struct proc_dir_entry *pdir;
static struct wd24_ctx *ctx;
static char *plan_txt, *modes_txt, *self_txt;
static size_t plan_len, modes_len, self_len;

/* live state; every hardware operation runs under hw_lock.
 * VGA/fbcon code takes console_lock and then this mutex. Do not take
 * console_lock while already holding hw_lock from a path fbcon can re-enter
 * (fault is the exception that is ordered console_lock, then hw_lock).
 * hw_task lets fb draw nest when printk fires on the holder: the mutex
 * itself is not recursive. */
static DEFINE_MUTEX(hw_lock);
static struct task_struct *hw_task;
static int s6d_in_draw;

static void hw_acq(void)
{
	mutex_lock(&hw_lock);
	hw_task = current;
}

static void hw_rel(void)
{
	hw_task = NULL;
	mutex_unlock(&hw_lock);
}

static int hw_try(void)
{
	if (!mutex_trylock(&hw_lock))
		return 0;
	hw_task = current;
	return 1;
}

static bool hw_nested(void)
{
	return hw_task == current;
}

/* Same-task reentry (printk into fbcon while this task holds hw_lock)
 * must not draw and must not lock again. The mutex is not recursive.
 * A nested draw used to run whenever the outer section was not itself
 * a draw; that touched the aperture mid-critical-section. */
static bool s6d_registering;
static bool wd24_exiting;
static struct task_struct *s6d_ender;

static void s6d_yield_ender(void)
{
	while (s6d_ender && s6d_ender != current) {
		hw_rel();
		schedule_timeout_uninterruptible(1);
		hw_acq();
	}
}

static struct wd24_ctx *hw;
static u8 *hw_planes;
static void __iomem *vmem;
static bool have_ports;
static bool hw_dead;	/* a live load failed after writing: restored, inert */
/* sticky: an S3 BITBLT timed out and was not seen idle afterwards; no
 * video memory access of any kind, module pinned (rmmod refused) */
static bool vram_dead;
static bool s3_pinned;
static struct wd24s3 *s3st;
static int vram_gate(const char *what);
static struct wd24_io hw_io;
static char *live_txt;
static size_t live_len;
static struct wd24s1 *s1st;
static char *s1_txt;
static size_t s1_len;
static struct wd24s2 *s2st;
static char *s2_txt;
static size_t s2_len;
static char *s3_txt;
static size_t s3_len;
static u8 *s3_mem;	/* 3 x 64 KiB: pre, model, read buffer */
static void trial_fn(struct work_struct *w);
static DECLARE_DELAYED_WORK(trial_work, trial_fn);

static bool other_module_loaded(const char *name)
{
	struct list_head *p;
	bool found = false;

	rcu_read_lock();
	list_for_each_rcu(p, &THIS_MODULE->list) {
		struct module *m;

		if (!is_vmalloc_addr(p))
			continue;	/* the global 'modules' head */
		m = list_entry(p, struct module, list);
		if (m->state == MODULE_STATE_UNFORMED)
			continue;
		if (!strcmp(m->name, name)) {
			found = true;
			break;
		}
	}
	rcu_read_unlock();
	return found;
}

static ssize_t rd(char __user *buf, size_t n, loff_t *pos, const char *src,
		  size_t len)
{
	loff_t p = *pos;

	if (p < 0)
		return -EINVAL;
	if (!src || p >= len || !n)
		return 0;
	n = min_t(size_t, n, len - p);
	if (copy_to_user(buf, src + p, n))
		return -EFAULT;
	*pos = p + n;
	return n;
}

static ssize_t plan_read(struct file *f, char __user *b, size_t n, loff_t *p)
{
	return rd(b, n, p, plan_txt, plan_len);
}

static ssize_t modes_read(struct file *f, char __user *b, size_t n, loff_t *p)
{
	return rd(b, n, p, modes_txt, modes_len);
}

static ssize_t self_read(struct file *f, char __user *b, size_t n, loff_t *p)
{
	return rd(b, n, p, self_txt, self_len);
}

static const struct proc_ops plan_ops = {
	.proc_read = plan_read, .proc_lseek = default_llseek };
static const struct proc_ops modes_ops = {
	.proc_read = modes_read, .proc_lseek = default_llseek };
static const struct proc_ops self_ops = {
	.proc_read = self_read, .proc_lseek = default_llseek };

static ssize_t live_read(struct file *f, char __user *b, size_t n, loff_t *p)
{
	/* Non-blocking: a draw or mode set may hold hw_lock. A blocked
	 * proc read is not what stopped the 2026-11-05 insmod (that task
	 * never returned to the shell). trylock still keeps a later read
	 * from sleeping behind a long paint. */
	static const char busy[] =
		"wd24adv: hw busy (draw/mode holds lock); retry\n";
	ssize_t r;

	if (!hw_try())
		return rd(b, n, p, busy, sizeof(busy) - 1);
	r = rd(b, n, p, live_txt, live_len);
	hw_rel();
	return r;
}

static ssize_t control_write(struct file *f, const char __user *b, size_t n,
			     loff_t *p);

static ssize_t s1_read(struct file *f, char __user *b, size_t n, loff_t *p)
{
	return rd(b, n, p, s1_txt, s1_len);
}

static const struct proc_ops s1_ops = {
	.proc_read = s1_read, .proc_lseek = default_llseek };

static ssize_t s2_read(struct file *f, char __user *b, size_t n, loff_t *p)
{
	return rd(b, n, p, s2_txt, s2_len);
}

static const struct proc_ops s2_ops = {
	.proc_read = s2_read, .proc_lseek = default_llseek };

static ssize_t s3_read(struct file *f, char __user *b, size_t n, loff_t *p)
{
	return rd(b, n, p, s3_txt, s3_len);
}

static const struct proc_ops s3_ops = {
	.proc_read = s3_read, .proc_lseek = default_llseek };

static bool s6c;
module_param(s6c, bool, 0444);
MODULE_PARM_DESC(s6c, "with s6=1: S6c, set PR31 b7, relock SR06 and run the dual-window copy test while LOCKED, then re-unlock, PR31 back to BIOS, relock");

static bool s6b;
module_param(s6b, bool, 0444);
MODULE_PARM_DESC(s6b, "S6b: /dev/wd24s6b explicit dual-window mmap (A0000 half = PR0B, A8000 half = PR0A via PR1 b3) + kernel dual-window copy (PR31 b7, SR06 locked); dryrun=1 runs a model session, dryrun=0 live=1 mode=640x480-8 opens the device");

static bool s6d;
module_param(s6d, bool, 0444);
MODULE_PARM_DESC(s6d, "S6d: fault-banked fbdev (640x480-8) using S6b dual-window + thrash latch; dryrun=0 live=1 mode=640x480-8; s6d_fbcon=0 skips register; s6d_fbcon=1 registers (s6d_bind selects fbcon open)");
static unsigned int s6d_thrash_bound = WD24S6D_THRASH_BOUND_DEFAULT;
module_param(s6d_thrash_bound, uint, 0444);
MODULE_PARM_DESC(s6d_thrash_bound, "S6d: consecutive window-switch bound before thrash latch (default 32)");

static bool s6d_fbcon = true;
module_param(s6d_fbcon, bool, 0444);
MODULE_PARM_DESC(s6d_fbcon, "S6d: 1 (default) call register_framebuffer; 0 arm dual-window only — no fbdev node, pattern stays. Does not by itself suppress fbcon when 1; see s6d_bind");

static bool s6d_bind = true;
module_param(s6d_bind, bool, 0444);
MODULE_PARM_DESC(s6d_bind, "S6d with s6d_fbcon=1: 1 (default) allow fb_open(info,0) so fbcon can bind; 0 return -EBUSY from that open so fbcon_startup fails and the VT stays put. Ignored when s6d_fbcon=0. Not skip_vt_switch. Requires /proc/fb empty (no fbcon driver already registered)");

static bool s6d_cmap;
module_param(s6d_cmap, bool, 0444);
MODULE_PARM_DESC(s6d_cmap, "S6d RAMDAC fb_setcmap, default 0 (no hook, baseline palette). 1 requires s6d=1 s6d_fbcon=1 s6d_bind=0 dryrun=0 live=1 mode=640x480-8 and no other stage. No colour write at register, in text, or while frozen");

static struct wd24s6 *s6st;
static u8 *s6_save;

static ssize_t s6_read(struct file *f, char __user *b, size_t n, loff_t *p)
{
	return s6st ? rd(b, n, p, s6st->log, s6st->loglen) : 0;
}

static const struct proc_ops s6_ops = {
	.proc_read = s6_read, .proc_lseek = default_llseek };

static const struct proc_ops live_ops = {
	.proc_read = live_read, .proc_lseek = default_llseek };
static const struct proc_ops control_ops = {
	.proc_write = control_write, .proc_lseek = default_llseek };

#define APPEND(buf, len, ...) \
	((len) += scnprintf((buf) + (len), TXT_MAX - (len), __VA_ARGS__))

static size_t format_events(char *buf, size_t len, const struct wd24_ctx *c,
			    unsigned int from)
{
	unsigned int i;
	char old[4];

	for (i = from; i < c->nev; i++) {
		const struct wd24_event *e = &c->ev[i];

		if (e->old_known)
			scnprintf(old, sizeof(old), "%02x", e->old);
		else
			scnprintf(old, sizeof(old), "--");
		switch (e->type) {
		case WD24_EV_WRITE:
			APPEND(buf, len, "  W %-4s %02x: %s -> %02x  [%s]\n",
			       wd24_rf_name(e->rf), e->idx, old, e->val,
			       e->note ? e->note : "?");
			break;
		case WD24_EV_REFUSED:
			APPEND(buf, len, "  REFUSED %-4s %02x: %s -> %02x (%d)\n",
			       wd24_rf_name(e->rf), e->idx, old, e->val, e->err);
			break;
		case WD24_EV_NOTE:
			APPEND(buf, len, "  # %s\n", e->note);
			break;
		case WD24_EV_CHECK:
			APPEND(buf, len, "  CHECK FAILED %-4s %02x: read %02x, expected %02x: %s\n",
			       wd24_rf_name(e->rf), e->idx, e->val, e->old,
			       e->note ? e->note : "");
			break;
		case WD24_EV_MISMATCH:
			APPEND(buf, len, "  MISMATCH %-4s %02x: wrote %02x, read %02x: %s\n",
			       wd24_rf_name(e->rf), e->idx, e->old, e->val,
			       e->note ? e->note : "");
			break;
		case WD24_EV_BULK:
			APPEND(buf, len, "  M %s\n", e->note ? e->note : "");
			break;
		}
	}
	if (c->dropped)
		APPEND(buf, len, "  (%u events dropped)\n", c->dropped);
	return len;
}

static void build_modes(void)
{
	const char *bad, *why;
	unsigned int i;
	size_t len = 0;
	int err = wd24_modes_validate(&bad, &why);

	APPEND(modes_txt, len, "validation: %s%s%s%s\n", err ? "FAIL " : "OK",
	       err ? bad : "", err ? ": " : "", err ? why : "");
	APPEND(modes_txt, len, "panel: PR19 must read %02x (LCD only, centring on, no expansion), PR18 %02x\n",
	       WD24_LIVE_PR19, WD24_LIVE_PR18);
	APPEND(modes_txt, len, "%-16s %5s %5s %5s %5s %3s bios %-8s flags\n",
	       "name", "xres", "yres", "vxres", "vyres", "bpp", "status");
	for (i = 0; i < wd24_nmodes; i++) {
		const struct wd24_mode *m = &wd24_modes[i];

		APPEND(modes_txt, len, "%-16s %5u %5u %5u %5u %3u  %02x  %-8s %04x\n    %s\n",
		       m->name, m->xres, m->yres, m->vxres, m->vyres, m->bpp,
		       m->bios_mode,
		       m->status == WD24_PROVEN ? "PROVEN" : "UNPROVEN",
		       m->flags, m->evidence);
	}
	modes_len = len;
}

struct st_out {
	size_t len;
};

static void st_report(void *arg, bool ok, const char *what)
{
	struct st_out *o = arg;

	APPEND(self_txt, o->len, "%s %s\n", ok ? "PASS" : "FAIL", what);
}

static int run_selftest(struct wd24_ctx *t)
{
	struct st_out o = { 0 };
	int fail = wd24_selftest(t, st_report, &o);

	APPEND(self_txt, o.len, "%d failure(s)\n", fail);
	self_len = o.len;
	return fail;
}

static void model_header(size_t *len)
{
	unsigned int i, n = 0;

	APPEND(plan_txt, *len, "boot model (L = live 755C read, P = POST trace, S = std VGA 03h):\n");
	for (i = 0; i < wd24_boot_model_n; i++) {
		const struct wd24_model_reg *r = &wd24_boot_model[i];

		if (r->src == WD24_SRC_STD)
			continue;
		APPEND(plan_txt, *len, "%s %s%02x=%02x", n++ % 6 ? "" : "\n ",
		       wd24_rf_name(r->rf), r->idx, r->val);
		APPEND(plan_txt, *len, "%c ", r->src);
	}
	APPEND(plan_txt, *len, "\n  (+ standard VGA mode 03h values)\n");
}

static void build_plan(void)
{
	size_t len = 0;
	unsigned int i, from;
	int err;

	APPEND(plan_txt, len, "wd24adv dry-run plan (no hardware register was written)\n");
	APPEND(plan_txt, len, "allow_clock=%d fix_sr06=%d; [tag] = the wd90c24fb code the write mirrors\n",
	       allow_clock, fix_sr06);
	model_header(&len);

	for (i = 0; i < wd24_nmodes; i++) {
		const struct wd24_mode *m = &wd24_modes[i];

		wd24_ctx_init(ctx, &wd24_dryrun_backend);
		wd24_model_boot(ctx);
		ctx->allow_clock = allow_clock;
		ctx->fix_sr06 = fix_sr06;
		err = wd24_plan_load(ctx);
		if (i == 0) {
			APPEND(plan_txt, len, "\n== load (writes=%u) ==\n", ctx->nwrites);
			len = format_events(plan_txt, len, ctx, 0);
			APPEND(plan_txt, len, "result: %d\n", err);
		}
		if (err)
			break;
		from = ctx->nev;
		ctx->nwrites = 0;
		err = wd24_plan_mode(ctx, m);
		APPEND(plan_txt, len, "\n== %s [%s] writes=%u ==\n", m->name,
		       m->status == WD24_PROVEN ? "PROVEN" : "UNPROVEN",
		       ctx->nwrites);
		len = format_events(plan_txt, len, ctx, from);
		APPEND(plan_txt, len, "result: %d\n", err);
		if (err || i)
			continue;
		from = ctx->nev;
		ctx->nwrites = 0;
		err = wd24_plan_text(ctx);
		APPEND(plan_txt, len, "\n== text restore after %s (writes=%u) ==\n",
		       m->name, ctx->nwrites);
		len = format_events(plan_txt, len, ctx, from);
		APPEND(plan_txt, len, "result: %d\n", err);
	}
	if (len >= TXT_MAX - 1)
		pr_warn("plan text truncated\n");
	plan_len = len;
}

/* ---- live backend ------------------------------------------------------- */

static u8 k_inb(void *priv, u16 port)
{
	return inb(port);
}

static void k_outb(void *priv, u8 val, u16 port)
{
	outb(val, port);
}

static u8 k_memr(void *priv, u32 off)
{
	return readb(vmem + (off & (WD24_PLANE - 1)));
}

static void k_memw(void *priv, u32 off, u8 val)
{
	writeb(val, vmem + (off & (WD24_PLANE - 1)));
}

/* one hardware operation: caller holds hw_lock */
enum hw_op { OP_LOAD, OP_MODE, OP_TEXT, OP_EXPIRE };
static const char *const op_name[] = { "load", "mode", "text", "trial expiry" };

static void s6b_stop(const char *why);
static void s6b_free(void);
static void s6d_stop(const char *why);
static void s6d_free(void);

static int hw_run(enum hw_op op, const struct wd24_mode *m)
{
	unsigned long d0 = hw->hw_data, i0 = hw->hw_index;
	unsigned int mis0 = hw->nmismatch, i;
	size_t len = 0;
	int err;

	s6d_yield_ender();
	if (op != OP_LOAD) {
		s6b_stop(op_name[op]);	/* S6b/S6d end before any mode change */
		s6d_stop(op_name[op]);
	}
	wd24_log_reset(hw);
	console_lock();
	hw->be->begin(hw);
	switch (op) {
	case OP_LOAD:
		err = wd24_plan_load(hw);
		break;
	case OP_MODE:
		err = wd24_plan_mode(hw, m);
		if (!err && pattern && wd24_has_pattern(m))
			err = wd24_pattern(hw, m);
		break;
	case OP_TEXT:
		err = wd24_plan_text(hw);
		break;
	default:
		err = wd24_trial_expire(hw);
		break;
	}
	hw->be->end(hw);
	console_unlock();

	APPEND(live_txt, len, "wd24adv live: %s%s%s -> %d\n", op_name[op],
	       m ? " " : "", m ? m->name : "", err);
	APPEND(live_txt, len, "port writes: data %lu, index %lu; readback mismatches %u; in_text %d; trial %s\n",
	       hw->hw_data - d0, hw->hw_index - i0, hw->nmismatch - mis0,
	       hw->in_text, hw->trial_pending ? "PENDING" : hw->confirmed ? "confirmed" : "-");
	live_len = format_events(live_txt, len, hw, 0);

	pr_info("live %s%s%s: %d (port data writes %lu, index writes %lu, mismatches %u)\n",
		op_name[op], m ? " " : "", m ? m->name : "", err,
		hw->hw_data - d0, hw->hw_index - i0, hw->nmismatch - mis0);
	for (i = 0; i < hw->nev; i++) {
		const struct wd24_event *e = &hw->ev[i];

		if (e->type == WD24_EV_CHECK)
			pr_err("entry check: %s%02x read %02x expected %02x: %s\n",
			       wd24_rf_name(e->rf), e->idx, e->val, e->old, e->note);
		else if (e->type == WD24_EV_MISMATCH)
			pr_err("readback: %s%02x wrote %02x read %02x: %s\n",
			       wd24_rf_name(e->rf), e->idx, e->old, e->val, e->note);
		else if (e->type == WD24_EV_REFUSED)
			pr_err("guard refused %s%02x=%02x (%d)\n",
			       wd24_rf_name(e->rf), e->idx, e->val, e->err);
	}
	return err;
}

static int hw_mode(const struct wd24_mode *m)
{
	int err;

	if (!m || m->status != WD24_PROVEN)
		return -EINVAL;
	if (!hw->in_text || delayed_work_pending(&trial_work))
		return -EBUSY;	/* text first; one trial timer at a time */
	if (hw->nmismatch)
		return -EIO;	/* after any readback mismatch: text/rmmod only */
	if (vram_gate("mode set"))
		return -EIO;
	err = hw_run(OP_MODE, m);
	if (!err) {
		queue_delayed_work_on(WORK_CPU_UNBOUND, system_percpu_wq,
				      &trial_work, (unsigned long)trial * HZ);
		pr_info("%s set: write \"confirm\" to /proc/wd24adv/control within %u s or it reverts to text\n",
			m->name, trial);
	}
	return err;
}

static void trial_fn(struct work_struct *w)
{
	hw_acq();
	/* register_framebuffer may have dropped hw_lock. Do not tear the
	 * session down under it; run this expiry once registration returns. */
	if (s6d_registering && !READ_ONCE(wd24_exiting)) {
		hw_rel();
		queue_delayed_work_on(WORK_CPU_UNBOUND, system_percpu_wq,
				      &trial_work, HZ);
		return;
	}
	s6d_yield_ender();
	if (hw && hw->trial_pending) {
		vram_gate("trial expiry: plane restore");	/* sets no_vram if locked */
		hw_run(OP_EXPIRE, NULL);
	}
	hw_rel();
}

static ssize_t control_write(struct file *f, const char __user *b, size_t n,
			     loff_t *p)
{
	char cmd[48];
	size_t k = min_t(size_t, n, sizeof(cmd) - 1);
	int err;

	if (copy_from_user(cmd, b, k))
		return -EFAULT;
	cmd[k] = 0;
	while (k && (cmd[k - 1] == '\n' || cmd[k - 1] == ' '))
		cmd[--k] = 0;
	/* trylock: do not sleep behind a draw that already holds hw_lock */
	if (!hw_try())
		return -EAGAIN;
	if (!hw)
		err = -ENODEV;
	else if (hw_dead)
		err = -EIO;
	else if (vram_gate("control"))
		err = -EIO;	/* every command, sticky */
	else if (!strcmp(cmd, "confirm"))
		err = wd24_confirm(hw);
	else if (!strcmp(cmd, "text"))
		err = hw_run(OP_TEXT, NULL);
	else if (!strncmp(cmd, "mode ", 5))
		err = hw_mode(wd24_mode_find(cmd + 5));
	else
		err = -EINVAL;
	hw_rel();
	if (!strcmp(cmd, "confirm") && !err)
		pr_info("mode confirmed: kept until \"text\" or rmmod\n");
	return err ? err : n;
}

static void free_hw(void)
{
	if (vmem)
		iounmap(vmem);
	vmem = NULL;
	if (have_ports)
		release_region(VGA_PORTS, VGA_NPORTS);
	have_ports = false;
	vfree(hw_planes);
	vfree(hw);
	vfree(live_txt);
	hw_planes = NULL;
	hw = NULL;
	live_txt = NULL;
}

static void free_all(void)
{
	s6b_free();
	s6d_free();
	if (pdir)
		proc_remove(pdir);
	pdir = NULL;
	vfree(plan_txt);
	vfree(modes_txt);
	vfree(self_txt);
	vfree(ctx);
	vfree(s1st);
	vfree(s1_txt);
	s1st = NULL;
	s1_txt = NULL;
	vfree(s2st);
	vfree(s2_txt);
	s2st = NULL;
	s2_txt = NULL;
	vfree(s6st);
	vfree(s6_save);
	s6st = NULL;
	s6_save = NULL;
	vfree(s3st);
	vfree(s3_txt);
	vfree(s3_mem);
	s3st = NULL;
	s3_txt = NULL;
	s3_mem = NULL;
	plan_txt = modes_txt = self_txt = NULL;
	ctx = NULL;
	free_hw();
}

/* ---- S1: read-only 23C0h probe ----------------------------------------- */

/* the entry check reads no memory; these make any access harmless */
static u8 s1_memr(void *priv, u32 off)
{
	return 0xff;
}

static void s1_memw(void *priv, u32 off, u8 val)
{
}

static void s1_plan_text(void)
{
	size_t len = 0;
	unsigned int p, k;

	APPEND(s1_txt, len, "wd24adv S1 plan (dryrun=1: no port was read or written)\n");
	APPEND(s1_txt, len, "1. entry check (as live=1): VGA index-register reads, index writes only, no data write\n");
	APPEND(s1_txt, len, "2. read 23C0h twice (S), 3C0h, 3C1h; refuse with no write unless stable, != FFFF, b15:14 = 0, != 3C1h:3C0h\n");
	APPEND(s1_txt, len, "3. passes (each select: W16 23C0h; R16 23C0h == select; R16 23C2h, echo %s, block 1 idx 0 ACT = 0):\n",
	       s1_echo ? "checked" : "recorded only");
	for (p = 0; p < WD24S1_NPASS; p++) {
		APPEND(s1_txt, len, "   pass %u:", p + 1);
		for (k = 0; k < WD24S1_NSEL; k++)
			APPEND(s1_txt, len, " %04x", wd24s1_sel(k));
		APPEND(s1_txt, len, "\n");
	}
	APPEND(s1_txt, len, "4. compare passes; W16 23C0h = S & 1FFF; R16 23C0h == S\n");
	APPEND(s1_txt, len, "writes: 23C0h %u (on a stop: selects so far + 1), 23C2h 0, 23C4h 0 (never accessed)\n",
	       WD24S1_MAX_WRITES);
	s1_len = len;
}

static void s1_report(int entry_err, unsigned long ewr_i, unsigned long ewr_d)
{
	const struct wd24s1 *s = s1st;
	size_t len = 0;
	unsigned int p, k, rst = s->restored ? 1 : 0, sel = s->writes - rst;

	APPEND(s1_txt, len, "wd24adv S1 result: %s%s%s\n", wd24s1_res_name(s->res),
	       s->why ? ": " : "", s->why ? s->why : "");
#ifdef WD24S1_QEMU_FAKE
	APPEND(s1_txt, len, "*** QEMU FAKE BUILD: 23C0h/23C2h values are modelled; entry check result ignored ***\n");
#endif
	APPEND(s1_txt, len, "entry check: %s (%d); VGA index writes %lu, data writes %lu\n",
	       entry_err ? "REFUSED" : "passed", entry_err, ewr_i, ewr_d);
	len = format_events(s1_txt, len, hw, 0);
	if (s->res == WD24S1_NOT_RUN) {
		s1_len = len;
		return;
	}
	if (s->nlog >= 2)
		APPEND(s1_txt, len, "23C0h at entry read %04x, %04x; 3C1h:3C0h = %04x; strict echo %d\n",
		       s->log[0].val, s->log[1].val, s->alias, s->strict_echo);
	for (p = 0; p < WD24S1_NPASS; p++) {
		APPEND(s1_txt, len, "pass %u (%u selects read):\n", p + 1, s->done[p]);
		for (k = 0; k < s->done[p]; k++)
			APPEND(s1_txt, len, "  blk %u idx %X: 23C0h %04x 23C2h %04x\n",
			       wd24s1_sel(k) & 0xff, (wd24s1_sel(k) >> 8) & 0xf,
			       s->ix[p][k], s->val[p][k]);
	}
	if (s->stop_pass >= 0 || s->stop_sel >= 0)
		APPEND(s1_txt, len, "stopped at pass %d select %d\n", s->stop_pass + 1, s->stop_sel);
	if (s->restored)
		APPEND(s1_txt, len, "restore: wrote %04x, read %04x (%s)\n", s->saved & 0x1fff,
		       s->restore_rb, s->restore_rb == s->saved ? "equal" : "DIFFERENT");
	APPEND(s1_txt, len, "23C0h writes %u (selects %u + restore %u), 23C2h/23C4h writes 0, guard refusals %u, reads %u\n",
	       s->writes, sel, rst, s->refused, s->reads);
	APPEND(s1_txt, len, "access log (in order):\n");
	for (k = 0; k < s->nlog; k++)
		APPEND(s1_txt, len, "  %c%u %04x %04x\n", s->log[k].w ? 'W' : 'R',
		       s->log[k].width, s->log[k].port, s->log[k].val);
	s1_len = len;
}

static int s1_live(void)
{
	unsigned long ewr_i, ewr_d;
	int entry_err;

	hw = vmalloc(sizeof(*hw));
	s1st = vmalloc(sizeof(*s1st));
	s1_txt = vmalloc(TXT_MAX);
	if (!hw || !s1st || !s1_txt)
		return -ENOMEM;
	if (!request_region(VGA_PORTS, VGA_NPORTS, "wd24adv")) {
		pr_err("s1: VGA ports 3C0-3DF busy; nothing touched\n");
		return -EBUSY;
	}
	if (!request_region(WD24S1_P_INDEX, 4, "wd24adv s1")) {
		release_region(VGA_PORTS, VGA_NPORTS);
		pr_err("s1: ports 23C0-23C3 busy; nothing touched\n");
		return -EBUSY;
	}
	wd24_ctx_init(hw, &wd24_port_backend);
	hw_io = (struct wd24_io){ k_inb, k_outb, s1_memr, s1_memw, NULL };
	hw->io = &hw_io;
	hw->fix_sr06 = fix_sr06;
	wd24s1_init(s1st, &wd24s1_kernel_io);
	s1st->strict_echo = s1_echo;

	hw_acq();
	console_lock();
	hw->be->begin(hw);
	entry_err = wd24_entry_check(hw, WD24_PH_LOAD);
	hw->be->end(hw);
	ewr_i = hw->hw_index;
	ewr_d = hw->hw_data;
#ifdef WD24S1_QEMU_FAKE
	if (entry_err)
		wd24_note(hw, "QEMU FAKE BUILD: entry check failure ignored");
	if (!ewr_d)
		wd24s1_run(s1st);
#else
	if (!entry_err && !ewr_d)
		wd24s1_run(s1st);
#endif
	console_unlock();
	hw_rel();
	release_region(WD24S1_P_INDEX, 4);
	release_region(VGA_PORTS, VGA_NPORTS);

	s1_report(entry_err, ewr_i, ewr_d);
	pr_info("S1: %s%s%s; entry check %d (index writes %lu, data %lu); 23C0h writes %u, reads %u\n",
		wd24s1_res_name(s1st->res), s1st->why ? ": " : "",
		s1st->why ? s1st->why : "", entry_err, ewr_i, ewr_d,
		s1st->writes, s1st->reads);
	if (s1st->restored)
		pr_info("S1: S = %04x, restore read back %04x; details: /proc/wd24adv/s1\n",
			s1st->saved, s1st->restore_rb);
	vfree(hw);
	hw = NULL;	/* rmmod: nothing to restore, no port touched */
	return 0;
}

/* ---- S2: write + readback, start bit never set ------------------------- */

static void s2_plan_text(void)
{
	size_t len = 0;
	unsigned int j;

	APPEND(s2_txt, len, "wd24adv S2 plan (dryrun=1: no port was read or written)\n");
	APPEND(s2_txt, len, "1. entry check (as live=1): VGA index-register reads, index writes only, no data write\n");
	APPEND(s2_txt, len, "2. read 23C0h twice, 3C0h, 3C1h; refuse with no write unless as S1 and 23C0h == %04x\n", WD24S2_S);
	APPEND(s2_txt, len, "3. snapshot: all 25 words read twice (selects + reads only); refuse unless both passes agree, each echoes its index, CNTRL1 ACT=0, cursor enable=0\n");
	APPEND(s2_txt, len, "4. per register: W 23C0h sel, R; W 23C2h test, R 23C0h, R; W 23C2h snapshot, R; W 23C0h 1001/1002, R, R 23C2h == its snapshot\n");
	for (j = 0; j < WD24S2_NW; j++) {
		const struct wd24s2_w *w = &wd24s2_list[j];

		APPEND(s2_txt, len, "   %2u blk %u idx %X %-13s sel %04x test %04x restore = snapshot\n", j + 1,
		       w->blk, w->idx, w->name, WD24S1_NOINC | (w->idx << 8) | w->blk,
		       (w->idx << 12) | w->test);
	}
	APPEND(s2_txt, len, "5. post-pass: all 25 == snapshot; W 23C0h %04x; R == %04x\n", WD24S2_S, WD24S2_S);
	APPEND(s2_txt, len, "never written: CNTRL1 (1001), blk1 C D F, cursor control (1002), blk2 1 2 8; 23C4h never accessed\n");
	APPEND(s2_txt, len, "guard: 23C2h = the listed register's test word, or exactly its snapshot word from this load\n");
	APPEND(s2_txt, len, "writes: 23C0h %u, 23C2h %u (any mismatch: stop, restore written registers to the snapshot, post-pass, 1002)\n",
	       2 * WD24S1_NSEL + 2 * WD24S2_NW + WD24S1_NSEL + 1, 2 * WD24S2_NW);
	s2_len = len;
}

static void s2_report(int entry_err, unsigned long ewr_i, unsigned long ewr_d)
{
	const struct wd24s2 *s = s2st;
	size_t len = 0;
	unsigned int j, k;

	APPEND(s2_txt, len, "wd24adv S2 result: %s%s%s\n", wd24s2_res_name(s->res),
	       s->why ? ": " : "", s->why ? s->why : "");
#ifdef WD24S1_QEMU_FAKE
	APPEND(s2_txt, len, "*** QEMU FAKE BUILD: 23C0h/23C2h values are modelled; entry check result ignored ***\n");
#endif
	APPEND(s2_txt, len, "entry check: %s (%d); VGA index writes %lu, data writes %lu\n",
	       entry_err ? "REFUSED" : "passed", entry_err, ewr_i, ewr_d);
	len = format_events(s2_txt, len, hw, 0);
	if (s->res == WD24S2_NOT_RUN) {
		s2_len = len;
		return;
	}
	APPEND(s2_txt, len, "snapshot (%u):", s->nsnap);
	for (k = 0; k < s->nsnap; k++)
		APPEND(s2_txt, len, " %04x", s->snap[k]);
	APPEND(s2_txt, len, "\nsnapshot 2 (%u):", s->nsnap2);
	for (k = 0; k < s->nsnap2; k++)
		APPEND(s2_txt, len, " %04x", s->snap2[k]);
	APPEND(s2_txt, len, "\n");
	for (j = 0; j < WD24S2_NW; j++) {
		const struct wd24s2_w *w = &wd24s2_list[j];

		if (!s->step[j])
			break;
		APPEND(s2_txt, len, "  %-13s step %u test rb %04x snap rb %04x safety %04x%s%s\n", w->name,
		       s->step[j], s->rb_test[j], s->rb_snap[j], s->rb_safe[j],
		       s->dirty[j] ? " DIRTY" : "", s->restore_fail & (1u << j) ? " RESTORE-FAIL" : "");
	}
	if (s->stop_j >= 0)
		APPEND(s2_txt, len, "stopped at entry %d (%s)\n", s->stop_j + 1, wd24s2_list[s->stop_j].name);
	APPEND(s2_txt, len, "post-pass (%u):", s->npost);
	for (k = 0; k < s->npost; k++)
		APPEND(s2_txt, len, " %04x", s->post[k]);
	APPEND(s2_txt, len, "\n");
	if (s->restored)
		APPEND(s2_txt, len, "restore: wrote %04x, read %04x (%s)\n", s->saved & 0x1fff,
		       s->restore_rb, s->restore_rb == s->saved ? "equal" : "DIFFERENT");
	APPEND(s2_txt, len, "23C0h writes %u, 23C2h writes %u, 23C4h 0, guard refusals %u, reads %u\n",
	       s->wix, s->wdata, s->refused, s->reads);
	APPEND(s2_txt, len, "access log (in order):\n");
	for (k = 0; k < s->nlog; k++)
		APPEND(s2_txt, len, "  %c%u %04x %04x\n", s->log[k].w ? 'W' : 'R',
		       s->log[k].width, s->log[k].port, s->log[k].val);
	s2_len = len;
}

static int s2_live(void)
{
	unsigned long ewr_i, ewr_d;
	int entry_err;

	hw = vmalloc(sizeof(*hw));
	s2st = vmalloc(sizeof(*s2st));
	s2_txt = vmalloc(TXT_MAX);
	if (!hw || !s2st || !s2_txt)
		return -ENOMEM;
	if (!request_region(VGA_PORTS, VGA_NPORTS, "wd24adv")) {
		pr_err("s2: VGA ports 3C0-3DF busy; nothing touched\n");
		return -EBUSY;
	}
	if (!request_region(WD24S1_P_INDEX, 4, "wd24adv s2")) {
		release_region(VGA_PORTS, VGA_NPORTS);
		pr_err("s2: ports 23C0-23C3 busy; nothing touched\n");
		return -EBUSY;
	}
	wd24_ctx_init(hw, &wd24_port_backend);
	hw_io = (struct wd24_io){ k_inb, k_outb, s1_memr, s1_memw, NULL };
	hw->io = &hw_io;
	hw->fix_sr06 = fix_sr06;
	wd24s2_init(s2st, &wd24s2_kernel_io);

	hw_acq();
	console_lock();
	hw->be->begin(hw);
	entry_err = wd24_entry_check(hw, WD24_PH_LOAD);
	hw->be->end(hw);
	ewr_i = hw->hw_index;
	ewr_d = hw->hw_data;
#ifdef WD24S1_QEMU_FAKE
	if (entry_err)
		wd24_note(hw, "QEMU FAKE BUILD: entry check failure ignored");
	if (!ewr_d)
		wd24s2_run(s2st);
#else
	if (!entry_err && !ewr_d)
		wd24s2_run(s2st);
#endif
	console_unlock();
	hw_rel();
	release_region(WD24S1_P_INDEX, 4);
	release_region(VGA_PORTS, VGA_NPORTS);

	s2_report(entry_err, ewr_i, ewr_d);
	pr_info("S2: %s%s%s; entry check %d (index writes %lu, data %lu); 23C0h writes %u, 23C2h writes %u, reads %u\n",
		wd24s2_res_name(s2st->res), s2st->why ? ": " : "",
		s2st->why ? s2st->why : "", entry_err, ewr_i, ewr_d,
		s2st->wix, s2st->wdata, s2st->reads);
	if (s2st->restored)
		pr_info("S2: 23C0h restore read back %04x; details: /proc/wd24adv/s2\n",
			s2st->restore_rb);
	vfree(hw);
	hw = NULL;
	return 0;
}

/* ---- S3: off-screen BITBLT ---------------------------------------------- */

static void s3_plan_text(void)
{
	size_t len = 0;
	unsigned int j, p;

	APPEND(s3_txt, len, "wd24adv %s plan (dryrun=1: no port was read or written)\n", sprog->stage);
	APPEND(s3_txt, len, "runs once inside insmod dryrun=0 live=1 mode=640x480-8 %s=1, after the trial mode set + pattern (timer queued)\n",
	       sprog->param);
	APPEND(s3_txt, len, "1. 23C0h read twice == %04x, 3C0h/3C1h alias check; snapshot 25 words twice (agree, echo, CNTRL1 ACT=0, cursor enable=0)\n", WD24S2_S);
	APPEND(s3_txt, len, "2. VRAM (reads; PR0A 00-60h): banks 0-6 except %02xh checksummed; PR0A %02xh: that bank read twice == ; every op must change >= 1 byte\n",
	       sprog->bank, sprog->bank);
	APPEND(s3_txt, len, "3. per op: W 23C0h 1001, R; poll 23C2h ACT=0 (<= %u reads); params W sel, R, W, R == ; W 1001, R, R ACT=0; W CNTRL1 (last); poll ACT=0; bank %02xh == model\n",
	       WD24S3_SPINS, sprog->bank);
	for (j = 0; j < sprog->nops; j++) {
		const struct wd24s3_op *o = &sprog->ops[j];

		APPEND(s3_txt, len, "   op %u %-38s:", j + 1, o->name);
		for (p = 0; p < o->npar; p++)
			APPEND(s3_txt, len, " %04x", (o->par[p].idx << 12) | o->par[p].val);
		APPEND(s3_txt, len, " CNTRL1 %04x\n", o->cntrl1);
	}
	APPEND(s3_txt, len, "4. restore every written parameter to the snapshot (W, R); post-pass 24 words == snapshot, ACT=0; other banks' checksums equal; PR0A 00; W 23C0h %04x, R ==\n", WD24S2_S);
	APPEND(s3_txt, len, "timeout: stop, no further write of any kind (no abort), no VRAM access; the trial expiry restores text\n");
	if (sprog->host) {
		APPEND(s3_txt, len, "host data: after the CNTRL1 start, per word: R 23C2h (sel 1001h) ACT=1 else stop (HOST DATA SHORT, restore), then W 23C4h = the next owed word only; exactly the word count, no drain\n");
		for (j = 0; j < sprog->nops; j++)
			APPEND(s3_txt, len, "   op %u: %u words (%u per row x %u), first %04x last %04x\n", j + 1,
			       wd24s5_words(&sprog->ops[j]), wd24s5_wpr(&sprog->ops[j]), sprog->ops[j].h,
			       wd24s5_word(&sprog->ops[j], 0),
			       wd24s5_word(&sprog->ops[j], wd24s5_words(&sprog->ops[j]) - 1));
		if (sprog->ops[0].pat == WD24S5_PAT_PROBE)
			APPEND(s3_txt, len, "probe (op 1): if still busy after its words, feed the 1 px/word words (pixel n, both bytes) up to w*h, each after ACT=1; idle = HOSTCOL 1 PX/WORD (stop, restore); busy = TIMEOUT\n");
		APPEND(s3_txt, len, "never written: block 2 (cursor), C, D, F; CNTRL1 only %04x; 23C4h only owed words of a started op\n",
		       sprog->ops[0].cntrl1);
	} else {
		APPEND(s3_txt, len, "never written: block 2 (cursor), BG, C, D, F, 23C4h; CNTRL1 only %s\n",
		       sprog == &wd24s3_prog4 ? "0908/0900/0D00 (packed; 0D00 = backward copy)" : "0908/0900 (forward, packed)");
	}
	s3_len = len;
}

static void s3_report(const char *pre)
{
	const struct wd24s3 *s = s3st;
	size_t len = 0;
	unsigned int j, k;

	APPEND(s3_txt, len, "wd24adv %s result: %s%s%s\n", sprog->stage, pre ? "NOT RUN: " : wd24s3_res_name(s->res),
	       pre ? pre : (s->why ? ": " : ""), pre ? "" : (s->why ? s->why : ""));
#ifdef WD24S1_QEMU_FAKE
	APPEND(s3_txt, len, "*** QEMU FAKE BUILD: 23C0h/23C2h values and video memory are modelled ***\n");
#endif
	if (pre || s->res == WD24S3_NOT_RUN) {
		s3_len = len;
		return;
	}
	APPEND(s3_txt, len, "snapshot (%u):", s->nsnap);
	for (k = 0; k < s->nsnap; k++)
		APPEND(s3_txt, len, " %04x", s->snap[k]);
	APPEND(s3_txt, len, "\nsnapshot 2 (%u):", s->nsnap2);
	for (k = 0; k < s->nsnap2; k++)
		APPEND(s3_txt, len, " %04x", s->snap2[k]);
	APPEND(s3_txt, len, "\nchecksums (banks 0-6 except %02xh) before:", sprog->bank);
	for (k = 0; k < WD24S3_NCRC; k++)
		APPEND(s3_txt, len, " %08x", s->crc0[k]);
	APPEND(s3_txt, len, "\nchecksums (banks 0-6 except %02xh) after: ", sprog->bank);
	for (k = 0; k < WD24S3_NCRC; k++)
		APPEND(s3_txt, len, " %08x", s->crc1[k]);
	APPEND(s3_txt, len, "\n");
	for (j = 0; j < sprog->nops; j++)
		APPEND(s3_txt, len, "  op %u %-38s model changes %u; idle polls %u; busy %s, polls %u; CNTRL1 after %04x; host words %u/%u%s\n",
		       j + 1, sprog->ops[j].name, s->changes[j], s->idle_spins[j],
		       s->busy_seen[j] ? "seen" : "not seen", s->spins[j], s->c1_after[j],
		       s->hwords[j], wd24s5_words(&sprog->ops[j]),
		       j < s->done ? " VERIFIED" : "");
	if (s->ndiff)
		APPEND(s3_txt, len, "VRAM: %u bytes differ; first %05x read %02x want %02x\n",
		       s->ndiff, s->diff_off, s->diff_got, s->diff_want);
	if (s->hclass)
		APPEND(s3_txt, len, "HOSTCOL readback class: %s\n", s->hclass);
	if (sprog->ops[0].pat == WD24S5_PAT_PROBE || s->alt_words)
		APPEND(s3_txt, len, "probe alternative words: %u\n", s->alt_words);
	APPEND(s3_txt, len, "post-pass (%u):", s->npost);
	for (k = 0; k < s->npost; k++)
		APPEND(s3_txt, len, " %04x", s->post[k]);
	APPEND(s3_txt, len, "\n");
	if (s->restored)
		APPEND(s3_txt, len, "restore: wrote %04x, read %04x (%s)\n", s->saved & 0x1fff,
		       s->restore_rb, s->restore_rb == s->saved ? "equal" : "DIFFERENT");
	if (s->dead)
		APPEND(s3_txt, len, "DEAD: BITBLT timed out; nothing written after the start\n");
	if (s->dead)
		APPEND(s3_txt, len, "idle re-check (reads only): %u checks, last 23C0h %04x, %u polls, CNTRL1 %04x: %s\n",
		       s->idle_checks, s->idle_last_ix, s->idle_last_spins, s->idle_last_c1,
		       s->idle_seen ? "IDLE seen: video memory unlocked, normal text restore" :
		       !vram_dead ? "busy, not locked yet" : "STILL BUSY: video memory LOCKED (registers-only restore, rmmod refused): power cycle required");
	APPEND(s3_txt, len, "23C0h writes %u, 23C2h writes %u (starts %u), PR0A writes %u, 23C4h %u, refusals %u, reads %u (polls %u), restore fails %x\n",
	       s->wix, s->wdata, s->wgo, s->wbank, s->whost, s->refused, s->reads, s->polls, s->restore_fail);
	APPEND(s3_txt, len, "access log (in order; polls and 23C4h words: first and last only; W8 03cf 09vv = PR0A):\n");
	for (k = 0; k < s->nlog; k++)
		APPEND(s3_txt, len, "  %c%u %04x %04x\n", s->log[k].w ? 'W' : 'R',
		       s->log[k].width, s->log[k].port, s->log[k].val);
	s3_len = len;
}

static bool s3_region;	/* 23C0-23C3 held by s3_live */

/* caller holds hw_lock.  0: video memory may be accessed.  After an S3
 * timeout, a bounded read-only ACT poll decides; still busy -> sticky
 * vram_dead (hw->no_vram: registers-only restore; every memory path
 * refused; module stays pinned). */
static int vram_gate(const char *what)
{
	bool got = false;

	if (!vram_dead && s3st && s3st->dead && !s3st->idle_seen) {
		if (!s3_region && request_region(WD24S1_P_INDEX, 4, "wd24adv s3 idle"))
			got = true;
		if (s3_region || got)
			wd24s3_idle_poll(s3st);
		if (got)
			release_region(WD24S1_P_INDEX, 4);
		if (s3st->idle_seen) {
			pr_info("S3: BITBLT seen idle (%s; %u polls): video memory unlocked\n",
				what, s3st->idle_last_spins);
			if (s3_pinned) {
				module_put(THIS_MODULE);
				s3_pinned = false;
			}
		} else {
			vram_dead = true;
			pr_crit("S3: BITBLT still busy (%s; 23C0h %04x, CNTRL1 %04x): video memory LOCKED, registers-only restore, rmmod refused: power cycle required\n",
				what, s3st->idle_last_ix, s3st->idle_last_c1);
		}
		if (s3_txt)
			s3_report(NULL);
	}
	if (vram_dead) {
		if (hw)
			hw->no_vram = true;
		pr_err("%s: video memory access refused (S3 BITBLT timeout): power cycle required\n", what);
		return -EIO;
	}
	return 0;
}

#ifdef WD24S1_QEMU_FAKE
extern u8 *wd24s3_fake_vram;
static u8 fake_pr0a;

static int s3_bank(void *priv, u8 v)
{
	fake_pr0a = v;		/* no port access: the RAM model only */
	return 0;
}

static void s3_vread(void *priv, u32 off, u8 *buf, u32 n)
{
	memcpy(buf, wd24s3_fake_vram + ((u32)fake_pr0a << 12) + off, n);
}
#else
static int s3_bank(void *priv, u8 v)
{
	return wd24_write(hw, WD24_GR, WD24_GR_PR0A, v);
}

static void s3_vread(void *priv, u32 off, u8 *buf, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		buf[i] = readb(vmem + ((off + i) & (WD24_PLANE - 1)));
}
#endif

static const struct wd24s3_mem s3_memops = { s3_bank, s3_vread, NULL };

static int s3_alloc(void)
{
	s3st = vmalloc(sizeof(*s3st));
	s3_txt = vmalloc(TXT_MAX);
	s3_mem = vmalloc(3 * WD24S3_WIN);
	return (s3st && s3_txt && s3_mem) ? 0 : -ENOMEM;
}

/* 23C0h-23C3h; S5 also the BITBLT I/O port 23C4h-23C5h */
static unsigned int s3_nports(void)
{
	return sprog->host ? 6 : 4;
}

/* caller holds hw_lock; the trial mode is set and its timer queued */
static void s3_live(const struct wd24_mode *m)
{
	const char *pre = NULL;

	wd24s3_init(s3st, &wd24s3_kernel_io, &s3_memops, s3_mem,
		    s3_mem + WD24S3_WIN, s3_mem + 2 * WD24S3_WIN);
	wd24s3_set_prog(s3st, sprog);
#ifndef WD24S1_QEMU_FAKE
	if (!hw || hw->in_text || !hw->trial_pending || hw->nmismatch ||
	    !m || strcmp(m->name, "640x480-8") || !vmem)
		pre = "not in the 640x480-8 trial state";
#endif
	if (!pre && !request_region(WD24S1_P_INDEX, s3_nports(), "wd24adv s3"))
		pre = sprog->host ? "ports 23C0-23C5 busy" : "ports 23C0-23C3 busy";
	if (pre) {
		s3_report(pre);
		pr_err("%s: not run: %s\n", sprog->stage, pre);
		return;
	}
	s3_region = true;
	console_lock();
#ifndef WD24S1_QEMU_FAKE
	hw->be->begin(hw);
	hw->tag = "S3: PR0A bank (video memory readback)";
#endif
	wd24s3_run(s3st);
#ifndef WD24S1_QEMU_FAKE
	hw->be->end(hw);
#endif
	console_unlock();
	if (s3st->dead) {
		__module_get(THIS_MODULE);	/* rmmod refused until seen idle */
		s3_pinned = true;
		vram_gate("after the S3 timeout");
	}
	s3_region = false;
	release_region(WD24S1_P_INDEX, s3_nports());
	s3_report(NULL);
	pr_info("%s: %s%s%s; ops verified %u; 23C2h writes %u (starts %u), PR0A writes %u; details: /proc/wd24adv/%s\n",
		sprog->stage, wd24s3_res_name(s3st->res), s3st->why ? ": " : "",
		s3st->why ? s3st->why : "", s3st->done, s3st->wdata, s3st->wgo, s3st->wbank,
		sprog->param);
}

#ifdef WD24S1_QEMU_FAKE
/* QEMU TEST BUILD ONLY: S3 on the modelled registers + RAM video memory */
static int s3_fake_run(void)
{
	u32 i;

	if (s3_alloc())
		return -ENOMEM;
	wd24s3_fake_vram = vmalloc(7 * WD24S3_WIN);
	if (!wd24s3_fake_vram)
		return -ENOMEM;
	for (i = 0; i < 7 * WD24S3_WIN; i++)
		wd24s3_fake_vram[i] = (u8)(i * 7 + (i >> 9));
	hw_acq();
	s3_live(NULL);
	hw_rel();
	vfree(wd24s3_fake_vram);
	wd24s3_fake_vram = NULL;
	return 0;
}
#endif

/* ---- S6: dual window ------------------------------------------------- */

static int s6_alloc(void)
{
	s6st = vmalloc(sizeof(*s6st));
	s6_save = vmalloc(WD24S6_NREG * WD24S6_N);
	return s6st && s6_save ? 0 : -ENOMEM;
}

/* dryrun=1 listing and the QEMU fake build: the 86Box-semantics model */
static int s6_model_run(void)
{
	struct wd24s6_model *mdl = vmalloc(sizeof(*mdl));
	u8 *vr = vmalloc(0x100000);
	struct wd24s6_io io = wd24s6_model_io;

	if (!mdl || !vr || s6_alloc()) {
		vfree(mdl);
		vfree(vr);
		return -ENOMEM;
	}
	wd24s6_model_init(mdl, vr);
	io.priv = mdl;
	wd24s6_run2(s6st, &io, s6_save, s6c);
	pr_info("S6 (model, no port access): %s\n", wd24s6_res_name(s6st->res));
	vfree(mdl);
	vfree(vr);
	return 0;
}

static u8 s6_rd(void *priv, enum wd24_rf rf, u8 idx)
{
	return wd24_read(hw, rf, idx);
}

static int s6_wr(void *priv, enum wd24_rf rf, u8 idx, u8 v)
{
	return wd24_write(hw, rf, idx, v);	/* guarded, read back */
}

static void s6_mr(void *priv, u32 off, u8 *buf, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		buf[i] = readb(vmem + ((off + i) & (WD24_PLANE - 1)));
}

static void s6_mw(void *priv, u32 off, const u8 *buf, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		writeb(buf[i], vmem + ((off + i) & (WD24_PLANE - 1)));
}

static void s6_open(void *priv, bool on)
{
	hw->s6_active = on;
	hw->dual_open = on;
	if (!on)
		hw->s6_lock_open = false;
}

static void s6_lock(void *priv, bool on)
{
	hw->s6_lock_open = on;
}

static int s6_unlocked(void *priv)
{
	return hw->be->seq_unlocked ? hw->be->seq_unlocked(hw) : -1;
}

static const struct wd24s6_io s6_live_io = { s6_rd, s6_wr, s6_mr, s6_mw, s6_open, s6_lock,
					     s6_unlocked, NULL };

/* caller holds hw_lock; the trial mode is set and its timer queued */
static void s6_live(const struct wd24_mode *m)
{
	if (!hw || hw->in_text || !hw->trial_pending || hw->nmismatch ||
	    !m || strcmp(m->name, "640x480-8") || !vmem || hw->no_vram) {
		pr_err("S6: not run: not in the 640x480-8 trial state\n");
		return;
	}
	console_lock();
	hw->be->begin(hw);
	hw->tag = s6c ? "S6c: PR31 b7 held across SR06 relock" :
			"S6: dual window (PR0B, PR1 b3, SR06 unlock, PR31 b7)";
	wd24s6_run2(s6st, &s6_live_io, s6_save, s6c);
	hw->dual_open = false;
	hw->s6_lock_open = false;
	hw->s6_active = false;
	hw->be->end(hw);
	console_unlock();
	pr_info("S6: %s%s%s; SR06 %02x unl %02x/%d rel %02x/%d, PR31 %02x; PR0B rb %02x, PR1 on %02x off %02x, PR31 on %02x off %02x; writes %u; /proc/wd24adv/s6\n",
		wd24s6_res_name(s6st->res), s6st->why ? ": " : "", s6st->why ? s6st->why : "",
		s6st->sr06, s6st->rb_sr06_unl, s6st->lk_unl, s6st->rb_sr06_rel, s6st->lk_rel,
		s6st->pr31, s6st->rb_pr0b, s6st->rb_pr1_on, s6st->rb_pr1_off, s6st->rb_pr31_on,
		s6st->rb_pr31_off, s6st->nwr);
	if (s6c)
		pr_info("S6c: held SR06 %02x/%d PR31(locked) %02x, re-unlock PR31 %02x (%s); D2 rd %u noecho %u V2 %u/%u/%u\n",
			s6st->rb_sr06_hold, s6st->lk_hold, s6st->rb_pr31_lk, s6st->rb_pr31_reunl,
			s6st->cleared ? "b7 cleared" : "b7 held", s6st->d_rd, s6st->d_wr_noecho,
			s6st->v_wr, s6st->v_copy, s6st->v_x);
}

/*
 * S6b: /dev/wd24s6b.  One exclusive open = one session (wd24s6b_begin ..
 * wd24s6b_end).  The window is mapped once (no fault handler: the tc755
 * kernel exports neither vmf_insert_pfn nor zap_vma_ptes, and a fault
 * banker would livelock on a cross-bank memmove anyway).  Teardown: the
 * vma holds the file, so release() (= end of session, registers restored)
 * runs at the last munmap/close, including process exit and kill -9.
 * Every operation runs under hw_lock + console_lock.
 */
static struct wd24s6b *s6b_st;
static char *s6b_txt;
static size_t s6b_len;
static int s6b_major = -1;
static struct class *s6b_cls;
static bool s6b_dev, s6b_busy, s6b_armed;
static const struct wd24s6_io *s6b_io;
#ifdef WD24S1_QEMU_FAKE
static struct wd24s6_model *s6b_mdl;
static u8 *s6b_vr, *s6b_win;	/* fake: model VRAM, user-visible window */
static struct wd24s6_io s6b_mio;
#endif

static ssize_t s6b_read(struct file *f, char __user *b, size_t n, loff_t *p)
{
	return s6b_txt ? rd(b, n, p, s6b_txt, s6b_len) : 0;
}

static const struct proc_ops s6b_ops = {
	.proc_read = s6b_read, .proc_lseek = default_llseek };

static void s6b_report(const char *what, int err)
{
	size_t len = s6b_len;

	if (!s6b_txt)
		return;
	if (len > TXT_MAX - 8192)
		len = 0;	/* keep the newest */
	APPEND(s6b_txt, len, "--- %s -> %d; state %s%s%s; w0 %02x w1 %02x; copies %u setwin %u writes %u restore-bad %u\n",
	       what, err, s6b_st ? wd24s6b_state_name(s6b_st->state) : "-",
	       s6b_st && s6b_st->why ? ": " : "", s6b_st && s6b_st->why ? s6b_st->why : "",
	       s6b_st ? s6b_st->w0 : 0, s6b_st ? s6b_st->w1 : 0,
	       s6b_st ? s6b_st->ncopy : 0, s6b_st ? s6b_st->nset : 0,
	       s6b_st ? s6b_st->nwr : 0, s6b_st ? s6b_st->rest_bad : 0);
	if (s6b_st)
		APPEND(s6b_txt, len, "%.*s", (int)s6b_st->loglen, s6b_st->log);
	s6b_len = len;
	if (s6b_st)
		s6b_st->loglen = 0;
}

#ifdef WD24S1_QEMU_FAKE
/* the fake window is plain memory: flush it to the model before any bank
 * change, reload it after (steady state: b3 on, b7 off) */
static void s6b_sync(bool out)
{
	if (!s6b_st || s6b_st->state != WD24S6B_ON)
		return;
	if (out)
		s6b_mio.mw(s6b_mio.priv, 0, s6b_win, 0x10000);
	else
		s6b_mio.mr(s6b_mio.priv, 0, s6b_win, 0x10000);
}
#define S6B_HW_BEGIN()	do { } while (0)
#define S6B_HW_END()	do { } while (0)
#else
static void s6b_sync(bool out) { }
#define S6B_HW_BEGIN()	do { console_lock(); hw->be->begin(hw); \
	hw->tag = "S6b: dual-window session"; } while (0)
#define S6B_HW_END()	do { hw->be->end(hw); console_unlock(); } while (0)
#endif

/* caller holds hw_lock */
static void s6b_end_locked(const char *why)
{
	int err;

	if (!s6b_st || s6b_st->state == WD24S6B_OFF || !s6b_busy)
		return;
	s6b_sync(true);
	S6B_HW_BEGIN();
	err = wd24s6b_end(s6b_st);
	S6B_HW_END();
	if (err)
		pr_err("S6b: end (%s): %d, %s\n", why, err, wd24s6b_state_name(s6b_st->state));
	s6b_report(why, err);
	s6b_st->state = WD24S6B_OFF;
}

/* hw_run (text, mode, trial expiry, rmmod): the session ends first and
 * no new session can start */
static void s6b_stop(const char *why)
{
	s6b_armed = false;
	s6b_end_locked(why);
}

static int s6b_fopen(struct inode *in, struct file *f)
{
	int err = 0;

	hw_acq();
	if (s6b_busy)
		err = -EBUSY;
#ifndef WD24S1_QEMU_FAKE
	else if (!s6b_armed || !hw || hw_dead || hw->in_text || hw->nmismatch ||
		 !vmem || hw->no_vram || vram_dead)
		err = -ENODEV;
#else
	else if (!s6b_armed)
		err = -ENODEV;
#endif
	if (!err) {
		S6B_HW_BEGIN();
		err = wd24s6b_begin(s6b_st, s6b_io);
		S6B_HW_END();
		s6b_report("open: begin", err);
		if (err) {
			if (s6b_st->state == WD24S6B_DEAD)
				s6b_armed = false;	/* failed after writes */
			s6b_st->state = WD24S6B_OFF;
		} else {
			s6b_busy = true;
			s6b_sync(false);
		}
	}
	hw_rel();
	return err;
}

static int s6b_frelease(struct inode *in, struct file *f)
{
	hw_acq();
	s6b_end_locked("release (close / munmap / exit)");
	s6b_busy = false;
	hw_rel();
	return 0;
}

static int s6b_fmmap(struct file *f, struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start;
	int err;

	if (vma->vm_pgoff || len > 0x10000)
		return -EINVAL;
	hw_acq();
	if (!s6b_st || s6b_st->state != WD24S6B_ON) {
		hw_rel();
		return -ENODEV;
	}
	vm_flags_set(vma, VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP);
#ifdef WD24S1_QEMU_FAKE
	err = remap_vmalloc_range(vma, s6b_win, 0);
#else
	/* uncached (486: UC-), like a framebuffer mmap */
	vma->vm_page_prot = pgprot_framebuffer(vma->vm_page_prot, vma->vm_start,
					       vma->vm_end, 0xa0000);
	err = vm_iomap_memory(vma, 0xa0000, 0x10000);
#endif
	hw_rel();
	return err;
}

static long s6b_fioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	void __user *up = (void __user *)arg;
	struct wd24s6b_win w;
	struct wd24s6b_copy c;
	struct wd24s6b_info in;
	int err;

	switch (cmd) {
	case WD24S6B_IOC_SETWIN:
		if (copy_from_user(&w, up, sizeof(w)))
			return -EFAULT;
		break;
	case WD24S6B_IOC_COPY:
		if (copy_from_user(&c, up, sizeof(c)))
			return -EFAULT;
		break;
	case WD24S6B_IOC_INFO:
		break;
	default:
		return -ENOTTY;
	}
	hw_acq();
	if (cmd == WD24S6B_IOC_INFO) {
		memset(&in, 0, sizeof(in));
		in.state = s6b_st->state;
		in.w0 = s6b_st->w0;
		in.w1 = s6b_st->w1;
		in.ncopy = s6b_st->ncopy;
		in.nset = s6b_st->nset;
		in.nwr = s6b_st->nwr;
		in.vram = WD24S6B_VRAM;
		in.frame = WD24S6B_FRAME;
		hw_rel();
		return copy_to_user(up, &in, sizeof(in)) ? -EFAULT : 0;
	}
	if (s6b_st->state != WD24S6B_ON) {
		hw_rel();
		return -EIO;
	}
	s6b_sync(true);
	S6B_HW_BEGIN();
	if (cmd == WD24S6B_IOC_SETWIN)
		err = wd24s6b_setwin(s6b_st, w.w0, w.w1);
	else
		err = wd24s6b_copy(s6b_st, c.dst, c.src, c.len);
	S6B_HW_END();
	s6b_sync(false);
	if (err || s6b_st->state != WD24S6B_ON) {
		s6b_report(cmd == WD24S6B_IOC_SETWIN ? "SETWIN" : "COPY", err);
		if (s6b_st->state == WD24S6B_DEAD) {
			s6b_armed = false;
			pr_err("S6b: session DEAD (%s); restored best effort; see /proc/wd24adv/s6b\n",
			       s6b_st->why ? s6b_st->why : "?");
		}
	}
	hw_rel();
	return err;
}

static const struct file_operations s6b_fops = {
	.owner = THIS_MODULE,
	.open = s6b_fopen,
	.release = s6b_frelease,
	.mmap = s6b_fmmap,
	.unlocked_ioctl = s6b_fioctl,
	.llseek = noop_llseek,
};

static void s6b_free(void)
{
	if (s6b_dev)
		device_destroy(s6b_cls, MKDEV(s6b_major, 0));
	s6b_dev = false;
	if (s6b_cls)
		class_destroy(s6b_cls);
	s6b_cls = NULL;
	if (s6b_major >= 0)
		__unregister_chrdev(s6b_major, 0, 1, "wd24s6b");
	s6b_major = -1;
	vfree(s6b_st);
	vfree(s6b_txt);
	s6b_st = NULL;
	s6b_txt = NULL;
#ifdef WD24S1_QEMU_FAKE
	vfree(s6b_mdl);
	vfree(s6b_vr);
	vfree(s6b_win);
	s6b_mdl = NULL;
	s6b_vr = NULL;
	s6b_win = NULL;
#endif
}

/* dryrun=1: one model session (cross-bank scroll + memmove), no device */
static int s6b_model_session(void)
{
	struct wd24s6_model *mdl = vmalloc(sizeof(*mdl));
	u8 *vr = vmalloc(0x100000);
	struct wd24s6_io io = wd24s6_model_io;
	int err;

	if (!mdl || !vr) {
		vfree(mdl);
		vfree(vr);
		return -ENOMEM;
	}
	wd24s6_model_init(mdl, vr);
	io.priv = mdl;
	err = wd24s6b_begin(s6b_st, &io);
	s6b_report("model: begin", err);
	if (!err) {
		err = wd24s6b_setwin(s6b_st, 0x50, 0x70);
		s6b_report("model: SETWIN 50/70", err);
	}
	if (!err) {
		err = wd24s6b_copy(s6b_st, 0, 640 * 16, WD24S6B_FRAME - 640 * 16);
		s6b_report("model: COPY frame scroll up 16 lines (cross-bank)", err);
	}
	if (!err) {
		err = wd24s6b_copy(s6b_st, 0x52000, 0x50800, 0x18000);
		s6b_report("model: COPY overlapping memmove forward-dst", err);
	}
	wd24s6b_end(s6b_st);
	s6b_report("model: end", 0);
	s6b_st->state = WD24S6B_OFF;
	pr_info("S6b (model, no port access): %s\n", err ? "FAIL" : "OK");
	vfree(mdl);
	vfree(vr);
	return err;
}

static int s6b_setup(void)
{
	struct device *d;

	s6b_st = vmalloc(sizeof(*s6b_st));
	s6b_txt = vmalloc(TXT_MAX);
	if (!s6b_st || !s6b_txt)
		return -ENOMEM;
	memset(s6b_st, 0, sizeof(*s6b_st));
	s6b_len = 0;
	if (dryrun)
		return s6b_model_session();
#ifdef WD24S1_QEMU_FAKE
	s6b_mdl = vmalloc(sizeof(*s6b_mdl));
	s6b_vr = vmalloc(0x100000);
	s6b_win = vmalloc_user(0x10000);
	if (!s6b_mdl || !s6b_vr || !s6b_win)
		return -ENOMEM;
	wd24s6_model_init(s6b_mdl, s6b_vr);
	s6b_mio = wd24s6_model_io;
	s6b_mio.priv = s6b_mdl;
	s6b_io = &s6b_mio;
	s6b_armed = true;
#else
	s6b_io = &s6_live_io;
#endif
	s6b_major = __register_chrdev(0, 0, 1, "wd24s6b", &s6b_fops);
	if (s6b_major < 0)
		return s6b_major;
	s6b_cls = class_create("wd24s6b");
	if (IS_ERR(s6b_cls)) {
		s6b_cls = NULL;
		return -ENOMEM;
	}
	d = device_create(s6b_cls, NULL, MKDEV(s6b_major, 0), NULL, "wd24s6b");
	if (IS_ERR(d))
		return PTR_ERR(d);
	s6b_dev = true;
	APPEND(s6b_txt, s6b_len, "S6b: /dev/wd24s6b major %d minor 0%s\n", s6b_major,
	       s6b_armed ? " (QEMU fake: model)" : " (armed after the 640x480-8 mode set)");
	pr_info("S6b: /dev/wd24s6b major %d\n", s6b_major);
	return 0;
}

#ifndef WD24S1_QEMU_FAKE
/* caller holds hw_lock; the 640x480-8 trial mode is set */
static void s6b_arm(const struct wd24_mode *m)
{
	if (!hw || hw->in_text || hw->nmismatch || !m || strcmp(m->name, "640x480-8") ||
	    !vmem || hw->no_vram || vram_dead) {
		pr_err("S6b: not armed: not in the 640x480-8 state\n");
		return;
	}
	s6b_armed = true;
	pr_info("S6b: armed: open /dev/wd24s6b (major %d); text/mode/trial expiry/rmmod ends it\n",
		s6b_major);
}
#else
static void s6b_arm(const struct wd24_mode *m) { }
#endif


/* ---- S6d fault-banked fbdev (visual: binds fbcon) ---- */
static struct wd24s6d *s6d_st;
static struct wd24s6b *s6d_b;
static struct fb_info *s6d_info;
static bool s6d_armed, s6d_busy;
static bool s6d_cmap_published, s6d_cmap_frozen;
static struct vm_area_struct *s6d_vma;	/* exclusive single mapper */

/* Drop every userspace PTE before the aperture is reprogrammed or the
 * module's vm_ops go away. Caller holds hw_lock. Order matches
 * wd24s6d_stop_step(): quiesce, unmap, then the caller ends the session.
 * unmap_mapping_range takes its own locks, so hw_lock is dropped across it.
 * stopping is set first so a fault cannot pass its check in that window. */
static void s6d_drop_maps(void)
{
	struct address_space *mapping = NULL;

	if (s6d_st)
		s6d_st->stopping = true;
	if (s6d_vma && s6d_vma->vm_file)
		mapping = s6d_vma->vm_file->f_mapping;
	s6d_vma = NULL;
	if (!mapping)
		return;
	hw_rel();
	unmap_mapping_range(mapping, 0, 0, 1);
	hw_acq();
}

static void s6d_end_locked(const char *why)
{
	u32 nf, ns, nt;
	int err;

	/* Caller holds hw_lock. A fault holds console_lock, then hw_lock.
	 * Taking console_lock here while still holding hw_lock deadlocks
	 * that fault. Drop hw first. s6d_ender keeps another hw_run from
	 * restoring text until wd24s6b_end has finished. */
	s6d_yield_ender();
	if (!s6d_st || !s6d_st->active)
		return;
	s6d_ender = current;
	nf = s6d_st->nfault;
	ns = s6d_st->nswitch;
	nt = s6d_st->nthrash;
	s6d_st->active = false;
	s6d_st->mapped_base = ~0u;
	s6d_vma = NULL;
	wd24s6d_thrash_reset(&s6d_st->thrash);
	hw_rel();
#ifndef WD24S1_QEMU_FAKE
	console_lock();
#endif
	hw_acq();
#ifndef WD24S1_QEMU_FAKE
	if (hw) {
		hw->be->begin(hw);
		hw->tag = "S6d: end session";
	}
#endif
	err = wd24s6b_end(s6d_b);
#ifndef WD24S1_QEMU_FAKE
	if (hw)
		hw->be->end(hw);
	console_unlock();
#endif
	/* Drop hw before printk. A fault may hold console_lock and be
	 * waiting for hw_lock; printing while holding hw_lock deadlocks it.
	 * s6d_ender stays set so another hw_run does not restore text yet. */
	hw_rel();
	if (err)
		pr_err("S6d: end (%s): %d\n", why, err);
	else
		pr_info("S6d: end (%s): faults %u switches %u thrash %u\n",
			why, nf, ns, nt);
	s6d_ender = NULL;
	hw_acq();
}

static void s6d_stop(const char *why)
{
	bool need_unreg = false;

	/* Before any lock drop. A SET waiting on hw_lock must see this
	 * and return without a 3C9 write. Text restore follows s6d_stop. */
	s6d_cmap_frozen = true;
	/* quiesce + unmap, end-session, then unregister. text-restore is
	 * hw_run's next step (trial expiry, "text", rmmod), not here. */
	s6d_armed = false;
	if (s6d_st && s6d_st->fb_reg) {
		s6d_st->fb_reg = false;
		need_unreg = true;
	}
	s6d_drop_maps();
	s6d_end_locked(why);
	s6d_busy = false;
	/*
	 * register/unregister_framebuffer must not run under hw_lock (fbcon
	 * callbacks re-enter). hw_run holds hw_lock; drop it around unregister.
	 */
	if (need_unreg && s6d_info) {
		hw_rel();
		unregister_framebuffer(s6d_info);
		hw_acq();
	}
}

static vm_fault_t s6d_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long off;
	const char *why = "refused";
	int err = 0;

	off = (vmf->address - vma->vm_start) + (vma->vm_pgoff << PAGE_SHIFT);
	if (off >= WD24S6D_MMAP_LEN)
		return VM_FAULT_SIGBUS;
	/* console_lock then hw_lock, same order as fbcon. Both sleeps are
	 * uninterruptible: a userspace alarm or SIGKILL cannot pull the
	 * task out of this fault. */
#ifndef WD24S1_QEMU_FAKE
	console_lock();
#endif
	hw_acq();
	if (!s6d_st || !s6d_st->active || !s6d_armed || s6d_st->stopping ||
	    vma != s6d_vma) {
		err = -ENODEV;
		goto out;
	}
#ifndef WD24S1_QEMU_FAKE
	if (hw) {
		hw->be->begin(hw);
		hw->tag = "S6d: fault bank";
	}
#endif
	err = wd24s6d_map_win(s6d_st, vma, (u32)off);
	if (s6d_st->why)
		why = s6d_st->why;
#ifndef WD24S1_QEMU_FAKE
	if (hw)
		hw->be->end(hw);
#endif
out:
	hw_rel();
#ifndef WD24S1_QEMU_FAKE
	console_unlock();
#endif
	if (err) {
		pr_err("S6d: fault off=%lx: %d (%s)\n", off, err, why);
		return VM_FAULT_SIGBUS;
	}
	return VM_FAULT_NOPAGE;
}

static void s6d_vma_close(struct vm_area_struct *vma)
{
	hw_acq();
	if (vma == s6d_vma)
		s6d_vma = NULL;
	hw_rel();
}

static const struct vm_operations_struct s6d_vm_ops = {
	.fault = s6d_fault,
	.close = s6d_vma_close,
};

static int s6d_fb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff || !len || len > WD24S6D_MMAP_LEN)
		return -EINVAL;
	hw_acq();
	if (!s6d_st || !s6d_st->active || !s6d_armed || s6d_st->stopping) {
		hw_rel();
		return -ENODEV;
	}
	if (s6d_vma) {
		hw_rel();
		return -EBUSY;
	}
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP |
		     VM_DONTCOPY);
	vma->vm_ops = &s6d_vm_ops;
	vma->vm_private_data = info;
	s6d_vma = vma;
	s6d_st->mapped_base = ~0u;
	hw_rel();
	return 0;
}

static void s6d_cmap_out(void *p, u8 v, u16 port)
{
	struct wd24_ctx *c = p;

	c->io->outb(c->io->priv, v, port);
	if (port == WD24_DAC_RI || port == WD24_DAC_WI)
		c->hw_index++;
	else
		c->hw_data++;
}

static u8 s6d_cmap_in(void *p, u16 port)
{
	struct wd24_ctx *c = p;

	return c->io->inb(c->io->priv, port);
}

static void s6d_cmap_view(struct wd24_cmap_view *v)
{
	memset(v, 0, sizeof(*v));
	v->param = s6d_cmap;
	v->dryrun = dryrun;
	v->live = live;
	v->bind = s6d_bind;
	v->fbcon = s6d_fbcon;
	v->armed = s6d_armed;
	v->active = s6d_st && s6d_st->active;
	v->stopping = s6d_st && s6d_st->stopping;
	v->busy = s6d_busy;
	v->registering = s6d_registering;
	v->in_text = !hw || hw->in_text;
	v->hw_dead = hw_dead || !hw;
	v->have_cmap = hw && (hw->have & WD24_HAVE_CMAP);
	v->bad = !hw || hw->nmismatch || hw->failed;
	v->published = s6d_cmap_published;
	v->frozen = s6d_cmap_frozen;
}

/* Caller holds info->lock and hw_lock. Reads the DAC (3C6=FF, 3C7=0,
 * 768 x 3C9). Publishes <<10 only when that read matches c->cmap.
 * Does not write 3C9. */
static int s6d_cmap_publish(struct fb_info *info)
{
	u8 live_dac[768];
	int err;

	BUILD_BUG_ON(EPERM != 1 || EIO != 5 || EBUSY != 16 ||
		     ENODEV != 19 || EINVAL != 22);
	if (!s6d_cmap)
		return 0;
	if (s6d_cmap_frozen || !hw || hw_dead || !hw->io || !hw->be->dac_read ||
	    !(hw->have & WD24_HAVE_CMAP) || hw->in_text || hw->nmismatch ||
	    hw->failed)
		return -ENODEV;
	hw->be->dac_read(hw, 0xff, live_dac);
	if (wd24_dac_diff6(live_dac, hw->cmap)) {
		s6d_cmap_published = false;
		fb_dealloc_cmap(&info->cmap);
		return -EIO;
	}
	err = fb_alloc_cmap(&info->cmap, 256, 0);
	if (err) {
		s6d_cmap_published = false;
		return err;
	}
	wd24_dac_expand_image(hw->cmap, info->cmap.red, info->cmap.green,
			      info->cmap.blue);
	s6d_cmap_published = true;
	return 0;
}

/* FBIOPUTCMAP holds info->lock, then this takes hw_lock. It does not
 * take console_lock. Faults take console_lock, then hw_lock. */
static int s6d_fb_setcmap(struct fb_cmap *cmap, struct fb_info *info)
{
	struct wd24_cmap_view view;
	struct wd24_dac_io dio;
	unsigned int i;
	int err;

	if (!cmap || !cmap->red || !cmap->green || !cmap->blue ||
	    cmap->start > 255 || cmap->len == 0 ||
	    cmap->len > 256 - cmap->start)
		return -EINVAL;
	hw_acq();
	s6d_cmap_view(&view);
	err = wd24_cmap_allow(&view);
	if (err || !hw || !hw->io || !hw->io->outb || !hw->io->inb) {
		hw_rel();
		return err ? err : -ENODEV;
	}
	dio.outb = s6d_cmap_out;
	dio.inb = s6d_cmap_in;
	dio.p = hw;
	wd24_dac_mask_ff(&dio);
	for (i = 0; i < cmap->len; i++) {
		struct wd24_dac_entry ent;

		err = wd24_dac_entry(cmap->start + i, cmap->red[i],
				     cmap->green[i], cmap->blue[i], &ent);
		if (!err)
			err = wd24_dac_put_entry(&dio, ent.index,
						 ent.rgb[0], ent.rgb[1],
						 ent.rgb[2]);
		if (err)
			break;
	}
	if (err) {
		int rst = wd24_restore_cmap(hw);

		if (!rst && info->cmap.len == 256 && info->cmap.red) {
			wd24_dac_expand_image(hw->cmap, info->cmap.red,
					      info->cmap.green, info->cmap.blue);
			s6d_cmap_published = true;
		} else {
			s6d_cmap_published = false;
			fb_dealloc_cmap(&info->cmap);
		}
		hw_rel();
		pr_err("S6d: cmap put failed %d (snapshot restore %d)\n", err, rst);
		return err;
	}
	hw_rel();
	return 0;
}

static int s6d_fb_open(struct fb_info *info, int user)
{
	int err = 0;

	/* user=0 is fbcon. Refusing it makes fbcon_startup return NULL on
	 * the first registration, so do_take_over_console does not bind.
	 * Safe only when no framebuffer is already registered (fbcon's
	 * console driver is not in the table yet). /proc/fb must be empty. */
	if (!user)
		return s6d_bind ? 0 : -EBUSY;
	hw_acq();
	s6d_yield_ender();
	if (s6d_st && s6d_st->stopping)
		err = -ENODEV;
	else if (s6d_busy)
		err = -EBUSY;
#ifndef WD24S1_QEMU_FAKE
	else if (!s6d_st || !s6d_armed || !hw || hw_dead || hw->in_text ||
		 hw->nmismatch || !vmem || hw->no_vram || vram_dead)
		err = -ENODEV;
#else
	else if (!s6d_st || !s6d_armed)
		err = -ENODEV;
#endif
	else if (!s6d_st->active) {
		/* console_lock, then hw_lock. A fault already holds console. */
		s6d_ender = current;
		hw_rel();
#ifndef WD24S1_QEMU_FAKE
		console_lock();
#endif
		hw_acq();
#ifndef WD24S1_QEMU_FAKE
		if (hw) {
			hw->be->begin(hw);
			hw->tag = "S6d: fb open";
		}
#endif
		err = wd24s6b_begin(s6d_b, s6b_io ? s6b_io : &s6_live_io);
#ifndef WD24S1_QEMU_FAKE
		if (hw)
			hw->be->end(hw);
		console_unlock();
#endif
		s6d_ender = NULL;
		if (err) {
			s6d_b->state = WD24S6B_OFF;
		} else {
			s6d_st->active = true;
			s6d_busy = true;
			s6d_st->stopping = false;
			wd24s6d_thrash_reset(&s6d_st->thrash);
			s6d_st->mapped_base = ~0u;
		}
	} else {
		s6d_busy = true;
	}
	if (!err && s6d_cmap) {
		int perr = s6d_cmap_publish(info);

		if (perr) {
			s6d_busy = false;
			err = perr;
		}
	}
	hw_rel();
	if (err)
		pr_err("S6d: fb open failed %d\n", err);
	else if (s6d_busy)
		pr_info("S6d: session ON (fb open)\n");
	return err;
}

static int s6d_fb_release(struct fb_info *info, int user)
{
	if (!user)
		return 0;
	hw_acq();
	s6d_cmap_frozen = true;
	s6d_yield_ender();
	/* munmap already ran on process exit; this also covers close()
	 * while a VMA still exists. */
	s6d_drop_maps();
	s6d_end_locked("fb release");
	s6d_busy = false;
	hw_rel();
	return 0;
}

/* Banked CPU fill for fbcon — SETWIN + A0000 writes; no accel.
 * printk on console=tty0 re-enters these while the same task holds
 * hw_lock. Nest instead of taking the mutex again, and do not print
 * from inside the draw (that recurses). */
static bool s6d_draw_enter(bool *took)
{
	/* Any same-task reentry already holds hw_lock. Drawing here would
	 * run inside that critical section; locking again would sleep forever. */
	if (hw_nested())
		return false;
	*took = true;
	hw_acq();
	/* Do not yield here. fbcon calls this with console_lock held, and
	 * the end path is waiting for that lock. */
	if (!s6d_st || !s6d_st->active || s6d_st->stopping || !vmem) {
		hw_rel();
		return false;
	}
	s6d_in_draw++;
	return true;
}

static void s6d_draw_leave(bool took)
{
	if (s6d_in_draw)
		s6d_in_draw--;
	if (took)
		hw_rel();
}

static int s6d_ensure_off(u32 off)
{
	u32 w0, w1, base, win, pg;

	if (!s6d_st || !s6d_st->active || s6d_st->stopping || !vmem)
		return -ENODEV;
	base = wd24s6d_win_base_pg(off);
	win = s6d_st->mapped_base;
	pg = off / WD24S6D_PAGE;
	if (win != ~0u && pg >= win && pg < win + (WD24S6D_WIN / WD24S6D_PAGE))
		return 0;
	if (wd24s6d_thrash_note(&s6d_st->thrash, base)) {
		s6d_st->nthrash++;
		s6d_st->why = "thrash latch";
		return -EIO;
	}
	wd24s6d_win_for_off(off, &w0, &w1);
	if (wd24s6b_setwin(s6d_b, w0, w1))
		return -EIO;
	s6d_st->mapped_base = base;
	s6d_st->nswitch++;
	return 0;
}

static void s6d_fillrect(struct fb_info *info, const struct fb_fillrect *r)
{
	u32 x, y, w, h, color, row, col;

	(void)info;
	if (!r || !r->width || !r->height)
		return;
	x = r->dx; y = r->dy; w = r->width; h = r->height; color = r->color;
	if (x >= WD24S6D_XRES || y >= WD24S6D_YRES)
		return;
	if (x + w > WD24S6D_XRES)
		w = WD24S6D_XRES - x;
	if (y + h > WD24S6D_YRES)
		h = WD24S6D_YRES - y;
	{
		bool took = false;
		const char *why = NULL;

		if (!s6d_draw_enter(&took))
			return;
		for (row = 0; row < h && !why; row++) {
			for (col = 0; col < w; col++) {
				u32 off = (y + row) * WD24S6D_LINE + x + col;
				u32 local;

				if (s6d_ensure_off(off)) {
					why = s6d_st->why ? s6d_st->why : "?";
					break;
				}
				local = off - (s6d_st->mapped_base << 12);
				writeb((u8)color, vmem + local);
			}
		}
		s6d_draw_leave(took);
		if (why)
			pr_err("S6d: fillrect stop (%s)\n", why);
	}
}

static void s6d_copyarea(struct fb_info *info, const struct fb_copyarea *a)
{
	/* First cut: no soft copyarea across banks; fbcon scrolls via fill+blit */
	(void)info;
	(void)a;
}

static void s6d_imageblit(struct fb_info *info, const struct fb_image *img)
{
	const u8 *src;
	u32 x, y, w, h, fg, bg, row, col, src_stride;

	(void)info;
	if (!img || !img->data || img->depth != 1)
		return;
	x = img->dx; y = img->dy; w = img->width; h = img->height;
	fg = img->fg_color; bg = img->bg_color;
	if (x >= WD24S6D_XRES || y >= WD24S6D_YRES)
		return;
	if (x + w > WD24S6D_XRES)
		w = WD24S6D_XRES - x;
	if (y + h > WD24S6D_YRES)
		h = WD24S6D_YRES - y;
	src = img->data;
	src_stride = (img->width + 7) >> 3;
	{
		bool took = false;
		const char *why = NULL;

		if (!s6d_draw_enter(&took))
			return;
		for (row = 0; row < h && !why; row++) {
			for (col = 0; col < w; col++) {
				u32 off = (y + row) * WD24S6D_LINE + x + col;
				u32 local;
				u8 bit = src[row * src_stride + (col >> 3)] &
					 (0x80 >> (col & 7));
				u8 pix = bit ? (u8)fg : (u8)bg;

				if (s6d_ensure_off(off)) {
					why = s6d_st->why ? s6d_st->why : "?";
					break;
				}
				local = off - (s6d_st->mapped_base << 12);
				writeb(pix, vmem + local);
			}
		}
		s6d_draw_leave(took);
		if (why)
			pr_err("S6d: imageblit stop (%s)\n", why);
	}
}

static const struct fb_ops s6d_fbops = {
	.owner		= THIS_MODULE,
	.fb_open	= s6d_fb_open,
	.fb_release	= s6d_fb_release,
	.fb_mmap	= s6d_fb_mmap,
	.fb_fillrect	= s6d_fillrect,
	.fb_copyarea	= s6d_copyarea,
	.fb_imageblit	= s6d_imageblit,
};

static const struct fb_ops s6d_fbops_cmap = {
	.owner		= THIS_MODULE,
	.fb_open	= s6d_fb_open,
	.fb_release	= s6d_fb_release,
	.fb_mmap	= s6d_fb_mmap,
	.fb_fillrect	= s6d_fillrect,
	.fb_copyarea	= s6d_copyarea,
	.fb_imageblit	= s6d_imageblit,
	.fb_setcmap	= s6d_fb_setcmap,
};

static void s6d_free(void)
{
	bool took = !hw_nested();

	if (took)
		hw_acq();
	s6d_stop("free");
	if (took)
		hw_rel();
	if (s6d_info) {
		framebuffer_release(s6d_info);
		s6d_info = NULL;
	}
	vfree(s6d_st);
	vfree(s6d_b);
	s6d_st = NULL;
	s6d_b = NULL;
}

static int s6d_setup(void)
{
	struct fb_info *info;
	struct fb_var_screeninfo *var;
	struct fb_fix_screeninfo *fix;

	if (s6b) {
		pr_err("S6d: refuse s6d=1 with s6b=1 (use one dual-window consumer)\n");
		return -EINVAL;
	}
	s6d_st = vmalloc(sizeof(*s6d_st));
	s6d_b = vmalloc(sizeof(*s6d_b));
	if (!s6d_st || !s6d_b)
		return -ENOMEM;
	memset(s6d_b, 0, sizeof(*s6d_b));
	wd24s6d_init(s6d_st, s6d_b, s6d_thrash_bound);

	info = framebuffer_alloc(0, NULL);
	if (!info)
		return -ENOMEM;
	s6d_info = info;
	info->node = -1;		/* 0 would look linked before register */
	info->fbops = s6d_cmap ? &s6d_fbops_cmap : &s6d_fbops;
	info->flags = FBINFO_VIRTFB;
	info->screen_base = NULL;	/* no linear/shadow; mmap is fault-banked */
	info->screen_size = WD24S6D_MMAP_LEN;

	var = &info->var;
	var->xres = WD24S6D_XRES;
	var->yres = WD24S6D_YRES;
	var->xres_virtual = WD24S6D_XRES;
	var->yres_virtual = WD24S6D_YRES;
	var->bits_per_pixel = WD24S6D_BPP;
	var->red.offset = 0; var->red.length = 8;
	var->green.offset = 0; var->green.length = 8;
	var->blue.offset = 0; var->blue.length = 8;
	var->activate = FB_ACTIVATE_NOW;
	var->height = -1;
	var->width = -1;
	var->vmode = FB_VMODE_NONINTERLACED;

	fix = &info->fix;
	fix->id[0] = 'w'; fix->id[1] = 'd'; fix->id[2] = '2';
	fix->id[3] = '4'; fix->id[4] = 's'; fix->id[5] = '6';
	fix->id[6] = 'd'; fix->id[7] = '\0';
	fix->type = FB_TYPE_PACKED_PIXELS;
	fix->visual = FB_VISUAL_PSEUDOCOLOR;
	fix->line_length = WD24S6D_LINE;
	fix->smem_start = 0;			/* no contiguous phys */
	fix->smem_len = WD24S6D_MMAP_LEN;
	fix->ypanstep = 0;
	fix->ywrapstep = 0;

#ifndef WD24S1_QEMU_FAKE
	s6b_io = &s6_live_io;
#endif
	pr_info("S6d: fbdev prepared (thrash bound %u); arms after 640x480-8\n",
		s6d_thrash_bound);
	return 0;
}

static void s6d_arm(const struct wd24_mode *m)
{
	int err;

	if (!hw || hw->in_text || hw->nmismatch || !m || strcmp(m->name, "640x480-8") ||
	    !vmem || hw->no_vram || vram_dead) {
		pr_err("S6d: not armed: not in the 640x480-8 state\n");
		return;
	}
	if (!s6d_info || !s6d_st)
		return;
	s6d_cmap_frozen = false;
	s6d_cmap_published = false;
	s6d_armed = true;
	S6B_HW_BEGIN();
	err = wd24s6b_begin(s6d_b, s6b_io ? s6b_io : &s6_live_io);
	S6B_HW_END();
	if (err) {
		pr_err("S6d: begin at arm failed %d — not registering fb\n", err);
		s6d_armed = false;
		s6d_b->state = WD24S6B_OFF;
		return;
	}
	s6d_st->active = true;
	s6d_st->stopping = false;
	s6d_busy = false;	/* userspace open is separate from the arm session */
	if (!s6d_fbcon) {
		/* Unchanged: no fbdev node, so no fbcon and no mmap. */
		s6d_st->fb_reg = false;
		pr_info("S6d: armed WITHOUT register_framebuffer (s6d_fbcon=0; no fb node; pattern stays); thrash bound %u\n",
			s6d_thrash_bound);
		return;
	}
	if (!s6d_bind) {
		/* fb_open(0) returns -EBUSY without taking hw_lock, and
		 * fbcon_startup fails before any draw. Holding hw_lock
		 * keeps the trial worker out of the session. The module
		 * cannot see whether fbcon's console driver is already
		 * registered (filp_open/kernel_read are not exported);
		 * that check is a shell step before insmod. */
		err = register_framebuffer(s6d_info);
	} else {
		/* fbcon draw takes hw_lock under console_lock. Cannot hold
		 * hw_lock across register or that draw sleeps forever. */
		s6d_registering = true;
		hw_rel();
		err = register_framebuffer(s6d_info);
		hw_acq();
		s6d_registering = false;
	}
	if (!s6d_armed || (s6d_st && s6d_st->stopping)) {
		if (s6d_info && s6d_info->node >= 0) {
			s6d_st->fb_reg = true;
			s6d_stop("stopped during register");
		}
		return;
	}
	if (!s6d_bind) {
		int node = s6d_info ? s6d_info->node : -1;

		/* fbcon_startup failed: device is linked, VT was not bound.
		 * Any other result (including 0) means bind was not refused. */
		if (err == -ENODEV && node >= 0) {
			s6d_st->fb_reg = true;
			hw_rel();
			pr_info("S6d: registered fb%d WITHOUT fbcon bind (s6d_bind=0, fb_open(0) refused, register %d); thrash bound %u\n",
				node, err, s6d_thrash_bound);
			pr_info("S6d: STOP if dmesg shows Console: switching to colour — bind was not refused\n");
			hw_acq();
			return;
		}
		hw_rel();
		pr_err("S6d: s6d_bind=0 fail closed (register %d, node %d)\n",
		       err, node);
		hw_acq();
		if (node >= 0 && s6d_st) {
			s6d_st->fb_reg = true;
			s6d_stop("bind=0 refused result");
		} else {
			s6d_end_locked("bind=0 register failed");
			s6d_armed = false;
			s6d_busy = false;
		}
		return;
	}
	if (err || !s6d_info || s6d_info->node < 0) {
		int node = s6d_info ? s6d_info->node : -1;

		hw_rel();
		pr_err("S6d: register_framebuffer failed %d (node %d)\n", err, node);
		hw_acq();
		if (node >= 0 && s6d_st) {
			s6d_st->fb_reg = true;
			s6d_stop("register fail");
		} else {
			s6d_end_locked("register fail");
			s6d_armed = false;
			s6d_busy = false;
		}
		return;
	}
	s6d_st->fb_reg = true;
	{
		int node = s6d_info->node;

		/* Outside hw_lock: tty0 printk would re-enter imageblit. */
		hw_rel();
		pr_info("S6d: armed + registered fb%d (s6d_bind=1; fbcon may bind); thrash bound %u\n",
			node, s6d_thrash_bound);
		hw_acq();
	}
}

static ssize_t s6d_read(struct file *f, char __user *b, size_t n, loff_t *p)
{
	char local[640];
	int len;
	static const char busy[] = "wd24adv s6d: hw busy; retry\n";

	if (!hw_try())
		return rd(b, n, p, busy, sizeof(busy) - 1);
	if (!s6d_st) {
		len = scnprintf(local, sizeof(local), "s6d: not set up\n");
	} else {
		len = scnprintf(local, sizeof(local),
			"s6d: armed %d active %d stopping %d busy %d fb_reg %d node %d\n"
			"fbcon_param %d bind_param %d\n"
			"faults %u switches %u thrash %u latched %u mapped_base 0x%x\n"
			"why %s\n"
			"cmap_param %d published %d frozen %d\n",
			s6d_armed, s6d_st->active, s6d_st->stopping, s6d_busy,
			s6d_st->fb_reg, s6d_info ? s6d_info->node : -1,
			s6d_fbcon, s6d_bind,
			s6d_st->nfault, s6d_st->nswitch, s6d_st->nthrash,
			s6d_st->thrash.latched, s6d_st->mapped_base,
			s6d_st->why ? s6d_st->why : "-",
			s6d_cmap, s6d_cmap_published, s6d_cmap_frozen);
	}
	hw_rel();
	if (len < 0)
		return -EINVAL;
	return rd(b, n, p, local, (size_t)len);
}

static const struct proc_ops s6d_ops = {
	.proc_read = s6d_read, .proc_lseek = default_llseek };


/* live: claim the ports, map A0000, save the text state */
static int live_start(void)
{
	int err;

	hw = vmalloc(sizeof(*hw));
	hw_planes = vmalloc(4 * WD24_PLANE);
	live_txt = vmalloc(TXT_MAX);
	if (!hw || !hw_planes || !live_txt)
		return -ENOMEM;
	if (!request_region(VGA_PORTS, VGA_NPORTS, "wd24adv")) {
		pr_err("live: VGA ports 3C0-3DF busy\n");
		return -EBUSY;
	}
	have_ports = true;
	vmem = ioremap(0xa0000, WD24_PLANE);
	if (!vmem)
		return -ENOMEM;
	wd24_ctx_init(hw, &wd24_port_backend);
	hw_io = (struct wd24_io){ k_inb, k_outb, k_memr, k_memw, NULL };
	hw->io = &hw_io;
	hw->planes = hw_planes;
	hw->allow_clock = allow_clock;
	hw->fix_sr06 = fix_sr06;
	hw_acq();
	err = hw_run(OP_LOAD, NULL);
	hw_rel();
	return err;
}

static int __init wd24adv_init(void)
{
	const struct wd24_mode *m = NULL;
	int fails, err;
	bool sx = s3 || s4 || s5;	/* the S3 machinery runs each program */

	if (allow_routing || allow_crt) {
		pr_err("allow_routing/allow_crt refused: wd24adv has no panel/CRT routing\n");
		return -EPERM;
	}
	if (other_module_loaded("wd90c24fb")) {
		pr_err("wd90c24fb is loaded; unload it first (only one driver may own the chip)\n");
		return -EBUSY;
	}
	if (also_conflict && *also_conflict &&
	    other_module_loaded(also_conflict)) {
		pr_err("%s is loaded (also_conflict); refusing\n", also_conflict);
		return -EBUSY;
	}
	if (dryrun && live) {
		pr_err("live=1 needs dryrun=0 as well; nothing done\n");
		return -EINVAL;
	}
	if (s1 && (live || *mode)) {
		pr_err("s1=1 is a separate read-only probe: live=1 / mode= refused with it\n");
		return -EINVAL;
	}
	if (s2 && (s1 || live || *mode)) {
		pr_err("s2=1 is a separate stage: s1=1 / live=1 / mode= refused with it\n");
		return -EINVAL;
	}
	if (sx && (s1 || s2)) {
		pr_err("s3=1 / s4=1 / s5 are separate stages: s1=1 / s2=1 refused with them\n");
		return -EINVAL;
	}
	if (s6c && !s6) {
		pr_err("s6c=1 needs s6=1\n");
		return -EINVAL;
	}
	if (s6 && (s1 || s2 || sx)) {
		pr_err("s6=1 is a separate stage: s1-s5 refused with it\n");
		return -EINVAL;
	}
#ifdef WD24S1_QEMU_FAKE
	if (s6 && !dryrun && (live || *mode)) {
		pr_err("QEMU fake build: s6=1 runs on the model only, without live=1\n");
		return -EINVAL;
	}
#else
	if (s6 && !dryrun && (!live || strcmp(mode, "640x480-8"))) {
		pr_err("s6=1 with dryrun=0 needs live=1 mode=640x480-8\n");
		return -EINVAL;
	}
#endif
	if (s6b && (s1 || s2 || sx || s6)) {
		pr_err("s6b=1 is a separate stage: s1-s6 refused with it\n");
		return -EINVAL;
	}
#ifdef WD24S1_QEMU_FAKE
	if (s6b && !dryrun && (live || *mode)) {
		pr_err("QEMU fake build: s6b=1 runs on the model only, without live=1\n");
		return -EINVAL;
	}
#else
	if (s6b && !dryrun && (!live || strcmp(mode, "640x480-8"))) {
		pr_err("s6b=1 with dryrun=0 needs live=1 mode=640x480-8\n");
		return -EINVAL;
	}
#endif
	if (s5 < 0 || s5 > 2) {
		pr_err("s5 must be 1 (MONO) or 2 (HOSTCOL)\n");
		return -EINVAL;
	}
	if ((s3 + s4 + !!s5) > 1) {
		pr_err("s3=1, s4=1 and s5 are separate stages: only one\n");
		return -EINVAL;
	}
	if (s4)
		sprog = &wd24s3_prog4;
	if (s5)
		sprog = s5 == 2 ? &wd24s3_prog5h : &wd24s3_prog5;
#ifdef WD24S1_QEMU_FAKE
	if (sx && !dryrun && (live || *mode)) {
		pr_err("QEMU fake build: s3=1 runs on the model only, without live=1\n");
		return -EINVAL;
	}
#else
	if (sx && !dryrun && (!live || strcmp(mode, "640x480-8"))) {
		pr_err("%s=%d with dryrun=0 needs live=1 mode=640x480-8\n", sprog->param, s5 ? s5 : 1);
		return -EINVAL;
	}
#endif
	if (!s1_echo && !s1) {
		pr_err("s1_echo= needs s1=1\n");
		return -EINVAL;
	}
	if (wd24_cmap_param_refuse(s6d_cmap, s6d, dryrun, live, s6d_bind,
				   s6d_fbcon,
				   mode && !strcmp(mode, "640x480-8"),
				   s1 || s2 || s3 || s4 || s5 || s6 || s6b || s6c)) {
		pr_err("s6d_cmap=1 refused: needs s6d=1 s6d_fbcon=1 s6d_bind=0 dryrun=0 live=1 mode=640x480-8 and no other stage\n");
		return -EINVAL;
	}
#ifdef WD24S1_QEMU_FAKE
	if (!dryrun && !live && !s1 && !s2 && !sx && !s6 && !s6b) {
#else
	if (!dryrun && !live && !s1 && !s2) {
#endif
		pr_err("dryrun=0 without live=1, s1=1 or s2=1 refused\n");
		return -EOPNOTSUPP;
	}
	if (!live && *mode) {
		pr_err("mode= needs dryrun=0 live=1\n");
		return -EINVAL;
	}
	if (s2 && !dryrun && other_module_loaded("vga16fb")) {
		pr_err("s2: vga16fb is loaded; refusing\n");
		return -EBUSY;
	}
	if (s1 && !dryrun && other_module_loaded("vga16fb")) {
		pr_err("s1: vga16fb is loaded; refusing\n");
		return -EBUSY;
	}
	if (live) {
		if (trial < TRIAL_MIN || trial > TRIAL_MAX) {
			pr_err("live: trial=%u refused (%u..%u s; it cannot be disabled)\n",
			       trial, TRIAL_MIN, TRIAL_MAX);
			return -EINVAL;
		}
		if (other_module_loaded("vga16fb")) {
			pr_err("live: vga16fb is loaded; refusing\n");
			return -EBUSY;
		}
		if (*mode) {
			m = wd24_mode_find(mode);
			if (!m || m->status != WD24_PROVEN) {
				pr_err("live: mode=%s is not a PROVEN mode\n", mode);
				return -EINVAL;
			}
		}
	}

	ctx = vmalloc(sizeof(*ctx));
	if (ctx)
		memset(ctx, 0, sizeof(*ctx));
	plan_txt = vmalloc(TXT_MAX);
	modes_txt = vmalloc(TXT_MAX);
	self_txt = vmalloc(TXT_MAX);
	if (!ctx || !plan_txt || !modes_txt || !self_txt) {
		free_all();
		return -ENOMEM;
	}

	build_modes();
	fails = run_selftest(ctx);
	build_plan();

	if (s1 && dryrun) {
		s1_txt = vmalloc(TXT_MAX);
		if (!s1_txt) {
			free_all();
			return -ENOMEM;
		}
		s1_plan_text();
	}
	if (s1 && !dryrun) {
		if (fails) {
			pr_err("s1: self-test failed; no hardware access\n");
			free_all();
			return -EINVAL;
		}
		err = s1_live();
		if (err) {
			pr_err("s1: not run (%d); nothing touched\n", err);
			free_all();
			return err;
		}
	}
	if (s2 && dryrun) {
		s2_txt = vmalloc(TXT_MAX);
		if (!s2_txt) {
			free_all();
			return -ENOMEM;
		}
		s2_plan_text();
	}
	if (s2 && !dryrun) {
		if (fails) {
			pr_err("s2: self-test failed; no hardware access\n");
			free_all();
			return -EINVAL;
		}
		err = s2_live();
		if (err) {
			pr_err("s2: not run (%d); nothing touched\n", err);
			free_all();
			return err;
		}
	}
	if (sx && dryrun) {
		s3_txt = vmalloc(TXT_MAX);
		if (!s3_txt) {
			free_all();
			return -ENOMEM;
		}
		s3_plan_text();
	}
#ifdef WD24S1_QEMU_FAKE
	if (s6 && (fails || s6_model_run())) {
#else
	if (s6 && (dryrun ? s6_model_run() : s6_alloc())) {
#endif
		free_all();
		return -ENOMEM;
	}
	if (s6b && (fails || s6b_setup())) {
		pr_err("S6b: setup failed; nothing touched\n");
		free_all();
		return -EINVAL;
	}
	if (s6d && (fails || s6d_setup())) {
		pr_err("S6d: setup failed; nothing touched\n");
		free_all();
		return -EINVAL;
	}
#ifdef WD24S1_QEMU_FAKE
	if (sx && !dryrun) {
		if (fails || s3_fake_run()) {
			free_all();
			return -EINVAL;
		}
	}
#else
	if (sx && !dryrun && s3_alloc()) {
		free_all();
		return -ENOMEM;
	}
#endif
	if (live) {
		if (fails) {
			pr_err("live: self-test failed; no hardware access\n");
			free_all();
			return -EINVAL;
		}
		err = live_start();
		if (err && hw && hw->hw_data) {
			/*
			 * Failed after writing (a readback mismatch): the saved state
			 * was already restored.  Stay loaded but inert instead of
			 * failing, because busybox insmod retries a failed
			 * finit_module() with init_module() and would run the whole
			 * load a second time.  rmmod then touches nothing.
			 */
			hw_dead = true;
			pr_err("live: load failed after writes (%d); state restored; module is INERT (see /proc/wd24adv/live, then rmmod)\n",
			       err);
		} else if (err) {
			pr_err("live: load failed (%d); see the lines above (nothing written)\n", err);
			free_all();
			return err;
		}
	}

	pdir = proc_mkdir("wd24adv", NULL);
	if (!pdir ||
	    !proc_create_data("plan", 0444, pdir, &plan_ops, NULL) ||
	    !proc_create_data("modes", 0444, pdir, &modes_ops, NULL) ||
	    !proc_create_data("selftest", 0444, pdir, &self_ops, NULL) ||
	    (s1 && !proc_create_data("s1", 0444, pdir, &s1_ops, NULL)) ||
	    (s2 && !proc_create_data("s2", 0444, pdir, &s2_ops, NULL)) ||
	    (sx && !proc_create_data(sprog->param, 0444, pdir, &s3_ops, NULL)) ||
	    (s6 && !proc_create_data("s6", 0444, pdir, &s6_ops, NULL)) ||
	    (s6b && !proc_create_data("s6b", 0444, pdir, &s6b_ops, NULL)) ||
	    (s6d && !proc_create_data("s6d", 0444, pdir, &s6d_ops, NULL)) ||
	    (live && (!proc_create_data("live", 0444, pdir, &live_ops, NULL) ||
		      !proc_create_data("control", 0200, pdir, &control_ops, NULL)))) {
		if (live && !hw_dead) {
			hw_acq();
			vram_gate("load failure: plane restore");
			hw_run(OP_TEXT, NULL);
			hw_rel();
		}
		free_all();
		return -ENOMEM;
	}
	if (hw_dead)
		return 0;
	if (s1) {
		pr_info("loaded, S1 %s; see /proc/wd24adv/s1\n",
			dryrun ? "plan only (dry-run, no port access)" : "done (no port region held)");
		return 0;
	}
	if (s2) {
		pr_info("loaded, S2 %s; see /proc/wd24adv/s2\n",
			dryrun ? "plan only (dry-run, no port access)" : "done, all restored inside insmod (no port region held)");
		return 0;
	}
	if (s6 && !live) {
		pr_info("loaded, S6 %s; see /proc/wd24adv/s6\n",
			dryrun ? "run on the model (dry-run, no port access)" : "QEMU fake run on the model done");
		return 0;
	}
	if (s6b && !live) {
		pr_info("loaded, S6b %s; see /proc/wd24adv/s6b\n",
			dryrun ? "model session run (dry-run, no port access)" :
			"QEMU fake: /dev/wd24s6b on the model");
		return 0;
	}
	if (sx && !live) {
		pr_info("loaded, %s %s; see /proc/wd24adv/%s\n", sprog->stage,
			dryrun ? "plan only (dry-run, no port access)" : "QEMU fake run done",
			sprog->param);
		return 0;
	}
	if (!live) {
		pr_info("loaded, dry-run (%u modes, selftest %s); see /proc/wd24adv/\n",
			wd24_nmodes, fails ? "FAILED" : "ok");
		return 0;
	}
	pr_info("LIVE: text state saved (4 planes, DAC, VGA + ext regs); trial %u s; control: /proc/wd24adv/control\n",
		trial);
	if (m) {
		hw_acq();
		err = hw_mode(m);
		hw_rel();
		if (err)
			pr_err("live: mode %s failed (%d); text restored, module stays loaded\n",
			       m->name, err);
		if (sx) {
			hw_acq();
			if (err)
				s3_report("the mode set failed");
			else
				s3_live(m);
			hw_rel();
		}
		if (s6 && !err) {
			hw_acq();
			s6_live(m);
			hw_rel();
		}
		if (s6b && !err) {
			hw_acq();
			s6b_arm(m);
			hw_rel();
		}
		if (s6d && !err) {
			hw_acq();
			s6d_arm(m);
			hw_rel();
		}
	}
	return 0;
}

static void __exit wd24adv_exit(void)
{
	if (hw && !hw_dead) {
		/* proc first: no new control writes; then no timer; then restore */
		if (pdir)
			proc_remove(pdir);
		pdir = NULL;
		WRITE_ONCE(wd24_exiting, true);
		cancel_delayed_work_sync(&trial_work);
		/* trial_fn may have queued one more delay as it exited. */
		cancel_delayed_work_sync(&trial_work);
		hw_acq();
		/* not reached while pinned; defensive: registers only if locked */
		if (!vram_gate("rmmod: plane restore") || !hw->in_text)
			hw_run(OP_TEXT, NULL);
		hw_rel();
	}
	free_all();
}

module_init(wd24adv_init);
module_exit(wd24adv_exit);

MODULE_DESCRIPTION("WD90C24 advanced driver (dry-run default; gated live backend; ThinkPad 755C)");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: fb");

