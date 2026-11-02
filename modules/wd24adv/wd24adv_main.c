// SPDX-License-Identifier: GPL-2.0
/*
 * wd24adv - experimental advanced driver for the WD90C24/WD90C24A(2)
 * (ThinkPad 755C).  SCAFFOLD: dry-run only.
 *
 * This is a separate module from wd90c24fb.  It refuses to load while
 * wd90c24fb is loaded.  In this build it never touches a hardware register:
 * the only backend is the dry-run recorder, which logs the register writes
 * and delays that a mode switch / routing change WOULD perform, so the
 * sequences and the safety interlocks can be reviewed (and unit-tested on
 * the host) before the Stage 0 BIOS trace supplies the WD extended values.
 *
 *   /proc/wd24adv/plan   - the recorded sequence for mode= / route=
 *   /proc/wd24adv/modes  - the mode table and its validation result
 *   /proc/wd24adv/selftest - guard self-test results
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
#include <linux/delay.h>

#include "wd24adv_core.h"

static bool dryrun = true;
module_param(dryrun, bool, 0444);
MODULE_PARM_DESC(dryrun, "record register writes only (the only mode in this build; 0 is refused)");

static bool allow_routing;
module_param(allow_routing, bool, 0444);
MODULE_PARM_DESC(allow_routing, "permit panel power sequencing and panel/CRT routing changes (default 0)");

static bool allow_crt;
module_param(allow_crt, bool, 0444);
MODULE_PARM_DESC(allow_crt, "permit CRT-only modes/routing with the panel powered down (default 0)");

static bool allow_clock;
module_param(allow_clock, bool, 0444);
MODULE_PARM_DESC(allow_clock, "permit PR68/PR69 clock register writes (default 0)");

static char *mode = "320x200-8";
module_param(mode, charp, 0444);
MODULE_PARM_DESC(mode, "mode to plan (see /proc/wd24adv/modes)");

static char *route = "panel";
module_param(route, charp, 0444);
MODULE_PARM_DESC(route, "panel|crt|both: target routing for the plan (changing it needs allow_routing=1)");

static char *also_conflict = "";
module_param(also_conflict, charp, 0444);
MODULE_PARM_DESC(also_conflict, "test aid: also refuse to load if this module is loaded (wd90c24fb is always checked)");

static unsigned int trial = 10;
module_param(trial, uint, 0444);
MODULE_PARM_DESC(trial, "seconds before auto-revert to text (reserved; real backend only)");

#define TXT_MAX	(64 * 1024)

static struct proc_dir_entry *pdir;
static struct wd24_ctx *ctx;
static char *plan_txt, *modes_txt, *self_txt;
static size_t plan_len, modes_len, self_len;

/*
 * Only one of wd90c24fb / wd24adv may drive the chip.  wd90c24fb's source
 * may not be changed, and the trimmed kernel exports no module lookup, so
 * walk the module list from our own node (TINY_RCU, UP: rcu_read_lock is
 * enough).  The list head itself lives in kernel .data; every struct
 * module lives in the module area, which on i386 is inside the vmalloc
 * range - that is how the head is told apart and skipped.
 */
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

