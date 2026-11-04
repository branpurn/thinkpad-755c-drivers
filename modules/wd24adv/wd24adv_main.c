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
 * text after trial= seconds (5..120, default 15, cannot be disabled) unless
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
#include <linux/workqueue.h>
#include <linux/jiffies.h>

#include "wd24adv_core.h"
#include "wd24adv_s2.h"
#include "wd24adv_s3.h"
#include "wd24adv_s1.h"

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
MODULE_PARM_DESC(pattern, "live: draw 16 colour bars after a packed or 320x200 mode set (default 1)");

static bool allow_routing;
module_param(allow_routing, bool, 0444);
MODULE_PARM_DESC(allow_routing, "REMOVED: runtime panel/CRT routing does not exist; 1 is refused");

static bool allow_crt;
module_param(allow_crt, bool, 0444);
MODULE_PARM_DESC(allow_crt, "REMOVED: no CRT modes; 1 is refused");

static bool allow_clock;
module_param(allow_clock, bool, 0444);
MODULE_PARM_DESC(allow_clock, "permit PR68 b4:3 / PR69 clock writes; no table mode needs them (default 0)");

static unsigned int trial = 15;
module_param(trial, uint, 0444);
MODULE_PARM_DESC(trial, "live: revert a mode to text after N s unless confirmed (5..120, default 15; 0 refused)");

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

#define TXT_MAX	(96 * 1024)

#define TRIAL_MIN	5
#define TRIAL_MAX	120
#define VGA_PORTS	0x3c0
#define VGA_NPORTS	0x20

static struct proc_dir_entry *pdir;
static struct wd24_ctx *ctx;
static char *plan_txt, *modes_txt, *self_txt;
static size_t plan_len, modes_len, self_len;

