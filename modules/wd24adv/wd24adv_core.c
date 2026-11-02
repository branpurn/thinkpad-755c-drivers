// SPDX-License-Identifier: GPL-2.0
/*
 * wd24adv core: register guard, the wd90c24fb unlock/save/restore idioms,
 * and the mode programming plans.
 *
 * Every write goes through wd24_write(), which asks wd24_guard() first.  The
 * guard is a WHITELIST: a write is allowed only if it is
 *   - a standard VGA register (MISC, SR00-04, CR00-18, GR00-08, AR00-14,
 *     AR palette-address-source 20h), or
 *   - an extended register that wd90c24fb already writes, with the same
 *     value constraints (see the per-register comments, which cite
 *     modules/wd90c24fb/wd90c24_hw.c by line).
 * Everything else is refused with -EPERM, whatever the module parameters
 * say.  In particular: PR18/PR19/PR1A/PR36 and every CR30-3F register except
 * CR34, the shadow CRTC key (CR34 = xxxxx110b), CR35/CR3A, panel power
 * (PR57 = SR19, PR4 = GR0E bit 5), the aperture bits (PR1 = GR0B b5:4) and
 * the MCLK bits (PR68 b2:0).  PM ports 24h-27h, 0D00h/0D01h, CMOS, the
 * local-bus ports and 23C0h blocks are not representable at all.
 *
 * Only the dry-run backend exists: writes are recorded against a model of
 * the chip (wd24_boot_model), never sent to hardware.
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

void wd24_ctx_init(struct wd24_ctx *c, const struct wd24_backend *be)
{
	memset(c, 0, sizeof(*c));
	c->be = be;
}

/* ---- guard -------------------------------------------------------------- */

int wd24_guard(const struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 old, u8 val)
{
	switch (rf) {
	case WD24_MISC:
		return idx == 0 ? 0 : -EPERM;
	case WD24_AR:
		return (idx <= 0x14 || idx == 0x20) ? 0 : -EPERM;
	case WD24_SR:
		if (idx <= 0x04)
			return 0;
		switch (idx) {
		case WD24_SR_PR20:	/* hw.c 72 unlock, 287/622/640 restore */
			if (val == ((old & 0xef) | 0x48))
				return 0;
			return (c->saved && val == c->bios.pr20) ? 0 : -EPERM;
		case WD24_SR_PR30A:	/* hw.c 275 */
		case WD24_SR_PR31:	/* hw.c 271 */
			return 0;
		case WD24_SR_PR72:	/* hw.c 222/225, 256/260: 50h then P */
			if (!c->pr72_open)
				return val == 0x50 ? 0 : -EPERM;
			return val == c->bios.pr72 ? 0 : -EPERM;
		case WD24_SR_PR68:	/* hw.c 257: b4:3 only, allow_clock */
			if (!c->allow_clock || !c->pr72_open)
				return -EPERM;
			return ((old ^ val) & ~0x18) ? -EPERM : 0;
		case WD24_SR_PR69:	/* hw.c 259 */
			return (c->allow_clock && c->pr72_open) ? 0 : -EPERM;
		default:		/* PR57 panel power and the rest */
			return -EPERM;
		}
	case WD24_GR:
		if (idx <= 0x08)
			return 0;
		switch (idx) {
		case WD24_GR_PR0A:	/* hw.c 252, 397 */
		case WD24_GR_PR0B:	/* hw.c 253 */
		case WD24_GR_PR3:	/* hw.c 246, 264 */
			return 0;
		case WD24_GR_PR1:	/* hw.c 254: mask C8h, aperture never */
			return ((old ^ val) & WD24_PR1_APERTURE) ? -EPERM : 0;
		case WD24_GR_PR4:	/* hw.c 247, 266: bit 5 kept */
			return ((old ^ val) & WD24_PR4_TRISTATE) ? -EPERM : 0;
		case WD24_GR_PR5:	/* hw.c 187 unlock, 289 relock */
			if (val == 0x05)
				return 0;
			return (c->saved && val == c->bios.pr5) ? 0 : -EPERM;
		default:		/* PR2 (not written on a C24) etc. */
			return -EPERM;
		}
	case WD24_CR:
		if (idx <= 0x18)
			return 0;
		switch (idx) {
		case WD24_CR_PR10:	/* hw.c 188 unlock, 288 relock */
			if (val == 0x85)
				return 0;
			return (c->saved && val == c->bios.pr10) ? 0 : -EPERM;
		case WD24_CR_PR11:	/* hw.c 250, 285 */
		case WD24_CR_PR13:	/* hw.c 268 */
		case WD24_CR_PR14:	/* hw.c 269 */
		case WD24_CR_PR15:	/* hw.c 273 */
		case WD24_CR_PR16:	/* hw.c 270 */
			return 0;
		case WD24_CR_PR1B:	/* hw.c 80/83, 591/593: read idiom only */
			if ((val & 0x07) == 0x06)
				return -EPERM;	/* shadow CRTC key: never */
			if (c->pr1b_open)
				return val == c->pr1b_lock ? 0 : -EPERM;
			if (!c->pr1b_armed)
				return -EPERM;
			return val == ((c->pr1b_lock & 0x0f) | WD24_PR1B_FP) ? 0 : -EPERM;
		default:		/* PR12, PR17, PR18-PR41, CR35, CR3A, ... */
			return -EPERM;
		}
	default:
		return -EPERM;
	}
}

