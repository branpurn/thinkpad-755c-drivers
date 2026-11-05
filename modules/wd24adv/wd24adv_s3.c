// SPDX-License-Identifier: GPL-2.0
/*
 * wd24adv S3: first BITBLT fill/copy, off screen only.  See wd24adv_s3.h
 * for the operations, the region, the sequence, the stop rules and the
 * guard.  Shared by the kernel module (s3=1) and tests/test_s3.c.
 */
#include "wd24adv_s3.h"
#ifdef __KERNEL__
#include <linux/io.h>
#include <linux/vmalloc.h>
#else
#include <string.h>
#endif

#define P_AR	0x3c0
#define P_ARR	0x3c1
#define IDX_WR	0x1fff
#define SEL_CNTRL1	0x1001
#define CC_K	WD24S1_N_BLT		/* cursor control: select 16 */

#define LO(a)	((a) & 0xfff)
#define HI(a)	((a) >> 12)
#define PARS(s, d, w, h)	{ 2, LO(s) }, { 3, HI(s) }, { 4, LO(d) }, { 5, HI(d) }, \
	{ 8, WD24S3_PITCH }, { 6, w }, { 7, h }, { 0xe, 0xff }, { 9, 0x300 }, { 1, 0 }

const struct wd24s3_op wd24s3_ops[WD24S3_NOPS] = {
	{ "FILL 8x8 @60000h c=3Ch", true, 0x60000, 0x60000, 8, 8, 0x3c, WD24S3_FILL, 11,
	  { PARS(0x60000, 0x60000, 8, 8), { 0xa, 0x3c } }, 0x300, false },
	{ "FILL 32x32 @60200h c=C3h", true, 0x60200, 0x60200, 32, 32, 0xc3, WD24S3_FILL, 11,
	  { PARS(0x60200, 0x60200, 32, 32), { 0xa, 0xc3 } }, 0x300, false },
	{ "COPY 32x32 60200h->60100h fwd", false, 0x60200, 0x60100, 32, 32, 0, WD24S3_COPY, 10,
	  { PARS(0x60200, 0x60100, 32, 32) }, 0x300, false },
};

/*
 * S4: ON SCREEN, bank 1 (10000h-1FFFFh = frame rows 102.4-204.8; every
 * rectangle is inside rows 110-203, x 16-591).  ROP codes are the data
 * sheet's (DS 19.7 / Table 19-3, as wd24adv_engine.h WD24X_ROP_*); SRC and
 * XOR fills and both copy directions are what wd90c24fb runs under X on
 * this machine (wd_blt_go / wd_blt_copy_lin); AND, OR, INVDST, CLEAR and
 * SET are new here.  The copies overlap source and destination and the
 * direction is chosen as wd24x_backward(): dst > src -> BACK, src and dst
 * programmed at the bottom-right pixel (wd_blt_copy_lin).
 */
#define A(x, y)	((u32)(y) * WD24S3_PITCH + (x))
#define PARR(s, d, w, h, r)	{ 2, LO(s) }, { 3, HI(s) }, { 4, LO(d) }, { 5, HI(d) }, \
	{ 8, WD24S3_PITCH }, { 6, w }, { 7, h }, { 0xe, 0xff }, { 9, r }, { 1, 0 }
#define FILL4(nm, x, rop, c) { nm, true, A(x, 112), A(x, 112), 64, 16, c, WD24S3_FILL, 11, \
	{ PARR(A(x, 112), A(x, 112), 64, 16, rop), { 0xa, c } }, rop, false }
#define BR(a, w, h)	((a) + ((h) - 1) * WD24S3_PITCH + (w) - 1)
#define COPYF(nm, s, d, w, h) { nm, false, s, d, w, h, 0, WD24S3_COPY, 10, \
	{ PARR(s, d, w, h, 0x300) }, 0x300, false }
#define COPYB(nm, s, d, w, h) { nm, false, s, d, w, h, 0, WD24S3_BACKC, 10, \
	{ PARR(BR(s, w, h), BR(d, w, h), w, h, 0x300) }, 0x300, true }

