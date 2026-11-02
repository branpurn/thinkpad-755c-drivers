// SPDX-License-Identifier: GPL-2.0
/* S1 host test: the read-only 23C0h probe (wd24adv_s1.c) against the
 * 23C0h block model in vgamodel.c.  The expected write list is restated
 * here independently of wd24s1_sel(). */
#include <stdio.h>
#include <string.h>
#include "vgamodel.h"
#include "../wd24adv_s1.h"

static int fails, passes;
#define T(cond, what) do { if (cond) passes++; else { fails++; \
	printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

static struct vgamodel m;
static struct vm_eng e;
static struct wd24x_io xio;
static struct wd24s1 s;

/* test io: the 23C0h model plus fault knobs, and an ordered trace */
static struct {
	int ix_override;	/* >= 0: every 23C0h read returns this */
	int second_read;	/* >= 0: 2nd 23C0h read returns this */
	u16 ar_word;		/* 3C1h:3C0h */
	unsigned int nr23;
	struct { char w; u16 port, val; } tr[256];
	unsigned int ntr, other;
} k;

static void tr(char w, u16 port, u16 val)
{
	if (k.ntr < 256)
		k.tr[k.ntr++] = (typeof(k.tr[0])){ w, port, val };
}

static u16 t_inw(void *p __attribute__((unused)), u16 port)
{
	u16 v = xio.inw(xio.priv, port);

	if (port == WD24S1_P_INDEX) {
		k.nr23++;
		if (k.ix_override >= 0)
			v = (u16)k.ix_override;
		if (k.nr23 == 2 && k.second_read >= 0)
			v = (u16)k.second_read;
	}
	if (port != WD24S1_P_INDEX && port != WD24S1_P_DATA)
		k.other++;
	tr('r', port, v);
	return v;
}

static void t_outw(void *p __attribute__((unused)), u16 v, u16 port)
{
	if (port != WD24S1_P_INDEX)
		k.other++;
	tr('W', port, v);
	xio.outw(xio.priv, v, port);
}

static u8 t_inb(void *p __attribute__((unused)), u16 port)
{
	if (port != 0x3c0 && port != 0x3c1)
		k.other++;
	tr('b', port, 0);
	return port == 0x3c0 ? k.ar_word & 0xff : k.ar_word >> 8;
}

static const struct wd24s1_io tio = { t_inw, t_outw, t_inb, NULL };

static void setup(u16 s0)
{
	vm_init(&m, VM_WD24, 1);
	vm_eng_init(&e, 1);
	m.eng = &e;
	vm_xio(&xio, &m);
	e.ix = s0;
	memset(&k, 0, sizeof(k));
	k.ix_override = k.second_read = -1;
	k.ar_word = 0x0020;	/* AR index 20h, as vgacon leaves it */
	/* block 1 / block 2 register contents (12 bit) */
	for (unsigned int i = 0; i < 16; i++) {
		e.reg[1][i] = (u16)(0x100 + i * 7) & 0x7ff;
		e.reg[2][i] = (u16)(0x200 + i * 5);
	}
	wd24s1_init(&s, &tio);
}

static unsigned int nwrites(void)
{
	unsigned int i, n = 0;

	for (i = 0; i < k.ntr; i++)
		n += k.tr[i].w == 'W';
	return n;
}

/* the spec: block 1 idx 0..F, block 2 idx 0..8, NOINC, block in 7:0 */
static const u16 spec_sel[25] = {
	0x1001, 0x1101, 0x1201, 0x1301, 0x1401, 0x1501, 0x1601, 0x1701,
	0x1801, 0x1901, 0x1a01, 0x1b01, 0x1c01, 0x1d01, 0x1e01, 0x1f01,
	0x1002, 0x1102, 0x1202, 0x1302, 0x1402, 0x1502, 0x1602, 0x1702,
	0x1802,
};