/* ---- accessors ---------------------------------------------------------- */

u8 wd24_read(struct wd24_ctx *c, enum wd24_rf rf, u8 idx)
{
	return c->be->read(c, rf, idx);
}

int wd24_write(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 val)
{
	struct wd24_event e = { .rf = rf, .idx = idx, .val = val,
				.note = c->tag };
	int err;

	if ((unsigned int)rf >= WD24_NRF || idx >= WD24_NIDX)
		return -EINVAL;
	e.old = c->reg[rf][idx];
	e.old_known = (c->known[rf] >> idx) & 1;
	err = wd24_guard(c, rf, idx, e.old, val);
	if (!err)
		err = c->be->write(c, rf, idx, val);
	if (err) {
		e.type = WD24_EV_REFUSED;
		e.err = err;
		ev_add(c, &e);
		c->nrefused++;
		return err;
	}
	e.type = WD24_EV_WRITE;
	ev_add(c, &e);
	c->nwrites++;
	c->reg[rf][idx] = val;
	c->known[rf] |= (u64)1 << idx;
	/* idiom state */
	if (rf == WD24_CR && idx == WD24_CR_PR1B) {
		c->pr1b_open = !c->pr1b_open;
		c->pr1b_armed = false;
	}
	if (rf == WD24_SR && idx == WD24_SR_PR72)
		c->pr72_open = (val == 0x50);
	return 0;
}

/* wd_modinx() (hw.c 46-49): always a read and a write */
int wd24_modify(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 mask, u8 val)
{
	return wd24_write(c, rf, idx,
			  (wd24_read(c, rf, idx) & ~mask) | (val & mask));
}

#define TRY(x)	do { int _e = (x); if (_e) return _e; } while (0)

/* ---- wd90c24fb idioms --------------------------------------------------- */

/* hw.c 75-84 / 587-595: CR34 = (L & 0Fh) | A0h, read, CR34 = L */
int wd24_read_panel(struct wd24_ctx *c, u8 *pr18, u8 *pr19)
{
	u8 l = wd24_read(c, WD24_CR, WD24_CR_PR1B);

	c->pr1b_lock = l;
	c->pr1b_armed = true;
	TRY(wd24_write(c, WD24_CR, WD24_CR_PR1B, (l & 0x0f) | WD24_PR1B_FP));
	*pr18 = wd24_read(c, WD24_CR, WD24_CR_PR18);
	*pr19 = wd24_read(c, WD24_CR, WD24_CR_PR19);
	return wd24_write(c, WD24_CR, WD24_CR_PR1B, l);
}

/*
 * Precondition of every plan: colour CRTC, and the panel route the BIOS set
 * at POST is exactly the live-verified one (LCD only, CRT off, PR19 b3:2 =
 * 01 centring, no expansion).  Anything else and wd24adv does nothing.
 */
int wd24_check_panel(struct wd24_ctx *c)
{
	const char *old = c->tag;
	u8 pr18, pr19;

	if (!(wd24_read(c, WD24_MISC, 0) & 0x01)) {
		wd24_note(c, "REFUSED: 3CC b0 = 0 (mono CRTC); not the 755C state");
		return -ENODEV;
	}
	c->tag = "panel read, hw.c 587-595";
	TRY(wd24_read_panel(c, &pr18, &pr19));
	c->tag = old;
	if (pr19 != WD24_LIVE_PR19 || pr18 != WD24_LIVE_PR18) {
		wd24_note(c, "REFUSED: PR18/PR19 differ from the live-verified C7/54 (LCD only, centring)");
		return -ENODEV;
	}
	return 0;
}

