// SPDX-License-Identifier: GPL-2.0
/*
 * wd24adv core: register guard (safety interlocks), panel power sequencing,
 * output routing and mode programming plans.
 *
 * Everything goes through wd24_write(), which asks wd24_guard() first.  The
 * guard is the single place that decides whether a register may change:
 *
 *  - PR18 (CR31), PR1A (CR33), PR36 (CR3B), PR19 b7:4 (CR32) and the panel
 *    shadow CRTC (CR34 = xxxxx110b) change only with allow_routing=1 AND the
 *    panel powered down by wd24_panel_off().  The datasheet forbids changing
 *    panel timing/routing while the panel is powered.
 *  - PNLOFF (PR57 = SR19 b2) and the panel tri-state (PR4 = GR0E b5) change
 *    only from inside wd24_panel_off()/wd24_panel_on(), so the power
 *    sequence and its delays cannot be bypassed.
 *  - Clock registers PR68/PR69 (SR31/SR32) need allow_clock=1.
 *  - The memory-map/aperture bits PR1 (GR0B) b5:4 are never written in this
 *    build (Stage 4), and the local-bus ports (A875h, 2DF0h-2DF2h) are not
 *    even representable here.
 *
 * In this scaffold only the dry-run backend exists: writes are recorded,
 * never sent to hardware, and delays are recorded instead of slept.
 */
#include "wd24adv_core.h"

#ifndef __KERNEL__
#include <string.h>
#endif

const char *wd24_rf_name(enum wd24_rf rf)
{
	static const char *const n[WD24_NRF] = { "SR", "GR", "CR", "AR", "MISC" };

	return (unsigned int)rf < WD24_NRF ? n[rf] : "??";
}

static void ev_add(struct wd24_ctx *c, const struct wd24_event *e)
{
	if (c->nev < WD24_MAXEV)
		c->ev[c->nev++] = *e;
	else
		c->dropped++;
}

void wd24_note(struct wd24_ctx *c, const char *text)
{
	struct wd24_event e = { .type = WD24_EV_NOTE, .note = text };

	ev_add(c, &e);
}

static void wd24_delay(struct wd24_ctx *c, unsigned int ms)
{
	struct wd24_event e = { .type = WD24_EV_DELAY, .ms = ms };

	ev_add(c, &e);
	if (c->be->delay)
		c->be->delay(c, ms);
}

void wd24_ctx_init(struct wd24_ctx *c, const struct wd24_backend *be)
{
	memset(c, 0, sizeof(*c));
	c->be = be;
	c->panel = WD24_PANEL_UNKNOWN;
}

static bool cr_is_shadowed(u8 idx)
{
	return idx <= 0x07 || (idx >= 0x10 && idx <= 0x12) ||
	       idx == 0x15 || idx == 0x16;
}

/* routing-class change: needs the parameter and a powered-down panel */
static int routing_ok(const struct wd24_ctx *c)
{
	if (!c->allow_routing)
		return -EPERM;
	if (c->panel != WD24_PANEL_OFF)
		return -EBUSY;
	return 0;
}

int wd24_guard(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 old, u8 val)
{
	u8 chg = old ^ val;

	if ((unsigned int)rf >= WD24_NRF || idx >= WD24_NIDX)
		return -EINVAL;

	switch (rf) {
	case WD24_CR:
		if (idx == WD24_CR_PR18 || idx == WD24_CR_PR1A ||
		    idx == WD24_CR_PR36)
			return chg ? routing_ok(c) : 0;
		if (idx == WD24_CR_PR19 && (chg & 0xf0))
			return routing_ok(c);
		if (idx == WD24_CR_PR1B && (val & 0x07) == 0x06)
			return routing_ok(c);
		if (c->shadow_open && cr_is_shadowed(idx))
			return routing_ok(c);
		return 0;
	case WD24_SR:
		if (idx == WD24_SR_PR57 && (chg & WD24_PR57_PNLON))
			return c->in_panel_seq ? 0 : -EPERM;
		if (idx == WD24_SR_PR68 || idx == WD24_SR_PR69)
			return c->allow_clock ? 0 : -EPERM;
		return 0;
	case WD24_GR:
		if (idx == WD24_GR_PR4 && (chg & WD24_PR4_TRISTATE))
			return c->in_panel_seq ? 0 : -EPERM;
		if (idx == WD24_GR_PR1 && (chg & 0x30))
			return -EPERM;	/* aperture: Stage 4, not in this build */
		return 0;
	default:
		return 0;
	}
}

