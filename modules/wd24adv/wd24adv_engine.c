// SPDX-License-Identifier: GPL-2.0
/*
 * wd24adv S0: BITBLT / hardware-cursor guard, validator and sequencer.
 * HOST MODEL ONLY: not in Kbuild, no caller in the module, and every
 * access is refused when the io is live (WD24X_LIVE_ALLOWED == 0).
 * See wd24adv_engine.h for the citation key (DS / OBS-FB / OBS-W /
 * UNPROVEN).  Written from the public data sheet; no vendor code.
 */
#ifndef __KERNEL__
#include <string.h>
#endif
#include "wd24adv_engine.h"

/* ---- closed register lists ---------------------------------------------- */

/* CNTRL1, DS 19.1.1 / 21.15.1: always packed (b9:8 = 01), always with the
 * activation bit (so the guard can never write the "0 while in progress"
 * value the DS 19.1.1 CAUTION warns about), never linear (b7/b6), never
 * destination = system I/O (b5:4 = 10), never reserved encodings. */
static const u16 c1_set[] = {
	WD24X_C1_ACT | WD24X_C1_PACKED,				/* COPY forward */
	WD24X_C1_ACT | WD24X_C1_PACKED | WD24X_C1_BACK,		/* COPY backward */
	WD24X_C1_ACT | WD24X_C1_PACKED | WD24X_C1_SRCF_FIXED,	/* FILL */
	WD24X_C1_ACT | WD24X_C1_PACKED | WD24X_C1_SRCF_MONO_HOST |
		WD24X_C1_SRC_IO,				/* MONO */
	WD24X_C1_ACT | WD24X_C1_PACKED | WD24X_C1_SRC_IO,	/* HOSTCOL */
};
/* CNTRL2, DS 19.1.2 / 21.15.2: no line draw, IRQ, quick start, update
 * destination, pattern, colour transparency; b1 reserved = 0. */
static const u16 c2_set[] = { 0x000, WD24X_C2_MTRANS };
static const u16 pitch_set[] = { WD24X_PITCH };
/* DS 19.7 Table 19-3; the X GC functions of design S4 */
static const u16 rop_set[] = {
	WD24X_ROP_SRC, WD24X_ROP_XOR, WD24X_ROP_CLEAR, WD24X_ROP_AND,
	WD24X_ROP_OR, WD24X_ROP_INVDST, WD24X_ROP_SET,
};
static const u16 mask_set[] = { 0x0ff };	/* all planes/maps updated */
/* cursor control, DS 18.1: off, or 32x32 2-bit pattern (b10:9 = 00),
 * two-colour with inversion (b7:5 = 001), no plane protection (b8) */
static const u16 cc_set[] = { 0x000, WD24X_CC_2COL_INV,
			      WD24X_CC_ENABLE | WD24X_CC_2COL_INV };
/* pattern address = the design's cursor slot 4D000h in doubleword units
 * (DS 18.2 + Table 18-3 packed mode) = 13400h */
static const u16 cpl_set[] = { (WD24X_CURSOR >> 2) & 0xfff };
static const u16 cph_set[] = { (WD24X_CURSOR >> 2) >> 12 };

#define SET(a)	(a), (u8)(sizeof(a) / sizeof((a)[0]))
#define P	WD24_PROVEN
#define U	WD24_UNPROVEN