/* hw.c 185-191 (wd_unlock) with 69-73 (wd_unlock_seq) */
int wd24_unlock(struct wd24_ctx *c)
{
	TRY(wd24_write(c, WD24_GR, WD24_GR_PR5, 0x05));
	TRY(wd24_write(c, WD24_CR, WD24_CR_PR10, 0x85));
	TRY(wd24_modify(c, WD24_CR, 0x11, 0x80, 0x00));
	return wd24_write(c, WD24_SR, WD24_SR_PR20,
			  (wd24_read(c, WD24_SR, WD24_SR_PR20) & 0xef) | 0x48);
}

/* hw.c 197-234 (wd_save_ext), C24, no 23C0 blocks */
int wd24_save_ext(struct wd24_ctx *c, struct wd24_ext *s)
{
	s->misc = wd24_read(c, WD24_MISC, 0);
	s->cr11 = wd24_read(c, WD24_CR, 0x11);
	s->pr5 = wd24_read(c, WD24_GR, WD24_GR_PR5);
	s->pr10 = wd24_read(c, WD24_CR, WD24_CR_PR10);
	s->pr20 = wd24_read(c, WD24_SR, WD24_SR_PR20);
	c->tag = "unlock, hw.c 185-191";
	TRY(wd24_unlock(c));
	s->pr0a = wd24_read(c, WD24_GR, WD24_GR_PR0A);
	s->pr0b = wd24_read(c, WD24_GR, WD24_GR_PR0B);
	s->pr1 = wd24_read(c, WD24_GR, WD24_GR_PR1);
	s->pr3 = wd24_read(c, WD24_GR, WD24_GR_PR3);
	s->pr4 = wd24_read(c, WD24_GR, WD24_GR_PR4);
	s->pr11 = wd24_read(c, WD24_CR, WD24_CR_PR11);
	s->pr13 = wd24_read(c, WD24_CR, WD24_CR_PR13);
	s->pr14 = wd24_read(c, WD24_CR, WD24_CR_PR14);
	s->pr15 = wd24_read(c, WD24_CR, WD24_CR_PR15);
	s->pr16 = wd24_read(c, WD24_CR, WD24_CR_PR16);
	s->pr30a = wd24_read(c, WD24_SR, WD24_SR_PR30A);
	s->pr31 = wd24_read(c, WD24_SR, WD24_SR_PR31);
	s->pr32 = wd24_read(c, WD24_SR, WD24_SR_PR32);
	s->pr72 = wd24_read(c, WD24_SR, WD24_SR_PR72);
	c->bios.pr72 = s->pr72;		/* the guard's "restore P" value */
	c->tag = "SR35 50h/P, hw.c 222-225";
	TRY(wd24_write(c, WD24_SR, WD24_SR_PR72, 0x50));
	s->pr68 = wd24_read(c, WD24_SR, WD24_SR_PR68);
	s->pr69 = wd24_read(c, WD24_SR, WD24_SR_PR69);
	return wd24_write(c, WD24_SR, WD24_SR_PR72, s->pr72);
}

/*
 * hw.c 241-280 (wd_restore_ext), C24 with the LCD on (PR11 mask FFh).
 * One deliberate difference: the clock block (SR35=50h, PR68 b4:3, PR69,
 * SR35=P; hw.c 256-260) is skipped when it would not change anything, and
 * refused (-EPERM) when it would, unless allow_clock=1.  For every mode in
 * the table the VCLK select bits already equal the target (live SR31=0Dh,
 * b4:3 = 01) and MISC selects clock 0, so it is always skipped.
 */
