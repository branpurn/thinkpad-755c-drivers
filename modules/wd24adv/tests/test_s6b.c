/* host tests: S6b dual-window session + PR31 b7 copy (wd24adv_s6b.c) on
 * the 86Box-semantics S6 model; cross-bank memmove through the split
 * windows, cross-bank / overlapping COPY and frame scroll against memmove,
 * the lock discipline during data moves, teardown, fault injection */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../wd24adv_s6b.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u8 vram[0x100000], ref[0x100000];
static struct wd24s6b B;
static struct wd24s6_model m;
static struct wd24s6_io io;
static u32 bad_lock, bad_state, nmove, nunlock;

/* wrap the model: every VRAM access must happen with SR06 LOCKED, and
 * either split (PR1 b3, b7 off) or rw (PR1 b3 off, b7 on), never both */
static int st_ok(void)
{
	int b3 = !!(m.gr[0x0b] & 0x08), b7 = !!(m.sr[0x11] & 0x80);

	return b3 != b7;
}
static void t_mr(void *p, u32 off, u8 *buf, u32 n)
{
	bad_lock += wd24s6_model_io.unlocked(p) != 0;
	bad_state += !st_ok();
	nmove++;
	wd24s6_model_io.mr(p, off, buf, n);
}
static void t_mw(void *p, u32 off, const u8 *buf, u32 n)
{
	bad_lock += wd24s6_model_io.unlocked(p) != 0;
	bad_state += !st_ok();
	nmove++;
	wd24s6_model_io.mw(p, off, buf, n);
}
static int t_wr(void *p, enum wd24_rf rf, u8 idx, u8 v)
{
	if (rf == WD24_SR && idx == 0x06 && v != 0x0f)
		nunlock++;
	return wd24s6_model_io.wr(p, rf, idx, v);
}

static void init(void)
{
	wd24s6_model_init(&m, vram);
	io = wd24s6_model_io;
	io.mr = t_mr;
	io.mw = t_mw;
	io.wr = t_wr;
	io.priv = &m;
	memcpy(ref, vram, sizeof(ref));
	bad_lock = bad_state = nmove = nunlock = 0;
}

static int steady(void)
{
	return m.gr[0x0b] == 0xcd && m.sr[0x11] == 0x5d && m.sr[6] == 0x0f &&
	       m.gr[0x0a] == B.w0 && m.gr[0x09] == B.w1 - 8 && !m.lock_open && m.open;
}

static int restored(void)
{
	return m.gr[0x0b] == 0xc5 && m.sr[0x11] == 0x5d && m.sr[6] == 0x0f &&
	       m.gr[0x09] == 0 && m.gr[0x0a] == 0 && !m.lock_open && !m.open;
}

/* userspace view of the mmap: 64 KiB, A0000 half = w0, A8000 half = w1 */
static void u_rd(u32 off, u8 *d, u32 n) { wd24s6_model_io.mr(&m, off, d, n); }
static void u_wr(u32 off, const u8 *d, u32 n) { wd24s6_model_io.mw(&m, off, d, n); }

/* the userspace helper (tools/wd24s6b-test.c does the same): cross-bank
 * memmove src -> dst (disjoint banks) with w0 on the source and w1 on the
 * destination, both mapped at once; 4 KiB granular window placement */
static int u_memmove(u32 dst, u32 src, u32 len)
{
	static u8 tmp[0x8000];
	u32 done = 0;

	while (done < len) {
		u32 so = src + done, dop = dst + done;
		u32 w0 = so >> 12, w1 = dop >> 12;
		u32 n = len - done, lim0 = (w0 + 8) * 0x1000 - so, lim1 = (w1 + 8) * 0x1000 - dop;

		if (n > lim0) n = lim0;
		if (n > lim1) n = lim1;
		if (wd24s6b_setwin(&B, w0, w1))
			return -1;
		/* one CPU pass: memcpy(map + 8000h + (dop - w1*4K), map + (so - w0*4K)) */
		u_rd(so - w0 * 0x1000, tmp, n);
		u_wr(0x8000 + dop - w1 * 0x1000, tmp, n);
		done += n;
	}
	return 0;
}

static void copy_case(u32 dst, u32 src, u32 len, const char *n)
{
	int r;

	memmove(ref + dst, ref + src, len);
	r = wd24s6b_copy(&B, dst, src, len);
	CHECK(r == 0 && B.state == WD24S6B_ON, "%s: copy rc %d %s", n, r, B.why ? B.why : "");
	CHECK(!memcmp(vram, ref, 0x100000), "%s: VRAM == memmove reference", n);
	CHECK(steady(), "%s: back to steady state (PR1 %02x PR31 %02x SR06 %02x)", n, m.gr[0x0b],
	      m.sr[0x11], m.sr[6]);
}