const struct wd24s3_op wd24s4_ops[WD24S4_NOPS] = {
	FILL4("FILL 64x16 (16,112) SRC c=3Ch", 16, 0x300, 0x3c),
	FILL4("FILL 64x16 (96,112) XOR c=3Fh", 96, 0x600, 0x3f),
	FILL4("FILL 64x16 (176,112) AND c=30h", 176, 0x100, 0x30),
	FILL4("FILL 64x16 (256,112) OR c=38h", 256, 0x700, 0x38),
	FILL4("FILL 64x16 (336,112) INVERT", 336, 0xa00, 0x00),
	FILL4("FILL 64x16 (416,112) CLEAR", 416, 0x000, 0x00),
	FILL4("FILL 64x16 (496,112) SET", 496, 0xf00, 0x00),
	COPYF("COPY 128x24 (48,150)->(40,146) fwd", A(48, 150), A(40, 146), 128, 24),
	COPYB("COPY 128x24 (300,150)->(308,154) back", A(300, 150), A(308, 154), 128, 24),
	COPYB("COPY 120x16 (460,184)->(468,184) back", A(460, 184), A(468, 184), 120, 16),
	COPYF("COPY 120x16 (48,184)->(40,184) fwd", A(48, 184), A(40, 184), 120, 16),
};

const struct wd24s3_prog wd24s3_prog3 = {
	"S3", wd24s3_ops, WD24S3_NOPS, WD24S3_BASE, WD24S3_BANK, 400, 64 };
const struct wd24s3_prog wd24s3_prog4 = {
	"S4", wd24s4_ops, WD24S4_NOPS, 0x10000, 0x10, 800, 200 };

/* registers S3 may write (snapshot restore allowed): block 1 idx 1-A, E */
static bool s3_reg(unsigned int idx)
{
	return (idx >= 1 && idx <= 0xa) || idx == 0xe;
}

u32 wd24s3_sum(const u8 *p, u32 n, u32 h)
{
	while (n--)
		h = (h ^ *p++) * 16777619u;
	return h;
}

/* DS Table 19-3 abcd: result bit = code bit (3 - (2 * src + dst)), the
 * X11 GX numbering (clear 0, and 1, copy 3, xor 6, or 7, invert A, set F) */
u8 wd24s3_rop(u16 rop, u8 src, u8 dst)
{
	unsigned int code = (rop >> 8) & 0xf, b;
	u8 r = 0;

	for (b = 0; b < 8; b++) {
		unsigned int sb = (src >> b) & 1, db = (dst >> b) & 1;

		if (code & (1u << (3 - (2 * sb + db))))
			r |= (u8)(1u << b);
	}
	return r;
}

/* the model: what the engine must have done to the bank window */
void wd24s3_apply_at(const struct wd24s3_op *o, u8 *win, u32 base)
{
	u32 rr, ii;

	for (rr = 0; rr < o->h; rr++)
		for (ii = 0; ii < o->w; ii++) {
			u32 r = o->back ? o->h - 1 - rr : rr;
			u32 i = o->back ? o->w - 1 - ii : ii;
			u32 d = o->dst - base + r * WD24S3_PITCH + i;
			u32 sr = o->src - base + r * WD24S3_PITCH + i;

			win[d] = wd24s3_rop(o->rop, o->fill ? o->fg : win[sr], win[d]);
		}
}

void wd24s3_apply(const struct wd24s3_op *o, u8 *win)
{
	wd24s3_apply_at(o, win, WD24S3_BASE);
}

static bool is_sel(u16 v)
{
	unsigned int k;

	for (k = 0; k < WD24S1_NSEL; k++)
		if (wd24s1_sel(k) == v)
			return true;
	return false;
}

static u16 sel1(u8 idx)
{
	return WD24S1_NOINC | (u16)(idx << 8) | 1;
}