int wd24_restore_ext(struct wd24_ctx *c, const struct wd24_ext *s)
{
	u8 cur68 = c->bios.pr68;	/* only wd24_save_ext reads it */
	u8 new68 = (cur68 & ~0x18) | (s->pr68 & 0x18);
	bool pr69 = (s->misc & 0x0c) == 0x08;

	c->tag = "unlock, hw.c 185-191";
	TRY(wd24_unlock(c));
	c->tag = "restore_ext, hw.c 246-254";
	TRY(wd24_write(c, WD24_GR, WD24_GR_PR3,
		       wd24_read(c, WD24_GR, WD24_GR_PR3) & 0x1c));
	TRY(wd24_write(c, WD24_GR, WD24_GR_PR4,
		       wd24_read(c, WD24_GR, WD24_GR_PR4) & 0xfb));
	TRY(wd24_write(c, WD24_CR, WD24_CR_PR11,
		       wd24_read(c, WD24_CR, WD24_CR_PR11) & 0xff));
	TRY(wd24_write(c, WD24_GR, WD24_GR_PR0A, s->pr0a));
	TRY(wd24_write(c, WD24_GR, WD24_GR_PR0B, s->pr0b));
	TRY(wd24_modify(c, WD24_GR, WD24_GR_PR1, 0xc8, s->pr1));
	if (new68 == cur68 && !pr69) {
		wd24_note(c, "clock block (SR35=50, SR31 b4:3, SR35=P; hw.c 256-260) skipped: SR31 b4:3 already = target, MISC clock 0");
	} else if (!c->allow_clock) {
		struct wd24_event e = { .type = WD24_EV_REFUSED, .rf = WD24_SR,
			.idx = WD24_SR_PR68, .old = cur68, .old_known = true,
			.val = new68, .err = -EPERM, .note = "clock change needs allow_clock" };

		ev_add(c, &e);
		c->nrefused++;
		return -EPERM;
	} else {
		c->tag = "clock, hw.c 256-260 (allow_clock)";
		TRY(wd24_write(c, WD24_SR, WD24_SR_PR72, 0x50));
		TRY(wd24_modify(c, WD24_SR, WD24_SR_PR68, 0x18, s->pr68));
		if (pr69)
			TRY(wd24_write(c, WD24_SR, WD24_SR_PR69, s->pr69));
		TRY(wd24_write(c, WD24_SR, WD24_SR_PR72, c->bios.pr72));
	}
	c->tag = "restore_ext, hw.c 264-275";
	TRY(wd24_write(c, WD24_GR, WD24_GR_PR3, s->pr3));
	TRY(wd24_write(c, WD24_GR, WD24_GR_PR4, (s->pr4 & ~WD24_PR4_TRISTATE) |
		       (wd24_read(c, WD24_GR, WD24_GR_PR4) & WD24_PR4_TRISTATE)));
	TRY(wd24_write(c, WD24_CR, WD24_CR_PR13, s->pr13));
	TRY(wd24_write(c, WD24_CR, WD24_CR_PR14, s->pr14));
	TRY(wd24_write(c, WD24_CR, WD24_CR_PR16, s->pr16));
	TRY(wd24_write(c, WD24_SR, WD24_SR_PR31, s->pr31));
	TRY(wd24_modify(c, WD24_CR, WD24_CR_PR15, 0x50, s->pr15));
	return wd24_modify(c, WD24_SR, WD24_SR_PR30A, 0xcf, s->pr30a);
	/* hw.c 276-279 (23C0h cursor block): not representable, not needed */
}

/* hw.c 283-290 (wd_relock) */
int wd24_relock(struct wd24_ctx *c, const struct wd24_ext *s)
{
	c->tag = "relock, hw.c 283-290";
	TRY(wd24_write(c, WD24_CR, WD24_CR_PR11, s->pr11));
	TRY(wd24_modify(c, WD24_CR, 0x11, 0x80, s->cr11));
	TRY(wd24_write(c, WD24_SR, WD24_SR_PR20, s->pr20));
	TRY(wd24_write(c, WD24_CR, WD24_CR_PR10, s->pr10));
	return wd24_write(c, WD24_GR, WD24_GR_PR5, s->pr5);
}

/* hw.c 351-355 */
int wd24_screen_on(struct wd24_ctx *c, bool on)
{
	c->tag = on ? "unblank, hw.c 354" : "blank, hw.c 354";
	return wd24_modify(c, WD24_SR, 0x01, 0x20, on ? 0x00 : 0x20);
}

/* hw.c 357-381 (wd_write_std): standard VGA registers only */
int wd24_write_std(struct wd24_ctx *c, const struct wd24_std *std)
{
	int i;

	c->tag = "std VGA, order of hw.c 357-381";
	TRY(wd24_write(c, WD24_SR, 0x00, 0x01));
	TRY(wd24_write(c, WD24_MISC, 0, std->misc));
	for (i = 1; i < 5; i++)
		TRY(wd24_write(c, WD24_SR, i,
			       i == 1 ? std->seq[i] | 0x20 : std->seq[i]));
	TRY(wd24_write(c, WD24_SR, 0x00, 0x03));
	TRY(wd24_modify(c, WD24_CR, 0x11, 0x80, 0x00));
	for (i = 0; i < 25; i++)
		if (i != 0x11)
			TRY(wd24_write(c, WD24_CR, i, std->crtc[i]));
	TRY(wd24_write(c, WD24_CR, 0x11, std->crtc[0x11]));
	for (i = 0; i < 9; i++)
		TRY(wd24_write(c, WD24_GR, i, std->gfx[i]));
	for (i = 0; i < 21; i++)
		TRY(wd24_write(c, WD24_AR, i, std->attr[i]));
	return wd24_write(c, WD24_AR, 0x20, 0x20);	/* palette address source */
}