static u32 rnd(void)
{
	static u32 x = 12345;

	x = x * 1103515245 + 12345;
	return x >> 8;
}

int main(void)
{
	u32 i, u0;
	int r;

	/* begin + windows */
	init();
	r = wd24s6b_begin(&B, &io);
	CHECK(r == 0 && B.state == WD24S6B_ON && steady() && nunlock == 0,
	      "begin: ON, PR1 cd, SR06 locked, no unlock (rc %d)", r);
	CHECK(B.nwr == 1 && !memcmp(vram, ref, 0x100000), "begin: 1 write (PR1), VRAM untouched (%u)", B.nwr);
	CHECK(wd24s6b_setwin(&B, 0, 4) == -22, "setwin: overlapping halves refused");
	CHECK(wd24s6b_setwin(&B, 0, 7) == -22, "setwin: w1 < 8 refused (PR0A < 0)");
	CHECK(wd24s6b_setwin(&B, 0x79, 0x10) == -22, "setwin: w0 > 78h refused");
	CHECK(wd24s6b_setwin(&B, 0x10, 0x79) == -22, "setwin: w1 > 78h refused");
	CHECK(B.state == WD24S6B_ON && steady(), "setwin refusals: no write, still ON");
	CHECK(wd24s6b_setwin(&B, 0x50, 0x70) == 0 && steady(), "setwin 50/70 ok");
	{
		u8 a[16], c[16];

		u_rd(0x0123, a, 16);
		u_rd(0x8123, c, 16);
		CHECK(!memcmp(a, vram + 0x50123, 16) && !memcmp(c, vram + 0x70123, 16),
		      "split map: A0000 -> 50000h, A8000 -> 70000h");
	}

	/* userspace cross-bank memmove: every pass maps both banks at once */
	u0 = B.nset;
	memmove(ref + 0x70456, ref + 0x4c123, 0x9000);
	CHECK(u_memmove(0x70456, 0x4c123, 0x9000) == 0, "user memmove ran");
	CHECK(!memcmp(vram, ref, 0x100000), "user cross-bank memmove 4C123h -> 70456h (9000h) == memmove");
	CHECK(B.nset - u0 <= 3, "user memmove: %u SETWIN for 36 KiB (no fault, no livelock)", B.nset - u0);
	CHECK(bad_lock == 0 && bad_state == 0, "user memmove: SR06 locked, split state");

	/* kernel COPY: cross-bank, unaligned, both directions, overlap */
	nunlock = 0;
	copy_case(0x62456, 0x4c123, 0x12000, "cross-bank forward unaligned");
	CHECK(nunlock == 2, "one COPY = exactly 2 SR06 unlocks (%u)", nunlock);
	copy_case(0x4c000, 0x4c001, 0x8fff, "overlap dst<src by 1");
	copy_case(0x4c001, 0x4c000, 0x8fff, "overlap dst>src by 1 (backward)");
	copy_case(0x50000, 0x5f000, 1, "1 byte");
	copy_case(0x50fff, 0x7e001, 0x1001, "4097 bytes across page edges");
	copy_case(0x7f000, 0x00000, 0x1000, "top page");
	/* frame scroll: up 16 lines (overlap, forward), down 16 lines (backward) */
	copy_case(0, 640 * 16, WD24S6B_FRAME - 640 * 16, "scroll up 16 lines (frame)");
	copy_case(640 * 16, 0, WD24S6B_FRAME - 640 * 16, "scroll down 16 lines (frame, backward)");
	copy_case(0, 640, WD24S6B_FRAME - 640, "scroll up 1 line");
	for (i = 0; i < 200; i++) {
		u32 len = 1 + rnd() % 0x20000;
		u32 s = rnd() % (WD24S6B_VRAM - len + 1), d = rnd() % (WD24S6B_VRAM - len + 1);
		char n[64];

		if (i % 3 == 0)	/* near overlap */
			d = s + (rnd() % 0x3000) <= WD24S6B_VRAM - len ? s + rnd() % 0x3000 : s;
		snprintf(n, sizeof(n), "random %u %05x->%05x len %x", i, s, d, len);
		memmove(ref + d, ref + s, len);
		r = wd24s6b_copy(&B, d, s, len);
		if (r || memcmp(vram, ref, 0x100000) || !steady()) {
			CHECK(0, "%s", n);
			break;
		}
	}
	CHECK(i == 200, "200 random COPY == memmove, steady after each");
	CHECK(bad_lock == 0 && bad_state == 0, "all data moves: SR06 LOCKED, split xor rw (lock %u state %u)",
	      bad_lock, bad_state);
	CHECK(wd24s6b_copy(&B, 0x7ffff, 0, 2) == -22 && wd24s6b_copy(&B, 0, 0, 0) == -22 &&
	      wd24s6b_copy(&B, 0, 0xffffffffu, 2) == -22 && B.state == WD24S6B_ON,
	      "COPY range refusals, no write, still ON");
	r = wd24s6b_end(&B);
	CHECK(r == 0 && B.state == WD24S6B_OFF && restored(), "end: PR1 c5 PR0A/B 0 PR31 5d SR06 locked, guard closed");

	/* refusals at begin: no write */
	init();
	m.gr[0x0b] = 0xcd;
	CHECK(wd24s6b_begin(&B, &io) == -19 && B.nwr == 0 && !m.open, "begin refused: PR1 b3 already set");
	init();
	m.sr[6] = 0x4f;
	CHECK(wd24s6b_begin(&B, &io) == -19 && B.nwr == 0, "begin refused: SR06 unlocked at entry");
	init();
	m.gr[0x09] = 3;
	CHECK(wd24s6b_begin(&B, &io) == -19 && B.nwr == 0, "begin refused: PR0A not 0");

	/* faults */
	init();
	m.pr1_stuck = 1;
	CHECK(wd24s6b_begin(&B, &io) == -5 && B.state == WD24S6B_DEAD, "PR1 stuck: begin DEAD");
	wd24s6b_end(&B);
	CHECK(!m.open && m.sr[6] == 0x0f && m.sr[0x11] == 0x5d, "PR1 stuck: closed, locked");

	init();
	wd24s6b_begin(&B, &io);
	m.relock_clears_b7 = 1;
	r = wd24s6b_copy(&B, 0x60000, 0x50000, 0x100);
	CHECK(r == -5 && B.state == WD24S6B_DEAD && B.why && strstr(B.why, "not held"),
	      "relock clears b7: DEAD, detected at re-unlock (%s)", B.why ? B.why : "");
	CHECK(wd24s6b_copy(&B, 0x60000, 0x50000, 0x100) == -19 && wd24s6b_setwin(&B, 0x10, 0x20) == -19,
	      "DEAD: COPY/SETWIN refused");
	wd24s6b_end(&B);
	CHECK(restored(), "relock clears b7: restored");

	init();
	wd24s6b_begin(&B, &io);
	m.lock_ignore = 1;
	r = wd24s6b_copy(&B, 0x60000, 0x50000, 0x100);
	CHECK(r == -5 && B.state == WD24S6B_DEAD && m.sr[0x11] == 0x5d && m.gr[0x0b] == 0xc5,
	      "SR06 ignored: COPY DEAD before b7, PR1 back");
	CHECK(!memcmp(vram, ref, 0x100000), "SR06 ignored: no VRAM write");
	wd24s6b_end(&B);

	init();
	wd24s6b_begin(&B, &io);
	m.relock_stuck = 1;
	r = wd24s6b_copy(&B, 0x60000, 0x50000, 0x100);
	CHECK(r == -5 && B.state == WD24S6B_DEAD && !memcmp(vram, ref, 0x100000),
	      "relock stuck: DEAD before any VRAM access");
	CHECK(wd24s6b_end(&B) == -5 && B.why, "relock stuck: end reports RESTORE FAIL");

	init();
	wd24s6b_begin(&B, &io);
	m.reunlock_fail = 1;	/* model: 3rd unlock on refused; nunl=1 makes it the COPY re-unlock */
	m.nunl = 1;
	r = wd24s6b_copy(&B, 0x60000, 0x50000, 0x100);
	CHECK(r == -5 && B.state == WD24S6B_DEAD && B.b7, "re-unlock fails: DEAD, b7 MAY BE SET");
	CHECK(wd24s6b_end(&B) == -5 && m.sr[6] == 0x0f && !m.open, "re-unlock fails: RESTORE FAIL, relocked, closed");

	init();
	m.sr[0x11] = 0xdd;	/* b7 already set (unlocked read) */
	wd24s6b_begin(&B, &io);
	r = wd24s6b_copy(&B, 0x60000, 0x50000, 0x100);
	CHECK(r == -5 && B.state == WD24S6B_DEAD && m.sr[6] == 0x0f && !m.lock_open,
	      "PR31 b7 set before COPY: DEAD, relocked");
	wd24s6b_end(&B);

	init();
	wd24s6b_begin(&B, &io);
	m.ignore_pr31b7 = 1;	/* register reads back but has no effect */
	memmove(ref + 0x60000, ref + 0x50000, 0x100);
	r = wd24s6b_copy(&B, 0x60000, 0x50000, 0x100);
	CHECK(r == 0 && memcmp(vram, ref, 0x100000),
	      "b7 without effect: NOT detected by registers (live data check needed)");
	wd24s6b_end(&B);

	printf("test_s6b: %d checks, %d failures\n", checks, fails);
	return fails != 0;
}