static void t_sequence(u16 s0)
{
	unsigned int i, p, n = 0, bad = 0;
	char what[96];

	setup(s0);
	T(wd24s1_run(&s) == 0 && s.res == WD24S1_OK, "run OK");
	T(nwrites() == 51 && s.writes == 51, "exactly 51 writes");
	/* trace: r23C0 r23C0 b3C0 b3C1, then per select W r23C0 r23C2, then W r */
	T(k.ntr == 4 + 50 * 3 + 2, "exact access count (4 + 150 + 2)");
	T(k.tr[0].w == 'r' && k.tr[0].port == 0x23c0 && k.tr[1].w == 'r' && k.tr[1].port == 0x23c0 &&
	  k.tr[2].w == 'b' && k.tr[2].port == 0x3c0 && k.tr[3].w == 'b' && k.tr[3].port == 0x3c1,
	  "prologue: two 23C0h reads, 3C0h, 3C1h; no write");
	for (p = 0; p < 2; p++)
		for (i = 0; i < 25; i++, n++) {
			unsigned int t = 4 + n * 3;

			bad += !(k.tr[t].w == 'W' && k.tr[t].port == 0x23c0 && k.tr[t].val == spec_sel[i]);
			bad += !(k.tr[t + 1].w == 'r' && k.tr[t + 1].port == 0x23c0 && k.tr[t + 1].val == spec_sel[i]);
			bad += !(k.tr[t + 2].w == 'r' && k.tr[t + 2].port == 0x23c2);
			bad += wd24s1_sel(i) != spec_sel[i];
		}
	T(bad == 0, "writes 1..50: the 25 selects twice, in spec order, each read back then 23C2h read");
	snprintf(what, sizeof(what), "write 51: restore %04x then 23C0h read", s0 & 0x1fff);
	T(k.tr[154].w == 'W' && k.tr[154].port == 0x23c0 && k.tr[154].val == (s0 & 0x1fff) &&
	  k.tr[155].w == 'r' && k.tr[155].port == 0x23c0, what);
	T(s.restored && s.restore_rb == s0 && e.ix == (s0 & 0x1fff), "restore read back S, model index = S");
	T(k.other == 0 && e.data_writes == 0 && e.host_words == 0 && e.ops == 0,
	  "no 23C2h/23C4h write, no 23C4h access, no op started");
	T(e.violations == 0 && m.violations == 0, "model: no violation");
	bad = 0;
	for (i = 0; i < 25; i++) {
		u16 blk = spec_sel[i] & 0xff, idx = (spec_sel[i] >> 8) & 0xf;

		bad += s.val[0][i] != (u16)((idx << 12) | e.reg[blk][idx]);
		bad += s.val[0][i] != s.val[1][i];
	}
	T(bad == 0, "recorded values = (idx << 12) | register, both passes");
	T(s.nlog == k.ntr, "module access log = actual trace");
}

static void t_refuse(void)
{
	/* each: zero writes, no index change */
	setup(0);
	k.second_read = 0x0001;
	T(wd24s1_run(&s) == -ENODEV && s.res == WD24S1_REFUSED && nwrites() == 0, "unstable S refused, 0 writes");
	setup(0);
	k.ix_override = 0xffff;
	T(wd24s1_run(&s) == -ENODEV && s.res == WD24S1_REFUSED && nwrites() == 0, "FFFF refused, 0 writes");
	setup(0);
	k.ix_override = 0x4000;
	T(wd24s1_run(&s) == -ENODEV && nwrites() == 0, "reserved b14 refused, 0 writes");
	setup(0);
	k.ix_override = 0x8001;
	T(wd24s1_run(&s) == -ENODEV && nwrites() == 0, "reserved b15 refused, 0 writes");
	setup(0x0001);
	k.ar_word = 0x0001;
	T(wd24s1_run(&s) == -ENODEV && nwrites() == 0 && s.why && strstr(s.why, "alias"),
	  "23C0h == 3C1h:3C0h (alias) refused, 0 writes");
	setup(0);
	T(wd24s1_run(&s) == 0 && wd24s1_run(&s) == -EBUSY && nwrites() == 51, "second run refused");
	T(e.violations == 0, "model clean");
}

static void t_stops(void)
{
	/* each stop: restore written at once, nothing after it */
	setup(0);
	e.bad_ix = true;
	T(wd24s1_run(&s) == -EIO && nwrites() == 2, "bad 23C0h readback: 1 select + restore");
	T(k.tr[k.ntr - 2].w == 'W' && k.tr[k.ntr - 2].val == (s.saved & 0x1fff) && s.restored,
	  "bad readback: restore is the last write");
	/* model: bad_ix also corrupts the restore readback */
	T(s.res == WD24S1_RESTORE_BAD, "bad_ix: restore readback differs -> RESTORE MISMATCH");

	setup(0);
	e.bad_echo = true;
	T(wd24s1_run(&s) == -EIO && s.res == WD24S1_STOPPED && nwrites() == 2 && s.restored &&
	  s.restore_rb == 0, "bad echo: stop after select 1, restored");
	setup(0);
	e.bad_echo = true;
	s.strict_echo = false;
	T(wd24s1_run(&s) == 0 && nwrites() == 51, "s1_echo=0: echo recorded only, full run");

	setup(0);
	e.busy = e.stuck_busy = true;
	T(wd24s1_run(&s) == -EIO && s.res == WD24S1_STOPPED && nwrites() == 2 &&
	  strstr(s.why, "ACT"), "busy at rest: stop after CNTRL1, restored");
	T(e.data_writes == 0, "busy: CNTRL1 never written");

}