/* hw.c 310-349 (wd_mode_640x480x8 extended part), or the saved BIOS state */
void wd24_mode_ext(const struct wd24_mode *m, const struct wd24_ext *bios,
		   struct wd24_ext *ext)
{
	*ext = *bios;
	if (m->ext != WD24_EXT_PACKED8)
		return;
	ext->misc = m->std->misc;
	ext->pr0a = 0;
	ext->pr0b = 0;
	ext->pr1 = bios->pr1 & ~0x08;	/* use_1meg is false on the TFT 755C */
	ext->pr3 = 0x00;
	ext->pr4 = 0x01;
	ext->pr13 = 0x00;
	ext->pr14 = 0x00;
	ext->pr15 = bios->pr15 & ~0x50;
	ext->pr16 = 0x00;
	ext->pr31 = (bios->pr31 & 0x03) | 0x5c;
	ext->pr30a = (bios->pr30a & ~0xcf) | 0xc1;
	ext->pr68 = (bios->pr68 & ~0x18) | 0x08;
}

/* ---- plans -------------------------------------------------------------- */

/* wd90c24fb load: panel read (hw.c 75-84), save (main.c 1446), relock (1448) */
int wd24_plan_load(struct wd24_ctx *c)
{
	wd24_note(c, "load: panel check, save BIOS state, relock (wd90c24fb_main.c 1446-1448)");
	TRY(wd24_check_panel(c));
	TRY(wd24_save_ext(c, &c->bios));
	c->saved = true;
	return wd24_relock(c, &c->bios);
}

/*
 * Packed 8 bpp (640x480, 640x400): wd_set_mode() (hw.c 383-392), then SR06
 * back to the BIOS value as wd90c24fb's unblank does (hw.c 637-640) - this
 * is the state probe3 read live while wd90c24fb was running (SR06 latch
 * locked, GR0F=05, CR29=85).  GR0F stays 05h: PR0A banking needs it.
 *
 * BIOS-ext modes (320x200 / mode 13h): wd_restore_text() order (main.c
 * 802-815) with the mode 13h standard registers in place of the saved text
 * registers: restore_ext(BIOS), std, relock(BIOS), unblank.
 */
int wd24_plan_mode(struct wd24_ctx *c, const struct wd24_mode *m)
{
	struct wd24_ext ext;
	const char *why;

	if (!c->saved)
		return -EINVAL;
	if (wd24_mode_check(m, &why)) {
		wd24_note(c, why);
		return -EINVAL;
	}
	if (m->status != WD24_PROVEN) {
		wd24_note(c, "UNPROVEN: disabled, no register is written");
		return -EOPNOTSUPP;
	}
	TRY(wd24_check_panel(c));
	wd24_mode_ext(m, &c->bios, &ext);
	TRY(wd24_screen_on(c, false));
	TRY(wd24_restore_ext(c, &ext));
	TRY(wd24_write_std(c, m->std));
	if (m->ext == WD24_EXT_PACKED8) {
		c->tag = "bank 0, hw.c 389-390/397";
		TRY(wd24_write(c, WD24_GR, WD24_GR_PR0A, 0x00));
		TRY(wd24_screen_on(c, true));
		c->tag = "SR06 = saved, hw.c 640";
		return wd24_write(c, WD24_SR, WD24_SR_PR20, c->bios.pr20);
	}
	TRY(wd24_relock(c, &c->bios));
	return wd24_screen_on(c, true);
}

/* wd_restore_text() (main.c 802-815) without restore_vga() and without
 * wd_restore_panel(): wd24adv never changes PR19/PR57/PR4 b5. */
int wd24_plan_text(struct wd24_ctx *c)
{
	if (!c->saved)
		return -EINVAL;
	TRY(wd24_restore_ext(c, &c->bios));
	wd24_note(c, "std VGA registers, fonts, palette: from the copy saved at load (vgastate), not modelled");
	TRY(wd24_relock(c, &c->bios));
	TRY(wd24_screen_on(c, true));
	wd24_note(c, "wd_restore_panel() omitted: PR19, PR57, PR4 b5 were never changed");
	return 0;
}

