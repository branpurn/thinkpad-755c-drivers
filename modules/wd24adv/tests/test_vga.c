// SPDX-License-Identifier: GPL-2.0
/* Host test of the wd24adv port backend against the port-level VGA model
 * (vgamodel.c): full save/restore round trip, trial revert/confirm, the
 * zero-write entry check (incl. a QEMU std VGA profile), readback mismatch
 * handling and forbidden-register detection. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "vgamodel.h"

static int fails, passes;
#define T(cond, what) do { if (cond) passes++; else { fails++; \
	printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

static struct vgamodel m, snap;
static struct wd24_ctx c;
static struct wd24_io io;
static u8 planes[4 * WD24_PLANE];

static void setup(enum vm_profile p, unsigned int seed)
{
	vm_init(&m, p, seed);
	vm_io(&io, &m);
	wd24_ctx_init(&c, &wd24_port_backend);
	c.io = &io;
	c.planes = planes;
	memset(planes, 0, sizeof(planes));
	snap = m;
}

/* the kernel module's bracket around every plan */
#define RUN(expr) ({ int _e; c.be->begin(&c); _e = (expr); c.be->end(&c); _e; })

static void dump(const char *what)
{
	unsigned int i;

	printf("  -- %s: events --\n", what);
	for (i = 0; i < c.nev; i++) {
		const struct wd24_event *e = &c.ev[i];

		if (e->type == WD24_EV_CHECK || e->type == WD24_EV_MISMATCH ||
		    e->type == WD24_EV_REFUSED)
			printf("  ev%u type %d %s%02X val %02X old %02X %s\n", i, e->type,
			       wd24_rf_name(e->rf), e->idx, e->val, e->old,
			       e->note ? e->note : "");
	}
}

