/* SPDX-License-Identifier: GPL-2.0 */
/* The canned cursor one-shot against a register file and a flat VRAM. */
#include <stdio.h>
#include <string.h>
#include "../wd24adv_cur.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

static const u16 snap0[WD24S1_NSEL] = {
	0x0108, 0x1000, 0x210b, 0x3004, 0x410b, 0x5004, 0x61e4, 0x713c,
	0x8280, 0x9300, 0xa000, 0xb0a5, 0xc000, 0xd000, 0xe0ff, 0xf0ff,
	0x0000, 0x1000, 0x2014, 0x3001, 0x4000, 0x5041, 0x6140, 0x70f0,
	0x8000,
};

#define VSZ 0x51000
static struct {
	u16 ix, reg[WD24S1_NSEL];
	u8 vram[VSZ];
	int reads, flip;
	int held, held_ok;
} m;

static int kof(u16 ix)
{
	if ((ix & 0xff) == 1 && ((ix >> 8) & 0xf) < 16)
		return (ix >> 8) & 0xf;
	if ((ix & 0xff) == 2 && ((ix >> 8) & 0xf) < 9)
		return 16 + ((ix >> 8) & 0xf);
	return -1;
}

static u16 t_inw(void *p, u16 port)
{
	int k;

	(void)p;
	if (port == WD24S1_P_INDEX)
		return m.ix;
	k = kof(m.ix);
	if (k < 0)
		return 0xffff;
	return m.reg[k];
}

static void t_outw(void *p, u16 v, u16 port)
{
	int k;

	(void)p;
	if (port == WD24S1_P_INDEX) {
		m.ix = v & 0x1fff;
		return;
	}
	if (port != WD24S1_P_DATA)
		return;
	/* The block is the index register. The register number is in the
	 * data word, which is how wd_cur_reg programs block 2. */
	if ((m.ix & 0xff) != 2)
		return;
	k = 16 + (v >> 12);
	if (k >= 16 && k < WD24S1_NSEL)
		m.reg[k] = v;
}

static u8 t_inb(void *p, u16 port)
{
	(void)p;
	(void)port;
	return 0;
}

static int t_bank(void *p, u8 v)
{
	(void)p;
	(void)v;
	return 0;
}

static void t_vread(void *p, u32 off, u8 *buf, u32 n)
{
	(void)p;
	(void)off;
	(void)buf;
	(void)n;
}

static const struct wd24s1_io io = { t_inw, t_outw, t_inb, NULL };
static const struct wd24s3_mem mem = { t_bank, t_vread, NULL };
static u8 pre[16], model[16], buf[16];
static struct wd24s3 s;

static int vread(void *p, u32 off, u8 *dst, u32 n)
{
	(void)p;
	if (off + n > VSZ)
		return -1;
	memcpy(dst, m.vram + off, n);
	if (off == WD24CUR_PAT && ++m.reads == m.flip) {
		dst[0] ^= 0xff;
		m.flip = 0;
	}
	return 0;
}

static int vwrite(void *p, u32 off, const u8 *src, u32 n)
{
	(void)p;
	if (off + n > VSZ)
		return -1;
	memcpy(m.vram + off, src, n);
	return 0;
}

static void hold(void *p)
{
	u8 pat[WD24CUR_BYTES];
	int i;

	(void)p;
	m.held = 1;
	wd24cur_pattern(pat);
	m.held_ok = !memcmp(m.vram + WD24CUR_PAT, pat, WD24CUR_BYTES);
	for (i = 0; i < 8; i++)
		m.held_ok &= m.reg[16 + i] ==
			(((u16)i << 12) | wd24cur_expect[i]);
}

static void reset(void)
{
	memset(&m, 0, sizeof(m));
	m.ix = WD24S2_S;
	memcpy(m.reg, snap0, sizeof(snap0));
	memset(m.vram, 0x5a, sizeof(m.vram));
	m.vram[WD24CUR_PAT] = 0xa5;
	wd24s3_init(&s, &io, &mem, pre, model, buf);
}

int main(void)
{
	u8 pat[WD24CUR_BYTES];
	struct wd24cur_mem vm = { vread, vwrite, hold, NULL };
	struct wd24cur_res res;
	unsigned int i;
	int r;

	wd24cur_pattern(pat);
	T(pat[0] == 0x3f && pat[1] == 0x80, "pat row0");
	T(pat[2] == 0xff && pat[3] == 0x00, "pat transparent");
	T(pat[254] == 0xfe && pat[255] == 0x01, "pat last pixel");
	T(wd24cur_expect[0] == 0x820 && wd24cur_expect[6] == 0x140 &&
	  wd24cur_expect[7] == 0x0f0, "755C centre line");
	T(wd24cur_show[2].idx == 1 && wd24cur_show[3].idx == 2 &&
	  wd24cur_show[7].val == 0x820, "address then enable");

	reset();
	s.have_saved = true;
	s.saved = WD24S2_S;
	s.armed = true;
	s.prog = &wd24s3_prog4;
	s.cur = WD24S1_NOINC | 2;
	T(wd24s3_guard_out(&s, WD24S1_P_DATA, 0x0820) != 0, "block 2 closed");
	s.cursor = true;
	T(wd24s3_guard_out(&s, WD24S1_P_DATA, 0x6140) == 0, "position word");
	T(wd24s3_guard_out(&s, WD24S1_P_DATA, 0x0820) != 0, "enable is not first");
	s.cstep = WD24CUR_NSHOW;
	s.chide = true;
	T(wd24s3_guard_out(&s, WD24S1_P_DATA, WD24CUR_HIDE) == 0, "enable clear");
	T(wd24s3_guard_out(&s, WD24S1_P_DATA, 0x6140) != 0, "no second show");

	reset();
	r = wd24cur_run(&s, &vm, &res);
	T(r == 0 && res.show == 1 && res.regs == 8 && res.vram == 256 &&
	  res.restored == 1, "run");
	T(m.held == 1 && m.held_ok == 1, "held while shown");
	T(m.vram[WD24CUR_PAT] == 0xa5, "pattern byte restored");
	T(m.ix == WD24S2_S, "index restored");
	for (i = r = 0; i < WD24S1_NSEL; i++)
		r += m.reg[i] != snap0[i];
	T(r == 0, "block 2 and block 1 restored");

	reset();
	m.flip = 2;
	r = wd24cur_run(&s, &vm, &res);
	T(r != 0 && res.show == 0 && m.held == 0, "bad pattern does not show");
	T(res.restored == 1 && m.vram[WD24CUR_PAT] == 0xa5, "bad pattern restored");
	for (i = r = 0; i < WD24S1_NSEL; i++)
		r += m.reg[i] != snap0[i];
	T(r == 0 && m.ix == WD24S2_S, "registers untouched");

	printf("test_cur: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
