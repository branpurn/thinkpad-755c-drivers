/* Host contract for the S6d fault plan and the stop/cleanup order.
 * No kernel, no ports. The live mmap tool uses the same touch list. */
#include <stdio.h>
#include <string.h>
#include "../wd24adv_s6d_fault.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u8 vram[WD24S6D_FRAME_BYTES];
static u32 mapped = ~0u;
static u32 hw_base = ~0u;
static u8 pte[WD24S6D_FRAME_BYTES / WD24S6D_PAGE];

static void clear_ptes(u32 base, u32 pages)
{
	u32 i;

	for (i = 0; i < pages; i++) {
		u32 pg = base + i;

		if (pg < (WD24S6D_FRAME_BYTES / WD24S6D_PAGE))
			pte[pg] = 0;
	}
}

static void set_ptes(u32 base, u32 pages)
{
	u32 i;

	for (i = 0; i < pages; i++) {
		u32 pg = base + i;

		if (pg < (WD24S6D_FRAME_BYTES / WD24S6D_PAGE))
			pte[pg] = 1;
	}
}

/* Apply a plan the way wd24s6d_map_win does. Refused plans leave the window. */
static int fault_at(struct wd24s6d_thrash *t, u32 off, u32 *nf, u32 *ns, u32 *nt)
{
	struct wd24s6d_mapreq q;
	struct wd24s6d_mapact a;
	u32 saved = mapped;

	q.off = off;
	q.mapped_base = mapped;
	q.mmap_len = WD24S6D_FRAME_BYTES;
	q.vma_len = WD24S6D_FRAME_BYTES;
	wd24s6d_plan_fault(t, &q, nf, nt, &a);
	if (a.err) {
		CHECK(!a.setwin && !a.insert, "refused plan off %u still wants hardware", off);
		CHECK(mapped == saved, "refused plan moved the window");
		(void)ns;
		return a.err;
	}
	if (a.zap)
		clear_ptes(a.zap_base, a.zap_pages);
	if (a.setwin) {
		hw_base = a.base;
		if (ns)
			(*ns)++;
	}
	if (a.insert)
		set_ptes(a.base, a.npages);
	mapped = a.base;
	return 0;
}

static int pte_on(u32 off)
{
	return pte[off / WD24S6D_PAGE];
}

static void store(u32 off, u8 val)
{
	CHECK(off < WD24S6D_FRAME_BYTES, "store past frame");
	CHECK(pte_on(off), "store off %u has no PTE", off);
	CHECK(wd24s6d_win_base_pg(off) == hw_base, "store off %u not in hw window", off);
	vram[off] = val;
}

static u8 load(u32 off)
{
	CHECK(pte_on(off), "load off %u has no PTE", off);
	CHECK(wd24s6d_win_base_pg(off) == hw_base, "load off %u not in hw window", off);
	return vram[off];
}

static void write_stamp(unsigned i)
{
	u32 base = wd24s6d_stamp_off(i);
	u8 pix = wd24s6d_stamp_pix(i);
	u32 row, col;

	for (row = 0; row < WD24S6D_STAMP; row++)
		for (col = 0; col < WD24S6D_STAMP; col++)
			store(base + row * 640u + col, pix);
}

static void check_stamp(unsigned i)
{
	u32 base = wd24s6d_stamp_off(i);
	u8 pix = wd24s6d_stamp_pix(i);
	u32 row, col;

	for (row = 0; row < WD24S6D_STAMP; row++)
		for (col = 0; col < WD24S6D_STAMP; col++) {
			u32 off = base + row * 640u + col;

			CHECK(load(off) == pix, "stamp %u off %u got %u want %u",
			      i, off, vram[off], pix);
		}
}