/* live state; every hardware operation runs under hw_lock + console_lock */
static DEFINE_MUTEX(hw_lock);
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
	ssize_t r;

	mutex_lock(&hw_lock);
	r = rd(b, n, p, live_txt, live_len);
	mutex_unlock(&hw_lock);
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
	APPEND(plan_txt, len, "allow_clock=%d; [tag] = the wd90c24fb code the write mirrors\n",
	       allow_clock);
	model_header(&len);

	for (i = 0; i < wd24_nmodes; i++) {
		const struct wd24_mode *m = &wd24_modes[i];

		wd24_ctx_init(ctx, &wd24_dryrun_backend);
		wd24_model_boot(ctx);
		ctx->allow_clock = allow_clock;
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

static int hw_run(enum hw_op op, const struct wd24_mode *m)
{
	unsigned long d0 = hw->hw_data, i0 = hw->hw_index;
	unsigned int mis0 = hw->nmismatch, i;
	size_t len = 0;
	int err;

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
	mutex_lock(&hw_lock);
	if (hw && hw->trial_pending) {
		vram_gate("trial expiry: plane restore");	/* sets no_vram if locked */
		hw_run(OP_EXPIRE, NULL);
	}
	mutex_unlock(&hw_lock);
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
	mutex_lock(&hw_lock);
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
	mutex_unlock(&hw_lock);
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
	wd24s1_init(s1st, &wd24s1_kernel_io);
	s1st->strict_echo = s1_echo;

	mutex_lock(&hw_lock);
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
	mutex_unlock(&hw_lock);
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
	wd24s2_init(s2st, &wd24s2_kernel_io);

	mutex_lock(&hw_lock);
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
	mutex_unlock(&hw_lock);
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

	APPEND(s3_txt, len, "wd24adv S3 plan (dryrun=1: no port was read or written)\n");
	APPEND(s3_txt, len, "runs once inside insmod dryrun=0 live=1 mode=640x480-8 s3=1, after the trial mode set + pattern (timer queued)\n");
	APPEND(s3_txt, len, "1. 23C0h read twice == %04x, 3C0h/3C1h alias check; snapshot 25 words twice (agree, echo, CNTRL1 ACT=0, cursor enable=0)\n", WD24S2_S);
	APPEND(s3_txt, len, "2. VRAM (reads; PR0A 00-50h): banks 0-5 checksummed; PR0A 60h: bank 6 read twice == ; every op must change >= 1 byte\n");
	APPEND(s3_txt, len, "3. per op: W 23C0h 1001, R; poll 23C2h ACT=0 (<= %u reads); params W sel, R, W, R == ; W 1001, R, R ACT=0; W CNTRL1 (last); poll ACT=0; bank 6 == model\n", WD24S3_SPINS);
	for (j = 0; j < WD24S3_NOPS; j++) {
		const struct wd24s3_op *o = &wd24s3_ops[j];

		APPEND(s3_txt, len, "   op %u %-30s:", j + 1, o->name);
		for (p = 0; p < o->npar; p++)
			APPEND(s3_txt, len, " %04x", (o->par[p].idx << 12) | o->par[p].val);
		APPEND(s3_txt, len, " CNTRL1 %04x\n", o->cntrl1);
	}
	APPEND(s3_txt, len, "4. restore every written parameter to the snapshot (W, R); post-pass 24 words == snapshot, ACT=0; banks 0-5 checksums equal; PR0A 00; W 23C0h %04x, R ==\n", WD24S2_S);
	APPEND(s3_txt, len, "timeout: stop, no further write of any kind (no abort), no VRAM access; the trial expiry restores text\n");
	APPEND(s3_txt, len, "never written: block 2 (cursor), BG, C, D, F, 23C4h; CNTRL1 only 0908/0900 (forward, packed)\n");
	s3_len = len;
}

static void s3_report(const char *pre)
{
	const struct wd24s3 *s = s3st;
	size_t len = 0;
	unsigned int j, k;

	APPEND(s3_txt, len, "wd24adv S3 result: %s%s%s\n", pre ? "NOT RUN: " : wd24s3_res_name(s->res),
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
	APPEND(s3_txt, len, "\nchecksums 0-5 before:");
	for (k = 0; k < WD24S3_NCRC; k++)
		APPEND(s3_txt, len, " %08x", s->crc0[k]);
	APPEND(s3_txt, len, "\nchecksums 0-5 after: ");
	for (k = 0; k < WD24S3_NCRC; k++)
		APPEND(s3_txt, len, " %08x", s->crc1[k]);
	APPEND(s3_txt, len, "\n");
	for (j = 0; j < WD24S3_NOPS; j++)
		APPEND(s3_txt, len, "  op %u %-30s model changes %u; idle polls %u; busy %s, polls %u; CNTRL1 after %04x%s\n",
		       j + 1, wd24s3_ops[j].name, s->changes[j], s->idle_spins[j],
		       s->busy_seen[j] ? "seen" : "not seen", s->spins[j], s->c1_after[j],
		       j < s->done ? " VERIFIED" : "");
	if (s->ndiff)
		APPEND(s3_txt, len, "VRAM: %u bytes differ; first %05x read %02x want %02x\n",
		       s->ndiff, s->diff_off, s->diff_got, s->diff_want);
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
	APPEND(s3_txt, len, "23C0h writes %u, 23C2h writes %u (starts %u), PR0A writes %u, 23C4h 0, refusals %u, reads %u (polls %u), restore fails %x\n",
	       s->wix, s->wdata, s->wgo, s->wbank, s->refused, s->reads, s->polls, s->restore_fail);
	APPEND(s3_txt, len, "access log (in order; polls: first and last only; W8 03cf 09vv = PR0A):\n");
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

/* caller holds hw_lock; the trial mode is set and its timer queued */
static void s3_live(const struct wd24_mode *m)
{
	const char *pre = NULL;

	wd24s3_init(s3st, &wd24s3_kernel_io, &s3_memops, s3_mem,
		    s3_mem + WD24S3_WIN, s3_mem + 2 * WD24S3_WIN);
#ifndef WD24S1_QEMU_FAKE
	if (!hw || hw->in_text || !hw->trial_pending || hw->nmismatch ||
	    !m || strcmp(m->name, "640x480-8") || !vmem)
		pre = "not in the 640x480-8 trial state";
#endif
	if (!pre && !request_region(WD24S1_P_INDEX, 4, "wd24adv s3"))
		pre = "ports 23C0-23C3 busy";
	if (pre) {
		s3_report(pre);
		pr_err("S3: not run: %s\n", pre);
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
	release_region(WD24S1_P_INDEX, 4);
	s3_report(NULL);
	pr_info("S3: %s%s%s; ops verified %u; 23C2h writes %u (starts %u), PR0A writes %u; details: /proc/wd24adv/s3\n",
		wd24s3_res_name(s3st->res), s3st->why ? ": " : "",
		s3st->why ? s3st->why : "", s3st->done, s3st->wdata, s3st->wgo, s3st->wbank);
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
	mutex_lock(&hw_lock);
	s3_live(NULL);
	mutex_unlock(&hw_lock);
	vfree(wd24s3_fake_vram);
	wd24s3_fake_vram = NULL;
	return 0;
}
#endif

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
	mutex_lock(&hw_lock);
	err = hw_run(OP_LOAD, NULL);
	mutex_unlock(&hw_lock);
	return err;
}

static int __init wd24adv_init(void)
{
	const struct wd24_mode *m = NULL;
	int fails, err;

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
	if (s3 && (s1 || s2)) {
		pr_err("s3=1 is a separate stage: s1=1 / s2=1 refused with it\n");
		return -EINVAL;
	}
#ifdef WD24S1_QEMU_FAKE
	if (s3 && !dryrun && (live || *mode)) {
		pr_err("QEMU fake build: s3=1 runs on the model only, without live=1\n");
		return -EINVAL;
	}
#else
	if (s3 && !dryrun && (!live || strcmp(mode, "640x480-8"))) {
		pr_err("s3=1 with dryrun=0 needs live=1 mode=640x480-8\n");
		return -EINVAL;
	}
#endif
	if (!s1_echo && !s1) {
		pr_err("s1_echo= needs s1=1\n");
		return -EINVAL;
	}
#ifdef WD24S1_QEMU_FAKE
	if (!dryrun && !live && !s1 && !s2 && !s3) {
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
	if (s3 && dryrun) {
		s3_txt = vmalloc(TXT_MAX);
		if (!s3_txt) {
			free_all();
			return -ENOMEM;
		}
		s3_plan_text();
	}
#ifdef WD24S1_QEMU_FAKE
	if (s3 && !dryrun) {
		if (fails || s3_fake_run()) {
			free_all();
			return -EINVAL;
		}
	}
#else
	if (s3 && !dryrun && s3_alloc()) {
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
	    (s3 && !proc_create_data("s3", 0444, pdir, &s3_ops, NULL)) ||
	    (live && (!proc_create_data("live", 0444, pdir, &live_ops, NULL) ||
		      !proc_create_data("control", 0200, pdir, &control_ops, NULL)))) {
		if (live && !hw_dead) {
			mutex_lock(&hw_lock);
			vram_gate("load failure: plane restore");
			hw_run(OP_TEXT, NULL);
			mutex_unlock(&hw_lock);
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
	if (s3 && !live) {
		pr_info("loaded, S3 %s; see /proc/wd24adv/s3\n",
			dryrun ? "plan only (dry-run, no port access)" : "QEMU fake run done");
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
		mutex_lock(&hw_lock);
		err = hw_mode(m);
		mutex_unlock(&hw_lock);
		if (err)
			pr_err("live: mode %s failed (%d); text restored, module stays loaded\n",
			       m->name, err);
		if (s3) {
			mutex_lock(&hw_lock);
			if (err)
				s3_report("the mode set failed");
			else
				s3_live(m);
			mutex_unlock(&hw_lock);
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
		cancel_delayed_work_sync(&trial_work);
		mutex_lock(&hw_lock);
		/* not reached while pinned; defensive: registers only if locked */
		if (!vram_gate("rmmod: plane restore") || !hw->in_text)
			hw_run(OP_TEXT, NULL);
		mutex_unlock(&hw_lock);
	}
	free_all();
}

module_init(wd24adv_init);
module_exit(wd24adv_exit);

MODULE_DESCRIPTION("WD90C24 advanced driver (dry-run default; gated live backend; ThinkPad 755C)");
MODULE_LICENSE("GPL");
