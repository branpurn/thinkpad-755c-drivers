// SPDX-License-Identifier: GPL-2.0
/* S0 host test of the BITBLT / cursor guard, validator and sequencer
 * (wd24adv_engine.c) against the 23C0h block model in vgamodel.c.  The
 * engine code has no live path: this is the only place it runs. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "vgamodel.h"

_Static_assert(WD24X_LIVE_ALLOWED == 0, "S0: no live 23C0h path");

static int fails, passes;
#define T(cond, what) do { if (cond) passes++; else { fails++; \
	printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

static struct vgamodel m;
static struct vm_eng e;
static struct wd24x x;
static struct wd24x_io io;
static u8 ref[WD24X_VRAM];
static u16 host[WD24X_HOST_MAX_WORDS];

static unsigned int rs = 1;
static u32 rnd(void)
{
	rs = rs * 1103515245u + 12345u;
	return (rs >> 8) & 0xffffff;
}

static void setup(unsigned int seed)
{
	vm_init(&m, VM_WD24, seed);
	vm_eng_init(&e, seed);
	m.eng = &e;
	vm_xio(&io, &m);
	wd24x_init(&x, &io);
	x.enabled = true;
}

static unsigned long acc(void)
{
	return e.ix_writes + e.data_writes + e.reads + e.host_words + m.violations;
}

static bool clean(void)
{
	if (e.violations || m.violations)
		printf("  model: %s %s\n", e.violation, m.violation);
	return !e.violations && !m.violations;
}

/* ---- independent restatement of the closed lists (spec in the test) ---- */
static bool in(u16 v, const u16 *s, unsigned int n)
{
	while (n--)
		if (s[n] == v)
			return true;
	return false;
}

static bool spec_has(u8 blk, u8 idx)
{
	if (blk == 1)
		return idx <= 11 || idx == 14;
	if (blk == 2)
		return idx <= 7;
	return false;
}

static bool spec_ok(u8 blk, u8 idx, u16 d)
{
	static const u16 c1[] = { 0x900, 0xd00, 0x908, 0x90e, 0x902 };
	static const u16 c2[] = { 0x000, 0x008 };
	static const u16 rop[] = { 0x000, 0x100, 0x300, 0x600, 0x700, 0xa00, 0xf00 };
	static const u16 cc[] = { 0x000, 0x020, 0x820 };

	if (blk == 1)
		switch (idx) {
		case 0: return in(d, c1, 5);
		case 1: return in(d, c2, 2);
		case 2: case 4: return true;
		case 3: case 5: return d <= 0xff;
		case 6: case 7: return d >= 1 && d <= 2048;
		case 8: return d == 640;
		case 9: return in(d, rop, 7);
		case 10: case 11: return d <= 0xff;
		case 14: return d == 0xff;
		default: return false;
		}
	if (blk == 2)
		switch (idx) {
		case 0: return in(d, cc, 3);
		case 1: return d == 0x400;
		case 2: return d == 0x013;
		case 3: case 4: return d <= 0xff;
		case 5: return !(d & ~0x7df);
		case 6: return d <= 639;
		case 7: return d <= 479;
		default: return false;
		}
	return false;
}

/* ---- reference results -------------------------------------------------- */
static u8 ref_rop(u16 r, u8 s, u8 d)
{
	u8 out = 0;
	int b;

	for (b = 0; b < 8; b++)
		out |= (((r >> 8) >> (3 - (2 * ((s >> b) & 1) + ((d >> b) & 1)))) & 1) << b;
	return out;
}

static void ref_op(const struct wd24x_cmd *c, const u16 *h)
{
	static u8 tmp[WD24X_PITCH * 2048];
	u32 r, k, per = (c->w + 3) / 4;

	if (c->op == WD24X_COPY)
		for (r = 0; r < c->h; r++)
			memcpy(tmp + r * c->w, ref + c->src + r * WD24X_PITCH, c->w);
	for (r = 0; r < c->h; r++)
		for (k = 0; k < c->w; k++) {
			u8 *d = &ref[c->dst + r * WD24X_PITCH + k], s;

			switch (c->op) {
			case WD24X_FILL: s = c->fg; break;
			case WD24X_COPY: s = tmp[r * c->w + k]; break;
			case WD24X_MONO: {
				bool one = (h[r * per + k / 4] >> (3 - k % 4)) & 1;

				if (!one && c->transparent)
					continue;
				s = one ? c->fg : c->bg;
				break;
			}
			default: {
				const u16 *q = h + r * per * 2 + (k / 4) * 2;
				u32 dw = q[0] | ((u32)q[1] << 16);

				s = dw >> (8 * (k % 4));
			}
			}
			*d = ref_rop(c->rop, s, *d);
		}
}