const struct wd24x_rule wd24x_rules[] = {
	{ 1, WD24X_B_CNTRL1, 0x0fff, 0, 0x0fff, SET(c1_set), 0x0010, P, "CNTRL1",
	  "DS 19.1.1; COPY fwd/back + FILL: OBS-FB (self-test, X); MONO: OBS-FB "
	  "(cexp var 0) + OBS-W; HOSTCOL (set[4]): UNPROVEN" },
	{ 1, WD24X_B_CNTRL2, 0x0fff, 0, 0x0fff, SET(c2_set), 0, P, "CNTRL2",
	  "DS 19.1.2; 000 and 008 (mono transparency): OBS-FB" },
	{ 1, WD24X_B_SRC_LO, 0x0fff, 0, 0x0fff, NULL, 0, 0, P, "SRC_LO",
	  "DS 19.2.1 bits 11:0 of a byte address (packed, DS 19.3.2): OBS-FB" },
	{ 1, WD24X_B_SRC_HI, 0x00ff, 0, 0x00ff, NULL, 0, 0, P, "SRC_HI",
	  "DS 19.2.2 bits 20:12, 11:9 reserved; b8 (>= 1 MB) refused: OBS-FB" },
	{ 1, WD24X_B_DST_LO, 0x0fff, 0, 0x0fff, NULL, 0, 0, P, "DST_LO",
	  "DS 19.2.3: OBS-FB" },
	{ 1, WD24X_B_DST_HI, 0x00ff, 0, 0x00ff, NULL, 0, 0, P, "DST_HI",
	  "DS 19.2.4 (as SRC_HI): OBS-FB" },
	{ 1, WD24X_B_DIM_X, 0x0fff, 1, WD24X_DIM_MAX, NULL, 0, 0, P, "DIM_X",
	  "DS 19.4.1 1..2K pixels, value = width: OBS-FB (w=1 cexp case)" },
	{ 1, WD24X_B_DIM_Y, 0x0fff, 1, WD24X_DIM_MAX, NULL, 0, 0, P, "DIM_Y",
	  "DS 19.4.2 1..2K rows: OBS-FB" },
	{ 1, WD24X_B_PITCH, 0x0fff, 0, 0x0fff, SET(pitch_set), 0, P, "PITCH",
	  "DS 19.4.3 bytes, packed: low 2 bits 0, shared by src/dst: OBS-FB" },
	{ 1, WD24X_B_ROP, 0x0f00, 0, 0x0f00, SET(rop_set), 0x007c, P, "ROP",
	  "DS 19.7 Table 19-3, 7:0 reserved; SRC, XOR: OBS-FB; CLEAR, AND, OR, "
	  "INVDST, SET: UNPROVEN" },
	{ 1, WD24X_B_FG, 0x00ff, 0, 0x00ff, NULL, 0, 0, P, "FG",
	  "DS 19.5.1, 11:8 reserved: OBS-FB" },
	{ 1, WD24X_B_BG, 0x00ff, 0, 0x00ff, NULL, 0, 0, P, "BG",
	  "DS 19.5.2, 11:8 reserved: OBS-FB" },
	{ 1, WD24X_B_MASK, 0x00ff, 0, 0x00ff, SET(mask_set), 0, P, "MASK",
	  "DS 19.6.3; FFh updates all (OBS-FB), 0 prevents update (DS 21.7)" },
	/* block 1 C (transparency colour), D (transparency mask), F: refused */
	{ 2, WD24X_C_CTRL, 0x0fff, 0, 0x0fff, SET(cc_set), 0x0007, U, "CUR_CTRL",
	  "DS 18.1; never written by wd24adv/wd90c24fb on the 755C: UNPROVEN" },
	{ 2, WD24X_C_PAT_LO, 0x0fff, 0, 0x0fff, SET(cpl_set), 0x0001, U, "CUR_PAT_LO",
	  "DS 18.2.1; doubleword units in packed mode (Table 18-3): UNPROVEN" },
	{ 2, WD24X_C_PAT_HI, 0x00ff, 0, 0x00ff, SET(cph_set), 0x0001, U, "CUR_PAT_HI",
	  "DS 18.2.2 bits 19:12, 11:8 reserved: UNPROVEN" },
	{ 2, WD24X_C_PRIMARY, 0x00ff, 0, 0x00ff, NULL, 0, 0, U, "CUR_PRIMARY",
	  "DS 18.5.1: UNPROVEN" },
	{ 2, WD24X_C_SECONDARY, 0x00ff, 0, 0x00ff, NULL, 0, 0, U, "CUR_SECONDARY",
	  "DS 18.5.2: UNPROVEN" },
	{ 2, WD24X_C_ORIGIN, 0x07df, 0, 0x07df, NULL, 0, 0, U, "CUR_ORIGIN",
	  "DS 18.3 Y 11:6, X 5:0, 0..31 for 32x32: UNPROVEN" },
	{ 2, WD24X_C_POS_X, 0x07ff, 0, 639, NULL, 0, 0, U, "CUR_POS_X",
	  "DS 18.4.1 10:0 pixels; panel centring offset: UNPROVEN" },
	{ 2, WD24X_C_POS_Y, 0x07ff, 0, 479, NULL, 0, 0, U, "CUR_POS_Y",
	  "DS 18.4.2: UNPROVEN" },
	/* block 2 index 8 (auxiliary colour) and 9-F: refused */
};
const unsigned int wd24x_nrules = sizeof(wd24x_rules) / sizeof(wd24x_rules[0]);
#undef P
#undef U