int wd24_write(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 val)
{
	struct wd24_event e = { .type = WD24_EV_WRITE, .rf = rf, .idx = idx,
				.val = val };
	int err;

	if ((unsigned int)rf >= WD24_NRF || idx >= WD24_NIDX)
		return -EINVAL;
	e.old = c->reg[rf][idx];
	err = wd24_guard(c, rf, idx, e.old, val);
	if (!err)
		err = c->be->write(c, rf, idx, val);
	if (err) {
		e.type = WD24_EV_REFUSED;
		e.err = err;
		ev_add(c, &e);
		return err;
	}
	ev_add(c, &e);
	c->reg[rf][idx] = val;
	if (rf == WD24_CR && idx == WD24_CR_PR1B)
		c->shadow_open = (val & 0x07) == 0x06;
	return 0;
}

int wd24_update(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 mask, u8 val)
{
	if ((unsigned int)rf >= WD24_NRF || idx >= WD24_NIDX)
		return -EINVAL;
	return wd24_write(c, rf, idx, (c->reg[rf][idx] & ~mask) | (val & mask));
}

/* Paradise/WD unlock keys (datasheet; same as XFree86 "wd" driver). */
int wd24_unlock(struct wd24_ctx *c)
{
	int err;

	err = wd24_write(c, WD24_GR, WD24_GR_PR5, 0x05);
	if (!err)
		err = wd24_write(c, WD24_CR, WD24_CR_PR10, 0x85);
	if (!err)
		err = wd24_write(c, WD24_SR, WD24_SR_PR20,
				 (c->reg[WD24_SR][WD24_SR_PR20] & 0xef) | 0x48);
	if (!err)
		err = wd24_write(c, WD24_CR, WD24_CR_PR1B, WD24_PR1B_FP_UNLOCK);
	if (!err && c->allow_clock)
		err = wd24_write(c, WD24_SR, WD24_SR_PR68UL, 0x50);
	return err;
}

#define WD24_PANEL_GAP_MS	100

/*
 * Panel power down.  Order: stop panel data (PR19 b4), tri-state the panel
 * interface (PR4 b5), then drop panel power (PNLOFF, PR57 b2).
 * Source: XFree86 wd driver / datasheet PNLOFF notes.  The BIOS's own order
 * (Fn+F7 path) is to be cross-checked in the Stage 0 trace before Stage 6.
 */
int wd24_panel_off(struct wd24_ctx *c)
{
	int err;

	if (c->panel == WD24_PANEL_OFF)
		return 0;
	if (!c->allow_routing)
		return -EPERM;
	wd24_note(c, "panel power OFF sequence");
	c->in_panel_seq = true;
	err = wd24_unlock(c);
	if (err)
		goto out;
	/* blank first so nothing is latched mid-frame */
	err = wd24_update(c, WD24_SR, 0x01, 0x20, 0x20);
	if (err)
		goto out;
	/* PR19 b4 is a routing bit: the guard needs panel==OFF, which is the
	 * state we are entering; mark it before the first routing write */
	c->panel = WD24_PANEL_OFF;
	err = wd24_update(c, WD24_CR, WD24_CR_PR19, WD24_PR19_PANEL, 0);
	if (!err)
		err = wd24_update(c, WD24_GR, WD24_GR_PR4, WD24_PR4_TRISTATE,
				  WD24_PR4_TRISTATE);
	if (!err)
		err = wd24_update(c, WD24_SR, WD24_SR_PR57, WD24_PR57_PNLON, 0);
	if (!err)
		wd24_delay(c, WD24_PANEL_GAP_MS);
	if (err)
		c->panel = WD24_PANEL_UNKNOWN;
out:
	c->in_panel_seq = false;
	return err;
}

/* Panel power up: reverse order with ~100 ms gaps, then unblank. */
int wd24_panel_on(struct wd24_ctx *c)
{
	int err;

	if (c->panel != WD24_PANEL_OFF)
		return c->panel == WD24_PANEL_ON ? 0 : -EBUSY;
	if (!c->allow_routing)
		return -EPERM;
	wd24_note(c, "panel power ON sequence");
	c->in_panel_seq = true;
	err = wd24_update(c, WD24_SR, WD24_SR_PR57, WD24_PR57_PNLON,
			  WD24_PR57_PNLON);
	if (err)
		goto out;
	wd24_delay(c, WD24_PANEL_GAP_MS);
	err = wd24_update(c, WD24_GR, WD24_GR_PR4, WD24_PR4_TRISTATE, 0);
	if (err)
		goto out;
	wd24_delay(c, WD24_PANEL_GAP_MS);
	err = wd24_update(c, WD24_CR, WD24_CR_PR19, WD24_PR19_PANEL,
			  WD24_PR19_PANEL);
	if (err)
		goto out;
	c->panel = WD24_PANEL_ON;
	err = wd24_update(c, WD24_SR, 0x01, 0x20, 0);
	if (!err)
		err = wd24_update(c, WD24_CR, 0x17, 0x80, 0x80);
out:
	c->in_panel_seq = false;
	return err;
}

