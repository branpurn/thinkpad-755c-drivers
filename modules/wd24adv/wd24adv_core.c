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
 * The plans run unchanged on two backends: the dry-run recorder (a model of
 * the chip, wd24_boot_model) and the port backend (wd24adv_port.c), which
 * the kernel module uses only with dryrun=0 live=1.  With a live backend
 * every plan starts with a read-only entry check (wd24_entry_check: zero
 * data writes on failure), and every write is read back (wd24_verify); a
 * mismatch stops the plan and runs wd24_restore_all().
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

/* start a new event log; counters and saved state are kept */
void wd24_log_reset(struct wd24_ctx *c)
{
	c->nev = 0;
	c->dropped = 0;
}

static void ev_check(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 got,
		     u8 want, const char *why)
{
	struct wd24_event e = { .type = WD24_EV_CHECK, .rf = rf, .idx = idx,
		.val = got, .old = want, .old_known = true, .note = why };

	ev_add(c, &e);
}

static void ev_bulk(struct wd24_ctx *c, u8 plane, const char *what)
{
	struct wd24_event e = { .type = WD24_EV_BULK, .val = plane,
		.note = what };

	ev_add(c, &e);
	c->nbulk++;
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

/* reads update the model, so with a live backend it holds what the chip
 * last said (dry-run: the read returns the model, nothing changes) */
u8 wd24_read(struct wd24_ctx *c, enum wd24_rf rf, u8 idx)
{
	u8 v = c->be->read(c, rf, idx);

	if ((unsigned int)rf < WD24_NRF && idx < WD24_NIDX) {
		c->reg[rf][idx] = v;
		c->known[rf] |= (u64)1 << idx;
	}
	return v;
}

/* registers whose guard rule depends on the current value */
static bool guard_needs_old(enum wd24_rf rf, u8 idx)
{
	return (rf == WD24_SR && (idx == WD24_SR_PR20 || idx == WD24_SR_PR68)) ||
	       (rf == WD24_GR && (idx == WD24_GR_PR1 || idx == WD24_GR_PR4));
}

/*
 * Bits that must read back as written.  Standard registers: the IBM VGA
 * read/write bits (reserved bits masked).  Paradise registers: SR06 is a
 * latch (wd24probe3, live: wrote 4F, read 0F; the lock state is only
 * visible through the 3C4 index width), GR0F bits 2:0, PR0A/PR0B bits 6:0
 * (data sheet); CR34 and SR35 echoed exactly in wd24probe3.  The full-byte
 * echo of the other Paradise registers is UNPROVEN: a difference stops the
 * plan and restores (fail-safe).  0 = not verified (AR 20h has no data).
 */
static u8 rb_mask(enum wd24_rf rf, u8 idx)
{
	static const u8 sr[5] = { 0x03, 0x3d, 0x0f, 0x3f, 0x0e };
	static const u8 gr[9] = { 0x0f, 0x0f, 0x0f, 0x1f, 0x03, 0x7b, 0x0f, 0x0f, 0xff };
	static const u8 ar[0x15] = { 0x3f, 0x3f, 0x3f, 0x3f, 0x3f, 0x3f, 0x3f, 0x3f,
				     0x3f, 0x3f, 0x3f, 0x3f, 0x3f, 0x3f, 0x3f, 0x3f,
				     0xef, 0xff, 0x3f, 0x0f, 0x0f };

	switch (rf) {
	case WD24_MISC:
		return 0xef;
	case WD24_SR:
		return idx < 5 ? sr[idx] : 0xff;
	case WD24_GR:
		if (idx < 9)
			return gr[idx];
		if (idx == WD24_GR_PR0A || idx == WD24_GR_PR0B)
			return 0x7f;
		return idx == WD24_GR_PR5 ? 0x07 : 0xff;
	case WD24_AR:
		return idx < 0x15 ? ar[idx] : 0;
	case WD24_CR:
		return 0xff;
	default:
		return 0;
	}
}

static int mismatch(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 wrote,
		    u8 got, const char *why)
{
	struct wd24_event e = { .type = WD24_EV_MISMATCH, .rf = rf, .idx = idx,
		.old = wrote, .old_known = true, .val = got, .note = why };

	ev_add(c, &e);
	c->nmismatch++;
	c->failed = true;
	return c->restoring ? 0 : -EIO;
}

/* read back what was just written (live backends only) */
int wd24_verify(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 val)
{
	u8 mask = rb_mask(rf, idx), got;

	if (!mask)
		return 0;
	got = wd24_read(c, rf, idx);
	if (rf == WD24_SR && idx == WD24_SR_PR20) {
		int want = (val & 0x58) == 0x48, unl;

		if (got != (val & 0x0f))
			return mismatch(c, rf, idx, val, got, "SR06 latch: bits 3:0 must echo, 7:4 read 0");
		unl = c->be->seq_unlocked ? c->be->seq_unlocked(c) : -1;
		if (unl != want)
			return mismatch(c, rf, idx, val, (u8)unl,
					"SR06 latch: 3C4 index width test (val = 1 unlocked, 0 locked)");
		return 0;
	}
	if ((got ^ val) & mask)
		return mismatch(c, rf, idx, val, got, "readback differs");
	return 0;
}

int wd24_write(struct wd24_ctx *c, enum wd24_rf rf, u8 idx, u8 val)
{
	struct wd24_event e = { .rf = rf, .idx = idx, .val = val,
				.note = c->tag };
	int err;

	if ((unsigned int)rf >= WD24_NRF || idx >= WD24_NIDX)
		return -EINVAL;
	if (c->be->live && guard_needs_old(rf, idx))
		(void)wd24_read(c, rf, idx);
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
	if (c->be->live)
		return wd24_verify(c, rf, idx, val);
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

/*
 * Read-only entry check, run first by every plan (also in dry-run, where
 * it reads the model).  Only index-port writes happen (needed to read an
 * indexed register; the port backend restores the indices); no data
 * register is written, so a refusal here leaves the chip untouched.
 *
 * Expected values and their evidence:
 *  - 3CC b0 = 1 (colour CRTC at 3D4; live, every probe);
 *  - CR34 (PR1B) reads A0 twice: the value the 755C holds both at boot and
 *    under wd90c24fb (wd24probe2/3 live reads; boot model).  With PR1B =
 *    A0 PR18/PR19 are readable without a write.  Whether PR1B is a latch
 *    that could read A0 while not open is UNPROVEN; the C7/54 reads below
 *    would then fail, which refuses (fail-safe);
 *  - PR18 = C7, PR19 = 54 (live: wd90c24fb load message, wd24probe3);
 *  - load: GR0F/CR29 = 00/00 (POST trace), 00/85 (live: the 755C text
 *    state after wd90c24fb text=1 + rmmod, wd24probe3 read 3CC=67 GR0E=40
 *    CR11=AE SR06=0F) or 05/85 (wd24probe3, read while wd90c24fb was live);
 *    later plans: exactly what the previous wd24adv plan left (c->exp_*);
 *  - SR06 reads 0F and the 3C4 index width test says "locked" (wd24probe3:
 *    "3C4test pre=01").  The task's literal "GR0F=05, CR29=85" is the
 *    wd90c24fb-live state; after wd90c24fb's unload relock the BIOS values
 *    are expected, so both pairs are accepted at load only;
 *  - load also requires AR10 b0 = 0 (text mode: vgastate save_vga_text
 *    returns early in graphics, vgastate.c "if (attr10 & 1) return").
 * WD24_PH_RESTORE (text restore on rmmod / trial expiry) refuses only when
 * the register view itself is invalid (MISC b0, CR34); other differences
 * are logged and the restore proceeds, because it writes only values read
 * from this chip at load.
 */
int wd24_entry_check(struct wd24_ctx *c, enum wd24_phase ph)
{
	bool load = ph == WD24_PH_LOAD, strict = ph != WD24_PH_RESTORE;
	unsigned int bad = 0;
	u8 misc, a, b, pr18, pr19, gr0f, cr29, sr06;
	int unl;

#define ENTRY_FAIL(rf, idx, got, want, why, fatal) do { \
		ev_check(c, rf, idx, got, want, why); \
		if (fatal) { wd24_note(c, "REFUSED by the entry check: no data register written"); \
			return -ENODEV; } \
		bad++; } while (0)

	misc = wd24_read(c, WD24_MISC, 0);
	if (!(misc & 0x01))
		ENTRY_FAIL(WD24_MISC, 0, misc, misc | 1, "3CC b0 = 0: mono CRTC, not the 755C", true);
	if (ph == WD24_PH_MODE && misc != c->exp_misc)
		ENTRY_FAIL(WD24_MISC, 0, misc, c->exp_misc, "MISC changed since the last plan", true);
	a = wd24_read(c, WD24_CR, WD24_CR_PR1B);
	b = wd24_read(c, WD24_CR, WD24_CR_PR1B);
	if (a != WD24_PR1B_FP || b != WD24_PR1B_FP)
		ENTRY_FAIL(WD24_CR, WD24_CR_PR1B, a != WD24_PR1B_FP ? a : b, WD24_PR1B_FP,
			   "CR34 must read A0 (twice): PR18/PR19 not readable without a write", true);
	pr18 = wd24_read(c, WD24_CR, WD24_CR_PR18);
	pr19 = wd24_read(c, WD24_CR, WD24_CR_PR19);
	if (pr18 != WD24_LIVE_PR18)
		ENTRY_FAIL(WD24_CR, WD24_CR_PR18, pr18, WD24_LIVE_PR18, "PR18 is not the live-verified C7", strict);
	if (pr19 != WD24_LIVE_PR19)
		ENTRY_FAIL(WD24_CR, WD24_CR_PR19, pr19, WD24_LIVE_PR19,
			   "PR19 is not the live-verified 54 (LCD only, centring)", strict);
	gr0f = wd24_read(c, WD24_GR, WD24_GR_PR5);
	cr29 = wd24_read(c, WD24_CR, WD24_CR_PR10);
	if (load) {
		if (!((gr0f == 0x00 && cr29 == 0x00) || (gr0f == 0x00 && cr29 == 0x85) ||
		      (gr0f == 0x05 && cr29 == 0x85)))
			ENTRY_FAIL(WD24_GR, WD24_GR_PR5, gr0f, cr29,
				   "GR0F/CR29 (val/old) not 00/00 (BIOS), 00/85 (after wd90c24fb) or 05/85 (wd90c24fb)", true);
	} else {
		if (gr0f != c->exp_gr0f)
			ENTRY_FAIL(WD24_GR, WD24_GR_PR5, gr0f, c->exp_gr0f, "GR0F changed since the last plan", strict);
		if (cr29 != c->exp_cr29)
			ENTRY_FAIL(WD24_CR, WD24_CR_PR10, cr29, c->exp_cr29, "CR29 changed since the last plan", strict);
	}
	sr06 = wd24_read(c, WD24_SR, WD24_SR_PR20);
	if (sr06 != (load ? 0x0f : (c->exp_sr06 & 0x0f)))
		ENTRY_FAIL(WD24_SR, WD24_SR_PR20, sr06, load ? 0x0f : c->exp_sr06 & 0x0f,
			   "SR06 does not read the locked value", strict);
	unl = c->be->seq_unlocked ? c->be->seq_unlocked(c) : 0;
	if (unl != 0)
		ENTRY_FAIL(WD24_SR, WD24_SR_PR20, (u8)unl, 0,
			   "3C4 index width test: SR06 latch not locked (val 1 = unlocked, FF = no answer)", strict);
	if (load && (wd24_read(c, WD24_AR, 0x10) & 0x01))
		ENTRY_FAIL(WD24_AR, 0x10, c->reg[WD24_AR][0x10], 0, "AR10 b0 = 1: graphics mode, not the text console", true);
#undef ENTRY_FAIL
	wd24_note(c, bad ? "entry check: differences logged, restore proceeds"
			 : "entry check passed (reads only)");
	return 0;
}

static void set_expect(struct wd24_ctx *c)
{
	c->exp_misc = c->reg[WD24_MISC][0];
	c->exp_gr0f = c->reg[WD24_GR][WD24_GR_PR5];
	c->exp_cr29 = c->reg[WD24_CR][WD24_CR_PR10];
	c->exp_sr06 = c->reg[WD24_SR][WD24_SR_PR20];
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
	if (s == &c->bios) {
		c->saved = true;	/* the guard's relock values */
		c->have |= WD24_HAVE_LOCKS;
	}
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
	TRY(wd24_write(c, WD24_SR, WD24_SR_PR72, s->pr72));
	if (s == &c->bios)
		c->have |= WD24_HAVE_EXT;
	return 0;
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
	u8 cur68 = ((c->known[WD24_SR] >> WD24_SR_PR68) & 1) ?
		   c->reg[WD24_SR][WD24_SR_PR68] : c->bios.pr68;
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
	if (s != &c->bios || (c->have & WD24_HAVE_EXT))
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

/* ---- standard VGA state, DAC and video memory (Linux vgastate) ------------
 *
 * wd90c24fb saves VGA_SAVE_MODE | VGA_SAVE_FONTS | VGA_SAVE_CMAP with
 * save_vga() right after wd_save_ext() at load (wd90c24fb_main.c 1444-1448)
 * and restores it with restore_vga() between wd_restore_ext() and
 * wd_relock() (wd_restore_text, main.c 802-815).  The functions below make
 * the same register writes in the same order as drivers/video/vgastate.c
 * (Linux 6.18.57; function names cited in the tags), with one deliberate
 * superset: all four planes are saved in full (64 KiB each) instead of
 * vgastate's 32 KiB of plane 2, plane 3 and 2 x 8 KiB of planes 0/1.
 * Every write still goes through the guard: these are standard registers.
 */

int wd24_save_cmap(struct wd24_ctx *c)
{
	if (c->be->dac_read) {
		c->be->dac_read(c, 0xff, c->cmap);
		ev_bulk(c, 0, "vgastate save_vga_cmap: 3C6=FF, 3C7=00, 768 x read 3C9");
	} else {
		ev_bulk(c, 0, "vgastate save_vga_cmap: (dry-run: DAC not read)");
	}
	c->have |= WD24_HAVE_CMAP;
	return 0;
}

int wd24_restore_cmap(struct wd24_ctx *c)
{
	if (!(c->have & WD24_HAVE_CMAP))
		return 0;
	if (c->be->dac_write) {
		u8 back[768];
		unsigned int i, bad = 0;

		c->be->dac_write(c, 0xff, c->cmap);
		ev_bulk(c, 0, "vgastate restore_vga_cmap: 3C6=FF, 3C8=00, 768 x write 3C9");
		if (c->be->live && c->be->dac_read) {
			c->be->dac_read(c, 0xff, back);
			for (i = 0; i < 768; i++)
				bad += ((back[i] ^ c->cmap[i]) & 0x3f) != 0;
			if (bad)
				return mismatch(c, WD24_MISC, 0xff, 0, bad > 255 ? 255 : bad,
						"DAC readback: val = differing entries");
		}
	} else {
		ev_bulk(c, 0, "vgastate restore_vga_cmap: (dry-run: DAC not written)");
	}
	return 0;
}

/* vgastate save_vga_mode: reads only (CRTC, ATTR, GFX, SEQ, MISC) */
int wd24_save_std(struct wd24_ctx *c)
{
	struct wd24_std *v = &c->vga;
	int i;

	v->misc = wd24_read(c, WD24_MISC, 0);
	for (i = 0; i < 25; i++)
		v->crtc[i] = wd24_read(c, WD24_CR, i);
	for (i = 0; i < 21; i++)
		v->attr[i] = wd24_read(c, WD24_AR, i);
	for (i = 0; i < 9; i++)
		v->gfx[i] = wd24_read(c, WD24_GR, i);
	for (i = 0; i < 5; i++)
		v->seq[i] = wd24_read(c, WD24_SR, i);
	c->have |= WD24_HAVE_STD;
	wd24_note(c, "vgastate save_vga_mode: MISC, CR00-18, AR00-14, GR00-08, SR00-04 read");
	return 0;
}

/* vgastate restore_vga_mode (the AR index 00h write is part of the
 * backend's AR access, which always clears the palette address source) */
int wd24_restore_std(struct wd24_ctx *c)
{
	const struct wd24_std *v = &c->vga;
	int i;

	if (!(c->have & WD24_HAVE_STD))
		return 0;
	c->tag = "vgastate restore_vga_mode";
	TRY(wd24_write(c, WD24_MISC, 0, v->misc));
	TRY(wd24_write(c, WD24_SR, 0x01, v->seq[1] | 0x20));
	TRY(wd24_write(c, WD24_SR, 0x00, 0x01));
	for (i = 2; i < 5; i++)
		TRY(wd24_write(c, WD24_SR, i, v->seq[i]));
	TRY(wd24_write(c, WD24_CR, 0x11, v->crtc[0x11] & ~0x80));
	for (i = 0; i < 25; i++)
		TRY(wd24_write(c, WD24_CR, i, v->crtc[i]));
	for (i = 0; i < 9; i++)
		TRY(wd24_write(c, WD24_GR, i, v->gfx[i]));
	for (i = 0; i < 21; i++)
		TRY(wd24_write(c, WD24_AR, i, v->attr[i]));
	TRY(wd24_write(c, WD24_SR, 0x00, 0x03));
	TRY(wd24_write(c, WD24_SR, 0x01, v->seq[1] & ~0x20));
	return wd24_write(c, WD24_AR, 0x20, 0x20);
}

static const char *const plane_note[2][4] = {
	{ "planar read: plane 0 (text characters), 64 KiB",
	  "planar read: plane 1 (text attributes), 64 KiB",
	  "planar read: plane 2 (fonts), 64 KiB",
	  "planar read: plane 3, 64 KiB" },
	{ "planar write: plane 0 (text characters), 64 KiB",
	  "planar write: plane 1 (text attributes), 64 KiB",
	  "planar write: plane 2 (fonts), 64 KiB",
	  "planar write: plane 3, 64 KiB" },
};

/* select plane p for CPU access: vgastate's per-font block */
static int plane_select(struct wd24_ctx *c, int p)
{
	TRY(wd24_write(c, WD24_SR, 0x02, 1 << p));
	TRY(wd24_write(c, WD24_SR, 0x04, 0x06));
	TRY(wd24_write(c, WD24_GR, 0x04, p));
	TRY(wd24_write(c, WD24_GR, 0x05, 0x00));
	return wd24_write(c, WD24_GR, 0x06, 0x05);
}

/* vgastate save_vga_text, all four planes in full */
int wd24_save_planes(struct wd24_ctx *c)
{
	u8 gr4, gr5, gr6, seq1, seq2, seq4;
	int p;

	c->tag = "vgastate save_vga_text";
	gr4 = wd24_read(c, WD24_GR, 0x04);
	gr5 = wd24_read(c, WD24_GR, 0x05);
	gr6 = wd24_read(c, WD24_GR, 0x06);
	seq2 = wd24_read(c, WD24_SR, 0x02);
	seq4 = wd24_read(c, WD24_SR, 0x04);
	seq1 = wd24_read(c, WD24_SR, 0x01);
	TRY(wd24_write(c, WD24_SR, 0x00, 0x01));
	TRY(wd24_write(c, WD24_SR, 0x01, seq1 | 0x20));
	TRY(wd24_write(c, WD24_SR, 0x00, 0x03));
	for (p = 0; p < 4; p++) {
		TRY(plane_select(c, p));
		if (c->planes && c->be->mem_read)
			c->be->mem_read(c, 0, c->planes + p * WD24_PLANE, WD24_PLANE);
		ev_bulk(c, p, c->planes ? plane_note[0][p] : "planar read: (dry-run: memory not read)");
	}
	TRY(wd24_write(c, WD24_SR, 0x02, seq2));
	TRY(wd24_write(c, WD24_SR, 0x04, seq4));
	TRY(wd24_write(c, WD24_GR, 0x04, gr4));
	TRY(wd24_write(c, WD24_GR, 0x05, gr5));
	TRY(wd24_write(c, WD24_GR, 0x06, gr6));
	TRY(wd24_write(c, WD24_SR, 0x00, 0x01));
	TRY(wd24_write(c, WD24_SR, 0x01, seq1 & ~0x20));
	TRY(wd24_write(c, WD24_SR, 0x00, 0x03));
	TRY(wd24_write(c, WD24_SR, 0x01, seq1));
	if (c->planes)
		c->have |= WD24_HAVE_PLANES;
	return 0;
}

/* vgastate restore_vga_text (depth-4 block included: write mode 0 with
 * rotate 0, bit mask FF, set/reset off), all four planes, each read back */
int wd24_restore_planes(struct wd24_ctx *c)
{
	u8 gr1, gr3, gr4, gr5, gr6, gr8, seq1, seq2, seq4;
	int p, err = 0;

	if (!(c->have & WD24_HAVE_STD))
		return 0;
	c->tag = "vgastate restore_vga_text";
	gr1 = wd24_read(c, WD24_GR, 0x01);
	gr3 = wd24_read(c, WD24_GR, 0x03);
	gr4 = wd24_read(c, WD24_GR, 0x04);
	gr5 = wd24_read(c, WD24_GR, 0x05);
	gr6 = wd24_read(c, WD24_GR, 0x06);
	gr8 = wd24_read(c, WD24_GR, 0x08);
	seq2 = wd24_read(c, WD24_SR, 0x02);
	seq4 = wd24_read(c, WD24_SR, 0x04);
	seq1 = wd24_read(c, WD24_SR, 0x01);
	TRY(wd24_write(c, WD24_SR, 0x00, 0x01));
	TRY(wd24_write(c, WD24_SR, 0x01, seq1 | 0x20));
	TRY(wd24_write(c, WD24_SR, 0x00, 0x03));
	TRY(wd24_write(c, WD24_GR, 0x03, 0x00));
	TRY(wd24_write(c, WD24_GR, 0x08, 0xff));
	TRY(wd24_write(c, WD24_GR, 0x01, 0x00));
	for (p = 0; p < 4; p++) {
		const u8 *src = c->planes ? c->planes + p * WD24_PLANE : NULL;

		TRY(plane_select(c, p));
		if (!(c->have & WD24_HAVE_PLANES) || !src || !c->be->mem_write) {
			ev_bulk(c, p, "planar write: (dry-run: memory not written)");
			continue;
		}
		c->be->mem_write(c, 0, src, WD24_PLANE);
		ev_bulk(c, p, plane_note[1][p]);
		if (c->be->live && c->be->mem_read) {
			static u8 chunk[256];
			u32 o, i, bad = 0;

			for (o = 0; o < WD24_PLANE; o += sizeof(chunk)) {
				c->be->mem_read(c, o, chunk, sizeof(chunk));
				for (i = 0; i < sizeof(chunk); i++)
					bad += chunk[i] != src[o + i];
			}
			if (bad && !err)
				err = mismatch(c, WD24_GR, 0x04, p, bad > 255 ? 255 : bad,
					       "plane readback: old = plane, val = differing bytes (max FF)");
		}
	}
	TRY(wd24_write(c, WD24_SR, 0x00, 0x01));
	TRY(wd24_write(c, WD24_SR, 0x01, seq1 & ~0x20));
	TRY(wd24_write(c, WD24_SR, 0x00, 0x03));
	TRY(wd24_write(c, WD24_GR, 0x01, gr1));
	TRY(wd24_write(c, WD24_GR, 0x03, gr3));
	TRY(wd24_write(c, WD24_GR, 0x04, gr4));
	TRY(wd24_write(c, WD24_GR, 0x05, gr5));
	TRY(wd24_write(c, WD24_GR, 0x06, gr6));
	TRY(wd24_write(c, WD24_GR, 0x08, gr8));
	TRY(wd24_write(c, WD24_SR, 0x01, seq1));
	TRY(wd24_write(c, WD24_SR, 0x02, seq2));
	TRY(wd24_write(c, WD24_SR, 0x04, seq4));
	return err;
}

/*
 * Everything back to the state saved at load, in wd_restore_text() order
 * (main.c 802-815): restore_ext(BIOS), restore_vga() [mode, fonts, cmap],
 * relock, unblank.  Used for text restore (rmmod, trial expiry, control
 * "text") and after any failure once a write has happened.  Readback
 * mismatches are logged but do not stop it (c->restoring); a guard refusal
 * stops only the step it happens in.  wd_restore_panel() is omitted: PR19,
 * PR57 and PR4 b5 are never written by wd24adv.
 */
int wd24_restore_all(struct wd24_ctx *c)
{
	bool was = c->restoring;
	unsigned int mis = c->nmismatch;
	int err = 0, e;

#define KEEP(x) do { e = (x); if (e && !err) err = e; } while (0)
	c->restoring = true;
	if (c->have & WD24_HAVE_EXT)
		KEEP(wd24_restore_ext(c, &c->bios));
	KEEP(wd24_restore_std(c));
	KEEP(wd24_restore_planes(c));
	KEEP(wd24_restore_cmap(c));
	if (c->have & WD24_HAVE_LOCKS)
		KEEP(wd24_relock(c, &c->bios));
	KEEP(wd24_screen_on(c, true));
	c->restoring = was;
	if (!err && c->nmismatch != mis)
		err = -EIO;	/* every step ran; a readback differed */
#undef KEEP
	wd24_note(c, "wd_restore_panel() omitted: PR19, PR57, PR4 b5 were never changed");
	if (!err)
		c->in_text = true;
	c->trial_pending = false;
	set_expect(c);
	return err;
}

/* ---- plans -------------------------------------------------------------- */

/*
 * wd90c24fb load (main.c 1444-1448): panel read (hw.c 75-84), wd_save_ext,
 * save_vga (cmap, mode, fonts: vgastate order), wd_relock.  wd24adv adds the
 * read-only entry check in front.  Nothing in the visible state changes:
 * the only data writes are value-identical idioms and vgastate's blank /
 * plane-select / restore block around the planar reads.
 */
int wd24_plan_load(struct wd24_ctx *c)
{
	int err;

	wd24_note(c, "load: entry check, panel check, save BIOS ext + VGA state, relock (wd90c24fb_main.c 1444-1448)");
	TRY(wd24_entry_check(c, WD24_PH_LOAD));
	err = wd24_check_panel(c);
	if (!err)
		err = wd24_save_ext(c, &c->bios);
	if (!err)
		err = wd24_save_cmap(c);
	if (!err)
		err = wd24_save_std(c);
	if (!err)
		err = wd24_save_planes(c);
	if (!err)
		err = wd24_relock(c, &c->bios);
	if (err) {
		if (c->nwrites)
			wd24_restore_all(c);
		return err;
	}
	c->in_text = true;
	set_expect(c);
	return 0;
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
 *
 * Any failure after the first write (guard refusal or readback mismatch)
 * runs wd24_restore_all().
 */
static int mode_steps(struct wd24_ctx *c, const struct wd24_mode *m)
{
	struct wd24_ext ext;

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

int wd24_plan_mode(struct wd24_ctx *c, const struct wd24_mode *m)
{
	unsigned int w0 = c->nwrites;
	const char *why;
	int err;

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
	if (c->be->live && !(c->have & WD24_HAVE_PLANES)) {
		wd24_note(c, "REFUSED: live mode set without saved video memory");
		return -EINVAL;
	}
	TRY(wd24_entry_check(c, WD24_PH_MODE));
	err = mode_steps(c, m);
	if (err) {
		if (c->nwrites != w0) {
			wd24_note(c, "mode set failed after a write: restoring the state saved at load");
			wd24_restore_all(c);
		}
		return err;
	}
	c->in_text = false;
	c->trial_pending = true;
	c->confirmed = false;
	set_expect(c);
	return 0;
}

int wd24_confirm(struct wd24_ctx *c)
{
	if (!c->trial_pending)
		return -EINVAL;
	c->confirmed = true;
	c->trial_pending = false;
	wd24_note(c, "trial: mode confirmed, kept until text restore or unload");
	return 0;
}

/* trial timer: revert unless confirmed (no-op if confirmed or in text) */
int wd24_trial_expire(struct wd24_ctx *c)
{
	if (!c->trial_pending)
		return 0;
	c->trial_pending = false;
	wd24_note(c, "trial expired without confirm: restoring text state");
	return wd24_plan_text(c);
}

/* text restore: wd_restore_text() (main.c 802-815) in full */
int wd24_plan_text(struct wd24_ctx *c)
{
	if (!c->saved)
		return -EINVAL;
	TRY(wd24_entry_check(c, WD24_PH_RESTORE));
	return wd24_restore_all(c);
}

/*
 * Optional test picture after a packed mode set: 16 vertical bars, 40
 * pixels wide, in the 16 text colours (the DAC is still the BIOS text DAC;
 * pixel values are the BIOS mode 03h AR00-0F entries), over the WHOLE
 * frame (xres * yres bytes), one 64 KiB PR0A bank at a time, so no stale
 * video memory (e.g. wd90c24fb's last fbcon frame) is ever shown.  From
 * 256 KiB on (the part of the frame outside the 4 x 64 KiB planar window
 * that text mode, vgastate and wd24adv's save/restore can reach) the bar
 * order is reversed: a correct picture shows reversed bars in the bottom
 * lines, a CRTC wrap at 256 KiB would show normal-order bars there.
 * Write-only; PR0A is left at bank 0 (as mode_steps() set it).
 * Video memory at and above 256 KiB is not saved/restored (not visible
 * in text mode).
 */
int wd24_pattern(struct wd24_ctx *c, const struct wd24_mode *m)
{
	static u8 line[2][640];
	u32 x, size, bank, L, end;

	if (m->ext != WD24_EXT_PACKED8 || c->in_text || m->xres > sizeof(line[0]))
		return -EINVAL;
	for (x = 0; x < m->xres; x++) {
		line[0][x] = c->vga.attr[(x / 40) & 15] & 0x3f;
		line[1][x] = c->vga.attr[15 - ((x / 40) & 15)] & 0x3f;
	}
	size = m->xres * m->yres;
	if (!c->be->mem_write) {
		ev_bulk(c, 0, "pattern: (dry-run: memory not written)");
		return 0;
	}
	c->tag = "pattern: PR0A bank per 64 KiB";
	for (bank = 0; bank * WD24_PLANE < size; bank++) {
		TRY(wd24_write(c, WD24_GR, WD24_GR_PR0A, bank * (WD24_PLANE >> 12)));
		end = (bank + 1) * WD24_PLANE < size ? (bank + 1) * WD24_PLANE : size;
		for (L = bank * WD24_PLANE; L < end; ) {
			u32 x0 = L % m->xres, n = m->xres - x0;

			if (n > end - L)
				n = end - L;
			c->be->mem_write(c, L - bank * WD24_PLANE,
					 line[L >= 4 * WD24_PLANE] + x0, n);
			L += n;
		}
	}
	TRY(wd24_write(c, WD24_GR, WD24_GR_PR0A, 0x00));
	ev_bulk(c, 0, "pattern: 16 colour bars, whole frame (banks 0-4), reversed from 256 KiB");
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
	      t->nwrites == 0, "PR19 changed after load: mode refused by the entry check, 0 writes");

	/* entry check: reads only, refusal = zero data writes */
	st_fresh(t, WD24_LIVE_PR19);
	t->reg[WD24_CR][WD24_CR_PR1B] = 0x00;
	CHECK(wd24_plan_load(t) == -ENODEV && t->nwrites == 0,
	      "entry: CR34 != A0 refused, 0 writes");
	st_fresh(t, WD24_LIVE_PR19);
	t->reg[WD24_MISC][0] = 0x66;
	CHECK(wd24_plan_load(t) == -ENODEV && t->nwrites == 0,
	      "entry: MISC b0 = 0 refused, 0 writes");
	st_fresh(t, WD24_LIVE_PR19);
	t->reg[WD24_CR][WD24_CR_PR18] = 0xc6;
	CHECK(wd24_plan_load(t) == -ENODEV && t->nwrites == 0,
	      "entry: PR18 != C7 refused, 0 writes");
	st_fresh(t, WD24_LIVE_PR19);
	t->reg[WD24_GR][WD24_GR_PR5] = 0x05;
	CHECK(wd24_plan_load(t) == -ENODEV && t->nwrites == 0,
	      "entry: GR0F/CR29 = 05/00 refused, 0 writes");
	st_fresh(t, WD24_LIVE_PR19);
	t->reg[WD24_GR][WD24_GR_PR5] = 0x05;
	t->reg[WD24_CR][WD24_CR_PR10] = 0x85;
	CHECK(wd24_plan_load(t) == 0 && t->nrefused == 0,
	      "entry: GR0F/CR29 = 05/85 (wd90c24fb live state) accepted");
	st_fresh(t, WD24_LIVE_PR19);
	t->reg[WD24_CR][WD24_CR_PR10] = 0x85;
	CHECK(wd24_plan_load(t) == 0 && t->nrefused == 0,
	      "entry: GR0F/CR29 = 00/85 (live text state after wd90c24fb unload) accepted");
	CHECK(t->bios.pr5 == 0x00 && t->bios.pr10 == 0x85,
	      "entry 00/85: saved relock values are 00/85");
	CHECK(wd24_plan_text(t) == 0 && t->nrefused == 0 &&
	      t->reg[WD24_GR][WD24_GR_PR5] == 0x00 && t->reg[WD24_CR][WD24_CR_PR10] == 0x85,
	      "entry 00/85: unload restore relocks to 00/85");
	st_fresh(t, WD24_LIVE_PR19);
	t->reg[WD24_CR][WD24_CR_PR10] = 0x05;
	CHECK(wd24_plan_load(t) == -ENODEV && t->nwrites == 0,
	      "entry: GR0F/CR29 = 00/05 refused, 0 writes");
	st_fresh(t, WD24_LIVE_PR19);
	t->reg[WD24_SR][WD24_SR_PR20] = 0x08;
	CHECK(wd24_plan_load(t) == -ENODEV && t->nwrites == 0,
	      "entry: SR06 not locked refused, 0 writes");
	st_fresh(t, WD24_LIVE_PR19);
	t->reg[WD24_AR][0x10] |= 0x01;
	CHECK(wd24_plan_load(t) == -ENODEV && t->nwrites == 0,
	      "entry: AR10 graphics refused, 0 writes");
	st_loaded(t);
	t->reg[WD24_CR][WD24_CR_PR10] = 0x85;
	t->nwrites = 0;
	CHECK(wd24_plan_mode(t, wd24_mode_find("640x480-8")) == -ENODEV &&
	      t->nwrites == 0, "entry: CR29 changed since load: mode refused, 0 writes");
	st_loaded(t);
	t->reg[WD24_CR][WD24_CR_PR19] = 0x5c;
	CHECK(wd24_plan_text(t) == 0 && t->nrefused == 0,
	      "entry (restore): PR19 difference logged, restore proceeds");
	st_loaded(t);
	t->reg[WD24_CR][WD24_CR_PR1B] = 0x00;
	CHECK(wd24_plan_text(t) == -ENODEV, "entry (restore): CR34 != A0 refused");

	/* a clock change is never silent */
	st_loaded(t);
	t->bios.pr68 = 0x15;
	t->reg[WD24_SR][WD24_SR_PR68] = 0x15;
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