static void test_probe(void)
{
	struct wd24s6d_thrash t;
	u32 nf = 0, ns = 0, nt = 0, i;
	struct wd24s6d_mapreq q;
	struct wd24s6d_mapact a;

	memset(vram, 0, sizeof(vram));
	memset(pte, 0, sizeof(pte));
	mapped = ~0u;
	hw_base = ~0u;
	wd24s6d_thrash_init(&t, 0);

	q.off = 0x40000u;
	q.mapped_base = ~0u;
	q.mmap_len = WD24S6D_FRAME_BYTES;
	q.vma_len = WD24S6D_FRAME_BYTES;
	wd24s6d_plan_fault(&t, &q, NULL, NULL, &a);
	CHECK(a.err == 0 && a.npages == 11, "last window pages %u", a.npages);
	wd24s6d_thrash_reset(&t);

	for (i = 0; i < WD24S6D_TOUCH_N; i++) {
		u32 off = wd24s6d_touch_off(i);

		/* A present PTE in the installed window does not fault. */
		if (!(pte_on(off) && wd24s6d_win_base_pg(off) == hw_base))
			CHECK(fault_at(&t, off, &nf, &ns, &nt) == 0, "touch %u fault", i);
		if (i < WD24S6D_STAMP_N)
			write_stamp(i);
		if (off == 0x4afffu) {
			store(off, 0x3c);
			CHECK(load(off) == 0x3c, "tail readback");
		}
	}
	CHECK(nf == WD24S6D_PROBE_FAULTS, "faults %u", nf);
	CHECK(ns == WD24S6D_PROBE_SWITCHES, "switches %u", ns);
	CHECK(nt == 0 && !t.latched, "probe latched");
	/* Same read-back the live tool does after the touches. Present PTEs
	 * do not fault; a bank that was zapped does. */
	for (i = 0; i < WD24S6D_STAMP_N; i++) {
		u32 off = wd24s6d_stamp_off(i);

		if (!(pte_on(off) && wd24s6d_win_base_pg(off) == hw_base))
			CHECK(fault_at(&t, off, &nf, &ns, &nt) == 0, "reread %u", i);
		check_stamp(i);
	}
	if (!(pte_on(0x4afffu) && wd24s6d_win_base_pg(0x4afffu) == hw_base))
		CHECK(fault_at(&t, 0x4afffu, &nf, &ns, &nt) == 0, "reread tail");
	CHECK(load(0x4afffu) == 0x3c, "tail survived");
	CHECK(nf == 10 && ns == 10 && nt == 0 && !t.latched,
	      "tool totals faults %u switches %u", nf, ns);
}

static void test_thrash_stops_hardware(void)
{
	struct wd24s6d_thrash t;
	u32 nf = 0, ns = 0, nt = 0, i;
	u32 base_before;

	mapped = ~0u;
	hw_base = ~0u;
	memset(pte, 0, sizeof(pte));
	wd24s6d_thrash_init(&t, 4);
	CHECK(fault_at(&t, 0, &nf, &ns, &nt) == 0, "seed");
	for (i = 0; i < 3; i++) {
		CHECK(fault_at(&t, (i & 1) ? 0 : 0x10000u, &nf, &ns, &nt) == 0,
		      "alt %u", i);
	}
	base_before = hw_base;
	CHECK(fault_at(&t, 0x20000u, &nf, &ns, &nt) == -EIO, "latch");
	CHECK(t.latched, "latched flag");
	CHECK(hw_base == base_before, "latched fault changed window");
	CHECK(fault_at(&t, 0, &nf, &ns, &nt) == -EIO, "stays latched");
	CHECK(hw_base == base_before, "second latched fault changed window");
}

static void test_refuse(void)
{
	struct wd24s6d_thrash t;
	struct wd24s6d_mapreq q;
	struct wd24s6d_mapact a;
	u32 nf = 0;

	wd24s6d_thrash_init(&t, 0);
	q.off = WD24S6D_FRAME_BYTES;
	q.mapped_base = ~0u;
	q.mmap_len = WD24S6D_FRAME_BYTES;
	q.vma_len = WD24S6D_FRAME_BYTES;
	wd24s6d_plan_fault(&t, &q, &nf, NULL, &a);
	CHECK(a.err == -EINVAL && nf == 0 && !a.setwin, "past frame");

	q.off = 0;
	q.vma_len = 0;
	wd24s6d_plan_fault(&t, &q, &nf, NULL, &a);
	CHECK(a.err == -EINVAL && !a.setwin, "empty VMA");
}

/* Cleanup order the driver must follow. unregister is omitted when no
 * fb node exists. text-restore is the call after stop (expiry / text / rmmod).
 * A non-zero restore leaves in_text clear (fail closed). */
struct life {
	int armed, active, stopping, mapped, fb_reg, in_text, dead;
	const char *step[8];
	int nstep;
};

static void push(struct life *L, const char *s)
{
	CHECK(L->nstep < 8, "too many steps");
	if (L->nstep < 8)
		L->step[L->nstep++] = s;
}

