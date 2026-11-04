// SPDX-License-Identifier: GPL-2.0
/*
 * wd24adv S2: write + readback of block 1/2 registers, start bit never
 * set, cursor never enabled.  See wd24adv_s2.h for the sequence, the
 * stop rules and the sources.  Shared by the kernel module (s2=1
 * dryrun=0) and tests/test_s2.c (port model).
 */
#include "wd24adv_s2.h"
#ifdef __KERNEL__
#include <linux/io.h>
#else
#include <string.h>
#endif

#define P_AR	0x3c0
#define P_ARR	0x3c1
#define IDX_WR	0x1fff
#define SEL_CNTRL1	0x1001		/* block 1 idx 0 */
#define SEL_CURCTL	0x1002		/* block 2 idx 0 */

/* S1 live readings, both runs, both passes (commit 3ad7142) */
const u16 wd24s2_ref[WD24S1_NSEL] = {
	0x0108, 0x1000, 0x210b, 0x3004, 0x410b, 0x5004, 0x61e4, 0x713c,
	0x8280, 0x9300, 0xa000, 0xb0a5, 0xc000, 0xd000, 0xe0ff, 0xf0ff,
	0x0000, 0x1000, 0x2014, 0x3001, 0x4000, 0x5041, 0x617a, 0x7047,
	0x8000,
};

/* the closed write list, in order; CNTRL2 last in block 1 */
const struct wd24s2_w wd24s2_list[WD24S2_NW] = {
	{ 1, 0x2, 0x000, "SRC_LO" },
	{ 1, 0x3, 0x04c, "SRC_HI" },
	{ 1, 0x4, 0x000, "DST_LO" },
	{ 1, 0x5, 0x04c, "DST_HI" },
	{ 1, 0x6, 0x008, "DIM_X" },
	{ 1, 0x7, 0x008, "DIM_Y" },
	{ 1, 0x8, 0x280, "PITCH" },
	{ 1, 0x9, 0x600, "ROP" },
	{ 1, 0xa, 0x05a, "FG" },
	{ 1, 0xb, 0x05a, "BG" },
	{ 1, 0xe, 0x0ff, "MASK" },
	{ 1, 0x1, 0x008, "CNTRL2" },
	{ 2, 0x3, 0x05a, "CUR_PRIMARY" },
	{ 2, 0x4, 0x0a5, "CUR_SECONDARY" },
	{ 2, 0x5, 0x000, "CUR_ORIGIN" },
	{ 2, 0x6, 0x010, "CUR_POS_X" },
	{ 2, 0x7, 0x010, "CUR_POS_Y" },
};

int wd24s2_k(u8 blk, u8 idx)
{
	if (blk == 1 && idx < WD24S1_N_BLT)
		return idx;
	if (blk == 2 && idx < WD24S1_N_CUR)
		return WD24S1_N_BLT + idx;
	return -1;
}

int wd24s2_find(u8 blk, u8 idx)
{
	int j;

	for (j = 0; j < WD24S2_NW; j++)
		if (wd24s2_list[j].blk == blk && wd24s2_list[j].idx == idx)
			return j;
	return -1;
}

static u16 ref_of(const struct wd24s2_w *w)
{
	return wd24s2_ref[(w->blk == 1 ? 0 : WD24S1_N_BLT) + (w->idx & 0xf)];
}

static u16 sel_of(u8 blk, u8 idx)
{
	return WD24S1_NOINC | (u16)(idx << 8) | blk;
}

static bool is_sel(u16 v)
{
	unsigned int k;

	for (k = 0; k < WD24S1_NSEL; k++)
		if (wd24s1_sel(k) == v)
			return true;
	return false;
}

int wd24s2_guard_out(const struct wd24s2 *s, u16 port, u16 val)
{
	u8 blk, idx;
	int j;

	if (!s->have_saved || s->restored)
		return -EPERM;
	if (port == WD24S1_P_INDEX) {
		if (s->wix >= WD24S2_NIX)
			return -EPERM;
		if (is_sel(val) || (val == (s->saved & IDX_WR) && s->saved == WD24S2_S))
			return 0;
		return -EPERM;
	}
	if (port != WD24S1_P_DATA || !s->armed || s->wdata >= WD24S2_NDATA)
		return -EPERM;		/* 23C4h and everything else: never */
	if ((s->cur & 0xf0ff) != (WD24S1_NOINC | 1) && (s->cur & 0xf0ff) != (WD24S1_NOINC | 2))
		return -EPERM;
	blk = s->cur & 0xff;
	idx = (s->cur >> 8) & 0xf;
	j = wd24s2_find(blk, idx);
	if (j < 0 || (val >> 12) != idx)
		return -EPERM;		/* CNTRL1, cursor control, C/D/F, PAT, 8 */
	if ((val & 0xfff) == wd24s2_list[j].test || val == ref_of(&wd24s2_list[j]))
		return 0;
	return -EPERM;
}

