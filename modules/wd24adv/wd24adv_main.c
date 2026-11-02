// SPDX-License-Identifier: GPL-2.0
/*
 * wd24adv - experimental advanced driver for the WD90C24/WD90C24A(2)
 * (ThinkPad 755C).  DRY-RUN ONLY.
 *
 * This is a separate module from wd90c24fb.  It refuses to load while
 * wd90c24fb is loaded.  In this build it never touches a hardware register:
 * the only backend is the dry-run recorder, which runs every plan against a
 * model of the 755C boot state (live reads + POST trace + standard VGA) and
 * logs the register writes it WOULD perform.
 *
 * There is no panel/CRT routing at all: allow_routing=1 and allow_crt=1 are
 * refused at load.  Every write is a standard VGA register or one that
 * wd90c24fb already performs (see wd24adv_core.c).
 *
 *   /proc/wd24adv/plan     - ordered writes: load, every mode, text restore
 *   /proc/wd24adv/modes    - mode table, PROVEN/UNPROVEN, evidence
 *   /proc/wd24adv/selftest - shared guard/plan self-test (same as the host)
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

#include "wd24adv_core.h"

static bool dryrun = true;
module_param(dryrun, bool, 0444);
MODULE_PARM_DESC(dryrun, "record register writes only (the only mode in this build; 0 is refused)");

static bool allow_routing;
module_param(allow_routing, bool, 0444);
MODULE_PARM_DESC(allow_routing, "REMOVED: runtime panel/CRT routing does not exist; 1 is refused");

static bool allow_crt;
module_param(allow_crt, bool, 0444);
MODULE_PARM_DESC(allow_crt, "REMOVED: no CRT modes; 1 is refused");

static bool allow_clock;
module_param(allow_clock, bool, 0444);
MODULE_PARM_DESC(allow_clock, "permit PR68 b4:3 / PR69 clock writes; no table mode needs them (default 0)");

static unsigned int trial = 10;
module_param(trial, uint, 0444);
MODULE_PARM_DESC(trial, "reserved: auto-revert to text after N seconds (future real backend; unused)");

static char *also_conflict = "";
module_param(also_conflict, charp, 0444);
MODULE_PARM_DESC(also_conflict, "test aid: also refuse to load if this module is loaded (wd90c24fb is always checked)");

#define TXT_MAX	(96 * 1024)

static struct proc_dir_entry *pdir;
static struct wd24_ctx *ctx;
static char *plan_txt, *modes_txt, *self_txt;
static size_t plan_len, modes_len, self_len;

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
}

static int __init wd24adv_init(void)
{
	int fails;

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
	if (!dryrun) {
		pr_err("dryrun=0 refused: no hardware backend in this build\n");
		return -EOPNOTSUPP;
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

	pdir = proc_mkdir("wd24adv", NULL);
	if (!pdir ||
	    !proc_create_data("plan", 0444, pdir, &plan_ops, NULL) ||
	    !proc_create_data("modes", 0444, pdir, &modes_ops, NULL) ||
	    !proc_create_data("selftest", 0444, pdir, &self_ops, NULL)) {
		free_all();
		return -ENOMEM;
	}
	pr_info("loaded, dry-run only (%u modes, selftest %s); see /proc/wd24adv/\n",
		wd24_nmodes, fails ? "FAILED" : "ok");
	return 0;
}

static void __exit wd24adv_exit(void)
{
	free_all();
}

module_init(wd24adv_init);
module_exit(wd24adv_exit);

MODULE_DESCRIPTION("WD90C24 advanced driver (dry-run, ThinkPad 755C)");
MODULE_LICENSE("GPL");
