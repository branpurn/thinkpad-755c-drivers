// SPDX-License-Identifier: GPL-2.0
/*
 * Cross-check: the wd24adv dry-run plans must make exactly the register
 * writes (order and values) that wd90c24fb's own register layer makes for
 * the same job, apart from the documented differences:
 *   - wd24adv skips wd_restore_ext()'s clock block when it is a no-op;
 *   - wd24adv ends a packed mode set with SR06 = saved (as wd90c24fb's
 *     unblank, hw.c 640, leaves it).
 * wd90c24fb's wd90c24_hw.c is compiled here unmodified, against a tiny
 * port model initialised from wd24adv's boot model.
 */
#include <stdio.h>
#include <string.h>
#include "../../wd90c24fb/wd90c24_hw.h"
#include "../wd24adv_core.h"

static int fails, passes;
#define T(cond, what) do { if (cond) passes++; else { fails++; \
	printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

/* ---- port model ---- */
static u8 reg[WD24_NRF][256];
static u8 idx_sr, idx_gr, idx_cr, idx_ar;
static bool ar_ff;

struct w { u8 rf, idx, val; };
static struct w trace[1024];
static unsigned int ntrace;

static void rec(u8 rf, u8 idx, u8 val)
{
	if (ntrace < 1024)
		trace[ntrace++] = (struct w){ rf, idx, val };
	reg[rf][idx] = val;
}

u8 wd_inb(u16 p)
{
	switch (p) {
	case 0x3cc: return reg[WD24_MISC][0];
	case 0x3c4: return idx_sr;
	case 0x3c5: return reg[WD24_SR][idx_sr];
	case 0x3ce: return idx_gr;
	case 0x3cf: return reg[WD24_GR][idx_gr];
	case 0x3d4: return idx_cr;
	case 0x3d5: return reg[WD24_CR][idx_cr];
	case 0x3da: ar_ff = false; return 0;
	default: return 0xff;
	}
}

void wd_outb(u8 v, u16 p)
{
	switch (p) {
	case 0x3c2: rec(WD24_MISC, 0, v); break;
	case 0x3c4: idx_sr = v; break;
	case 0x3c5: rec(WD24_SR, idx_sr, v); break;
	case 0x3ce: idx_gr = v; break;
	case 0x3cf: rec(WD24_GR, idx_gr, v); break;
	case 0x3d4: idx_cr = v; break;
	case 0x3d5: rec(WD24_CR, idx_cr, v); break;
	case 0x3c0:
		if (!ar_ff)
			idx_ar = v & 0x3f;
		else
			rec(WD24_AR, idx_ar & 0x1f, v);
		/* index 20h with no data byte: palette address source */
		if (!ar_ff && v == 0x20)
			rec(WD24_AR, 0x20, 0x20);
		else
			ar_ff = !ar_ff;
		break;
	default:
		printf("FAIL: wd90c24fb touched port %04x\n", p);
		fails++;
	}
}

u16 wd_inw(u16 p) { (void)p; fails++; printf("FAIL inw\n"); return 0; }
void wd_outw(u16 v, u16 p) { (void)v; (void)p; fails++; printf("FAIL outw\n"); }
void wd_msleep(unsigned int ms) { (void)ms; }
void wd_udelay(unsigned int us) { (void)us; }
void wd_win_write(struct wd_hw *hw, u32 o, const void *s, size_t n) { (void)hw; (void)o; (void)s; (void)n; }
void wd_win_writeb(struct wd_hw *hw, u32 o, u8 v) { (void)hw; (void)o; (void)v; }
u8 wd_win_readb(struct wd_hw *hw, u32 o) { (void)hw; (void)o; return 0; }

/* ---- helpers ---- */
static struct wd_hw hw;
static struct wd_ext_state bios;
static struct wd24_ctx c;
static struct w got[WD24_MAXEV];
static unsigned int ngot;