int wd24s3_guard_out(const struct wd24s3 *s, u16 port, u16 val)
{
	unsigned int idx, j;
	const struct wd24s3_op *o;

	if (!s->have_saved || s->restored || s->dead)
		return -EPERM;
	if (port == WD24S1_P_INDEX) {
		if (s->wix >= s->prog->max_ix)
			return -EPERM;
		if (is_sel(val) || (val == (s->saved & IDX_WR) && s->saved == WD24S2_S))
			return 0;
		return -EPERM;
	}
	if (port != WD24S1_P_DATA || !s->armed || s->wdata >= s->prog->max_data)
		return -EPERM;		/* 23C4h and everything else: never */
	if ((s->cur & 0xf0ff) != (WD24S1_NOINC | 1))
		return -EPERM;		/* block 1 only: cursor block never */
	idx = (s->cur >> 8) & 0xf;
	if ((val >> 12) != idx)
		return -EPERM;
	if (idx == 0) {			/* CNTRL1: the start */
		if (!s->go || s->op < 0 || s->op >= (int)s->prog->nops)
			return -EPERM;
		return val == s->prog->ops[s->op].cntrl1 ? 0 : -EPERM;
	}
	if (!s3_reg(idx))
		return -EPERM;		/* B (BG), C, D, F */
	if (s->snap_ok && val == s->snap[idx])
		return 0;		/* this load's snapshot word */
	if (s->op < 0 || s->op >= (int)s->prog->nops)
		return -EPERM;
	o = &s->prog->ops[s->op];
	for (j = 0; j < o->npar; j++)
		if (o->par[j].idx == idx && o->par[j].val == (val & 0xfff))
			return 0;
	return -EPERM;
}

void wd24s3_init(struct wd24s3 *s, const struct wd24s1_io *io,
		 const struct wd24s3_mem *mem, u8 *pre, u8 *model, u8 *buf)
{
	memset(s, 0, sizeof(*s));
	s->io = io;
	s->mem = mem;
	s->pre = pre;
	s->model = model;
	s->buf = buf;
	s->op = -1;
	s->prog = &wd24s3_prog3;
}

void wd24s3_set_prog(struct wd24s3 *s, const struct wd24s3_prog *p)
{
	if (s->res == WD24S3_NOT_RUN && !s->have_saved && p && p->nops &&
	    p->nops <= WD24S3_MAXOPS)
		s->prog = p;
}

const char *wd24s3_res_name(enum wd24s3_res r)
{
	switch (r) {
	case WD24S3_OK: return "OK";
	case WD24S3_REFUSED: return "REFUSED (reads only)";
	case WD24S3_SNAP_BAD: return "SNAPSHOT REFUSED (selects only, no 23C2h write)";
	case WD24S3_PRE_BAD: return "VRAM PRE-CHECK REFUSED (no 23C2h write)";
	case WD24S3_MISMATCH: return "REGISTER MISMATCH (stopped, restored)";
	case WD24S3_VRAM_BAD: return "VRAM MISMATCH (stopped, restored)";
	case WD24S3_TIMEOUT: return "TIMEOUT (engine busy: stopped, nothing more written)";
	case WD24S3_RESTORE_BAD: return "RESTORE MISMATCH";
	default: return "not run";
	}
}

static void logacc(struct wd24s3 *s, bool w, int width, u16 port, u16 val)
{
	if (s->nlog < WD24S3_NLOG)
		s->log[s->nlog++] = (struct wd24s1_acc){ w, (u8)width, port, val };
}

static u16 in16(struct wd24s3 *s, u16 port)
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

static u8 in8(struct wd24s3 *s, u16 port)
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