static size_t format_events(char *buf, size_t len, const struct wd24_ctx *c)
{
	unsigned int i;

	for (i = 0; i < c->nev; i++) {
		const struct wd24_event *e = &c->ev[i];

		switch (e->type) {
		case WD24_EV_WRITE:
			APPEND(buf, len, "  W %-4s %02x: %02x -> %02x\n",
			       wd24_rf_name(e->rf), e->idx, e->old, e->val);
			break;
		case WD24_EV_DELAY:
			APPEND(buf, len, "  DELAY %u ms\n", e->ms);
			break;
		case WD24_EV_REFUSED:
			APPEND(buf, len, "  REFUSED %-4s %02x: %02x -> %02x (%d)\n",
			       wd24_rf_name(e->rf), e->idx, e->old, e->val,
			       e->err);
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
	APPEND(modes_txt, len, "%-16s %5s %5s %5s %5s %3s bios stage flags     regs\n",
	       "name", "xres", "yres", "vxres", "vyres", "bpp");
	for (i = 0; i < wd24_nmodes; i++) {
		const struct wd24_mode *m = &wd24_modes[i];

		APPEND(modes_txt, len, "%-16s %5u %5u %5u %5u %3u  %02x  %u    %04x  %3u%s\n",
		       m->name, m->xres, m->yres, m->vxres, m->vyres, m->bpp,
		       m->bios_mode, m->stage, m->flags, m->nregs,
		       (m->flags & WD24M_STAGE0) ? "  [pending Stage 0]" : "");
	}
	modes_len = len;
}

/* In-kernel guard self-test: the interlocks must refuse by default. */
static int check(size_t *len, bool ok, const char *what)
{
	APPEND(self_txt, *len, "%s %s\n", ok ? "PASS" : "FAIL", what);
	return ok ? 0 : 1;
}

static int run_selftest(struct wd24_ctx *t)
{
	size_t len = 0;
	int fail = 0;

	wd24_ctx_init(t, &wd24_dryrun_backend);
	t->panel = WD24_PANEL_ON;
	t->reg[WD24_SR][WD24_SR_PR57] = WD24_PR57_PNLON;
	t->reg[WD24_CR][WD24_CR_PR19] = WD24_PR19_PANEL;
	fail += check(&len, wd24_update(t, WD24_CR, WD24_CR_PR19, 0x30, 0x20) == -EPERM,
		      "PR19 route change refused without allow_routing");
	fail += check(&len, wd24_write(t, WD24_CR, WD24_CR_PR1B, WD24_PR1B_SHADOW) == -EPERM,
		      "shadow CRTC unlock refused without allow_routing");
	fail += check(&len, wd24_update(t, WD24_SR, WD24_SR_PR57, 0x04, 0x00) == -EPERM,
		      "PNLOFF refused outside the panel sequence");
	fail += check(&len, wd24_panel_off(t) == -EPERM,
		      "panel_off refused without allow_routing");
	t->allow_routing = true;
	fail += check(&len, wd24_update(t, WD24_CR, WD24_CR_PR19, 0x30, 0x20) == -EBUSY,
		      "PR19 route change refused while the panel is powered");
	fail += check(&len, wd24_update(t, WD24_GR, WD24_GR_PR4, 0x20, 0x20) == -EPERM,
		      "tri-state refused outside the panel sequence");
	fail += check(&len, wd24_set_route(t, WD24_ROUTE_CRT, NULL, 0) == -EPERM,
		      "CRT-only route refused without allow_crt");
	fail += check(&len, wd24_panel_off(t) == 0 && t->panel == WD24_PANEL_OFF,
		      "panel_off with allow_routing");
	fail += check(&len, wd24_update(t, WD24_CR, WD24_CR_PR19, 0x30, 0x20) == 0,
		      "PR19 route change allowed with the panel off");
	fail += check(&len, wd24_update(t, WD24_GR, WD24_GR_PR1, 0x30, 0x10) == -EPERM,
		      "aperture bits (PR1 b5:4) always refused");
	fail += check(&len, wd24_write(t, WD24_SR, WD24_SR_PR68, 0x00) == -EPERM,
		      "PR68 refused without allow_clock");
	APPEND(self_txt, len, "%d failure(s)\n", fail);
	self_len = len;
	return fail;
}

static enum wd24_route parse_route(const char *s)
{
	if (!strcmp(s, "panel"))
		return WD24_ROUTE_PANEL;
	if (!strcmp(s, "crt"))
		return WD24_ROUTE_CRT;
	if (!strcmp(s, "both"))
		return WD24_ROUTE_BOTH;
	return 0;
}

static void build_plan(void)
{
	const struct wd24_mode *m = wd24_mode_find(mode);
	enum wd24_route r = parse_route(route);
	size_t len = 0;
	int err;

	APPEND(plan_txt, len, "wd24adv dry-run plan (no hardware register was written)\n");
	APPEND(plan_txt, len, "mode=%s route=%s allow_routing=%d allow_crt=%d allow_clock=%d\n",
	       mode, route, allow_routing, allow_crt, allow_clock);
	if (!m || !r) {
		APPEND(plan_txt, len, "error: unknown %s\n", m ? "route" : "mode");
		plan_len = len;
		return;
	}
	wd24_ctx_init(ctx, &wd24_dryrun_backend);
	ctx->allow_routing = allow_routing;
	ctx->allow_crt = allow_crt;
	ctx->allow_clock = allow_clock;
	/* the 755C boots on the panel with the panel powered */
	ctx->panel = WD24_PANEL_ON;
	ctx->reg[WD24_CR][WD24_CR_PR19] = WD24_PR19_PANEL;
	ctx->reg[WD24_SR][WD24_SR_PR57] = WD24_PR57_PNLON;

	err = 0;
	if (r != WD24_ROUTE_PANEL) {
		APPEND(plan_txt, len, "[1] routing change to %s\n", route);
		/* to CRT-only: route first so the mode is set with the panel off */
		wd24_note(ctx, "routing change");
		err = wd24_set_route(ctx, r, NULL, 0);
	}
	if (!err) {
		wd24_note(ctx, "mode programming");
		err = wd24_plan_mode(ctx, m, r);
	}
	len = format_events(plan_txt, len, ctx);
	APPEND(plan_txt, len, "result: %d%s\n", err,
	       err == -EPERM ? " (refused by interlock: see module parameters)" :
	       err == -EBUSY ? " (refused: panel powered)" : "");
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
	kfree(ctx);
	plan_txt = modes_txt = self_txt = NULL;
	ctx = NULL;
}

static int __init wd24adv_init(void)
{
	int fails;

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
		pr_err("dryrun=0 refused: no hardware backend in this build (needs Stage 0 data)\n");
		return -EOPNOTSUPP;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
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
	pr_info("scaffold loaded, dry-run only (%u modes, selftest %s); see /proc/wd24adv/\n",
		wd24_nmodes, fails ? "FAILED" : "ok");
	return 0;
}

static void __exit wd24adv_exit(void)
{
	free_all();
}

module_init(wd24adv_init);
module_exit(wd24adv_exit);

MODULE_DESCRIPTION("WD90C24 advanced driver scaffold (dry-run, ThinkPad 755C)");
MODULE_LICENSE("GPL");