/* ---- dry-run backend ---------------------------------------------------- */

static int dry_write(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 val)
{
	(void)c; (void)rf; (void)idx; (void)val;
	return 0;		/* recorded by wd24_write(); hardware untouched */
}

static u8 dry_read(struct wd24_ctx *c, enum wd24_rf rf, u8 idx)
{
	if ((unsigned int)rf >= WD24_NRF || idx >= WD24_NIDX)
		return 0xff;
	return c->reg[rf][idx];
}

const struct wd24_backend wd24_dryrun_backend = {
	.name = "dry-run",
	.write = dry_write,
	.read = dry_read,
};

void wd24_model_boot(struct wd24_ctx *c)
{
	unsigned int i;

	for (i = 0; i < wd24_boot_model_n; i++) {
		const struct wd24_model_reg *r = &wd24_boot_model[i];

		c->reg[r->rf][r->idx] = r->val;
		c->known[r->rf] |= (u64)1 << r->idx;
	}
}

/* ---- shared self-test --------------------------------------------------- */

static void st_fresh(struct wd24_ctx *t, u8 pr19)
{
	wd24_ctx_init(t, &wd24_dryrun_backend);
	wd24_model_boot(t);
	t->reg[WD24_CR][WD24_CR_PR19] = pr19;
}

static bool st_loaded(struct wd24_ctx *t)
{
	st_fresh(t, WD24_LIVE_PR19);
	return wd24_plan_load(t) == 0 && t->nrefused == 0;
}

/* count writes to rf/idx in events [from, nev) */
static unsigned int st_writes(const struct wd24_ctx *t, unsigned int from,
			      enum wd24_rf rf, u8 idx)
{
	unsigned int i, n = 0;

	for (i = from; i < t->nev; i++)
		if (t->ev[i].type == WD24_EV_WRITE && t->ev[i].rf == rf &&
		    t->ev[i].idx == idx)
			n++;
	return n;
}

/* any write to a panel/routing register other than the CR34 idiom */
static bool st_panel_untouched(const struct wd24_ctx *t, unsigned int from)
{
	unsigned int i;

	for (i = from; i < t->nev; i++) {
		const struct wd24_event *e = &t->ev[i];

		if (e->type != WD24_EV_WRITE)
			continue;
		if (e->rf == WD24_CR && e->idx >= 0x30 && e->idx != WD24_CR_PR1B)
			return false;
		if (e->rf == WD24_CR && e->idx == WD24_CR_PR1B &&
		    (e->val & 0x07) == 0x06)
			return false;
		if (e->rf == WD24_SR && (e->idx == WD24_SR_PR57 ||
		    e->idx == WD24_SR_PR68 || e->idx == WD24_SR_PR69))
			return false;
		if (e->rf == WD24_GR && e->idx == WD24_GR_PR4 &&
		    ((e->old ^ e->val) & WD24_PR4_TRISTATE))
			return false;
		if (e->rf == WD24_GR && e->idx == WD24_GR_PR1 &&
		    ((e->old ^ e->val) & WD24_PR1_APERTURE))
			return false;
	}
	return true;
}

static bool st_has_note(const struct wd24_ctx *t, unsigned int from,
			const char *prefix)
{
	unsigned int i;

	for (i = from; i < t->nev; i++)
		if (t->ev[i].type == WD24_EV_NOTE && t->ev[i].note &&
		    !strncmp(t->ev[i].note, prefix, strlen(prefix)))
			return true;
	return false;
}

struct st_refuse {
	u8 rf, idx, val;
	const char *what;
};