void wd24s2_init(struct wd24s2 *s, const struct wd24s1_io *io)
{
	memset(s, 0, sizeof(*s));
	s->io = io;
	s->stop_j = -1;
}

const char *wd24s2_res_name(enum wd24s2_res r)
{
	switch (r) {
	case WD24S2_OK: return "OK";
	case WD24S2_REFUSED: return "REFUSED (no 23C0h write)";
	case WD24S2_NOT_REF: return "NOT REFERENCE (selects only, no 23C2h write)";
	case WD24S2_MISMATCH: return "MISMATCH (stopped, restored)";
	case WD24S2_RESTORE_BAD: return "RESTORE MISMATCH";
	default: return "not run";
	}
}

static void logacc(struct wd24s2 *s, bool w, int width, u16 port, u16 val)
{
	if (s->nlog < WD24S2_NLOG)
		s->log[s->nlog++] = (struct wd24s1_acc){ w, (u8)width, port, val };
}

static u16 in16(struct wd24s2 *s, u16 port)
{
	u16 v;

	if (wd24s1_guard_in(port, 16)) {
		s->refused++;
		return 0xffff;
	}
	v = s->io->rd16(s->io->priv, port);
	s->reads++;
	logacc(s, false, 16, port, v);
	return v;
}

static u8 in8(struct wd24s2 *s, u16 port)
{
	u8 v;

	if (wd24s1_guard_in(port, 8)) {
		s->refused++;
		return 0xff;
	}
	v = s->io->rd8(s->io->priv, port);
	s->reads++;
	logacc(s, false, 8, port, v);
	return v;
}

static int out16(struct wd24s2 *s, u16 port, u16 val)
{
	if (wd24s2_guard_out(s, port, val)) {
		s->refused++;
		return -EPERM;
	}
	s->io->wr16(s->io->priv, val, port);
	if (port == WD24S1_P_INDEX) {
		s->wix++;
		s->cur = val;
	} else {
		s->wdata++;
	}
	logacc(s, true, 16, port, val);
	return 0;
}

/* W 23C0h sel, R 23C0h == sel */
static int sel_rb(struct wd24s2 *s, u16 sel)
{
	if (out16(s, WD24S1_P_INDEX, sel))
		return -EPERM;
	return in16(s, WD24S1_P_INDEX) == sel ? 0 : -EIO;
}

static int refuse(struct wd24s2 *s, const char *why)
{
	s->res = WD24S2_REFUSED;
	s->why = why;
	return -ENODEV;
}

/* all 25 selects: read and compare with the reference */
static int verify_all(struct wd24s2 *s, u16 *out, unsigned int *n)
{
	unsigned int k;

	for (k = 0; k < WD24S1_NSEL; k++) {
		if (sel_rb(s, wd24s1_sel(k)))
			return -EIO;
		out[k] = in16(s, WD24S1_P_DATA);
		*n = k + 1;
		if (out[k] != wd24s2_ref[k])
			return -EIO;
	}
	return 0;
}

/* one write each to every register that may hold a test value */
static void restore_dirty(struct wd24s2 *s)
{
	int j;

	for (j = 0; j < WD24S2_NW; j++) {
		const struct wd24s2_w *w = &wd24s2_list[j];
		u16 ref = ref_of(w);

		if (!s->dirty[j])
			continue;
		if (sel_rb(s, sel_of(w->blk, w->idx)) ||
		    out16(s, WD24S1_P_DATA, ref) ||
		    in16(s, WD24S1_P_DATA) != ref) {
			s->restore_fail |= 1u << j;
			continue;
		}
		s->dirty[j] = false;
	}
}

static const char *one(struct wd24s2 *s, int j)
{
	const struct wd24s2_w *w = &wd24s2_list[j];
	u16 sel = sel_of(w->blk, w->idx);
	u16 tw = (u16)(w->idx << 12) | w->test;
	u16 ref = ref_of(w);
	u16 ssel = w->blk == 1 ? SEL_CNTRL1 : SEL_CURCTL;
	u16 sref = w->blk == 1 ? wd24s2_ref[0] : wd24s2_ref[WD24S1_N_BLT];

	s->step[j] = 1;
	if (sel_rb(s, sel))
		return "23C0h did not read back the select";
	s->dirty[j] = true;
	if (out16(s, WD24S1_P_DATA, tw))
		return "guard refused the test write";
	s->step[j] = 2;
	if (in16(s, WD24S1_P_INDEX) != sel)
		return "23C0h changed after the 23C2h write";
	s->rb_test[j] = in16(s, WD24S1_P_DATA);
	if (s->rb_test[j] != tw)
		return "test value did not read back";
	if (out16(s, WD24S1_P_DATA, ref))
		return "guard refused the reference write";
	s->step[j] = 3;
	s->rb_ref[j] = in16(s, WD24S1_P_DATA);
	if (s->rb_ref[j] != ref)
		return "reference value did not read back";
	s->dirty[j] = false;
	if (sel_rb(s, ssel))
		return "23C0h did not read back the safety select";
	s->rb_safe[j] = in16(s, WD24S1_P_DATA);
	if (s->rb_safe[j] != sref)
		return w->blk == 1 ? "CNTRL1 changed (ACT / start must stay 0108h)"
				   : "cursor control changed (must stay 0000h)";
	s->step[j] = 4;
	return NULL;
}