const struct wd24x_rule *wd24x_rule_find(u8 blk, u8 idx)
{
	unsigned int i;

	for (i = 0; i < wd24x_nrules; i++)
		if (wd24x_rules[i].blk == blk && wd24x_rules[i].idx == idx)
			return &wd24x_rules[i];
	return NULL;
}

/* the state-free part: is data12 a listed value of this register */
static int rule_ok(const struct wd24x_rule *r, u16 data)
{
	unsigned int i;

	if (!r)
		return -EPERM;
	if (data & ~r->mask)
		return -EINVAL;
	if (r->set) {
		for (i = 0; i < r->nset; i++)
			if (r->set[i] == data)
				return 0;
		return -EINVAL;
	}
	return (data >= r->lo && data <= r->hi) ? 0 : -EINVAL;
}

static bool live_refused(const struct wd24x *x)
{
	return !x->io || (x->io->live && !WD24X_LIVE_ALLOWED);
}

/* ---- guard -------------------------------------------------------------- */

int wd24x_guard(const struct wd24x *x, u16 port, u16 val)
{
	u8 blk, idx;
	u16 data;
	int err;

	if (live_refused(x))
		return -EPERM;
	switch (port) {
	case WD24X_P_INDEX:
		/* DS 17.2: 15:14 reserved, 13 read-only */
		if (val & ~WD24X_IX_WRITABLE)
			return -EINVAL;
		/* putting back the value this op saved (OBS-W keeps 23C0h) */
		if (x->have_saved && val == (x->saved_ix & WD24X_IX_WRITABLE))
			return 0;
		blk = val & WD24X_IX_BLK_MASK;
		idx = (val >> WD24X_IX_RIDX_SHIFT) & 0xf;
		/* our selects: block 1 or 2, a listed index, always NOINC so a
		 * stray read cannot move the index (DS 17.2 bit 12) */
		if (!(val & WD24X_IX_NOINC))
			return -EINVAL;
		if (blk != WD24X_BLK_BLT && blk != WD24X_BLK_CUR)
			return -EPERM;
		return wd24x_rule_find(blk, idx) ? 0 : -EPERM;
	case WD24X_P_DATA:
		if (x->dead)
			return -EBUSY;
		blk = x->cur_blk;
		idx = val >> 12;
		data = val & 0x0fff;
		err = rule_ok(wd24x_rule_find(blk, idx), data);
		if (err)
			return err;
		if (!x->armed)
			return -EBUSY;	/* only inside a validated plan */
		if (blk == WD24X_BLK_BLT) {
			/* never reprogram a running engine (OBS-W waits idle;
			 * DS 19.2 "should not read ... while in progress") */
			if (x->busy || !x->idle_seen)
				return -EBUSY;
			if (idx == WD24X_B_CNTRL1 && data != x->plan_cntrl1)
				return -EINVAL;
		}
		return 0;
	case WD24X_P_HOST:
		if (x->dead)
			return -EBUSY;
		/* DS 21.12: with no op waiting the chip holds the host off
		 * (UNPROVEN what follows): only words the op is owed */
		return (x->busy && x->host_op && x->host_left) ? 0 : -EBUSY;
	default:
		return -EPERM;	/* VGA, PM 24h-27h, 23C6h... not representable */
	}
}

int wd24x_guard_in(const struct wd24x *x, u16 port)
{
	if (live_refused(x))
		return -EPERM;
	/* 23C4h reads are display-to-system data (DS 21.13): never */
	return (port == WD24X_P_INDEX || port == WD24X_P_DATA) ? 0 : -EPERM;
}

/* ---- log ---------------------------------------------------------------- */

static void ev(struct wd24x *x, u8 type, u16 port, u16 val, int err,
	       const char *why)
{
	struct wd24x_event *e;

	if (x->nev >= WD24X_MAXEV) {
		x->dropped++;
		return;
	}
	e = &x->ev[x->nev++];
	e->type = type;
	e->port = port;
	e->val = val;
	e->err = err;
	e->why = why;
}