static void reset(void)
{
	unsigned int i;

	memset(reg, 0, sizeof(reg));
	for (i = 0; i < wd24_boot_model_n; i++)
		reg[wd24_boot_model[i].rf][wd24_boot_model[i].idx] =
			wd24_boot_model[i].val;
	ar_ff = false;
	memset(&hw, 0, sizeof(hw));
	hw.crtc = 0x3d4;
	hw.chip = WD_C24;
	hw.pr18 = WD24_LIVE_PR18;
	hw.pr19 = WD24_LIVE_PR19;
	hw.bank = -1;
	/* wd90c24fb load: probe panel read, save_ext, relock */
	(void)wd_read_pr19(&hw);
	wd_save_ext(&hw, &bios);
	wd_relock(&hw, &bios);

	wd24_ctx_init(&c, &wd24_dryrun_backend);
	wd24_model_boot(&c);
}

/* wd24adv writes tagged "vgastate ..." are its copy of Linux vgastate
 * (save_vga/restore_vga, which wd90c24fb calls but this harness does not
 * compile); they are compared against the vgastate source order by
 * test_vga, not here */
static bool is_vgastate(const struct wd24_event *e)
{
	return e->note && !strncmp(e->note, "vgastate", 8);
}

static void wd24_writes(unsigned int from)
{
	ngot = 0;
	for (unsigned int i = from; i < c.nev; i++)
		if (c.ev[i].type == WD24_EV_WRITE && !is_vgastate(&c.ev[i]))
			got[ngot++] = (struct w){ c.ev[i].rf, c.ev[i].idx, c.ev[i].val };
}

/* stand-in for wd90c24fb's restore_vga(): apply wd24adv's vgastate
 * writes from events [from, nev) to the port model, untraced */
static void replay_vgastate(unsigned int from)
{
	for (unsigned int i = from; i < c.nev; i++)
		if (c.ev[i].type == WD24_EV_WRITE && is_vgastate(&c.ev[i]))
			reg[c.ev[i].rf][c.ev[i].idx] = c.ev[i].val;
}

/* drop wd_restore_ext's clock block (SR35=50 .. SR35=P) from trace[from..] */
static void drop_clock(unsigned int from)
{
	unsigned int i, o = from;
	bool in = false;

	for (i = from; i < ntrace; i++) {
		const struct w *x = &trace[i];

		if (x->rf == WD24_SR && x->idx == WD24_SR_PR72 && x->val == 0x50) {
			in = true;
			continue;
		}
		if (in) {
			if (x->rf == WD24_SR && x->idx == WD24_SR_PR72)
				in = false;
			continue;
		}
		trace[o++] = *x;
	}
	ntrace = o;
}

static bool same(unsigned int from, const char *what)
{
	unsigned int n = ntrace - from, i;

	if (n != ngot) {
		printf("  %s: wd90c24fb %u writes, wd24adv %u\n", what, n, ngot);
		return false;
	}
	for (i = 0; i < n; i++) {
		const struct w *a = &trace[from + i], *b = &got[i];

		if (a->rf != b->rf || a->idx != b->idx || a->val != b->val) {
			printf("  %s: #%u wd90c24fb %s%02X=%02X, wd24adv %s%02X=%02X\n",
			       what, i, wd24_rf_name(a->rf), a->idx, a->val,
			       wd24_rf_name(b->rf), b->idx, b->val);
			return false;
		}
	}
	return true;
}

/* model (wd24adv) and port model (wd90c24fb) agree on every known register */
static bool same_state(void)
{
	for (int rf = 0; rf < WD24_NRF; rf++)
		for (int i = 0; i < WD24_NIDX; i++)
			if (((c.known[rf] >> i) & 1) && c.reg[rf][i] != reg[rf][i]) {
				printf("  state %s%02X: wd24adv %02X wd90c24fb %02X\n",
				       wd24_rf_name(rf), i, c.reg[rf][i], reg[rf][i]);
				return false;
			}
	return true;
}

static void to_std(const struct wd24_std *s, struct wd_std *d)
{
	memcpy(d, s, sizeof(*d));
}