static void order_ok(const struct life *L, const char *const *want, int n)
{
	int i;

	CHECK(L->nstep == n, "step count %d want %d", L->nstep, n);
	for (i = 0; i < n && i < L->nstep; i++)
		CHECK(strcmp(L->step[i], want[i]) == 0, "step %d %s want %s",
		      i, L->step[i], want[i]);
	for (i = 1; i < L->nstep; i++) {
		unsigned a = 0, b = 0, k;

		for (k = 0; k < WD24S6D_STOP_N; k++) {
			if (!strcmp(L->step[i - 1], wd24s6d_stop_step(k)))
				a = k;
			if (!strcmp(L->step[i], wd24s6d_stop_step(k)))
				b = k;
		}
		CHECK(b > a, "order %s then %s", L->step[i - 1], L->step[i]);
	}
}

static void k_quiesce(struct life *L)
{
	push(L, "quiesce");
	L->stopping = 1;
}

static void k_unmap(struct life *L)
{
	push(L, "unmap");
	L->mapped = 0;
}

static void k_end(struct life *L)
{
	if (!L->active)
		return;
	push(L, "end-session");
	L->active = 0;
	L->mapped = 0;
}

static void k_unreg(struct life *L)
{
	if (!L->fb_reg)
		return;
	push(L, "unregister");
	L->fb_reg = 0;
}

static void k_text(struct life *L, int bad)
{
	push(L, "text-restore");
	if (bad) {
		L->dead = 1;
		return;		/* in_text stays as it was */
	}
	L->in_text = 1;
}

/* s6d_stop: armed clear, quiesce+unmap, end, unregister-if-registered. */
static void k_stop(struct life *L)
{
	int had = L->fb_reg;

	L->armed = 0;
	k_quiesce(L);
	k_unmap(L);
	k_end(L);
	if (had)
		k_unreg(L);
}

static void k_expire(struct life *L, int bad)
{
	k_stop(L);
	k_text(L, bad);
}

/* Process death: vma close, then fb release (drop maps + end). armed stays
 * set; stopping blocks another open. The fb node stays until s6d_stop. */
static void k_death(struct life *L)
{
	k_quiesce(L);
	k_unmap(L);
	k_end(L);
}

static void test_cleanup(void)
{
	struct life L;
	const char *full[] = {
		"quiesce", "unmap", "end-session", "unregister", "text-restore"
	};
	const char *nofb[] = {
		"quiesce", "unmap", "end-session", "text-restore"
	};
	const char *die[] = { "quiesce", "unmap", "end-session" };

	memset(&L, 0, sizeof(L));
	L.armed = L.active = L.mapped = L.fb_reg = 1;
	k_expire(&L, 0);
	order_ok(&L, full, 5);
	CHECK(L.in_text && !L.active && !L.fb_reg && !L.mapped && L.stopping,
	      "clean expiry");

	memset(&L, 0, sizeof(L));
	L.armed = L.active = L.mapped = L.fb_reg = 1;
	k_expire(&L, 1);
	order_ok(&L, full, 5);
	CHECK(!L.in_text && L.dead && !L.fb_reg, "mismatch stays out of text");

	memset(&L, 0, sizeof(L));
	L.armed = L.active = L.mapped = 1;
	k_expire(&L, 0);
	order_ok(&L, nofb, 4);
	CHECK(L.in_text && !L.fb_reg, "no fb node still restores");

	/* SIGKILL / alarm exit: session ends, fb node stays until expiry. */
	memset(&L, 0, sizeof(L));
	L.armed = L.active = L.mapped = L.fb_reg = 1;
	k_death(&L);
	order_ok(&L, die, 3);
	CHECK(L.armed && !L.active && !L.mapped && L.fb_reg && !L.in_text && L.stopping,
	      "death leaves fb registered");
	k_expire(&L, 0);
	CHECK(L.in_text && !L.fb_reg, "expiry after death");

	/* munmap alone does not end the session or unregister. */
	memset(&L, 0, sizeof(L));
	L.armed = L.active = L.mapped = L.fb_reg = 1;
	L.mapped = 0;
	CHECK(L.active && L.fb_reg && !L.in_text, "unmap keeps session");

	/* rmmod with the session already ended by release. */
	memset(&L, 0, sizeof(L));
	L.fb_reg = 1;
	L.stopping = 1;
	k_expire(&L, 0);
	CHECK(L.in_text && !L.fb_reg, "unload after release");
}

int main(void)
{
	test_probe();
	test_thrash_stops_hardware();
	test_refuse();
	test_cleanup();
	if (fails) {
		printf("test_s6d_fault: %d/%d FAILED\n", fails, checks);
		return 1;
	}
	printf("test_s6d_fault: %d checks OK\n", checks);
	return 0;
}