void wd24x_kill(struct wd24x *x, const char *why)
{
	if (!x->dead) {
		x->dead = true;
		x->dead_why = why;
		ev(x, WD24X_EV_DEAD, 0, 0, -EIO, why);
	}
	x->armed = false;
	x->host_op = false;
	x->host_left = 0;
}

void wd24x_init(struct wd24x *x, const struct wd24x_io *io)
{
	memset(x, 0, sizeof(*x));
	x->io = io;
	x->readback = true;
	x->spins = WD24X_SPIN_MAX;
}

static int xout(struct wd24x *x, u16 port, u16 val, const char *why)
{
	int err = wd24x_guard(x, port, val);

	if (err) {
		x->nrefused++;
		ev(x, WD24X_EV_REFUSED, port, val, err, why);
		return err;
	}
	x->io->outw(x->io->priv, val, port);
	x->nout++;
	if (port == WD24X_P_INDEX) {
		x->cur_blk = val & WD24X_IX_BLK_MASK;
	} else if (port == WD24X_P_DATA) {
		if (x->cur_blk <= WD24X_BLK_CUR)
			x->shadow[x->cur_blk][val >> 12] = val & 0x0fff;
	} else if (port == WD24X_P_HOST) {
		x->nhost++;
		x->host_left--;
	}
	ev(x, WD24X_EV_OUT, port, val, 0, why);
	return 0;
}

static int xin(struct wd24x *x, u16 port, u16 *v)
{
	int err = wd24x_guard_in(x, port);

	if (err) {
		x->nrefused++;
		ev(x, WD24X_EV_REFUSED, port, 0, err, "read");
		return err;
	}
	*v = x->io->inw(x->io->priv, port);
	x->nin++;
	return 0;
}

/* save 23C0h for this op (OBS-W keeps the index around its accesses) */
static int begin(struct wd24x *x)
{
	u16 v;
	int err = xin(x, WD24X_P_INDEX, &v);

	if (err)
		return err;
	x->saved_ix = v;
	x->have_saved = true;
	return 0;
}

/* put 23C0h back and read it: UNPROVEN that it reads back (S1) */
static int end(struct wd24x *x)
{
	u16 want, got = 0;
	int err;

	if (!x->have_saved)
		return 0;
	want = x->saved_ix & WD24X_IX_WRITABLE;
	err = xout(x, WD24X_P_INDEX, want, "restore 23C0h");
	if (!err)
		err = xin(x, WD24X_P_INDEX, &got);
	x->have_saved = false;
	if (err)
		return err;
	if ((got & WD24X_IX_WRITABLE) != want) {
		x->nmismatch++;
		wd24x_kill(x, "23C0h did not restore");
		return -EIO;
	}
	x->cur_blk = want & WD24X_IX_BLK_MASK;
	return 0;
}

static int sel(struct wd24x *x, u8 blk, u8 idx)
{
	return xout(x, WD24X_P_INDEX,
		    WD24X_IX_NOINC | blk | (u16)(idx << WD24X_IX_RIDX_SHIFT),
		    "select");
}

/* one CNTRL1 status read; the selected index must echo (DS 17.1) */
static int status(struct wd24x *x, bool *busy)
{
	u16 v;
	int err = xin(x, WD24X_P_DATA, &v);

	if (err)
		return err;
	if (v >> 12) {
		x->nmismatch++;
		wd24x_kill(x, "status read: index nibble not 0");
		return -EIO;
	}
	*busy = v & WD24X_C1_ACT;
	return 0;
}

/* bounded wait for CNTRL1 b11 = 0 (DS 19.1.1: resets automatically).
 * On timeout: latch dead.  Never writes CNTRL1 (no abort: DS 19.1.1
 * CAUTION; DS 21.18 says an abort ends "within a few memory clocks" but
 * the op cannot be resumed; which applies on the 755C is UNPROVEN). */
static int wait_idle(struct wd24x *x, u32 spins)
{
	u32 n;
	bool busy;
	int err = sel(x, WD24X_BLK_BLT, WD24X_B_CNTRL1);

	if (err)
		return err;
	for (n = 0; n < spins; n++) {
		err = status(x, &busy);
		if (err)
			return err;
		if (!busy) {
			x->busy = false;
			x->idle_seen = true;
			return 0;
		}
	}
	x->ntimeout++;
	wd24x_kill(x, "BITBLT busy past its bound");
	return -ETIMEDOUT;
}