static bool run_ref(const struct wd24x_cmd *c, u32 n, int *err)
{
	memcpy(ref, e.vram, sizeof(ref));
	ref_op(c, host);
	*err = wd24x_run(&x, c, host, n);
	return !memcmp(ref, e.vram, sizeof(ref));
}

static void fill_host(u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		host[i] = rnd();
}

/* ---- static: nothing live references the engine ------------------------ */
static int scan(const char *path, const char *const *bad)
{
	static char buf[1 << 20];
	FILE *f = fopen(path, "r");
	size_t n;
	int hits = 0;

	if (!f) {
		printf("  cannot open %s\n", path);
		return 1;
	}
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	for (n = 0; buf[n]; n++)
		buf[n] = (buf[n] >= 'A' && buf[n] <= 'Z') ? buf[n] + 32 : buf[n];
	for (; *bad; bad++)
		if (strstr(buf, *bad)) {
			printf("  %s contains '%s'\n", path, *bad);
			hits++;
		}
	return hits;
}

static void t_gate(void)
{
	static const char *const bad[] = { "0x23c", "wd24x", "engine", "inw(",
					   "outw(", NULL };
	static const char *const files[] = { "../wd24adv_main.c", "../wd24adv_port.c",
		"../wd24adv_core.c", "../wd24adv_modes.c", "../wd24adv_core.h",
		"../wd24adv_ioctl.h", "../Kbuild", NULL };
	struct wd24x_io live;
	struct wd24x_cmd c = { WD24X_FILL, 1, 0, false, WD24X_ROP_SRC, 0, 0, 8, 8 };
	unsigned int i, refused = 0;
	u32 p;

	static const char *const s1bad[] = { "wd24x", "wd24adv_engine", "0x23c4", NULL };
	static const char *const s1files[] = { "../wd24adv_s1.c", "../wd24adv_s1.h", NULL };

	for (i = 0; files[i]; i++)
		T(scan(files[i], bad) == 0, "live module source never references the engine");
	/* S1 (in Kbuild) is separate from the S0 engine and has no 23C4h */
	for (i = 0; s1files[i]; i++)
		T(scan(s1files[i], s1bad) == 0, "S1 source never references the engine or 23C4h");

	setup(1);
	live = io;
	live.live = true;
	wd24x_init(&x, &live);
	x.enabled = true;
	for (p = 0; p <= 0xffff; p++) {
		refused += wd24x_guard(&x, p, 0x1001) == -EPERM;
		refused += wd24x_guard_in(&x, p) == -EPERM;
	}
	T(refused == 2 * 0x10000, "live io: guard refuses every port, read and write");
	T(wd24x_run(&x, &c, NULL, 0) == -EPERM, "live io: run refused");
	T(wd24x_cursor(&x, true, 10, 10, 0, 0, 15, 0) == -EPERM, "live io: cursor refused");
	T(wd24x_sync(&x) == 0 && wd24x_cpu_begin(&x) == 0, "live io: sync is a no-op");
	T(acc() == 0, "live io: zero accesses");
}

/* ---- the existing VGA guard still refuses the dangerous registers ------- */
static void t_core_guard(void)
{
	struct wd24_ctx c;

	wd24_ctx_init(&c, &wd24_port_backend);
	c.pr72_open = true;
	c.allow_clock = false;
	T(wd24_guard(&c, WD24_CR, WD24_CR_PR18, 0, 0) == -EPERM, "core: PR18 refused");
	T(wd24_guard(&c, WD24_CR, WD24_CR_PR19, 0, 0) == -EPERM, "core: PR19 refused");
	T(wd24_guard(&c, WD24_CR, WD24_CR_PR1A, 0, 0) == -EPERM, "core: PR1A refused");
	T(wd24_guard(&c, WD24_CR, WD24_CR_PR36, 0, 0) == -EPERM, "core: PR36 refused");
	T(wd24_guard(&c, WD24_CR, WD24_CR_PR1B, 0, 0x06) == -EPERM, "core: CR34 shadow key refused");
	T(wd24_guard(&c, WD24_CR, WD24_CR_PR1B, 0, 0xa5) == -EPERM, "core: CR34 open (routing) refused unarmed");
	T(wd24_guard(&c, WD24_SR, WD24_SR_PR57, 0, 0) == -EPERM, "core: PR57 panel power refused");
	T(wd24_guard(&c, WD24_SR, WD24_SR_PR68, 0, 0) == -EPERM, "core: PR68 refused without allow_clock");
	T(wd24_guard(&c, WD24_SR, WD24_SR_PR69, 0, 0) == -EPERM, "core: PR69 refused without allow_clock");
	T(wd24_guard(&c, WD24_GR, WD24_GR_PR1, 0xc5, 0xd5) == -EPERM, "core: PR1 b5:4 aperture refused");
	T(wd24_guard(&c, WD24_GR, WD24_GR_PR4, 0x00, 0x20) == -EPERM, "core: PR4 b5 tri-state refused");
}