int wd24s2_run(struct wd24s2 *s)
{
	enum wd24s2_res res = WD24S2_OK;
	const char *why = NULL;
	u16 a, b;
	int j;

	if (s->res != WD24S2_NOT_RUN)
		return -EBUSY;

	/* reads only until every precondition holds (as S1, plus S = 1002h) */
	a = in16(s, WD24S1_P_INDEX);
	b = in16(s, WD24S1_P_INDEX);
	if (a != b)
		return refuse(s, "23C0h read twice gave different values");
	if (a == 0xffff)
		return refuse(s, "23C0h reads FFFF: not decoded in this state");
	if (a & WD24S1_RESV)
		return refuse(s, "23C0h bits 15:14 (reserved) set: not the index control");
	s->alias = in8(s, P_AR) | (u16)(in8(s, P_ARR) << 8);
	if (s->alias == a)
		return refuse(s, "23C0h equals 3C0h/3C1h: possible 10-bit alias, no write");
	if (a != WD24S2_S)
		return refuse(s, "23C0h is not the S1 reference 1002h");
	s->saved = a;
	s->have_saved = true;

	/* pre-pass: selects + reads only; the chip must equal the reference */
	if (verify_all(s, s->pre, &s->npre)) {
		res = WD24S2_NOT_REF;
		why = "pre-pass: a register differs from the S1 reference";
	} else {
		s->armed = true;
		for (j = 0; j < WD24S2_NW; j++) {
			why = one(s, j);
			if (why) {
				res = WD24S2_MISMATCH;
				s->stop_j = j;
				break;
			}
		}
	}
	if (res == WD24S2_MISMATCH)
		restore_dirty(s);
	s->armed = false;		/* no 23C2h write after this point */
	if (res != WD24S2_NOT_REF && verify_all(s, s->post, &s->npost)) {
		if (res == WD24S2_OK)
			why = "post-pass: a register differs from the S1 reference";
		res = WD24S2_RESTORE_BAD;
	}
	if (s->restore_fail)
		res = WD24S2_RESTORE_BAD;

	if (out16(s, WD24S1_P_INDEX, s->saved & IDX_WR) == 0) {
		s->restored = true;
		s->restore_rb = in16(s, WD24S1_P_INDEX);
	}
	if (!s->restored || s->restore_rb != s->saved) {
		res = WD24S2_RESTORE_BAD;
		why = why ? why : "23C0h does not read back 1002h after the restore";
	}
	s->res = res;
	s->why = why;
	return res == WD24S2_OK ? 0 : -EIO;
}

#ifdef __KERNEL__
#ifdef WD24S1_QEMU_FAKE
/*
 * QEMU TEST BUILD ONLY (wd24adv-qemufake.ko, never installed): real
 * inw()/outw() for the trace; values from a register file preset to the
 * S1 reference, so the full S2 sequence runs.
 */
static u16 fake_ix = WD24S2_S;
static u16 fake_reg[WD24S1_NSEL];
static bool fake_init;

static u16 k_inw(void *priv, u16 port)
{
	int k = wd24s2_k(fake_ix & 0xff, (fake_ix >> 8) & 0xf);

	if (!fake_init) {
		memcpy(fake_reg, wd24s2_ref, sizeof(fake_reg));
		fake_init = true;
	}
	(void)inw(port);
	if (port == WD24S1_P_INDEX)
		return fake_ix;
	return k < 0 ? 0xffff : fake_reg[k];
}

static void k_outw(void *priv, u16 val, u16 port)
{
	int k = wd24s2_k(fake_ix & 0xff, (fake_ix >> 8) & 0xf);

	outw(val, port);
	if (port == WD24S1_P_INDEX)
		fake_ix = val & IDX_WR;
	else if (port == WD24S1_P_DATA && k >= 0)
		fake_reg[k] = val;
}
#else
static u16 k_inw(void *priv, u16 port)
{
	return inw(port);
}

static void k_outw(void *priv, u16 val, u16 port)
{
	outw(val, port);
}
#endif

static u8 k_inb(void *priv, u16 port)
{
	return inb(port);
}

const struct wd24s1_io wd24s2_kernel_io = { k_inw, k_outw, k_inb, NULL };
#endif