int main(void)
{
	const struct wd24_mode *m480 = wd24_mode_find("640x480-8");
	const struct wd24_mode *m400 = wd24_mode_find("640x400-8");
	const struct wd24_mode *m13 = wd24_mode_find("320x200-8");
	struct wd_std std, ref;
	struct wd_ext_state ext;
	struct wd24_ext e24;
	unsigned int from;

	_Static_assert(sizeof(struct wd_std) == sizeof(struct wd24_std), "std layout");

	/* load */
	memset(reg, 0, sizeof(reg));
	reset();
	from = 0;
	T(wd24_plan_load(&c) == 0, "wd24 load");
	wd24_writes(0);
	T(same(from, "load"), "load = wd_read_pr19 + wd_save_ext + wd_relock");
	T(same_state(), "load end state");
	T(c.bios.pr20 == bios.pr20 && c.bios.pr5 == bios.pr5 &&
	  c.bios.pr68 == bios.pr68 && c.bios.pr31 == bios.pr31 &&
	  c.bios.pr4 == bios.pr4 && c.bios.pr72 == bios.pr72,
	  "saved BIOS state matches wd_save_ext");

	/* 640x480-8 tables are wd90c24fb's */
	wd_mode_640x480x8(&hw, &bios, &ref, &ext);
	T(!memcmp(&ref, m480->std, sizeof(ref)), "640x480 std = wd_mode_640x480x8");
	wd24_mode_ext(m480, &c.bios, &e24);
	T(e24.pr1 == ext.pr1 && e24.pr3 == ext.pr3 && e24.pr4 == ext.pr4 &&
	  e24.pr13 == ext.pr13 && e24.pr14 == ext.pr14 && e24.pr15 == ext.pr15 &&
	  e24.pr16 == ext.pr16 && e24.pr31 == ext.pr31 && e24.pr30a == ext.pr30a &&
	  e24.pr68 == ext.pr68 && e24.misc == ext.misc && e24.pr0a == ext.pr0a,
	  "packed ext = wd_mode_640x480x8 ext");

	/* packed modes: wd_read_pr19 + wd_set_mode + SR06=saved */
	for (int k = 0; k < 2; k++) {
		const struct wd24_mode *m = k ? m400 : m480;

		reset();
		wd24_plan_load(&c);
		wd_mode_640x480x8(&hw, &bios, &ref, &ext);
		to_std(m->std, &std);
		from = ntrace;
		(void)wd_read_pr19(&hw);
		wd_set_mode(&hw, &std, &ext);
		wd_wrinx(WD_SEQ, PR20, bios.pr20);
		drop_clock(from);
		{ unsigned int f = c.nev;
		  T(wd24_plan_mode(&c, m) == 0, m->name);
		  wd24_writes(f); }
		T(same(from, m->name), "packed mode = wd_set_mode sequence");
		T(same_state(), "packed mode end state");
	}

	/* 320x200-8: restore_ext(bios) + write_std(13h) + relock + unblank */
	reset();
	wd24_plan_load(&c);
	to_std(m13->std, &std);
	from = ntrace;
	(void)wd_read_pr19(&hw);
	wd_screen_on(&hw, false);
	wd_restore_ext(&hw, &bios);
	wd_write_std(&hw, &std);
	wd_relock(&hw, &bios);
	wd_screen_on(&hw, true);
	drop_clock(from);
	{ unsigned int f = c.nev;
	  T(wd24_plan_mode(&c, m13) == 0, "320x200 plan");
	  wd24_writes(f); }
	T(same(from, "320x200-8"), "320x200 = wd90c24fb writers, 13h regs");
	T(same_state(), "320x200 end state");

	/* text restore after 320x200: wd_restore_text minus panel; restore_vga
	 * is stood in for by wd24adv's own vgastate writes */
	{ unsigned int f = c.nev;
	  T(wd24_plan_text(&c) == 0, "text plan");
	  wd24_writes(f);
	  from = ntrace;
	  wd_restore_ext(&hw, &bios);
	  replay_vgastate(f);
	  wd_relock(&hw, &bios);
	  wd_screen_on(&hw, true);
	  drop_clock(from); }
	T(same(from, "text"), "text restore = wd_restore_text ext part");
	T(same_state(), "text end state");

	printf("wd24adv xcheck: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