/* ---- the 23Cxh guard ---------------------------------------------------- */
static void t_guard(void)
{
	unsigned long acc_ix = 0, exp_ix = 0, acc_d = 0, exp_d = 0, bad = 0;
	unsigned int blk, i;
	u32 v, p;

	setup(2);
	/* 23C0h: only NOINC selects of a listed register, or the saved value */
	for (v = 0; v <= 0xffff; v++) {
		bool want = !(v & 0xe000) && (v & 0x1000) && spec_has(v & 0xff, (v >> 8) & 0xf) &&
			    (v & 0xff) <= 2;
		bool got = wd24x_guard(&x, WD24X_P_INDEX, v) == 0;

		acc_ix += got;
		exp_ix += want;
		bad += got != want;
	}
	T(bad == 0 && acc_ix == exp_ix && acc_ix == 21, "23C0h: exactly the 21 listed selects");
	x.have_saved = true;
	x.saved_ix = 0x2345;
	T(wd24x_guard(&x, WD24X_P_INDEX, 0x0345) == 0, "23C0h: restore of the saved value");
	T(wd24x_guard(&x, WD24X_P_INDEX, 0x2345) == -EINVAL, "23C0h: read-only bit 13 never written");
	x.have_saved = false;
	T(wd24x_guard(&x, WD24X_P_INDEX, 0x0001) == -EINVAL, "23C0h: auto-increment select refused");
	T(wd24x_guard(&x, WD24X_P_INDEX, 0x1000) == -EPERM, "23C0h: block 0 (interrupts) refused");
	T(wd24x_guard(&x, WD24X_P_INDEX, 0x1003) == -EPERM, "23C0h: block 3 refused");
	T(wd24x_guard(&x, WD24X_P_INDEX, 0x1801) == 0, "23C0h: select PITCH allowed");
	T(wd24x_guard(&x, WD24X_P_INDEX, 0x1c01) == -EPERM, "23C0h: select transparency colour refused");
	T(wd24x_guard(&x, WD24X_P_INDEX, 0x1f01) == -EPERM, "23C0h: select block 1 idx F refused");
	T(wd24x_guard(&x, WD24X_P_INDEX, 0x1802) == -EPERM, "23C0h: select cursor aux refused");

	/* 23C2h fuzz, every value, every block, armed + idle */
	bad = 0;
	x.armed = true;
	x.idle_seen = true;
	x.plan_cntrl1 = 0x908;
	for (blk = 0; blk < 4; blk++) {
		x.cur_blk = blk;
		for (v = 0; v <= 0xffff; v++) {
			u8 idx = v >> 12;
			bool want = spec_ok(blk, idx, v & 0xfff) &&
				    !(blk == 1 && idx == 0 && (v & 0xfff) != 0x908);
			bool got = wd24x_guard(&x, WD24X_P_DATA, v) == 0;

			acc_d += got;
			exp_d += want;
			bad += got != want;
		}
	}
	T(bad == 0 && acc_d == exp_d, "23C2h: accepts exactly the closed lists (all 262144)");
	printf("  guard: %lu 23C0h and %lu 23C2h values accepted of 65536/262144\n", acc_ix, acc_d);

	x.cur_blk = 1;
	{
		static const struct { u16 v; const char *w; } no[] = {
			{ 0x0000, "CNTRL1 0 (abort) refused" },
			{ 0x0100, "CNTRL1 without ACT refused" },
			{ 0x0988, "CNTRL1 linear destination refused" },
			{ 0x0948, "CNTRL1 linear source refused" },
			{ 0x0920, "CNTRL1 destination system I/O refused" },
			{ 0x0a00, "CNTRL1 planar mode refused" },
			{ 0x0904, "CNTRL1 mono-from-comparators refused (not planned)" },
			{ 0x1800, "CNTRL2 line draw refused" },
			{ 0x1400, "CNTRL2 IRQ refused" },
			{ 0x1080, "CNTRL2 quick start refused" },
			{ 0x1040, "CNTRL2 update destination refused" },
			{ 0x1001, "CNTRL2 colour transparency refused" },
			{ 0x1002, "CNTRL2 reserved b1 refused" },
			{ 0x3100, "SRC_HI >= 1 MB refused" },
			{ 0x3200, "SRC_HI reserved bits refused" },
			{ 0x5100, "DST_HI >= 1 MB refused" },
			{ 0x6000, "DIM_X 0 refused" },
			{ 0x6801, "DIM_X 2049 refused" },
			{ 0x7000, "DIM_Y 0 refused" },
			{ 0x8400, "PITCH 1024 refused" },
			{ 0x8282, "PITCH 642 refused" },
			{ 0x9c00, "ROP C (src-inverted) not in list" },
			{ 0x9301, "ROP reserved low byte refused" },
			{ 0xa100, "FG reserved bits refused" },
			{ 0xc000, "transparency colour C refused" },
			{ 0xd0ff, "transparency mask D refused" },
			{ 0xe0fe, "mask other than FF refused" },
			{ 0xf000, "block 1 index F refused" },
		};
		for (i = 0; i < sizeof(no) / sizeof(no[0]); i++)
			T(wd24x_guard(&x, WD24X_P_DATA, no[i].v) < 0, no[i].w);
	}
	x.cur_blk = 2;
	T(wd24x_guard(&x, WD24X_P_DATA, 0x8000) == -EPERM, "cursor aux colour (idx 8) refused");
	T(wd24x_guard(&x, WD24X_P_DATA, 0x9000) == -EPERM, "cursor idx 9 refused");
	T(wd24x_guard(&x, WD24X_P_DATA, 0x0a20) < 0, "cursor 64x64 refused");
	T(wd24x_guard(&x, WD24X_P_DATA, 0x0920) < 0, "cursor plane protection refused");
	T(wd24x_guard(&x, WD24X_P_DATA, 0x0840) < 0, "cursor other colour mode refused");
	T(wd24x_guard(&x, WD24X_P_DATA, 0x1000) < 0, "cursor pattern other than 4D000h refused");
	T(wd24x_guard(&x, WD24X_P_DATA, 0x5020) < 0, "cursor origin X 32 refused");
	T(wd24x_guard(&x, WD24X_P_DATA, 0x5800) < 0, "cursor origin Y 32 refused");
	T(wd24x_guard(&x, WD24X_P_DATA, 0x6280) < 0, "cursor X 640 refused");
	T(wd24x_guard(&x, WD24X_P_DATA, 0x71e0) < 0, "cursor Y 480 refused");

	/* state rules */
	x.cur_blk = 1;
	x.armed = false;
	T(wd24x_guard(&x, WD24X_P_DATA, 0x6010) == -EBUSY, "23C2h refused outside a plan");
	x.armed = true;
	x.busy = true;
	T(wd24x_guard(&x, WD24X_P_DATA, 0x6010) == -EBUSY, "block 1 refused while busy");
	x.cur_blk = 2;
	T(wd24x_guard(&x, WD24X_P_DATA, 0x3007) == 0, "cursor colour allowed while BITBLT busy");
	x.busy = false;
	x.idle_seen = false;
	x.cur_blk = 1;
	T(wd24x_guard(&x, WD24X_P_DATA, 0x6010) == -EBUSY, "block 1 refused until idle seen");
	x.idle_seen = true;
	x.dead = true;
	T(wd24x_guard(&x, WD24X_P_DATA, 0x6010) == -EBUSY, "23C2h refused once dead");
	x.dead = false;
	T(wd24x_guard(&x, WD24X_P_HOST, 0) == -EBUSY, "23C4h refused with no host op");
	x.busy = x.host_op = true;
	x.host_left = 0;
	T(wd24x_guard(&x, WD24X_P_HOST, 0) == -EBUSY, "23C4h refused when no word is owed");
	x.host_left = 1;
	T(wd24x_guard(&x, WD24X_P_HOST, 0) == 0, "23C4h allowed for an owed word");
	T(wd24x_guard_in(&x, WD24X_P_HOST) == -EPERM, "23C4h never read");
	bad = 0;
	for (p = 0; p <= 0xffff; p++)
		if (p != WD24X_P_INDEX && p != WD24X_P_DATA && p != WD24X_P_HOST)
			bad += !wd24x_guard(&x, p, 0) || !wd24x_guard_in(&x, p);
	T(bad == 0, "every other port refused (VGA, PM 24h-27h, 23C1h/23C6h...)");
	T(acc() == 0, "guard checks make no access");
}

