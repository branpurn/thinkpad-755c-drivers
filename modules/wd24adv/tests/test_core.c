// SPDX-License-Identifier: GPL-2.0
/* Host unit test for the wd24adv core (guards, panel sequence, mode table).
 * Build/run: make -C modules/wd24adv/tests   (or: make wd24adv-test) */
#include <stdio.h>
#include <string.h>
#include "../wd24adv_core.h"

static int fails, passes;

#define T(cond, what) do { if (cond) passes++; else { fails++; \
	printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

static struct wd24_ctx c;

static void fresh(bool routing, bool crt)
{
	wd24_ctx_init(&c, &wd24_dryrun_backend);
	c.allow_routing = routing;
	c.allow_crt = crt;
	c.panel = WD24_PANEL_ON;
	c.reg[WD24_CR][WD24_CR_PR19] = WD24_PR19_PANEL;
	c.reg[WD24_SR][WD24_SR_PR57] = WD24_PR57_PNLON;
}

/* index of the first write of rf/idx with (val & mask) == want, or -1 */
static int find_w(enum wd24_rf rf, u8 idx, u8 mask, u8 want, int from)
{
	for (unsigned int i = from; i < c.nev; i++)
		if (c.ev[i].type == WD24_EV_WRITE && c.ev[i].rf == rf &&
		    c.ev[i].idx == idx && (c.ev[i].val & mask) == want &&
		    (c.ev[i].old & mask) != want)
			return i;
	return -1;
}

static unsigned int delays_between(int a, int b)
{
	unsigned int ms = 0;

	for (int i = a; i < b; i++)
		if (c.ev[i].type == WD24_EV_DELAY)
			ms += c.ev[i].ms;
	return ms;
}

static unsigned int count(enum wd24_ev t)
{
	unsigned int n = 0;

	for (unsigned int i = 0; i < c.nev; i++)
		n += c.ev[i].type == t;
	return n;
}