/* write one register and (by default) read it back via a NOINC select */
static int wreg(struct wd24x *x, u8 blk, u8 idx, u16 data)
{
	u16 v, want = (u16)(idx << 12) | data;
	int err = xout(x, WD24X_P_DATA, want, "reg");

	if (err || !x->readback)
		return err;
	err = sel(x, blk, idx);
	if (!err)
		err = xin(x, WD24X_P_DATA, &v);
	if (err)
		return err;
	/* UNPROVEN that every listed register reads back exactly (S1/S2) */
	if (v != want) {
		x->nmismatch++;
		wd24x_kill(x, "register readback mismatch");
		return -EIO;
	}
	return 0;
}

/* ---- validator ---------------------------------------------------------- */

static const struct { u32 base, end; } surfaces[] = {
	{ WD24X_FRAME, WD24X_FRAME_END },
	{ WD24X_SCRATCH, WD24X_SCRATCH_END },
	{ WD24X_POOL, WD24X_POOL_END },
};

/* rectangle at pitch 640 wholly inside one surface, rows not wrapping */
bool wd24x_surface(u32 off, u32 w, u32 h, u32 *base)
{
	unsigned int i;

	if (!w || !h || w > WD24X_PITCH)
		return false;
	for (i = 0; i < sizeof(surfaces) / sizeof(surfaces[0]); i++) {
		u64 last = (u64)off + (u64)(h - 1) * WD24X_PITCH + w;

		if (off < surfaces[i].base || last > surfaces[i].end)
			continue;
		if ((off - surfaces[i].base) % WD24X_PITCH + w > WD24X_PITCH)
			return false;
		if (base)
			*base = surfaces[i].base;
		return true;
	}
	return false;
}

/* DS 21.14 (mono, packed: 4 px from the low 4 bits of each 16-bit write,
 * no word straddles rows; msb first, no padding: OBS-FB var 0) and DS
 * 21.12 (colour: 32-bit groups as two 16-bit writes, low word first; no
 * doubleword straddles rows: UNPROVEN on the 755C).  Source alignment
 * (SRC_LO low 2 bits) is always 0. */
u32 wd24x_host_words(const struct wd24x_cmd *cmd)
{
	u32 per4 = (cmd->w + 3) / 4;

	switch (cmd->op) {
	case WD24X_MONO:
		return cmd->h * per4;
	case WD24X_HOSTCOL:
		return cmd->h * per4 * 2;
	default:
		return 0;
	}
}

/* same pitch: a destination above the source in memory is copied from
 * the end (bottom-right first, DS 19.1.1 b10 and 19.3 starting corner) */
bool wd24x_backward(const struct wd24x_cmd *cmd)
{
	return cmd->op == WD24X_COPY && cmd->dst > cmd->src;
}

int wd24x_check(const struct wd24x_cmd *cmd, const char **why)
{
	const char *w = NULL;
	unsigned int i;
	bool rop = false;

	for (i = 0; i < sizeof(rop_set) / sizeof(rop_set[0]); i++)
		rop |= cmd->rop == rop_set[i];
	if (cmd->op < WD24X_FILL || cmd->op > WD24X_HOSTCOL)
		w = "unknown op";
	else if (!cmd->w || !cmd->h || cmd->w > WD24X_DIM_MAX || cmd->h > WD24X_DIM_MAX)
		w = "size outside 1..2048 (DS 19.4)";
	else if (cmd->w > WD24X_PITCH)
		w = "wider than the pitch";
	else if (!rop)
		w = "raster op not in the list";
	else if (cmd->transparent && cmd->op != WD24X_MONO)
		w = "transparency is MONO only";
	else if (!wd24x_surface(cmd->dst, cmd->w, cmd->h, NULL))
		w = "destination not inside frame/scratch/pool";
	else if (cmd->op == WD24X_COPY && !wd24x_surface(cmd->src, cmd->w, cmd->h, NULL))
		w = "source not inside frame/scratch/pool";
	else if (wd24x_host_words(cmd) > WD24X_HOST_MAX_WORDS)
		w = "too much host data for one op";
	if (why)
		*why = w;
	return w ? -EINVAL : 0;
}