/* pass mismatch: a register whose value changes on its second read */
static unsigned int flip_reads;
static u16 f_inw(void *p, u16 port)
{
	u16 v = t_inw(p, port);

	if (port == WD24S1_P_DATA && (e.ix & 0xff) == 2 && ((e.ix >> 8) & 0xf) == 5 && flip_reads++)
		v ^= 0x0004;
	return v;
}

static void t_unstable(void)
{
	static const struct wd24s1_io fio = { f_inw, t_outw, t_inb, NULL };

	setup(0);
	flip_reads = 0;
	wd24s1_init(&s, &fio);
	T(wd24s1_run(&s) == -EIO && s.res == WD24S1_UNSTABLE && s.stop_sel == 21 && nwrites() == 51 &&
	  s.restored && s.restore_rb == 0, "pass 1 != pass 2: UNSTABLE, both passes + restore");

}

static void t_guard(void)
{
	unsigned long ok = 0, ok_pre = 0, bad = 0;
	unsigned int i;
	u32 v, p;

	setup(0x0abc & 0x1fff);
	/* before S is saved: every write refused */
	for (v = 0; v <= 0xffff; v++)
		ok_pre += wd24s1_guard_out(&s, 0x23c0, v) == 0;
	T(ok_pre == 0, "guard: no write before S is read");
	s.have_saved = true;
	s.saved = 0x0abc;
	for (v = 0; v <= 0xffff; v++) {
		bool want = v == 0x0abc;

		for (i = 0; i < 25; i++)
			want |= v == spec_sel[i];
		ok += wd24s1_guard_out(&s, 0x23c0, v) == 0;
		bad += (wd24s1_guard_out(&s, 0x23c0, v) == 0) != want;
	}
	T(bad == 0 && ok == 26, "guard 23C0h: exactly the 25 selects + S & 1FFF");
	s.saved = 0xe001;	/* restore value masks 15:13 */
	T(wd24s1_guard_out(&s, 0x23c0, 0x0001) == 0 && wd24s1_guard_out(&s, 0x23c0, 0xe001) == -EPERM,
	  "guard: restore value is S & 1FFF");
	s.saved = 0x0abc;
	bad = 0;
	for (p = 0; p <= 0xffff; p++) {
		if (p == 0x23c0)
			continue;
		for (i = 0; i < 25; i++)
			bad += wd24s1_guard_out(&s, p, spec_sel[i]) == 0;
		bad += wd24s1_guard_out(&s, p, 0x0abc) == 0;
		bad += wd24s1_guard_out(&s, p, 0) == 0;
		bad += wd24s1_guard_out(&s, p, 0x0800) == 0;
	}
	T(bad == 0, "guard: every other port refused (23C2h data, 23C4h host, VGA)");
	for (v = 0, bad = 0; v <= 0xffff; v++)
		bad += wd24s1_guard_out(&s, 0x23c2, v) == 0 || wd24s1_guard_out(&s, 0x23c4, v) == 0;
	T(bad == 0, "guard: no 23C2h or 23C4h write of any value");
	for (p = 0, bad = 0, ok = 0; p <= 0xffff; p++) {
		ok += wd24s1_guard_in(p, 16) == 0;
		ok += wd24s1_guard_in(p, 8) == 0;
	}
	T(ok == 4 && wd24s1_guard_in(0x23c0, 16) == 0 && wd24s1_guard_in(0x23c2, 16) == 0 &&
	  wd24s1_guard_in(0x3c0, 8) == 0 && wd24s1_guard_in(0x3c1, 8) == 0 &&
	  wd24s1_guard_in(0x23c4, 16) == -EPERM, "guard reads: 23C0h, 23C2h, 3C0h, 3C1h only (never 23C4h)");
	s.restored = true;
	for (v = 0, ok = 0; v <= 0xffff; v++)
		ok += wd24s1_guard_out(&s, 0x23c0, v) == 0;
	T(ok == 0, "guard: nothing after the restore");
	s.restored = false;
	s.writes = WD24S1_MAX_WRITES;
	T(wd24s1_guard_out(&s, 0x23c0, 0x1001) == -EPERM, "guard: write cap 51");
}

/* the read-only allowance: S1 may SELECT registers S0 must never write
 * (block 1 C, D, F, block 2 aux colour) but writes no data to any of them;
 * the S0 engine guard is unchanged and still refuses those selects */