int main(void)
{
	const char *bad, *why;
	const struct wd24_mode *m;

	/* mode table */
	T(wd24_modes_validate(&bad, &why) == 0, "mode table validates");
	if (bad)
		printf("  %s: %s\n", bad, why);
	T(wd24_mode_find("320x200-8") && wd24_mode_find("800x600-8-crt"),
	  "mode lookup");
	T(!wd24_mode_find("1024x768-8"), "unknown mode not found");
	{
		struct wd24_mode x = *wd24_mode_find("320x200-8");
		static const struct wd24_reg evil[] = { { WD24_CR, 0x32, 0x20 } };

		x.regs = evil; x.nregs = 1;
		T(wd24_mode_check(&x, &why) == -EINVAL,
		  "mode list may not touch PR19");
		x = *wd24_mode_find("800x600-8-crt");
		x.flags |= WD24M_PANEL_OK;
		T(wd24_mode_check(&x, &why) == -EINVAL,
		  "800x600 cannot be PANEL_OK");
		x = *wd24_mode_find("640x480-8");
		x.flags = (x.flags & ~WD24M_BANKED) | WD24M_DIRECT;
		T(wd24_mode_check(&x, &why) == -EINVAL,
		  "307200-byte mode cannot be DIRECT");
	}

	/* default parameters: everything routing-related refused */
	fresh(false, false);
	T(wd24_update(&c, WD24_CR, WD24_CR_PR19, 0x30, 0x20) == -EPERM, "PR19 route, no param");
	T(wd24_update(&c, WD24_CR, WD24_CR_PR19, 0x0c, 0x04) == 0, "PR19 scale bits allowed");
	T(wd24_write(&c, WD24_CR, WD24_CR_PR18, 0x55) == -EPERM, "PR18, no param");
	T(wd24_write(&c, WD24_CR, WD24_CR_PR1A, 0x55) == -EPERM, "PR1A, no param");
	T(wd24_write(&c, WD24_CR, WD24_CR_PR36, 0x55) == -EPERM, "PR36, no param");
	T(wd24_write(&c, WD24_CR, WD24_CR_PR1B, 0xa6) == -EPERM, "shadow unlock, no param");
	T(wd24_write(&c, WD24_CR, WD24_CR_PR1B, 0xa0) == 0, "FP unlock A0h allowed");
	T(wd24_update(&c, WD24_SR, WD24_SR_PR57, 4, 0) == -EPERM, "PNLOFF outside sequence");
	T(wd24_update(&c, WD24_GR, WD24_GR_PR4, 0x20, 0x20) == -EPERM, "tri-state outside sequence");
	T(wd24_write(&c, WD24_SR, WD24_SR_PR68, 1) == -EPERM, "PR68, no allow_clock");
	T(wd24_update(&c, WD24_GR, WD24_GR_PR1, 0x30, 0x30) == -EPERM, "aperture bits");
	T(wd24_panel_off(&c) == -EPERM, "panel_off, no param");
	T(wd24_set_route(&c, WD24_ROUTE_BOTH, NULL, 0) == -EPERM, "route, no param");
	T(c.panel == WD24_PANEL_ON, "panel untouched");
	T(c.reg[WD24_SR][WD24_SR_PR57] == WD24_PR57_PNLON, "PNLOFF untouched");
	T(count(WD24_EV_REFUSED) >= 9, "refusals logged");

	/* mode plan on the panel with defaults: allowed, no routing writes */
	fresh(false, false);
	m = wd24_mode_find("320x200-8");
	T(wd24_plan_mode(&c, m, WD24_ROUTE_PANEL) == 0, "13h plan on panel");
	T(find_w(WD24_CR, WD24_CR_PR19, 0x30, 0x00, 0) < 0, "plan keeps route");
	T(find_w(WD24_SR, WD24_SR_PR57, 4, 0, 0) < 0, "plan keeps panel power");
	T(c.reg[WD24_MISC][0] == 0x63 && c.reg[WD24_CR][0x13] == 0x28, "13h regs written");
	T(c.reg[WD24_CR][0x11] == 0x8e, "CR11 protect restored");
	T(!(c.reg[WD24_SR][1] & 0x20), "unblanked at the end");
	{
		int ub = -1;

		for (unsigned int i = 0; i < c.nev; i++)
			if (c.ev[i].type == WD24_EV_WRITE && c.ev[i].rf == WD24_SR &&
			    c.ev[i].idx == 1 && !(c.ev[i].val & 0x20))
				ub = i;
		T(ub >= 0 && delays_between(0, 0) == 0 &&
		  find_w(WD24_SR, 1, 0x20, 0, 0) == ub,
		  "screen stays blanked until the final unblank");
	}
	T(wd24_plan_mode(&c, wd24_mode_find("800x600-8-crt"), WD24_ROUTE_PANEL) == -EINVAL,
	  "CRT-only mode on panel refused");

	/* panel power-off then on, with allow_routing */
	fresh(true, false);
	T(wd24_update(&c, WD24_CR, WD24_CR_PR19, 0x30, 0x20) == -EBUSY, "route while powered");
	T(wd24_panel_off(&c) == 0, "panel_off");
	{
		int b4 = find_w(WD24_CR, WD24_CR_PR19, WD24_PR19_PANEL, 0, 0);
		int ts = find_w(WD24_GR, WD24_GR_PR4, 0x20, 0x20, 0);
		int pw = find_w(WD24_SR, WD24_SR_PR57, 4, 0, 0);
		int bl = find_w(WD24_SR, 0x01, 0x20, 0x20, 0);

		T(bl >= 0 && b4 > bl, "blank before panel disable");
		T(b4 >= 0 && ts > b4 && pw > ts, "off order: PR19 b4, PR4 b5, PNLOFF");
		T(find_w(WD24_GR, WD24_GR_PR5, 0xff, 0x05, 0) < ts, "GR0F=05 before tri-state");
	}
	T(c.panel == WD24_PANEL_OFF && !c.in_panel_seq, "state OFF");
	c.nev = 0;
	T(wd24_panel_on(&c) == 0, "panel_on");
	{
		int pw = find_w(WD24_SR, WD24_SR_PR57, 4, 4, 0);
		int ts = find_w(WD24_GR, WD24_GR_PR4, 0x20, 0, 0);
		int b4 = find_w(WD24_CR, WD24_CR_PR19, WD24_PR19_PANEL, WD24_PR19_PANEL, 0);
		int ub = find_w(WD24_SR, 0x01, 0x20, 0, 0);

		T(pw >= 0 && ts > pw && b4 > ts && ub > b4, "on order: PNLOFF, PR4, PR19 b4, unblank");
		T(delays_between(pw, ts) >= 100 && delays_between(ts, b4) >= 100,
		  ">=100 ms gaps in power-on");
	}
	T(c.panel == WD24_PANEL_ON, "state ON");

	/* CRT-only routing */
	fresh(true, false);
	T(wd24_set_route(&c, WD24_ROUTE_CRT, NULL, 0) == -EPERM, "CRT needs allow_crt");
	T(c.panel == WD24_PANEL_ON, "refused before panel_off");
	fresh(true, true);
	{
		static const struct wd24_reg sh[] = { { WD24_CR, 0x00, 0x7f }, { WD24_CR, 0x06, 0x72 } };

		c.reg[WD24_CR][0x00] = 0x5f;
		T(wd24_set_route(&c, WD24_ROUTE_CRT, sh, 2) == 0, "CRT route");
		T(c.panel == WD24_PANEL_OFF, "panel stays off for CRT-only");
		T((c.reg[WD24_CR][WD24_CR_PR19] & 0x30) == WD24_PR19_CRT, "PR19 = CRT");
		T(c.reg[WD24_CR][WD24_CR_PR1B] == 0xa0 && !c.shadow_open, "shadow closed");
		T(c.reg[WD24_CR][0x00] == 0x5f, "base CRTC reloaded after shadow");
		int open = find_w(WD24_CR, WD24_CR_PR1B, 0x07, 0x06, 0);
		int pw = find_w(WD24_SR, WD24_SR_PR57, 4, 0, 0);
		T(pw >= 0 && open > pw, "shadow written only after panel power off");
	}
	T(wd24_plan_mode(&c, wd24_mode_find("800x600-8-crt"), WD24_ROUTE_CRT) == 0,
	  "800x600 plan on CRT");

	/* simultaneous: panel comes back on with the sequence */
	fresh(true, false);
	T(wd24_set_route(&c, WD24_ROUTE_BOTH, NULL, 0) == 0, "both route");
	T((c.reg[WD24_CR][WD24_CR_PR19] & 0x30) == 0x30 && c.panel == WD24_PANEL_ON,
	  "PR19 = both, panel on");
	T(count(WD24_EV_REFUSED) == 0, "no refusals in a permitted sequence");

	printf("%d passed, %d failed\n", passes, fails);
	return fails != 0;
}