static void put(struct wd24x_plan *p, u8 idx, u16 val)
{
	p->idx[p->n] = idx;
	p->val[p->n] = val & 0x0fff;
	p->n++;
}

int wd24x_plan(const struct wd24x_cmd *cmd, struct wd24x_plan *p, const char **why)
{
	u32 src = cmd->src, dst = cmd->dst, back = 0;
	u16 c1 = WD24X_C1_ACT | WD24X_C1_PACKED;
	int err = wd24x_check(cmd, why);

	memset(p, 0, sizeof(*p));
	if (err)
		return err;
	switch (cmd->op) {
	case WD24X_FILL:
		c1 |= WD24X_C1_SRCF_FIXED;
		src = dst;	/* unused (DS 19.3/21.11); OBS-FB uses dst */
		break;
	case WD24X_COPY:
		if (wd24x_backward(cmd)) {
			back = (cmd->h - 1) * WD24X_PITCH + cmd->w - 1;
			c1 |= WD24X_C1_BACK;
		}
		break;
	case WD24X_MONO:
		c1 |= WD24X_C1_SRCF_MONO_HOST | WD24X_C1_SRC_IO;
		src = 0;	/* alignment 0 (DS 21.12/21.14) */
		break;
	case WD24X_HOSTCOL:
		c1 |= WD24X_C1_SRC_IO;
		src = 0;
		break;
	}
	src += back;
	dst += back;
	put(p, WD24X_B_SRC_LO, src & 0xfff);
	put(p, WD24X_B_SRC_HI, src >> 12);
	put(p, WD24X_B_DST_LO, dst & 0xfff);
	put(p, WD24X_B_DST_HI, dst >> 12);
	put(p, WD24X_B_PITCH, WD24X_PITCH);
	put(p, WD24X_B_DIM_X, cmd->w);
	put(p, WD24X_B_DIM_Y, cmd->h);
	put(p, WD24X_B_MASK, 0x0ff);
	put(p, WD24X_B_ROP, cmd->rop);
	put(p, WD24X_B_CNTRL2, cmd->transparent ? WD24X_C2_MTRANS : 0);
	put(p, WD24X_B_FG, cmd->fg);
	if (cmd->op == WD24X_MONO)
		put(p, WD24X_B_BG, cmd->bg);
	p->cntrl1 = c1;
	p->host_words = wd24x_host_words(cmd);
	p->words_per_row = cmd->h ? p->host_words / cmd->h : 0;
	/* bound: base + one poll per 8 pixels, capped (design 1.4: < 0.2 s) */
	p->spins = WD24X_SPIN_BASE + (cmd->w * cmd->h) / 8;
	if (p->spins > WD24X_SPIN_MAX)
		p->spins = WD24X_SPIN_MAX;
	return 0;
}

/* ---- sequencer ---------------------------------------------------------- */

static int finish(struct wd24x *x, int err)
{
	int e2;

	x->armed = false;
	x->host_op = false;
	e2 = end(x);
	return err ? err : e2;
}