static int out16(struct wd24s3 *s, u16 port, u16 val)
{
	if (wd24s3_guard_out(s, port, val)) {
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

static int sel_rb(struct wd24s3 *s, u16 sel)
{
	if (out16(s, WD24S1_P_INDEX, sel))
		return -EPERM;
	return in16(s, WD24S1_P_INDEX) == sel ? 0 : -EIO;
}

static int bank(struct wd24s3 *s, u8 v)
{
	s->wbank++;
	logacc(s, true, 8, 0x3cf, (u16)(0x0900 | v));	/* GR09 = PR0A */
	return s->mem->bank(s->mem->priv, v);
}

static void vread(struct wd24s3 *s, u8 *dst)
{
	u32 o;

	for (o = 0; o < WD24S3_WIN; o += 4096)
		s->mem->vread(s->mem->priv, o, dst + o, 4096);
}

/* busy wait: 0 idle (CNTRL1 word in *last), -ETIMEDOUT; only the first
 * and the last read are logged, the count is kept */
static int wait_idle(struct wd24s3 *s, u32 *spins, bool *busy, u16 *last)
{
	u32 n;
	u16 v = 0;

	for (n = 0; n < WD24S3_SPINS; n++) {
		v = s->io->rd16(s->io->priv, WD24S1_P_DATA);
		s->reads++;
		s->polls++;
		if (n == 0)
			logacc(s, false, 16, WD24S1_P_DATA, v);
		if (!(v & WD24S1_ACT))
			break;
		if (busy)
			*busy = true;
	}
	if (n)
		logacc(s, false, 16, WD24S1_P_DATA, v);
	*spins = n;
	*last = v;
	return n < WD24S3_SPINS ? 0 : -ETIMEDOUT;
}

int wd24s3_idle_poll(struct wd24s3 *s)
{
	u32 n;
	u16 v = WD24S1_ACT;

	if (!s->dead || s->idle_seen)
		return 1;
	s->idle_checks++;
	s->idle_last_ix = in16(s, WD24S1_P_INDEX);
	s->idle_last_spins = 0;
	s->idle_last_c1 = 0xffff;
	if (s->idle_last_ix != SEL_CNTRL1)
		return 0;	/* select not as left at the start: unknown */
	for (n = 0; n < WD24S3_SPINS; n++) {
		v = s->io->rd16(s->io->priv, WD24S1_P_DATA);
		s->reads++;
		s->polls++;
		if (!(v & WD24S1_ACT))
			break;
	}
	logacc(s, false, 16, WD24S1_P_DATA, v);
	s->idle_last_spins = n;
	s->idle_last_c1 = v;
	if (n < WD24S3_SPINS) {
		s->idle_seen = true;
		return 1;
	}
	return 0;
}

static int read_all(struct wd24s3 *s, u16 *out, unsigned int *n)
{
	unsigned int k;

	for (k = 0; k < WD24S1_NSEL; k++) {
		if (sel_rb(s, wd24s1_sel(k)))
			return -EIO;
		out[k] = in16(s, WD24S1_P_DATA);
		*n = k + 1;
	}
	return 0;
}

static const char *snapshot(struct wd24s3 *s)
{
	unsigned int k;

	if (read_all(s, s->snap, &s->nsnap))
		return "snapshot pass 1: 23C0h did not read back a select";
	if (read_all(s, s->snap2, &s->nsnap2))
		return "snapshot pass 2: 23C0h did not read back a select";
	for (k = 0; k < WD24S1_NSEL; k++)
		if (s->snap[k] != s->snap2[k])
			return "snapshot unstable: the two passes differ";
	for (k = 0; k < WD24S1_NSEL; k++)
		if ((s->snap[k] >> 12) != (k < WD24S1_N_BLT ? k : k - WD24S1_N_BLT))
			return "snapshot: a word does not echo its index in bits 15:12";
	if (s->snap[0] & WD24S1_ACT)
		return "snapshot: CNTRL1 ACT set (engine busy or started)";
	if (s->snap[CC_K] & WD24S2_CC_EN)
		return "snapshot: cursor control enable set";
	return NULL;
}

/* banks 0-6 except the program's bank: checksums (S3: 0-5); then the
 * program's bank (left selected) into dst */
static int vram_pass(struct wd24s3 *s, u32 *crc, u8 *dst)
{
	unsigned int b, k = 0;

	for (b = 0; b <= WD24S3_NCRC; b++) {
		if ((u8)(b << 4) == s->prog->bank)
			continue;
		if (k >= WD24S3_NCRC || bank(s, (u8)(b << 4)))
			return -EIO;
		vread(s, s->buf);
		crc[k++] = wd24s3_sum(s->buf, WD24S3_WIN, 2166136261u);
	}
	if (k != WD24S3_NCRC || bank(s, s->prog->bank))
		return -EIO;
	if (dst)
		vread(s, dst);
	return 0;
}

static const char *pre_check(struct wd24s3 *s)
{
	unsigned int j;
	u32 i, n;

	if (vram_pass(s, s->crc0, s->pre))
		return "PR0A did not read back";
	vread(s, s->buf);
	if (memcmp(s->pre, s->buf, WD24S3_WIN))
		return "the stage bank read twice differs";
	memcpy(s->model, s->pre, WD24S3_WIN);
	for (j = 0; j < s->prog->nops; j++) {
		memcpy(s->buf, s->model, WD24S3_WIN);
		wd24s3_apply_at(&s->prog->ops[j], s->model, s->prog->base);
		for (i = n = 0; i < WD24S3_WIN; i++)
			n += s->buf[i] != s->model[i];
		s->changes[j] = n;
		if (!n)
			return "an operation would change no byte (inconclusive)";
	}
	memcpy(s->model, s->pre, WD24S3_WIN);
	return NULL;
}

static void restore_dirty(struct wd24s3 *s)
{
	unsigned int idx;

	for (idx = 1; idx < WD24S1_N_BLT; idx++) {
		u16 ref = s->snap[idx];

		if (!s->dirty[idx])
			continue;
		if (sel_rb(s, sel1((u8)idx)) || out16(s, WD24S1_P_DATA, ref) ||
		    in16(s, WD24S1_P_DATA) != ref) {
			s->restore_fail |= 1u << idx;
			continue;
		}
		s->dirty[idx] = false;
	}
}

/* one operation; NULL = ran and verified.  *res set on failure */
static const char *one(struct wd24s3 *s, int j, enum wd24s3_res *res)
{
	const struct wd24s3_op *o = &s->prog->ops[j];
	unsigned int p;
	u16 v;
	u32 i;

	*res = WD24S3_MISMATCH;
	s->op = j;
	if (sel_rb(s, SEL_CNTRL1))
		return "23C0h did not read back 1001h";
	if (wait_idle(s, &s->idle_spins[j], NULL, &v)) {
		*res = WD24S3_TIMEOUT;
		return "engine busy before the operation";
	}
	for (p = 0; p < o->npar; p++) {
		u16 w = (u16)(o->par[p].idx << 12) | o->par[p].val;

		if (sel_rb(s, sel1(o->par[p].idx)))
			return "23C0h did not read back the select";
		s->dirty[o->par[p].idx] = true;
		if (out16(s, WD24S1_P_DATA, w))
			return "guard refused a parameter write";
		if (in16(s, WD24S1_P_DATA) != w)
			return "parameter did not read back";
	}
	if (sel_rb(s, SEL_CNTRL1))
		return "23C0h did not read back 1001h";
	if (in16(s, WD24S1_P_DATA) & WD24S1_ACT)
		return "CNTRL1 ACT set before the start";
	s->go = true;
	i = out16(s, WD24S1_P_DATA, o->cntrl1);
	s->go = false;
	if (i)
		return "guard refused the CNTRL1 write";
	s->wgo++;
	if (wait_idle(s, &s->spins[j], &s->busy_seen[j], &s->c1_after[j])) {
		*res = WD24S3_TIMEOUT;
		return "engine still busy after the operation";
	}
	s->op = -1;
	wd24s3_apply_at(o, s->model, s->prog->base);
	vread(s, s->buf);
	for (i = 0; i < WD24S3_WIN; i++)
		if (s->buf[i] != s->model[i]) {
			if (!s->ndiff++) {
				s->diff_off = s->prog->base + i;
				s->diff_got = s->buf[i];
				s->diff_want = s->model[i];
			}
		}
	if (s->ndiff) {
		*res = WD24S3_VRAM_BAD;
		return "the stage bank differs from the model";
	}
	s->done++;
	return NULL;
}

int wd24s3_run(struct wd24s3 *s)
{
	enum wd24s3_res res = WD24S3_OK;
	const char *why = NULL;
	unsigned int k;
	u16 a, b;
	int j;

	if (s->res != WD24S3_NOT_RUN)
		return -EBUSY;
	a = in16(s, WD24S1_P_INDEX);
	b = in16(s, WD24S1_P_INDEX);
	s->res = WD24S3_REFUSED;
	if (a != b)
		return s->why = "23C0h read twice gave different values", -ENODEV;
	if (a == 0xffff)
		return s->why = "23C0h reads FFFF: not decoded in this state", -ENODEV;
	if (a & WD24S1_RESV)
		return s->why = "23C0h bits 15:14 (reserved) set", -ENODEV;
	s->alias = in8(s, P_AR) | (u16)(in8(s, P_ARR) << 8);
	if (s->alias == a)
		return s->why = "23C0h equals 3C0h/3C1h: possible alias", -ENODEV;
	if (a != WD24S2_S)
		return s->why = "23C0h is not 1002h", -ENODEV;
	s->saved = a;
	s->have_saved = true;

	why = snapshot(s);
	if (why) {
		res = WD24S3_SNAP_BAD;
		goto final;
	}
	s->snap_ok = true;
	why = pre_check(s);
	if (why) {
		res = WD24S3_PRE_BAD;
		goto banks;
	}
	s->armed = true;
	for (j = 0; j < (int)s->prog->nops; j++) {
		why = one(s, j, &res);
		if (why)
			break;
	}
	if (!why)
		res = WD24S3_OK;
	if (res == WD24S3_TIMEOUT) {
		s->dead = true;		/* no write of any kind from here */
		s->armed = false;
		s->res = res;
		s->why = why;
		return -ETIMEDOUT;
	}
	restore_dirty(s);
	s->armed = false;
	if (read_all(s, s->post, &s->npost)) {
		if (!why)
			why = "post-pass: 23C0h did not read back a select";
		res = WD24S3_RESTORE_BAD;
	} else {
		for (k = 1; k < WD24S1_NSEL; k++)
			if (s->post[k] != s->snap[k])
				break;
		if (k < WD24S1_NSEL || (s->post[0] & WD24S1_ACT)) {
			if (!why)
				why = "post-pass: a register differs from the snapshot (or ACT set)";
			res = WD24S3_RESTORE_BAD;
		}
	}
	if (s->restore_fail)
		res = WD24S3_RESTORE_BAD;
	if (vram_pass(s, s->crc1, NULL)) {
		if (!why)
			why = "PR0A did not read back (post check)";
		res = WD24S3_RESTORE_BAD;
	} else {
		for (k = 0; k < WD24S3_NCRC; k++)
			if (s->crc0[k] != s->crc1[k]) {
				if (res == WD24S3_OK) {
					res = WD24S3_VRAM_BAD;
					why = "video memory outside the stage bank changed";
				}
			}
	}
banks:
	if (bank(s, 0x00) && res == WD24S3_OK) {
		res = WD24S3_RESTORE_BAD;
		why = "PR0A 00h did not read back";
	}
final:
	if (out16(s, WD24S1_P_INDEX, s->saved & IDX_WR) == 0) {
		s->restored = true;
		s->restore_rb = in16(s, WD24S1_P_INDEX);
	}
	if (!s->restored || s->restore_rb != s->saved) {
		res = WD24S3_RESTORE_BAD;
		why = why ? why : "23C0h does not read back 1002h after the restore";
	}
	s->res = res;
	s->why = why;
	return res == WD24S3_OK ? 0 : -EIO;
}

#ifdef __KERNEL__
#ifdef WD24S1_QEMU_FAKE
#include <linux/moduleparam.h>
/*
 * QEMU TEST BUILD ONLY (wd24adv-qemufake.ko, never installed): real
 * inw()/outw() for the trace; values from a register file preset to the
 * 2026-11-04 S2 live snapshot and an emulated engine working on the RAM
 * model of video memory in main.c (wd24s3_fake_vram).
 */
u8 *wd24s3_fake_vram;
static u16 fake_ix = WD24S2_S;
static u16 fake_reg[WD24S1_NSEL] = {
	0x0108, 0x1000, 0x210b, 0x3004, 0x410b, 0x5004, 0x61e4, 0x713c,
	0x8280, 0x9300, 0xa000, 0xb0a5, 0xc000, 0xd000, 0xe0ff, 0xf0ff,
	0x0000, 0x1000, 0x2014, 0x3001, 0x4000, 0x5041, 0x6140, 0x70f0,
	0x8000,
};
static unsigned int fake_busy;
/* 0: busy 3 reads per op; 1: the first start never finishes; 2: the first
 * start stays busy for SPINS + 100 reads (S3 times out, then goes idle) */
static int fake_hang;
module_param(fake_hang, int, 0444);
static bool fake_started;

static void fake_blt(u16 c1)
{
	u32 src = (u32)(fake_reg[3] & 0xfff) << 12 | (fake_reg[2] & 0xfff);
	u32 dst = (u32)(fake_reg[5] & 0xfff) << 12 | (fake_reg[4] & 0xfff);
	u32 w = fake_reg[6] & 0xfff, h = fake_reg[7] & 0xfff;
	u32 p = fake_reg[8] & 0xfff, r, i;

	if (!wd24s3_fake_vram)
		return;
	/* BACK: addresses are the bottom-right pixel, rows bottom-up, right
	 * to left; result = ROP (idx 9 bits 11:8) of source and destination */
	for (r = 0; r < h; r++)
		for (i = 0; i < w; i++) {
			bool bk = c1 & 0x0400;
			u32 d = bk ? dst - r * p - i : dst + r * p + i;
			u32 sa = bk ? src - r * p - i : src + r * p + i;

			if (d < 7 * WD24S3_WIN && sa < 7 * WD24S3_WIN)
				wd24s3_fake_vram[d] = wd24s3_rop(fake_reg[9],
					(c1 & 0x8) ? (u8)fake_reg[0xa] : wd24s3_fake_vram[sa],
					wd24s3_fake_vram[d]);
		}
	fake_busy = 3;
	if (!fake_started && fake_hang)
		fake_busy = fake_hang == 1 ? UINT_MAX : WD24S3_SPINS + 100;
	fake_started = true;
}

static u16 k_inw(void *priv, u16 port)
{
	int k = wd24s2_k(fake_ix & 0xff, (fake_ix >> 8) & 0xf);

	(void)inw(port);
	if (port == WD24S1_P_INDEX)
		return fake_ix;
	if (k == 0 && fake_busy) {
		if (fake_busy != UINT_MAX)
			fake_busy--;
		return fake_reg[0] | WD24S1_ACT;
	}
	return k < 0 ? 0xffff : fake_reg[k];
}

static void k_outw(void *priv, u16 val, u16 port)
{
	int k = wd24s2_k(fake_ix & 0xff, (fake_ix >> 8) & 0xf);

	outw(val, port);
	if (port == WD24S1_P_INDEX)
		fake_ix = val & IDX_WR;
	else if (port == WD24S1_P_DATA && k == 0 && (val & WD24S1_ACT))
		fake_blt(val);
	else if (port == WD24S1_P_DATA && k > 0)
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

const struct wd24s1_io wd24s3_kernel_io = { k_inw, k_outw, k_inb, NULL };
#endif