static void t_readonly_allowance(void)
{
	static const u16 ro[] = { 0x1c01, 0x1d01, 0x1f01, 0x1802 };
	struct wd24x x;
	unsigned int i, bad = 0;
	u32 v;

	setup(0);
	s.have_saved = true;
	for (i = 0; i < 4; i++) {
		bad += wd24s1_guard_out(&s, 0x23c0, ro[i]) != 0;
		for (v = 0; v <= 0xffff; v++)
			bad += wd24s1_guard_out(&s, 0x23c2, v) == 0;
	}
	T(bad == 0, "S1: C, D, F, aux selects allowed; no data write to any of them");
	wd24x_init(&x, &xio);
	x.enabled = true;
	for (i = 0, bad = 0; i < 4; i++)
		bad += wd24x_guard(&x, WD24X_P_INDEX, ro[i]) != -EPERM;
	T(bad == 0, "S0 engine guard still refuses the C, D, F, aux selects");
	setup(0);
	T(wd24s1_run(&s) == 0, "full run");
	for (i = 0, bad = 0; i < 4; i++)
		bad += e.reg[ro[i] & 0xff][(ro[i] >> 8) & 0xf] !=
		       (u16)((ro[i] & 0xff) == 1 ? (0x100 + ((ro[i] >> 8) & 0xf) * 7) & 0x7ff
						: 0x200 + 8 * 5);
	T(bad == 0 && e.data_writes == 0, "full run: C, D, F, aux registers unchanged");
}

/* block 2 missing (23C0h b13 after 1002h): stop at select 17, restored */
static u16 nb2_inw(void *p, u16 port)
{
	u16 v = t_inw(p, port);

	if (port == WD24S1_P_INDEX && (e.ix & 0xff) == 2)
		v |= WD24S1_INVALID;
	return v;
}

static void t_no_block2(void)
{
	static const struct wd24s1_io nio = { nb2_inw, t_outw, t_inb, NULL };

	setup(0);
	wd24s1_init(&s, &nio);
	T(wd24s1_run(&s) == -EIO && s.res == WD24S1_STOPPED && s.stop_sel == 16 && nwrites() == 18 &&
	  strstr(s.why, "b13") && s.restored && s.restore_rb == 0,
	  "block 2 invalid: stop at 1002h (17 selects + restore = 18 writes)");
}


/* The entry check s1_live() runs first, against the 755C model (QEMU
 * stops it at CR34): the exact VGA writes it makes there, all index
 * writes, every touched index put back, no data writes. */
static struct wd24_io ent_io, ent_inner;
static struct { u16 port; u8 val; } ent_w[64];
static unsigned int ent_n;

static u8 ent_inb(void *p __attribute__((unused)), u16 port)
{
	return ent_inner.inb(ent_inner.priv, port);
}

static void ent_outb(void *p __attribute__((unused)), u8 val, u16 port)
{
	if (ent_n < 64)
		ent_w[ent_n] = (typeof(ent_w[0])){ port, val };
	ent_n++;
	ent_inner.outb(ent_inner.priv, val, port);
}

static void t_entry_writes(void)
{
	static struct vgamodel before;
	static u8 planes[4 * WD24_PLANE];
	struct wd24_ctx c;
	unsigned int i;
	int err;

	vm_init(&m, VM_WD24, 3);
	before = m;
	vm_io(&ent_inner, &m);
	ent_io = ent_inner;
	ent_io.inb = ent_inb;
	ent_io.outb = ent_outb;
	ent_n = 0;
	wd24_ctx_init(&c, &wd24_port_backend);
	c.io = &ent_io;
	c.planes = planes;
	c.be->begin(&c);
	err = wd24_entry_check(&c, WD24_PH_LOAD);
	c.be->end(&c);
	T(err == 0, "entry check passes on the 755C model");
	T(m.data_writes == 0, "entry check: zero data writes");
	T(m.violations == 0, "entry check: no violations");
	T(ent_n == m.index_writes, "entry check: every write is an index write");
	T(vm_cmp(&m, &before, "entry check") == 0, "entry check: registers, DAC, VRAM unchanged");
	T(m.sri == before.sri && m.gri == before.gri && m.cri == before.cri,
	  "entry check: SR/GR/CR index put back");
	printf("  entry check on the 755C model: %u VGA writes:", ent_n);
	for (i = 0; i < ent_n && i < 64; i++)
		printf(" %03X<-%02X", ent_w[i].port, ent_w[i].val);
	printf("\n");
}

int main(void)
{
	t_entry_writes();
	t_sequence(0x0000);
	t_sequence(0x1001);	/* wd90c24fb left NOINC|BLT */
	t_sequence(0x0f02);
	t_refuse();
	t_stops();
	t_unstable();
	t_guard();
	t_readonly_allowance();
	t_no_block2();
	printf("test_s1: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
