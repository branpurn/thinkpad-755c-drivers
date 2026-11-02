// SPDX-License-Identifier: GPL-2.0
/*
 * wd24adv S1: read-only probe of 23C0h blocks 1 and 2.  See wd24adv_s1.h
 * for the exact write list, the stop rules and the sources.  Shared by the
 * kernel module (s1=1 dryrun=0) and tests/test_s1.c (port model).
 */
#include "wd24adv_s1.h"
#ifdef __KERNEL__
#include <linux/io.h>
#else
#include <string.h>
#endif

#define P_AR	0x3c0
#define P_ARR	0x3c1
#define IDX_WR	0x1fff		/* DS 17.2: 15:14 reserved, 13 read-only */

u16 wd24s1_sel(unsigned int k)
{
	if (k < WD24S1_N_BLT)
		return WD24S1_NOINC | (u16)(k << 8) | 1;
	return WD24S1_NOINC | (u16)((k - WD24S1_N_BLT) << 8) | 2;
}

static bool is_sel(u16 v)
{
	unsigned int k;

	for (k = 0; k < WD24S1_NSEL; k++)
		if (wd24s1_sel(k) == v)
			return true;
	return false;
}

int wd24s1_guard_out(const struct wd24s1 *s, u16 port, u16 val)
{
	if (port != WD24S1_P_INDEX)
		return -EPERM;		/* 23C2h, 23C4h, everything else */
	if (!s->have_saved || s->restored || s->writes >= WD24S1_MAX_WRITES)
		return -EPERM;
	if (is_sel(val) || val == (s->saved & IDX_WR))
		return 0;
	return -EPERM;
}

int wd24s1_guard_in(u16 port, int width)
{
	if (width == 16 && (port == WD24S1_P_INDEX || port == WD24S1_P_DATA))
		return 0;
	if (width == 8 && (port == P_AR || port == P_ARR))
		return 0;
	return -EPERM;		/* 23C4h: never (DS 21.13) */
}

void wd24s1_init(struct wd24s1 *s, const struct wd24s1_io *io)
{
	memset(s, 0, sizeof(*s));
	s->io = io;
	s->strict_echo = true;
	s->stop_pass = s->stop_sel = -1;
}

const char *wd24s1_res_name(enum wd24s1_res r)
{
	switch (r) {
	case WD24S1_OK: return "OK";
	case WD24S1_REFUSED: return "REFUSED (no 23C0h write)";
	case WD24S1_STOPPED: return "STOPPED (restored)";
	case WD24S1_UNSTABLE: return "UNSTABLE (pass 1 != pass 2, restored)";
	case WD24S1_RESTORE_BAD: return "RESTORE MISMATCH";
	default: return "not run";
	}
}

static void logacc(struct wd24s1 *s, bool w, int width, u16 port, u16 val)
{
	if (s->nlog < WD24S1_NLOG)
		s->log[s->nlog++] = (struct wd24s1_acc){ w, (u8)width, port, val };
}

static u16 in16(struct wd24s1 *s, u16 port)
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

static u8 in8(struct wd24s1 *s, u16 port)
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

static int out16(struct wd24s1 *s, u16 val)
{
	if (wd24s1_guard_out(s, WD24S1_P_INDEX, val)) {
		s->refused++;
		return -EPERM;
	}
	s->io->wr16(s->io->priv, val, WD24S1_P_INDEX);
	s->writes++;
	logacc(s, true, 16, WD24S1_P_INDEX, val);
	return 0;
}

static int refuse(struct wd24s1 *s, const char *why)
{
	s->res = WD24S1_REFUSED;
	s->why = why;
	return -ENODEV;
}

int wd24s1_run(struct wd24s1 *s)
{
	enum wd24s1_res res = WD24S1_OK;
	unsigned int p, k;
	u16 a, b;

	if (s->res != WD24S1_NOT_RUN)
		return -EBUSY;		/* one run per load */

	/* reads only until every precondition holds */
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
	s->saved = a;
	s->have_saved = true;

	for (p = 0; p < WD24S1_NPASS && res == WD24S1_OK; p++) {
		for (k = 0; k < WD24S1_NSEL; k++) {
			u16 sel = wd24s1_sel(k), ix, v;

			if (out16(s, sel)) {
				res = WD24S1_STOPPED;
				s->why = "guard refused a select";
				break;
			}
			ix = in16(s, WD24S1_P_INDEX);
			s->ix[p][k] = ix;
			if (ix != sel) {
				res = WD24S1_STOPPED;
				s->why = ix & WD24S1_INVALID ? "23C0h b13 set: block does not exist"
					: "23C0h did not read back the select";
				break;
			}
			v = in16(s, WD24S1_P_DATA);
			s->val[p][k] = v;
			s->done[p] = k + 1;
			if (s->strict_echo && (v >> 12) != ((sel >> 8) & 0xf)) {
				res = WD24S1_STOPPED;
				s->why = "23C2h bits 15:12 do not echo the index";
				break;
			}
			if (k == 0 && (v & WD24S1_ACT)) {
				res = WD24S1_STOPPED;
				s->why = "block 1 CNTRL1 ACT = 1: engine busy at rest";
				break;
			}
		}
		if (res != WD24S1_OK) {
			s->stop_pass = p;
			s->stop_sel = k;
		}
	}
	if (res == WD24S1_OK)
		for (k = 0; k < WD24S1_NSEL; k++)
			if (s->val[0][k] != s->val[1][k] || s->ix[0][k] != s->ix[1][k]) {
				res = WD24S1_UNSTABLE;
				s->why = "pass 1 and pass 2 differ";
				s->stop_sel = k;
				break;
			}

	/* restore: S with the read-only / reserved bits cleared, read back */
	if (out16(s, s->saved & IDX_WR) == 0) {
		s->restored = true;
		s->restore_rb = in16(s, WD24S1_P_INDEX);
	}
	if (!s->restored || s->restore_rb != s->saved) {
		res = WD24S1_RESTORE_BAD;
		s->why = s->why ? s->why : "23C0h does not read back S after the restore";
	}
	s->res = res;
	return res == WD24S1_OK ? 0 : -EIO;
}

#ifdef __KERNEL__
#ifdef WD24S1_QEMU_FAKE
/*
 * QEMU TEST BUILD ONLY (tests/qemu-wd24adv.py builds it into a separate
 * wd24adv-qemufake.ko that is never installed): the real inw()/outw()
 * happen, so QEMU's cpu_in/cpu_out trace shows them, but 23C0h/23C2h
 * read values come from this tiny model, because QEMU has no 23C0h.
 */
static u16 fake_ix = 0x1001;	/* modelled S: what wd90c24fb leaves */

static u16 k_inw(void *priv, u16 port)
{
	u16 blk = fake_ix & 0xff, idx = (fake_ix >> 8) & 0xf;

	(void)inw(port);
	if (port == WD24S1_P_INDEX)
		return fake_ix;
	return (u16)(idx << 12) | (u16)(blk << 4) | idx;
}

static void k_outw(void *priv, u16 val, u16 port)
{
	outw(val, port);
	if (port == WD24S1_P_INDEX)
		fake_ix = val & IDX_WR;
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

const struct wd24s1_io wd24s1_kernel_io = { k_inw, k_outw, k_inb, NULL };
#endif