static const struct st_refuse st_refusals[] = {
	{ WD24_CR, 0x32, 0x54, "refuse PR19 write (even same value)" },
	{ WD24_CR, 0x32, 0x5c, "refuse PR19 expansion" },
	{ WD24_CR, 0x31, 0xc7, "refuse PR18 write" },
	{ WD24_CR, 0x33, 0x00, "refuse PR1A write" },
	{ WD24_CR, 0x3b, 0xef, "refuse PR36 write" },
	{ WD24_CR, 0x3c, 0x9a, "refuse PR37 write" },
	{ WD24_CR, 0x35, 0x00, "refuse CR35 write" },
	{ WD24_CR, 0x3a, 0x00, "refuse CR3A write" },
	{ WD24_CR, 0x3e, 0x20, "refuse CR3E write" },
	{ WD24_CR, 0x34, 0x06, "refuse CR34 shadow key 06" },
	{ WD24_CR, 0x34, 0xa0, "refuse CR34 A0 outside the read idiom" },
	{ WD24_CR, 0x2b, 0x00, "refuse PR12 write" },
	{ WD24_CR, 0x29, 0xff, "refuse CR29 other than 85/saved" },
	{ WD24_SR, 0x19, 0x35, "refuse PR57 (panel power)" },
	{ WD24_SR, 0x31, 0x0d, "refuse SR31 without allow_clock" },
	{ WD24_SR, 0x32, 0x00, "refuse SR32 without allow_clock" },
	{ WD24_SR, 0x35, 0x51, "refuse SR35 other than 50/saved" },
	{ WD24_SR, 0x06, 0x00, "refuse SR06 other than unlock/saved" },
	{ WD24_SR, 0x21, 0x15, "refuse unknown SR21" },
	{ WD24_GR, 0x0e, 0x60, "refuse PR4 b5 (panel tri-state)" },
	{ WD24_GR, 0x0b, 0xd5, "refuse PR1 b5:4 (aperture)" },
	{ WD24_GR, 0x0c, 0x01, "refuse PR2 write" },
	{ WD24_GR, 0x0f, 0x07, "refuse GR0F other than 05/saved" },
	{ WD24_AR, 0x15, 0x00, "refuse AR15" },
	{ WD24_MISC, 1, 0x63, "refuse MISC index != 0" },
};