/* ---- validation, VRAM range, overlap direction -------------------------- */
static void t_check(void)
{
	static const struct { struct wd24x_cmd c; const char *w; } no[] = {
		{ { WD24X_FILL, 1, 0, false, WD24X_ROP_SRC, 0, 0, 0, 8 }, "w = 0" },
		{ { WD24X_FILL, 1, 0, false, WD24X_ROP_SRC, 0, 0, 8, 0 }, "h = 0" },
		{ { WD24X_FILL, 1, 0, false, WD24X_ROP_SRC, 0, 0, 641, 1 }, "w > pitch" },
		{ { WD24X_FILL, 1, 0, false, WD24X_ROP_SRC, 0x4c000, 0, 8, 2049 }, "h > 2048" },
		{ { WD24X_FILL, 1, 0, false, WD24X_ROP_SRC, 0, 0, 8, 481 }, "past frame end" },
		{ { WD24X_FILL, 1, 0, false, WD24X_ROP_SRC, 636, 0, 8, 1 }, "row wraps" },
		{ { WD24X_FILL, 1, 0, false, WD24X_ROP_SRC, 0x4b000, 0, 8, 1 }, "guard page" },
		{ { WD24X_FILL, 1, 0, false, WD24X_ROP_SRC, 0x4bf00, 0, 640, 2 }, "frame into guard" },
		{ { WD24X_FILL, 1, 0, false, WD24X_ROP_SRC, 0x4d000, 0, 8, 1 }, "cursor slot" },
		{ { WD24X_FILL, 1, 0, false, WD24X_ROP_SRC, 0x4cf00, 0, 640, 1 }, "scratch into cursor" },
		{ { WD24X_FILL, 1, 0, false, WD24X_ROP_SRC, 0xfff00, 0, 640, 1 }, "past 1 MB" },
		{ { WD24X_FILL, 1, 0, false, WD24X_ROP_SRC, 0x100000, 0, 1, 1 }, "at 1 MB" },
		{ { WD24X_COPY, 0, 0, false, WD24X_ROP_SRC, 0, 0x4b000, 8, 1 }, "source in guard" },
		{ { WD24X_COPY, 0, 0, false, WD24X_ROP_SRC, 0, 0x4d080, 8, 1 }, "source in cursor slot" },
		{ { WD24X_COPY, 0, 0, false, WD24X_ROP_SRC, 0, 0xfffff, 2, 1 }, "source past 1 MB" },
		{ { WD24X_FILL, 1, 0, false, 0x0c00, 0, 0, 8, 1 }, "rop not listed" },
		{ { WD24X_FILL, 1, 0, false, 0x0301, 0, 0, 8, 1 }, "rop low bits" },
		{ { WD24X_FILL, 1, 0, true, WD24X_ROP_SRC, 0, 0, 8, 1 }, "transparent fill" },
		{ { WD24X_MONO, 1, 0, false, WD24X_ROP_SRC, 0x4e000, 0, 640, 410 }, "host data cap" },
		{ { 0, 1, 0, false, WD24X_ROP_SRC, 0, 0, 8, 1 }, "op 0" },
		{ { 9, 1, 0, false, WD24X_ROP_SRC, 0, 0, 8, 1 }, "op 9" },
	};
	static const struct wd24x_cmd yes[] = {
		{ WD24X_FILL, 1, 0, false, WD24X_ROP_SRC, 0, 0, 640, 480 },
		{ WD24X_FILL, 1, 0, false, WD24X_ROP_SRC, 0x4c000, 0, 640, 6 },
		{ WD24X_FILL, 1, 0, false, WD24X_ROP_SRC, 0xfffff, 0, 1, 1 },
		{ WD24X_COPY, 0, 0, false, WD24X_ROP_SRC, 0x4e000, 0, 640, 480 },
	};
	struct wd24x_cmd c;
	unsigned int i, n, wrong = 0, dirbad = 0, ran = 0, back = 0;
	unsigned long a;
	int err;

	setup(3);
	for (i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
		char w[80];

		a = acc();
		snprintf(w, sizeof(w), "refused: %s", no[i].w);
		T(wd24x_check(&no[i].c, NULL) == -EINVAL &&
		  wd24x_run(&x, &no[i].c, host, wd24x_host_words(&no[i].c)) == -EINVAL &&
		  acc() == a, w);
	}
	for (i = 0; i < sizeof(yes) / sizeof(yes[0]); i++)
		T(wd24x_check(&yes[i], NULL) == 0, "legal rectangle accepted");

	/* overlap: 400 random overlapping copies vs memmove semantics */
	for (n = 0; n < 400; n++) {
		c = (struct wd24x_cmd){ WD24X_COPY, 0, 0, false, WD24X_ROP_SRC, 0, 0, 0, 0 };
		c.w = 1 + rnd() % 96;
		c.h = 1 + rnd() % 40;
		c.src = (rnd() % (480 - c.h)) * 640 + rnd() % (640 - c.w);
		c.dst = c.src + ((int)(rnd() % 9) - 4) * 640 + (int)(rnd() % 9) - 4;
		if (wd24x_check(&c, NULL))
			continue;
		dirbad += wd24x_backward(&c) != (c.dst > c.src);
		ran++;
		back += wd24x_backward(&c);
		wrong += !run_ref(&c, 0, &err) || err;
	}
	printf("  overlap: %u copies (%u backward), %lu ops in the model\n", ran, back, e.ops);
	T(ran > 300 && back > 100 && ran - back > 100 && e.ops == ran, "overlap sample is real");
	T(dirbad == 0, "backward exactly when destination > source");
	T(wrong == 0 && clean(), "overlapping copies match memmove (both directions)");

	/* the model is sensitive to direction: a forward overlap-down copy
	 * driven straight into the model corrupts */
	{
		static const u16 seq[] = { 0x1001, 0x2000 | 0x100, 0x3000, 0x4000 | 0x380,
			0x5000, 0x6000 | 64, 0x7000 | 16, 0x8000 | 640, 0x9300, 0xe0ff, 0x1000,
			0x0900 };
		struct wd24x_cmd d = { WD24X_COPY, 0, 0, false, WD24X_ROP_SRC, 0x380, 0x100, 64, 16 };

		memcpy(ref, e.vram, sizeof(ref));
		ref_op(&d, NULL);
		for (i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
			io.outw(io.priv, seq[i], i ? WD24X_P_DATA : WD24X_P_INDEX);
		T(memcmp(ref, e.vram, sizeof(ref)) != 0, "model: wrong direction corrupts");
	}
}

/* ---- exact host-data word counts ---------------------------------------- */
static void t_host(void)
{
	struct wd24x_cmd c;
	unsigned int bad = 0, wrong = 0, w, h;
	unsigned long a, hw;
	int err, op;

	for (w = 1; w <= 72; w++) {
		c = (struct wd24x_cmd){ WD24X_MONO, 0, 0, false, WD24X_ROP_SRC, 0, 0, w, 3 };
		bad += wd24x_host_words(&c) != 3 * ((w + 3) / 4);
		c.op = WD24X_HOSTCOL;
		bad += wd24x_host_words(&c) != 3 * 2 * ((w + 3) / 4);
		c.op = WD24X_FILL;
		bad += wd24x_host_words(&c) != 0;
	}
	T(bad == 0, "host words: MONO h*ceil(w/4), HOSTCOL 2h*ceil(w/4), else 0");

	setup(4);
	for (op = WD24X_MONO; op <= WD24X_HOSTCOL; op++)
		for (w = 1; w <= 40; w++)
			for (h = 1; h <= 3; h++) {
				u32 n;

				c = (struct wd24x_cmd){ op, rnd(), rnd(), op == WD24X_MONO && (w & 1),
					w % 3 ? WD24X_ROP_SRC : WD24X_ROP_XOR,
					0x4e000 + rnd() % 400 * 640 + rnd() % 600, 0, w, h };
				if (wd24x_check(&c, NULL))
					continue;
				n = wd24x_host_words(&c);
				fill_host(n);
				hw = e.host_words;
				wrong += !run_ref(&c, n, &err) || err || e.host_words - hw != n ||
					 e.host_got != e.host_need || x.busy || e.busy;
			}
	printf("  host ops: %lu ran, %lu words\n", e.ops, e.host_words);
	T(e.ops == 240, "every host-data case ran");
	T(wrong == 0 && clean(), "MONO/HOSTCOL: exactly the planned words, results match");

	c = (struct wd24x_cmd){ WD24X_MONO, 1, 0, false, WD24X_ROP_SRC, 0x4e000, 0, 13, 5 };
	a = acc();
	T(wd24x_run(&x, &c, host, wd24x_host_words(&c) - 1) == -EINVAL && acc() == a,
	  "one word short: refused, no access");
	T(wd24x_run(&x, &c, host, wd24x_host_words(&c) + 1) == -EINVAL && acc() == a,
	  "one word extra: refused, no access");
	T(wd24x_run(&x, &c, NULL, wd24x_host_words(&c)) == -EINVAL && acc() == a,
	  "no buffer: refused, no access");

	/* the chip wants one more word than computed: it never goes idle */
	setup(5);
	e.host_skew = 1;
	fill_host(wd24x_host_words(&c));
	err = wd24x_run(&x, &c, host, wd24x_host_words(&c));
	T(err == -ETIMEDOUT && x.dead && clean(), "chip wants more words: timeout, dead");
	/* the chip wants one fewer: it is idle before our last word */
	setup(6);
	e.host_skew = -1;
	err = wd24x_run(&x, &c, host, wd24x_host_words(&c));
	T(err == -EIO && x.dead && e.host_got == e.host_need && clean(),
	  "chip wants fewer words: detected idle, no word sent to an idle engine");
}

/* ---- no CPU VRAM access while busy -------------------------------------- */
static void t_cpu(void)
{
	struct wd24x_cmd c = { WD24X_FILL, 7, 0, false, WD24X_ROP_SRC, 0, 0, 640, 480 };
	struct wd24_io vio;
	unsigned long r;
	int err;

	setup(7);
	vm_io(&vio, &m);
	x.async = true;
	T(wd24x_run(&x, &c, NULL, 0) == 0 && x.busy && e.busy, "async fill leaves the engine busy");
	vio.memw(vio.priv, 0, 1);
	T(e.cpu_busy == 1, "model detects CPU VRAM access while busy");
	e.violations = 0;
	e.cpu_busy = 0;
	T(wd24x_cpu_begin(&x) == 0 && !x.busy && !e.busy, "cpu_begin waits for idle");
	vio.memw(vio.priv, 0, 1);
	vio.memr(vio.priv, 0);
	T(e.cpu_busy == 0 && clean(), "no CPU access while busy after cpu_begin");
	T(wd24x_run(&x, &c, NULL, 0) == 0 && wd24x_run(&x, &c, NULL, 0) == 0 && clean(),
	  "back-to-back async ops: each waits idle before programming");
	T(wd24x_cpu_begin(&x) == 0, "and drains");

	setup(8);
	x.async = true;
	e.stuck_busy = true;
	T(wd24x_run(&x, &c, NULL, 0) == 0, "async start on a stuck engine");
	r = e.reads;
	err = wd24x_cpu_begin(&x);
	T(err == -EBUSY && x.dead, "stuck: cpu_begin refuses, acceleration dead");
	T(e.reads - r <= x.spins + 4, "stuck: the wait is bounded");
	T(e.cpu_busy == 0 && clean(), "stuck: no CPU access, no abort written");
}

/* ---- timeout and mismatch disable acceleration -------------------------- */
static void t_dead(void)
{
	struct wd24x_cmd c = { WD24X_COPY, 0, 0, false, WD24X_ROP_SRC, 0x4e000, 0x0, 64, 32 };
	struct wd24x_plan p;
	unsigned long a, w;
	unsigned int i, bad = 0;
	int err;

	setup(9);
	e.stuck_busy = true;
	err = wd24x_run(&x, &c, NULL, 0);
	T(err == -ETIMEDOUT && x.dead && x.ntimeout == 1 && clean(), "stuck busy: timeout, dead, no abort");
	a = acc();
	T(wd24x_run(&x, &c, NULL, 0) == -ENODEV && x.nfallback == 1 && acc() == a,
	  "dead: next op falls back with zero access");
	T(wd24x_cursor(&x, true, 1, 1, 0, 0, 1, 2) == -ENODEV && acc() == a, "dead: cursor too");

	setup(10);
	e.busy = e.stuck_busy = true;	/* busy before we ever started */
	w = e.data_writes;
	T(wd24x_run(&x, &c, NULL, 0) == -ETIMEDOUT && e.data_writes == w && x.dead,
	  "busy at entry: no register written, dead");

	wd24x_plan(&c, &p, NULL);
	for (i = 0; i < p.n; i++) {
		setup(11 + i);
		e.bad_blk = 1;
		e.bad_idx = p.idx[i];
		err = wd24x_run(&x, &c, NULL, 0);
		bad += err != -EIO || !x.dead || e.ops != 0 || x.nmismatch != 1 || !clean();
	}
	T(bad == 0, "readback mismatch on any register: dead, op never started");

	setup(40);
	e.bad_echo = true;
	T(wd24x_run(&x, &c, NULL, 0) == -EIO && x.dead && e.ops == 0, "index echo mismatch: dead");
	setup(41);
	e.bad_ix = true;
	T(wd24x_run(&x, &c, NULL, 0) == -EIO && x.dead, "23C0h restore mismatch: dead");
	setup(42);
	e.bad_blk = 2;
	e.bad_idx = WD24X_C_POS_X;
	T(wd24x_cursor(&x, true, 100, 50, 0, 0, 1, 2) == -EIO && x.dead &&
	  e.reg[2][0] == 0, "cursor readback mismatch: dead, control never written");
}

/* ---- 23C0h restored after every op and every failure -------------------- */
static void t_restore(void)
{
	static const u16 start[] = { 0x0000, 0x1001, 0x0102, 0x1f02, 0x0a01, 0x0003, 0x1fff };
	struct wd24x_cmd f = { WD24X_FILL, 3, 0, false, WD24X_ROP_SRC, 0x4c000, 0, 33, 7 };
	struct wd24x_cmd cp = { WD24X_COPY, 0, 0, false, WD24X_ROP_SRC, 0x4e000, 0x0, 100, 20 };
	struct wd24x_cmd mo = { WD24X_MONO, 9, 1, true, WD24X_ROP_SRC, 0x4e000, 0, 21, 4 };
	unsigned int i, bad = 0;

	for (i = 0; i < sizeof(start) / sizeof(start[0]); i++) {
		u16 s = start[i];

#define ONE(prep, expr, want) do { setup(50 + i); e.ix = s; prep; \
	bad += (expr) != (want) || e.ix != s; } while (0)
		ONE((void)0, wd24x_run(&x, &f, NULL, 0), 0);
		ONE((void)0, wd24x_run(&x, &cp, NULL, 0), 0);
		ONE(fill_host(wd24x_host_words(&mo)), wd24x_run(&x, &mo, host, wd24x_host_words(&mo)), 0);
		ONE((void)0, wd24x_cursor(&x, true, 320, 240, 3, 4, 15, 0), 0);
		ONE((void)0, wd24x_run(&x, &f, NULL, 1), -EINVAL);
		ONE(e.stuck_busy = true, wd24x_run(&x, &f, NULL, 0), -ETIMEDOUT);
		ONE((e.bad_blk = 1, e.bad_idx = WD24X_B_PITCH), wd24x_run(&x, &f, NULL, 0), -EIO);
		ONE(e.host_skew = -1, wd24x_run(&x, &mo, host, wd24x_host_words(&mo)), -EIO);
		ONE((x.async = true, wd24x_run(&x, &f, NULL, 0)), wd24x_cpu_begin(&x), 0);
#undef ONE
	}
	T(bad == 0, "23C0h identical after every op and failure (7 start values x 9 paths)");
}

