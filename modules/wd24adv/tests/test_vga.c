// SPDX-License-Identifier: GPL-2.0
/* Host test of the wd24adv port backend against the port-level VGA model
 * (vgamodel.c): full save/restore round trip, trial revert/confirm, the
 * zero-write entry check (incl. a QEMU std VGA profile), readback mismatch
 * handling and forbidden-register detection. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "vgamodel.h"
#include "../wd24adv_dac_entry.h"

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

	/* 4b. floppy-like: SR06 data 0x0F but width unlocked (unlock value 4Fh).
	 * Without fix_sr06: refuse, 0 data writes. With fix_sr06: write 0x0F once,
	 * width locks, load proceeds. Wrong nibble + fix_sr06 still refuses. */
	setup(VM_WD24, 330);
	m.sr[6] = 0x4f; snap = m;
	err = RUN(wd24_plan_load(&c));
	T(err == -ENODEV && m.data_writes == 0 && vm_cmp(&m, &snap, "4f-nofix") == 0,
	  "SR06=4F unlocked, fix_sr06=0: refused, 0 data writes, identical");

	setup(VM_WD24, 331);
	m.sr[6] = 0x4f; snap = m;
	c.fix_sr06 = true;
	err = RUN(wd24_plan_load(&c));
	T(err == 0 && c.nmismatch == 0 && c.nrefused == 0, "SR06=4F + fix_sr06: load ok");
	T(m.violations == 0, "SR06=4F + fix_sr06: no forbidden register");
	T((m.sr[6] & 0x58) != 0x48, "SR06=4F + fix_sr06: latch locked after load");
	T((m.sr[6] & 0x0f) == 0x0f, "SR06=4F + fix_sr06: SR06 nibble still 0F");
	if (err)
		dump("fix_sr06 load");

	setup(VM_WD24, 332);
	m.sr[6] = 0x48; snap = m;	/* reads as 0x08 — not the 0x0F gate */
	c.fix_sr06 = true;
	err = RUN(wd24_plan_load(&c));
	T(err == -ENODEV && m.data_writes == 0 && vm_cmp(&m, &snap, "48-fix") == 0,
	  "SR06=48 + fix_sr06: still refused (data != 0x0F), 0 writes");
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
	T(err == -EIO && c.nmismatch >= 1 && !c.in_text,
	  "mismatch during restore: -EIO and in_text stays false");
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
	T(err == -EIO && c.nmismatch >= 1 && !c.in_text,
	  "plane 2 not restored: -EIO and in_text stays false");
	m.ro_plane = -1;
	memcpy(m.vram[2], snap.vram[2], WD24_PLANE);
	T(vm_cmp(&m, &snap, "ro plane") == 0, "plane 2 not restored: everything else restored");

	/* 7. pattern covers the whole 640x480 frame (banks 0-4, 300 KiB): every
	 * packed pixel (scanout: linear L = plane L&3, offset L>>2) is a bar,
	 * order reversed from row 410, the first whole row >= 256 KiB; PR0A back to 0; trial revert identical */
	setup(VM_WD24, 60);
	RUN(wd24_plan_load(&c));
	RUN(wd24_plan_mode(&c, m480));
	err = RUN(wd24_pattern(&c, m480));
	{
		u32 L, bad = 0;

		for (L = 0; L < 640 * 480; L++) {
			u32 b = (L % 640) / 40;
			u8 want = c.vga.attr[L / 640 >= 410 ? 15 - b : b] & 0x3f;
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

	/* One DAC entry on a copy of the image. The saved snapshot stays.
	 * Full restore writes that snapshot and readback-compares 6 bits. */
	setup(VM_WD24, 80);
	err = RUN(wd24_plan_load(&c));
	T(err == 0 && !memcmp(c.cmap, m.dac, 768), "dac entry: load snapshot matches");
	{
		struct wd24_dac_entry e;
		u8 saved[768];

		T(wd24_dac_entry(256, 0, 0, 0, &e) == -22, "dac entry: regno 256 refused");
		T(wd24_dac_entry(1, 0xffff, 0x8000, 0x0400, &e) == 0, "dac entry: regno 1 accepted");
		T(e.index == 1 && e.pel == 0xff && e.rgb[0] == 0x3f &&
		  e.rgb[1] == 0x20 && e.rgb[2] == 0x01, "dac entry: 6-bit channels");
		memcpy(saved, c.cmap, 768);
		{
			u8 before[768];
			unsigned int n, diff = 0;

			memcpy(before, m.dac, 768);
			wd24_dac_entry_store(m.dac, &e);
			for (n = 0; n < 768; n++)
				diff += (m.dac[n] != before[n]);
			T(diff == 3 && m.dac[3] == 0x3f && m.dac[4] == 0x20 &&
			  m.dac[5] == 0x01,
			  "dac entry: exactly three bytes change in the working image");
		}
		T(!memcmp(c.cmap, saved, 768), "dac entry: saved snapshot untouched");
		err = RUN(wd24_restore_cmap(&c));
		T(err == 0 && c.nmismatch == 0 && m.violations == 0 &&
		  !memcmp(m.dac, snap.dac, 768),
		  "dac entry: full restore puts the snapshot back");
	}

	/* Port sequence on the model: one mask write, no 3C6 read, snapshot
	 * immutable, neighbours and non-DAC state unchanged, then restore.
	 * A lying 3C9 read must fail the entry and the full restore must
	 * put the snapshot back. The lie is not the model's own compare. */
	{
		struct lie {
			struct wd24_io *real;
			unsigned pel_w, idx_w, data_w, pel_r, nread, lie_at;
		} L;
		struct wd24_dac_io dio;
		struct wd24_dac_span sp[2];
		struct vgamodel before;
		u8 saved[768];
		u16 er[256], eg[256], eb[256];

		void lie_out(void *p, u8 v, u16 port)
		{
			struct lie *lp = p;

			if (port == 0x3c6)
				lp->pel_w++;
			else if (port == 0x3c7 || port == 0x3c8)
				lp->idx_w++;
			else if (port == 0x3c9)
				lp->data_w++;
			lp->real->outb(lp->real->priv, v, port);
		}
		u8 lie_in(void *p, u16 port)
		{
			struct lie *lp = p;
			u8 v = lp->real->inb(lp->real->priv, port);

			if (port == 0x3c6)
				lp->pel_r++;
			if (port == 0x3c9) {
				lp->nread++;
				if (lp->lie_at && lp->nread == lp->lie_at)
					v ^= 0x01;
			}
			return v;
		}

		T(EINVAL == 22 && EIO == 5 && EPERM == 1 && EBUSY == 16 &&
		  ENODEV == 19, "dac errno numbers are the Linux values");
		T(wd24_cmap_param_refuse(0, 0, 1, 0, 1, 0, 0, 1) == 0,
		  "cmap param off is not a refuse");
		T(wd24_cmap_param_refuse(1, 1, 0, 1, 0, 1, 1, 0) == 0,
		  "cmap param accepts the one live combination");
		T(wd24_cmap_param_refuse(1, 0, 0, 1, 0, 1, 1, 0) == -22 &&
		  wd24_cmap_param_refuse(1, 1, 1, 1, 0, 1, 1, 0) == -22 &&
		  wd24_cmap_param_refuse(1, 1, 0, 0, 0, 1, 1, 0) == -22 &&
		  wd24_cmap_param_refuse(1, 1, 0, 1, 1, 1, 1, 0) == -22 &&
		  wd24_cmap_param_refuse(1, 1, 0, 1, 0, 0, 1, 0) == -22 &&
		  wd24_cmap_param_refuse(1, 1, 0, 1, 0, 1, 0, 0) == -22 &&
		  wd24_cmap_param_refuse(1, 1, 0, 1, 0, 1, 1, 1) == -22,
		  "cmap param refuses every other combination");
		{
			struct wd24_cmap_view v;
			unsigned int *fields[14];
			unsigned int n;

			memset(&v, 0, sizeof(v));
			v.param = v.live = v.fbcon = 1;
			v.armed = v.active = v.busy = v.have_cmap = v.published = 1;
			T(wd24_cmap_allow(&v) == 0, "cmap allow: live user session");
			T(wd24_cmap_allow(NULL) == -1, "cmap allow: no view");
			fields[0] = &v.param; fields[1] = &v.live; fields[2] = &v.fbcon;
			for (n = 0; n < 3; n++) {
				unsigned int old = *fields[n];

				*fields[n] = 0;
				T(wd24_cmap_allow(&v) == -1, "cmap allow: missing opt-in");
				*fields[n] = old;
			}
			v.dryrun = 1;
			T(wd24_cmap_allow(&v) == -1, "cmap allow: dry-run");
			v.dryrun = 0;
			v.bind = 1;
			T(wd24_cmap_allow(&v) == -1, "cmap allow: fbcon bind");
			v.bind = 0;
			fields[0] = &v.armed; fields[1] = &v.active; fields[2] = &v.busy;
			for (n = 0; n < 3; n++) {
				unsigned int old = *fields[n];

				*fields[n] = 0;
				T(wd24_cmap_allow(&v) == -16, "cmap allow: not in a user session");
				*fields[n] = old;
			}
			v.stopping = 1;
			T(wd24_cmap_allow(&v) == -16, "cmap allow: stopping");
			v.stopping = 0;
			v.registering = 1;
			T(wd24_cmap_allow(&v) == -16, "cmap allow: registering");
			v.registering = 0;
			v.frozen = 1;
			T(wd24_cmap_allow(&v) == -16, "cmap allow: frozen");
			v.frozen = 0;
			v.in_text = 1;
			T(wd24_cmap_allow(&v) == -19, "cmap allow: text");
			v.in_text = 0;
			v.hw_dead = 1;
			T(wd24_cmap_allow(&v) == -19, "cmap allow: dead");
			v.hw_dead = 0;
			v.have_cmap = 0;
			T(wd24_cmap_allow(&v) == -19, "cmap allow: no snapshot");
			v.have_cmap = 1;
			v.bad = 1;
			T(wd24_cmap_allow(&v) == -19, "cmap allow: prior mismatch");
			v.bad = 0;
			v.published = 0;
			T(wd24_cmap_allow(&v) == -19, "cmap allow: unpublished");
			v.published = 1;
			T(wd24_cmap_allow(&v) == 0, "cmap allow: restored");
		}

		setup(VM_WD24, 81);
		err = RUN(wd24_plan_load(&c));
		T(err == 0, "dac span: load");
		memcpy(saved, c.cmap, 768);
		before = m;
		memset(&L, 0, sizeof(L));
		L.real = &io;
		dio.outb = lie_out;
		dio.inb = lie_in;
		dio.p = &L;
		T(wd24_dac_put_span(&dio, NULL, 0) == -22 && L.pel_w == 0,
		  "dac span: empty request writes nothing");
		sp[0].index = 4;
		sp[0].r = 0xff;
		sp[0].g = 0x20;
		sp[0].b = 0x01;
		sp[1].index = 255;
		sp[1].r = 0x3f;
		sp[1].g = 0x00;
		sp[1].b = 0x02;
		err = wd24_dac_put_span(&dio, sp, 2);
		T(err == 0 && L.pel_w == 1 && L.pel_r == 0 && L.idx_w == 4 &&
		  L.data_w == 6 && L.nread == 6 && m.violations == 0,
		  "dac span: one mask write, no 3C6 read, two entries");
		T(m.dac[12] == 0x3f && m.dac[13] == 0x20 && m.dac[14] == 0x01 &&
		  m.dac[765] == 0x3f && m.dac[766] == 0x00 && m.dac[767] == 0x02,
		  "dac span: 6-bit bytes land on those indices");
		T(memcmp(m.dac, before.dac, 12) == 0 &&
		  memcmp(m.dac + 15, before.dac + 15, 750) == 0,
		  "dac span: every other colour byte stays");
		T(!memcmp(c.cmap, saved, 768), "dac span: snapshot immutable");
		T(memcmp(m.vram, before.vram, sizeof(m.vram)) == 0 &&
		  memcmp(m.sr, before.sr, sizeof(m.sr)) == 0 &&
		  memcmp(m.gr, before.gr, sizeof(m.gr)) == 0 &&
		  memcmp(m.cr, before.cr, sizeof(m.cr)) == 0 &&
		  memcmp(m.ar, before.ar, sizeof(m.ar)) == 0 &&
		  m.misc == before.misc,
		  "dac span: planes and indexed registers stay");
		wd24_dac_expand_image(c.cmap, er, eg, eb);
		T((er[4] >> 10) == (c.cmap[12] & 0x3f) &&
		  (eg[4] >> 10) == (c.cmap[13] & 0x3f) &&
		  (eb[4] >> 10) == (c.cmap[14] & 0x3f) &&
		  (er[255] >> 10) == (c.cmap[765] & 0x3f),
		  "dac expand: <<10 round-trips the snapshot, not the new bytes");
		err = RUN(wd24_restore_cmap(&c));
		T(err == 0 && c.nmismatch == 0 && !memcmp(m.dac, snap.dac, 768) &&
		  !memcmp(c.cmap, saved, 768),
		  "dac span: restore writes the untouched snapshot");

		setup(VM_WD24, 82);
		err = RUN(wd24_plan_load(&c));
		memcpy(saved, c.cmap, 768);
		memset(&L, 0, sizeof(L));
		L.real = &io;
		L.lie_at = 4;
		dio.p = &L;
		sp[0].index = 1;
		sp[0].r = 1;
		sp[0].g = 2;
		sp[0].b = 3;
		sp[1].index = 2;
		sp[1].r = 4;
		sp[1].g = 5;
		sp[1].b = 6;
		err = wd24_dac_put_span(&dio, sp, 2);
		T(err == -5 && m.dac[3] == 1 && m.dac[4] == 2 && m.dac[5] == 3,
		  "dac span: a bad readback fails after the earlier entry was written");
		T(!memcmp(c.cmap, saved, 768), "dac span: failed readback leaves the snapshot");
		err = RUN(wd24_restore_cmap(&c));
		T(err == 0 && c.nmismatch == 0 && m.violations == 0 &&
		  !memcmp(m.dac, snap.dac, 768),
		  "dac span: restore after the lie matches the load snapshot");
	}

	printf("wd24adv vga model test: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