int wd24_selftest(struct wd24_ctx *t, wd24_report_fn report, void *arg)
{
	const char *bad, *why;
	unsigned int i, from, fails = 0;
	bool ok;

#define CHECK(cond, what) do { bool _ok = (cond); if (!_ok) fails++; \
	report(arg, _ok, (what)); } while (0)

	CHECK(wd24_modes_validate(&bad, &why) == 0, "mode table validates");

	CHECK(st_loaded(t), "load plan: no refusal");
	CHECK(t->reg[WD24_GR][WD24_GR_PR5] == 0x00 &&
	      t->reg[WD24_CR][WD24_CR_PR10] == 0x00 &&
	      t->reg[WD24_SR][WD24_SR_PR20] == 0x0f &&
	      t->reg[WD24_CR][WD24_CR_PR1B] == 0xa0 &&
	      t->reg[WD24_SR][WD24_SR_PR72] == 0x00,
	      "load plan: locks, CR34, SR35 back to boot values");
	CHECK(st_panel_untouched(t, 0), "load plan: panel untouched");

	for (i = 0; i < sizeof(st_refusals) / sizeof(st_refusals[0]); i++) {
		const struct st_refuse *r = &st_refusals[i];
		unsigned int w;

		st_loaded(t);
		w = t->nwrites;
		CHECK(wd24_write(t, r->rf, r->idx, r->val) == -EPERM &&
		      t->nwrites == w, r->what);
	}
	st_loaded(t);
	t->allow_clock = true;
	CHECK(wd24_write(t, WD24_CR, 0x32, 0x54) == -EPERM &&
	      wd24_write(t, WD24_SR, 0x19, 0x31) == -EPERM,
	      "allow_clock does not open PR19/PR57");
	t->pr1b_lock = 0xa0;
	t->pr1b_armed = true;
	CHECK(wd24_write(t, WD24_CR, 0x34, 0xa6) == -EPERM,
	      "refuse CR34 A6 even inside the idiom");

	for (i = 0; i < wd24_nmodes; i++) {
		const struct wd24_mode *m = &wd24_modes[i];
		int err;

		st_loaded(t);
		from = t->nev;
		t->nwrites = 0;
		err = wd24_plan_mode(t, m);
		if (m->status != WD24_PROVEN) {
			CHECK(err == -EOPNOTSUPP && t->nwrites == 0,
			      "UNPROVEN mode refused with 0 writes");
			continue;
		}
		CHECK(err == 0 && t->nrefused == 0, "PROVEN mode plan: no refusal");
		CHECK(st_writes(t, from, WD24_CR, WD24_CR_PR19) == 0 &&
		      st_panel_untouched(t, from), "PROVEN mode: panel untouched");
		CHECK(t->reg[WD24_CR][WD24_CR_PR19] == WD24_LIVE_PR19 &&
		      t->reg[WD24_CR][WD24_CR_PR18] == WD24_LIVE_PR18 &&
		      t->reg[WD24_SR][WD24_SR_PR57] == 0x35 &&
		      !(t->reg[WD24_GR][WD24_GR_PR4] & WD24_PR4_TRISTATE) &&
		      t->reg[WD24_CR][WD24_CR_PR1B] == 0xa0 &&
		      (t->reg[WD24_GR][WD24_GR_PR1] & 0x30) == 0x00,
		      "PROVEN mode: PR18/PR19/PR57/PR4b5/CR34/PR1b5:4 unchanged");
		CHECK(st_writes(t, from, WD24_SR, WD24_SR_PR72) == 0 &&
		      st_has_note(t, from, "clock block"),
		      "PROVEN mode: clock block is a no-op and skipped");
		CHECK((t->reg[WD24_SR][0x01] & 0x20) == 0, "PROVEN mode: screen on");
		if (m->ext == WD24_EXT_PACKED8 && m->yres == 480)
			CHECK(t->reg[WD24_MISC][0] == 0xe3 &&
			      t->reg[WD24_GR][0x0f] == 0x05 &&
			      t->reg[WD24_GR][0x0e] == 0x01 &&
			      t->reg[WD24_CR][0x11] == 0x8c &&
			      t->reg[WD24_CR][0x29] == 0x85 &&
			      t->reg[WD24_SR][0x11] == 0x5d &&
			      t->reg[WD24_SR][0x31] == 0x0d &&
			      t->reg[WD24_SR][0x06] == 0x0f,
			      "640x480-8 end state = live wd90c24fb values");
		if (m->ext == WD24_EXT_BIOS)
			CHECK(t->reg[WD24_GR][0x0f] == t->bios.pr5 &&
			      t->reg[WD24_CR][0x29] == t->bios.pr10 &&
			      t->reg[WD24_GR][0x0e] == t->bios.pr4 &&
			      t->reg[WD24_SR][0x11] == t->bios.pr31 &&
			      st_writes(t, from, WD24_GR, WD24_GR_PR0A) == 1,
			      "320x200-8: BIOS ext, relocked, no banking");
		from = t->nev;
		CHECK(wd24_plan_text(t) == 0 && t->nrefused == 0 &&
		      st_panel_untouched(t, from), "text restore: no refusal, panel untouched");
		CHECK(t->reg[WD24_GR][0x0f] == t->bios.pr5 &&
		      t->reg[WD24_CR][0x29] == t->bios.pr10 &&
		      t->reg[WD24_SR][0x06] == t->bios.pr20 &&
		      t->reg[WD24_CR][0x2a] == t->bios.pr11 &&
		      t->reg[WD24_GR][0x0e] == t->bios.pr4 &&
		      t->reg[WD24_SR][0x11] == t->bios.pr31,
		      "text restore: locks and ext back to saved values");
	}

	/* the BIOS route must be exactly the live one */
	st_fresh(t, 0x5c);
	CHECK(wd24_plan_load(t) == -ENODEV, "PR19=5C (expansion): refused at load");
	st_fresh(t, 0x74);
	CHECK(wd24_plan_load(t) == -ENODEV, "PR19=74 (CRT on): refused at load");
	st_loaded(t);
	t->reg[WD24_CR][WD24_CR_PR19] = 0x5c;
	t->nwrites = 0;
	CHECK(wd24_plan_mode(t, wd24_mode_find("640x480-8")) == -ENODEV &&
	      t->nwrites == 2, "PR19 changed after load: mode refused after the read idiom");

	/* a clock change is never silent */
	st_loaded(t);
	t->bios.pr68 = 0x15;
	t->nwrites = 0;
	from = t->nev;
	ok = wd24_plan_mode(t, wd24_mode_find("640x480-8")) == -EPERM &&
	     st_writes(t, from, WD24_SR, WD24_SR_PR68) == 0;
	CHECK(ok, "clock change without allow_clock: -EPERM, SR31 untouched");
	st_loaded(t);
	t->bios.pr68 = 0x15;
	t->reg[WD24_SR][WD24_SR_PR68] = 0x15;
	t->allow_clock = true;
	from = t->nev;
	ok = wd24_plan_mode(t, wd24_mode_find("640x480-8")) == 0 &&
	     st_writes(t, from, WD24_SR, WD24_SR_PR68) == 1 &&
	     t->reg[WD24_SR][WD24_SR_PR68] == 0x0d &&
	     t->reg[WD24_SR][WD24_SR_PR72] == 0x00;
	CHECK(ok, "clock change with allow_clock: only b4:3, SR35 restored");

	CHECK(t->dropped == 0, "event log never overflowed");
#undef CHECK
	return fails;
}