int main(void)
{
	const struct wd24_mode *m480 = wd24_mode_find("640x480-8");
	const struct wd24_mode *m400 = wd24_mode_find("640x400-8");
	const struct wd24_mode *m13 = wd24_mode_find("320x200-8");
	const struct wd24_mode *modes[3] = { m480, m400, m13 };
	unsigned long dw;
	int err, k;

	/* 1. load: saves everything, changes nothing */
	setup(VM_WD24, 1);
	err = RUN(wd24_plan_load(&c));
	T(err == 0 && c.nmismatch == 0 && c.nrefused == 0, "load: ok, no mismatch");
	if (err)
		dump("load");
	T(m.violations == 0, "load: no forbidden register");
	T(vm_cmp(&m, &snap, "load") == 0, "load leaves the whole model identical");
	T(!memcmp(planes, snap.vram, sizeof(planes)), "load: 4 planes x 64 KiB saved exactly");
	T(!memcmp(c.cmap, snap.dac, 768), "load: DAC saved exactly");
	T(c.have == (WD24_HAVE_LOCKS | WD24_HAVE_EXT | WD24_HAVE_CMAP |
		     WD24_HAVE_STD | WD24_HAVE_PLANES), "load: all state saved");

	/* 1b. live 755C text state after wd90c24fb text=1 + rmmod (wd24probe3):
	 * 3CC=67 GR0F=00 GR0E=40 CR11=AE CR29=85 SR06=0F: load and unload
	 * leave the whole model identical */
	setup(VM_WD24, 2);
	m.misc = 0x67; m.gr[0x0f] = 0x00; m.gr[0x0e] = 0x40;
	m.cr[0x11] = 0xae; m.cr[0x29] = 0x85; m.sr[6] = 0x0f;
	snap = m;
	err = RUN(wd24_plan_load(&c));
	T(err == 0 && c.nmismatch == 0 && c.nrefused == 0, "00/85 text state: load ok");
	if (err)
		dump("load 00/85");
	T(m.violations == 0 && vm_cmp(&m, &snap, "load 00/85") == 0,
	  "00/85 text state: load leaves the whole model identical");
	err = RUN(wd24_plan_text(&c));
	T(err == 0 && c.nmismatch == 0 && m.violations == 0 &&
	  vm_cmp(&m, &snap, "unload 00/85") == 0,
	  "00/85 text state: unload restore, whole model identical");
	setup(VM_WD24, 3);
	m.cr[0x11] = 0xae; m.cr[0x29] = 0x85; snap = m;
	RUN(wd24_plan_load(&c));
	err = RUN(wd24_plan_mode(&c, m480));
	T(err == 0 && c.nmismatch == 0 && m.violations == 0, "00/85 text state: 640x480-8 plan ok (model)");
	err = RUN(wd24_trial_expire(&c));
	T(err == 0 && vm_cmp(&m, &snap, "trial 00/85") == 0,
	  "00/85 text state: trial revert, whole model identical");

	/* 2. every PROVEN mode, trial expiry: back to identical */
	for (k = 0; k < 3; k++) {
		setup(VM_WD24, 10 + k);
		RUN(wd24_plan_load(&c));
		err = RUN(wd24_plan_mode(&c, modes[k]));
		T(err == 0 && c.nmismatch == 0 && m.violations == 0, modes[k]->name);
		if (err || c.nmismatch || m.violations) {
			dump(modes[k]->name);
			printf("  err %d violations %u %s\n", err, m.violations, m.violation);
		}
		RUN(wd24_pattern(&c, modes[k]));
		T(m.violations == 0, "mode + pattern: no forbidden register");
		T(memcmp(&m, &snap, sizeof(m)) != 0, "mode changed the model");
		if (k == 0)
			T(m.misc == 0xe3 && m.gr[0x0f] == 0x05 && m.cr[0x29] == 0x85 &&
			  (m.sr[6] & 0x0f) == 0x0f && m.cr[0x11] == 0x8c && m.sr[0x11] == 0x5d &&
			  m.cr[0x31] == 0xc7 && m.cr[0x32] == 0x54,
			  "640x480-8 on the model = probe3 live values");
		T(c.trial_pending, "mode set leaves a pending trial");
		err = RUN(wd24_trial_expire(&c));
		T(err == 0 && c.nmismatch == 0 && m.violations == 0, "trial expiry restore: ok");
		T(vm_cmp(&m, &snap, "trial revert") == 0, "trial revert: whole model identical (regs, DAC, 4 planes)");
		T(c.in_text && !c.trial_pending, "trial revert: back in text");
	}

	/* 3. confirm keeps the mode; unload restores */
	setup(VM_WD24, 20);
	RUN(wd24_plan_load(&c));
	RUN(wd24_plan_mode(&c, m480));
	T(wd24_confirm(&c) == 0, "confirm");
	dw = m.data_writes;
	T(RUN(wd24_trial_expire(&c)) == 0 && m.data_writes == dw, "confirmed: expiry writes nothing");
	T(RUN(wd24_plan_text(&c)) == 0 && vm_cmp(&m, &snap, "unload") == 0,
	  "unload restore after confirm: identical");
	T(wd24_confirm(&c) == -EINVAL, "confirm without a pending mode refused");

	/* 4. entry check refusals: zero data writes, model identical */
	setup(VM_QEMU, 30);
	err = RUN(wd24_plan_load(&c));
	T(err == -ENODEV && m.data_writes == 0 && vm_cmp(&m, &snap, "qemu") == 0,
	  "QEMU std VGA profile: load refused, 0 data writes, model identical");
	setup(VM_WD24, 31);
	m.cr[0x32] = 0x5c; snap = m;
	err = RUN(wd24_plan_load(&c));
	T(err == -ENODEV && m.data_writes == 0 && vm_cmp(&m, &snap, "pr19") == 0,
	  "PR19=5C: load refused, 0 data writes");
	setup(VM_WD24, 32);
	m.cr[0x34] = 0x00; snap = m;
	err = RUN(wd24_plan_load(&c));
	T(err == -ENODEV && m.data_writes == 0, "CR34 closed (PR18/19 read FF): refused, 0 data writes");
	setup(VM_WD24, 33);
	m.sr[6] = 0x48; snap = m;	/* latch left unlocked */
	err = RUN(wd24_plan_load(&c));
	T(err == -ENODEV && m.data_writes == 0, "SR06 latch unlocked at entry: refused, 0 data writes");
	setup(VM_WD24, 34);
	m.ar[0x10] = 0x41; snap = m;	/* graphics mode */
	err = RUN(wd24_plan_load(&c));
	T(err == -ENODEV && m.data_writes == 0, "AR10 graphics at entry: refused, 0 data writes");
	setup(VM_WD24, 35);
	RUN(wd24_plan_load(&c));
	m.gr[0x0f] = 0x05;		/* someone unlocked PR5 behind our back */
	dw = m.data_writes;
	err = RUN(wd24_plan_mode(&c, m480));
	T(err == -ENODEV && m.data_writes == dw, "state changed since load: mode refused, 0 data writes");

	/* 5. readback mismatch: stop, restore, model back to identical */
	setup(VM_WD24, 40);
	RUN(wd24_plan_load(&c));
	m.stuck_rf = WD24_CR; m.stuck_idx = 0x13; m.stuck = 0x28;
	err = RUN(wd24_plan_mode(&c, m480));
	T(err == -EIO && c.nmismatch >= 1 && c.failed, "stuck CR13: mode stops with -EIO");
	T(m.violations == 0 && vm_cmp(&m, &snap, "mismatch restore") == 0,
	  "stuck CR13: restore ran, whole model identical");
	T(c.in_text && !c.trial_pending, "stuck CR13: no pending trial");

	setup(VM_WD24, 41);		/* unlock latch never opens */
	m.stuck_rf = WD24_SR; m.stuck_idx = 6; m.stuck = 0x0f;
	err = RUN(wd24_plan_load(&c));
	T(err == -EIO && c.nmismatch >= 1, "SR06 latch stuck locked: load stops at the unlock");
	T(m.violations == 0 && vm_cmp(&m, &snap, "latch") == 0,
	  "SR06 latch stuck: relock restore, model identical");

	setup(VM_WD24, 42);		/* mismatch during the restore itself */
	RUN(wd24_plan_load(&c));
	RUN(wd24_plan_mode(&c, m480));
	m.stuck_rf = WD24_GR; m.stuck_idx = 0x06; m.stuck = 0x05;
	err = RUN(wd24_plan_text(&c));
	T(err == -EIO && c.nmismatch >= 1, "mismatch during restore: reported -EIO");
	m.stuck_rf = -1;
	m.gr[6] = snap.gr[6];
	T(vm_cmp(&m, &snap, "restore with stuck GR06") == 0,
	  "mismatch during restore: every other step still ran");

	/* 6. plane readback: a plane that does not take the restore is reported */
	setup(VM_WD24, 50);
	RUN(wd24_plan_load(&c));
	RUN(wd24_plan_mode(&c, m480));
	RUN(wd24_pattern(&c, m480));	/* chain-4 writes hit planes 0-3 */
	m.ro_plane = 2;
	err = RUN(wd24_plan_text(&c));
	T(err == -EIO && c.nmismatch >= 1, "plane 2 not restored: readback reports -EIO");
	m.ro_plane = -1;
	memcpy(m.vram[2], snap.vram[2], WD24_PLANE);
	T(vm_cmp(&m, &snap, "ro plane") == 0, "plane 2 not restored: everything else restored");

	/* 7. pattern covers the whole 640x480 frame (banks 0-4, 300 KiB): every
	 * packed pixel (scanout: linear L = plane L&3, offset L>>2) is a bar,
	 * order reversed from 256 KiB; PR0A back to 0; trial revert identical */
	setup(VM_WD24, 60);
	RUN(wd24_plan_load(&c));
	RUN(wd24_plan_mode(&c, m480));
	err = RUN(wd24_pattern(&c, m480));
	{
		u32 L, bad = 0;

		for (L = 0; L < 640 * 480; L++) {
			u32 b = (L % 640) / 40;
			u8 want = c.vga.attr[L >> 18 ? 15 - b : b] & 0x3f;
			u8 got = L >> 18 ? m.vram_hi[L & 3][(L >> 2) & 0xffff]
					 : m.vram[L & 3][L >> 2];

			bad += got != want;
		}
		T(err == 0 && bad == 0 && c.nmismatch == 0, "pattern: all 480 lines drawn, no stale pixel");
		T(m.gr[0x09] == 0x00, "pattern: PR0A back to bank 0");
	}
	err = RUN(wd24_trial_expire(&c));
	T(err == 0 && vm_cmp(&m, &snap, "full-frame pattern revert") == 0,
	  "full-frame pattern: trial revert leaves the model identical");

	/* 8. 320x200-8 (mode 13h) pattern: all 64000 bytes through the A0000
	 * window, 16 bars of 20 px, chain-4 (linear L = plane L&3, offset L>>2),
	 * no port write at all (no PR0A), no violation; mode-set counts as
	 * predicted for the live run; trial revert identical */
	setup(VM_WD24, 70);
	RUN(wd24_plan_load(&c));
	{
		unsigned long d0 = c.hw_data, i0 = c.hw_index, d1, i1;
		u32 L, bad = 0, w0;
		unsigned long wsum;

		c.be->begin(&c);
		err = wd24_plan_mode(&c, m13);
		d1 = c.hw_data; i1 = c.hw_index;
		w0 = m.mem_writes;
		wsum = m.data_writes + m.index_writes;
		if (!err && wd24_has_pattern(m13))
			err = wd24_pattern(&c, m13);
		T(c.hw_data == d1 && c.hw_index == i1 && m.data_writes + m.index_writes == wsum,
		  "320x200-8 pattern: no port write (no PR0A, no map mask)");
		c.be->end(&c);
		T(err == 0 && c.nmismatch == 0 && m.violations == 0, "320x200-8 + pattern: ok, no forbidden register");
		T(m.mem_writes - w0 == 64000, "320x200-8 pattern: exactly 64000 window writes");
		T(c.hw_data - d0 == 90 && c.hw_index - i0 == 258,
		  "320x200-8 mode + pattern: 90 data / 258 index port writes (live prediction)");
		for (L = 0; L < 320 * 200; L++) {
			u8 want = c.vga.attr[((L % 320) / 20) & 15] & 0x3f;

			bad += m.vram[L & 3][L >> 2] != want;
		}
		T(bad == 0, "320x200-8 pattern: all 64000 pixels are the right bar colour");
		T(m.gr[0x09] == 0x00 && m.sr[2] == 0x0f && (m.sr[4] & 0x08), "320x200-8: bank 0, map mask 0F, chain-4");
	}
	err = RUN(wd24_trial_expire(&c));
	T(err == 0 && c.nmismatch == 0 && vm_cmp(&m, &snap, "13h pattern revert") == 0,
	  "320x200-8 pattern: trial revert leaves the model identical");
	{
		const struct wd24_mode *mx = wd24_mode_find("320x240-8x");

		T(wd24_has_pattern(m480) && wd24_has_pattern(m400) && wd24_has_pattern(m13) &&
		  !wd24_has_pattern(mx), "pattern only for PROVEN packed/direct modes");
	}
	/* BIOS PR0A != 0: the 13h pattern refuses without writing anything */
	setup(VM_WD24, 71);
	RUN(wd24_plan_load(&c));
	RUN(wd24_plan_mode(&c, m13));
	{
		unsigned long d0 = c.hw_data, i0 = c.hw_index;
		u32 w0 = m.mem_writes;
		u8 keep = c.bios.pr0a;

		c.bios.pr0a = 0x10;
		err = RUN(wd24_pattern(&c, m13));
		c.bios.pr0a = keep;
		T(err == -EINVAL && m.mem_writes == w0 && c.hw_data == d0 &&
		  c.hw_index - i0 <= 3, "13h pattern with BIOS PR0A != 0: refused, nothing drawn");
	}
	RUN(wd24_trial_expire(&c));

	printf("wd24adv vga model test: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