/*
 * Change output routing (datasheet "changing display type" sequence):
 * panel off -> PR19 b5:4 = 00 -> program PRs -> open shadow CRTC
 * (CR34 = xxxxx110b), load it, close (CR34 = A0h) -> reload the base CRTC
 * -> set the route bits -> wait -> panel on (if the panel is a target).
 */
int wd24_set_route(struct wd24_ctx *c, enum wd24_route r,
		   const struct wd24_reg *shadow, unsigned int nshadow)
{
	u8 base[WD24_NIDX];
	unsigned int i;
	int err;

	if (r != WD24_ROUTE_PANEL && r != WD24_ROUTE_CRT &&
	    r != WD24_ROUTE_BOTH)
		return -EINVAL;
	if (!c->allow_routing)
		return -EPERM;
	if (r == WD24_ROUTE_CRT && !c->allow_crt)
		return -EPERM;

	err = wd24_panel_off(c);
	if (err)
		return err;
	err = wd24_update(c, WD24_CR, WD24_CR_PR19, WD24_PR19_ROUTE, 0);
	if (err)
		return err;
	wd24_note(c, "PR18/PR1A/PR36 panel timing: values from Stage 0 trace (none yet)");

	memcpy(base, c->reg[WD24_CR], sizeof(base));
	if (nshadow) {
		err = wd24_write(c, WD24_CR, WD24_CR_PR1B, WD24_PR1B_SHADOW);
		for (i = 0; !err && i < nshadow; i++) {
			if (shadow[i].rf != WD24_CR ||
			    !cr_is_shadowed(shadow[i].idx))
				err = -EINVAL;
			else
				err = wd24_write(c, WD24_CR, shadow[i].idx,
						 shadow[i].val);
		}
		/* always try to close the shadow window */
		if (wd24_write(c, WD24_CR, WD24_CR_PR1B, WD24_PR1B_FP_UNLOCK)
		    && !err)
			err = -EIO;
		if (err)
			return err;
		for (i = 0; i < WD24_NIDX && !err; i++)
			if (cr_is_shadowed(i))
				err = wd24_write(c, WD24_CR, i, base[i]);
		if (err)
			return err;
	}
	if (r & WD24_ROUTE_CRT) {
		err = wd24_update(c, WD24_CR, WD24_CR_PR19, WD24_PR19_CRT,
				  WD24_PR19_CRT);
		if (err)
			return err;
	}
	if (r & WD24_ROUTE_PANEL) {
		wd24_delay(c, WD24_PANEL_GAP_MS);
		return wd24_panel_on(c);
	}
	/* CRT only: panel stays powered down, unblank the CRT */
	return wd24_update(c, WD24_SR, 0x01, 0x20, 0);
}

/*
 * Mode programming plan: blank, unlock, write the mode's register list,
 * set the panel scaler, unblank.  Routing is a separate, guarded step.
 */
int wd24_plan_mode(struct wd24_ctx *c, const struct wd24_mode *m,
		   enum wd24_route r)
{
	const char *why;
	unsigned int i;
	int err;

	err = wd24_mode_check(m, &why);
	if (err) {
		wd24_note(c, why);
		return err;
	}
	if ((m->flags & WD24M_CRT_ONLY) && (r & WD24_ROUTE_PANEL)) {
		wd24_note(c, "mode is CRT-only; the panel must be off");
		return -EINVAL;
	}
	if ((m->flags & WD24M_CRT_ONLY) && !c->allow_crt) {
		wd24_note(c, "CRT-only mode needs allow_crt=1");
		return -EPERM;
	}
	if (m->flags & WD24M_STAGE0)
		wd24_note(c, "register list incomplete: pending Stage 0 trace");

	err = wd24_update(c, WD24_SR, 0x01, 0x20, 0x20);	/* blank */
	if (!err)
		err = wd24_unlock(c);
	for (i = 0; !err && i < m->nregs; i++) {
		u8 v = m->regs[i].val;

		/* keep the screen blanked (SR01 b5) until the very end */
		if (m->regs[i].rf == WD24_SR && m->regs[i].idx == 0x01)
			v |= 0x20;
		err = wd24_write(c, m->regs[i].rf, m->regs[i].idx, v);
	}
	if (!err && (m->flags & WD24M_PANEL_OK) && (r & WD24_ROUTE_PANEL))
		err = wd24_update(c, WD24_CR, WD24_CR_PR19, WD24_PR19_SCALE,
				  m->pr19_scale);
	if (!err)
		err = wd24_update(c, WD24_SR, 0x01, 0x20, 0);	/* unblank */
	return err;
}

/* ---- dry-run backend --------------------------------------------------- */
static int dry_write(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 val)
{
	(void)c; (void)rf; (void)idx; (void)val;
	return 0;
}

const struct wd24_backend wd24_dryrun_backend = {
	.name = "dry-run",
	.write = dry_write,
	.delay = NULL,		/* recorded only */
};