/* ---- cursor --------------------------------------------------------------- */
static void t_cursor(void)
{
	unsigned int i, last = 0, n = 0;
	unsigned long a;

	setup(60);
	T(wd24x_cursor(&x, true, 639, 479, 31, 31, 0xf0, 0x0f) == 0 && clean(), "cursor on");
	T(e.reg[2][WD24X_C_CTRL] == 0x820 && e.reg[2][WD24X_C_PAT_LO] == 0x400 &&
	  e.reg[2][WD24X_C_PAT_HI] == 0x013 && e.reg[2][WD24X_C_ORIGIN] == 0x7df &&
	  e.reg[2][WD24X_C_POS_X] == 639 && e.reg[2][WD24X_C_POS_Y] == 479 &&
	  e.reg[2][WD24X_C_PRIMARY] == 0xf0 && e.reg[2][WD24X_C_SECONDARY] == 0x0f,
	  "cursor registers as planned");
	for (i = 0; i < x.nev; i++)
		if (x.ev[i].type == WD24X_EV_OUT && x.ev[i].port == WD24X_P_DATA) {
			last = x.ev[i].val;
			n++;
		}
	T(n == 8 && (last >> 12) == WD24X_C_CTRL, "8 data writes, control last");
	T(e.reg[2][WD24X_C_AUX] == 0 && e.ops == 0, "aux colour and BITBLT untouched");
	a = acc();
	T(wd24x_cursor(&x, true, 640, 0, 0, 0, 0, 0) == -EINVAL && acc() == a, "X 640: no access");
	T(wd24x_cursor(&x, true, 0, 480, 0, 0, 0, 0) == -EINVAL && acc() == a, "Y 480: no access");
	T(wd24x_cursor(&x, true, 0, 0, 32, 0, 0, 0) == -EINVAL && acc() == a, "hot X 32: no access");
	T(wd24x_cursor(&x, false, 0, 0, 0, 0, 0, 0) == 0 && e.reg[2][0] == 0, "cursor off");
}

/* ---- every op: start is the last 23C2h write, and is ACT + listed ------- */
static void t_order(void)
{
	struct wd24x_cmd c = { WD24X_FILL, 5, 0, false, WD24X_ROP_XOR, 0x4e000, 0, 80, 9 };
	unsigned int i, starts = 0, after = 0;
	bool seen = false;

	setup(70);
	T(wd24x_run(&x, &c, NULL, 0) == 0 && clean(), "fill ok");
	for (i = 0; i < x.nev; i++) {
		const struct wd24x_event *v = &x.ev[i];

		if (v->type != WD24X_EV_OUT || v->port != WD24X_P_DATA)
			continue;
		if (seen)
			after++;
		if ((v->val >> 12) == 0) {
			starts++;
			seen = true;
			T(v->val == 0x908, "start value is the planned CNTRL1");
		}
	}
	T(starts == 1 && after == 0, "CNTRL1 written once, last");
	T(x.nrefused == 0, "no refusals in a good op");
}

int main(void)
{
	t_gate();
	t_core_guard();
	t_guard();
	t_check();
	t_host();
	t_cpu();
	t_dead();
	t_restore();
	t_cursor();
	t_order();
	printf("test_engine: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