int wd24x_run(struct wd24x *x, const struct wd24x_cmd *cmd, const u16 *host,
	      u32 nhost)
{
	struct wd24x_plan p;
	const char *why;
	unsigned int i;
	u32 r, k, word = 0;
	bool busy;
	int err;

	if (live_refused(x)) {
		x->nrefused++;
		ev(x, WD24X_EV_REFUSED, 0, 0, -EPERM, "live io: S0 has no live path");
		return -EPERM;
	}
	if (!x->enabled || x->dead) {
		x->nfallback++;
		return -ENODEV;		/* CPU path; no port touched */
	}
	if (wd24x_plan(cmd, &p, &why))
		return -EINVAL;
	if (p.host_words != nhost || (nhost && !host))
		return -EINVAL;		/* exact word count or nothing */
	for (i = 0; i < p.n; i++)	/* whole plan listed before any access */
		if (rule_ok(wd24x_rule_find(WD24X_BLK_BLT, p.idx[i]), p.val[i]))
			return -EPERM;
	if (rule_ok(wd24x_rule_find(WD24X_BLK_BLT, WD24X_B_CNTRL1), p.cntrl1))
		return -EPERM;

	err = begin(x);
	if (err)
		return err;
	/* wait before programming (OBS-W); bound of the previous op */
	err = wait_idle(x, x->spins);
	if (err)
		return finish(x, err);
	x->armed = true;
	x->plan_cntrl1 = p.cntrl1;
	for (i = 0; i < p.n && !err; i++)
		err = wreg(x, WD24X_BLK_BLT, p.idx[i], p.val[i]);
	if (err)
		return finish(x, err);
	if (x->readback) {	/* back to block 1 for the data port */
		err = sel(x, WD24X_BLK_BLT, WD24X_B_CNTRL1);
		if (err)
			return finish(x, err);
	}
	err = xout(x, WD24X_P_DATA, p.cntrl1, "start");
	x->armed = false;
	if (err)
		return finish(x, err);
	x->busy = true;
	x->idle_seen = false;
	x->spins = p.spins;
	x->nops++;

	if (p.host_words) {
		x->host_op = true;
		x->host_left = p.host_words;
		/* 23C0h stays on CNTRL1: reads give the busy bit (OBS-FB) */
		err = sel(x, WD24X_BLK_BLT, WD24X_B_CNTRL1);
		for (r = 0; r < cmd->h && !err; r++)
			for (k = 0; k < p.words_per_row && !err; k++) {
				/* never a host word to an idle engine */
				err = status(x, &busy);
				if (!err && !busy) {
					x->nmismatch++;
					wd24x_kill(x, "engine idle before all host words");
					err = -EIO;
				}
				if (!err)
					err = xout(x, WD24X_P_HOST, host[word++], "host");
			}
		x->host_op = false;
		if (!err && x->host_left) {
			wd24x_kill(x, "host word count");
			err = -EIO;
		}
		if (err)
			return finish(x, err);
	}
	if (p.host_words || !x->async)
		err = wait_idle(x, p.spins);
	return finish(x, err);
}

int wd24x_sync(struct wd24x *x)
{
	int err;

	if (!x->busy)
		return 0;
	err = begin(x);
	if (err)
		return err;
	return finish(x, wait_idle(x, x->spins));
}

/* design Q5: CPU access during a blit is UNPROVEN, so never while busy */
int wd24x_cpu_begin(struct wd24x *x)
{
	if (!x->busy)
		return 0;
	return wd24x_sync(x) ? -EBUSY : 0;
}

int wd24x_cursor(struct wd24x *x, bool on, u16 px, u16 py, u8 hotx, u8 hoty,
		 u8 c0, u8 c1)
{
	static const u8 order[] = {
		WD24X_C_PAT_LO, WD24X_C_PAT_HI, WD24X_C_PRIMARY,
		WD24X_C_SECONDARY, WD24X_C_ORIGIN, WD24X_C_POS_X,
		WD24X_C_POS_Y, WD24X_C_CTRL,
	};
	u16 val[WD24X_C_NREG];
	unsigned int i;
	int err;

	if (live_refused(x)) {
		x->nrefused++;
		return -EPERM;
	}
	if (!x->enabled || x->dead)
		return -ENODEV;
	val[WD24X_C_PAT_LO] = cpl_set[0];
	val[WD24X_C_PAT_HI] = cph_set[0];
	val[WD24X_C_PRIMARY] = c0;
	val[WD24X_C_SECONDARY] = c1;
	val[WD24X_C_ORIGIN] = (u16)((hoty & 0x3f) << 6) | (hotx & 0x3f);
	val[WD24X_C_POS_X] = px;
	val[WD24X_C_POS_Y] = py;
	/* control last: address/origin take effect after it (DS 18.2 note) */
	val[WD24X_C_CTRL] = on ? (WD24X_CC_ENABLE | WD24X_CC_2COL_INV) : 0;
	for (i = 0; i < sizeof(order); i++)
		if (rule_ok(wd24x_rule_find(WD24X_BLK_CUR, order[i]), val[order[i]]))
			return -EINVAL;
	err = begin(x);
	if (err)
		return err;
	err = sel(x, WD24X_BLK_CUR, WD24X_C_CTRL);
	x->armed = true;
	/* cursor reads return the last data written (DS 18.4/18.6) */
	for (i = 0; i < sizeof(order) && !err; i++)
		err = wreg(x, WD24X_BLK_CUR, order[i], val[order[i]]);
	return finish(x, err);
}
