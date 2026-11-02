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
MODULE_PARM_DESC(pattern, "live: draw 16 colour bars after a packed mode set (default 1)");

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
static struct wd24_io hw_io;
static char *live_txt;
static size_t live_len;
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
		if (!err && pattern && m->ext == WD24_EXT_PACKED8)
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
	if (hw && hw->trial_pending)
		hw_run(OP_EXPIRE, NULL);
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
	plan_txt = modes_txt = self_txt = NULL;
	ctx = NULL;
	free_hw();
}

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
	if (!dryrun && !live) {
		pr_err("dryrun=0 without live=1 refused\n");
		return -EOPNOTSUPP;
	}
	if (!live && *mode) {
		pr_err("mode= needs dryrun=0 live=1\n");
		return -EINVAL;
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
	    (live && (!proc_create_data("live", 0444, pdir, &live_ops, NULL) ||
		      !proc_create_data("control", 0200, pdir, &control_ops, NULL)))) {
		if (live && !hw_dead) {
			mutex_lock(&hw_lock);
			hw_run(OP_TEXT, NULL);
			mutex_unlock(&hw_lock);
		}
		free_all();
		return -ENOMEM;
	}
	if (hw_dead)
		return 0;
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
		hw_run(OP_TEXT, NULL);
		mutex_unlock(&hw_lock);
	}
	free_all();
}

module_init(wd24adv_init);
module_exit(wd24adv_exit);

MODULE_DESCRIPTION("WD90C24 advanced driver (dry-run default; gated live backend; ThinkPad 755C)");
MODULE_LICENSE("GPL");
