# wd24adv next stage after S6b live OK

## Proven (live)

| Stage | Result | Notes |
|---|---|---|
| S1–S5 | OK | Engine FILL/COPY/MONO/HOSTCOL proven |
| S6 / S6c | OK | PR1 b3 split; PR31 b7 effective with SR06 locked |
| **S6b** | **OK 18/18** (2026-11-05, commit `6be7a42`, trial=300) | `/dev/wd24s6b` chrdev: once-mapped A0000–AFFFF, explicit SETWIN, kernel PR31 b7 COPY. kill -9 restore clean; expiry mismatches 0; X-UP after. Baseline L13 skipped (`/tmp/wd24probe3.ko` missing on 755C). |

S6b is a **chrdev**, not fbdev: tc755x does not export `vmf_insert_pfn`,
`zap_vma_ptes`, or `unmap_mapping_range` to modules (`CONFIG_TRIM_UNUSED_KSYMS=y`;
symbols exist in vmlinux / System.map but are trimmed from Module.symvers).
`CONFIG_FRAMEBUFFER_CONSOLE=y` would also bind fbcon on `register_framebuffer`
(DEFERRED_TAKEOVER needs `FB=y`, which the floppy budget rejects).

## Standing blockers (unchanged by S6b OK)

1. **Export trim** — fault-banked mmap cannot move banks under a live VMA until
   the three symbols are kept for OOT modules (see below).
2. **fbcon** — any `register_framebuffer` binds fbcon with the current config.
   Acceptable for an X desktop path (wd90c24fb already does); not acceptable
   for text-only proof stages that need vgacon alone → those stay chrdev.
3. **Fault livelock** — one x86 instruction that touches two different 64 KiB
   banks under single-bank mapping restart-faults forever (DESKTOP-DESIGN
   §3.3 / parked S6b note). S6b avoided it by mapping both window halves at
   once with explicit SETWIN.

RAM-shadow (wd90c24fb-style full-frame / timer upload) stays rejected.

## Options toward X on wd24adv

| | Path | Buildable now? | Proves | Risks |
|---|---|---|---|---|
| **A** | Keep the three symbols via `config/ksyms-keep.txt`, rebuild tc755x kernel, then **S6d**: real fbdev + fault-banked mmap (S6b dual-window under the hood), unaccelerated Xfbdev, then S7 ioctl accel | Export list + host tests yes; module needs new KDIR; **install/flash needs Brandon OK** | Design §3.3 / §6 item 1; stock Xfbdev on wd24adv | Kernel rebuild; livelock on spanning ops; fbcon bind during graphics |
| B | fbdev that reuses S6b once-mapped 64 KiB + SETWIN (no fault path) | Kernel side yes | Banking under fbdev node | **Xfbdev cannot drive explicit-bank devices** without X-side SETWIN before every draw; not stock TinyX |
| C | Kernel CAPS/SUBMIT ioctl (FILL/COPY/MONO/HOSTCOL, engine S3–S5) + TinyX `wd24accel` without full linear mmap | Yes (headers + validator + host tests); no new exports | Accel desktop path | Screen pixmap still needs a CPU path (mmap or damage+HOSTCOL); alone is not stock Xfbdev |
| D | Other | — | — | No better path found; damage+HOSTCOL-only is a shadow detour |

## Recommendation: **A → S6d** (real-driver path)

Prefer the design’s fault-banked fbdev + later ioctl accel. S6b already proved
the banking and COPY primitives; the missing piece for stock Xfbdev is exports
plus an fbdev wrapper with a thrash latch for the livelock hole.

**Not B** — Xfbdev will not call SETWIN. **Not C alone** — needed for S7, but
does not give a linear mmap for unaccelerated Xfbdev; do it *after* S6d maps.
**No RAM-shadow.**

### What A proves (S6d)

- `register_framebuffer` for proven 640x480-8; `fb_mmap` exposes the frame
  (and later 1 MB) as a VMA.
- Fault handler: `wait_idle` → SETWIN-equivalent dual window covering the
  faulting 64 KiB → `vmf_insert_pfn` × 16 for A0000 → zap the previous
  window’s PTEs (`zap_vma_ptes` / `unmap_mapping_range`).
- Thrash latch: if faults alternate between two windows faster than a bound
  (same-instruction livelock symptom), mark session degraded / refuse further
  faults and log — stop rule, not silent hang.
- Last close / kill -9 / trial expiry → `restore_all` (registers + text), same
  discipline as S6b.
- Hand-started Xfbdev (not boot autostart) draws without wd90c24fb.

### Export patch proposal (this tree — no mm/ source edit)

Symbols are already `EXPORT_SYMBOL*` in upstream `mm/memory.c`. Trim drops
them. Add to `config/ksyms-keep.txt`:

```
unmap_mapping_range
vmf_insert_pfn
zap_vma_ptes
```

Then on the box: `make kernel` (TRIM_KSYMS=1 uses the whitelist via
`scripts/build-kernel.sh`). Confirm the three appear in
`build/kernel-obj/Module.symvers`. **Do not install or flash the new bzImage
on the 755C without Brandon’s OK.** Out-of-tree wd24adv then builds against
that KDIR.

fbcon: leave `CONFIG_FRAMEBUFFER_CONSOLE=y` / `CONFIG_FB=m` as today.
DEFERRED_TAKEOVER is not available (`depends on FB=y`). S6d accepts fbcon
bind during graphics (wd90c24fb pattern); proof-only text stages keep using
chrdev. Optional live sysfs vtcon unbind is a stop-rule tool, not a config
change.

### Live stop rules sketch (S6d — not run until approved)

1. Box: host bank tests green; QEMU fault path + thrash latch; module loads
   against new KDIR; PII/scan/push-checked clean.
2. Brandon OK for **kernel install** (new bzImage + modules) and separately
   for **S6d live**.
3. Live: trial mode, short arm; controlled single-bank paints first; then a
   deliberate spanning write to confirm thrash latch fires cleanly (no hang);
   restore_all on close / kill -9; vgacon usable; wd90c24fb reload OK.
4. Stop on: thrash latch in normal paint, register mismatch, trial restore
   fail, panel/routing oddity, or any unexpected value — report, no retry
   without a new plan.
5. Xfbdev only after the controlled live paints are OK (separate approval).

### Separate S6d floppy (box, 2026-11-05) — **BUILT** (Tier1+2+3)

Brandon OK: separate floppy path (pt426u) + Tier1+2+3 drops (widget t426s1393).
CF boot stays OFF. Recovery `tinycore-32-ram/out/{tc32.img,core.xz,busybox}`
**untouched** (not replaced by S6d artifacts). Box did **not** burn media or
touch the 755C / MBR.

| Artifact | Size | sha256 (prefix…) |
|---|---|---|
| `tc755-s6d.img` | 1474560 | `e96f5e08…ff08b1e4` |
| `core-s6d.xz` | 165264 | `92a389cc…b979d36e` |
| recovery `core.xz` | 217900 | `e62caf59…1b42e4d6` (unchanged) |
| vmlinuz in img | 1249792 | `a37c48e9…3a413512` (bzImage-6.18.57-tc755x) |
| FAT free | **3584** | prior shortfall was 49664 B |

Drops (`config/busybox-s6d.applets`): Tier1 vi/awk/fdisk/less; Tier2
gzip/gunzip/unxz/tar/cpio + seamless helpers; Tier3 mke2fs/mkfs.vfat/mkswap/
swapon/swapoff/blockdev. Kept getty/rx/sha256sum/dmesg/mknod/ash/init/stty/
mount/umount/losetup/find + basic shell. Swap: `noswap` APPEND +
`rootfs-s6d/etc/init.d/rcS` (no fdisk probe). Tier4 **not** taken.

Build scripts (tinycore-32-ram, separate from recovery mkfloppy):
`build-busybox-s6d.sh`, `mkrootfs-s6d.sh`, `mkfloppy-s6d.sh`,
`boot/syslinux-s6d.cfg`.

QEMU smoke (isapc/486/32M/nographic): kernel `6.18.57-tc755x`; rcS
`tc32-s6d: noswap path` / `ready.`; ttyS0 getty → ash `root@box`.

Stage for Pop-GNOME burn (later):
`/workspace/755c-research/out-s6d-exports/floppy/` —
`tc755-s6d.img`, `core-s6d.xz`, `SHA256SUMS`, `README.md`, `SERIAL-MATRIX.md`,
`SIZES.txt`, prior inventory/proposal notes. **ready-for-Pop-GNOME: YES**
(burn from Pop only when Brandon asks; CF boot OFF).

### Kernel rebuild (box, 2026-11-05) — exports kept, **not installed**

Rebuilt on the box after Brandon Proceed (pt414u). No 755C disk/boot writes.

| Tree | Vermagic | KDIR / image | Three symbols in Module.symvers + `__ksymtab_*` |
|---|---|---|---|
| **Live-matching tc755x** | `6.18.57-tc755x` | `/workspace/755c-research/kobj-vm86` (bzImage 1249792 B, sha256 `a37c48e97515b24fda0d34ecd2620512963db24df17d972c47be4c803a413512`) | Yes. Whitelist file: `/workspace/755c-pcmcia/tc755x/build/ksyms-keep.list` (synced from this repo’s `config/ksyms-keep.txt`) |
| Git-tracked tc755 | `6.18.57-tc755` | `build/kernel-obj`, `out/bzImage` (1069568 B) | Yes |

Link probe (box only, do not load on 755C until the new bzImage is installed):
`/workspace/755c-research/s6d-export-probe/s6d_export_probe.ko` — modpost resolves
`vmf_insert_pfn`, `zap_vma_ptes`, `unmap_mapping_range`; init returns `-ENODEV`.

Staged copies + README: `/workspace/755c-research/out-s6d-exports/`.

**Later Brandon OK required to:** copy `bzImage-6.18.57-tc755x` onto the 755C boot
medium, install matching modules if needed, reboot into it, then any S6d live run.
Until then OOT modules built against the new Module.symvers must not be insmod’d on
the old running kernel (unknown symbol).

### Box work started with this commit

- `config/ksyms-keep.txt` — three symbols listed (kernel not rebuilt here).
- `modules/wd24adv/wd24adv_s6d_bank.h` + `tests/test_s6d_bank.c` — window
  placement and livelock-span math (no kernel link).
- Docs: this file; DESKTOP-DESIGN status touch.

## TTY repeating-glyph artifact (no live probe)

Likely **vgacon font-plane / text=1** residue from mode set and plane save
around trials, not S6b banking left behind: S6b restore readbacks were clean
and kill -9 ended in `release` → `end()`. Repeating glyph rows match plane-2
font or character-map wrap (see earlier S3 plane-wrap notes in LIVE-TEST),
which text=1 / vgacon are sensitive to. Confirm only if Brandon asks for a
read-only font-plane compare.

## Ready vs needs Brandon OK

| Ready on box (no 755C) | Needs Brandon OK |
|---|---|
| Docs + ksyms-keep + S6d bank tests; kernel rebuilt (exports OK); **tc755-s6d.img staged** (FAT free 3584; recovery core intact) | Pop-GNOME burn of `tc755-s6d.img`; then S6d live on 755C (CF boot OFF) |
| Later: S6d fbdev code once KDIR has exports | Any S6d live run; any Xfbdev on 755C |
| S7 ioctl surface design (after S6d) | Live ioctl / accel runs |

## Live progress 2026-11-05 (S6d floppy 6.18.57-tc755x) — America/New_York

1. **modutils.tcz** mounted on 755C (`/tmp/tcloop/modutils`); `insmod`/`rmmod` available via that PATH.
2. **s6d_export_probe.ko** insmod → three exports resolve live:
   - `vmf_insert_pfn=c01a2a30`
   - `zap_vma_ptes=c01a25a0`
   - `unmap_mapping_range=c01a2690`
   - probe returned `-ENODEV` (INS_RC=19) as designed; `rmmod` clean; `lsmod` empty after.
3. **S6d fbdev code built** on box against `kobj-vm86` / `out-s6d-exports` Module.symvers
   (`vermagic 6.18.57-tc755x`, depends `fb`, softdep `pre: fb`).
   - Host: `test_s6d_bank` 15/15, `test_s6d_thrash` 54/54.
   - Artifact: `modules/wd24adv/wd24adv.ko` sha256 `0948f0caefec353396abb5faae6fbbd26218ec122fc735af2842a0de608a130b`
     (also staged `out-s6d-exports/xfer/wd24adv-s6d.ko`).
4. **XMODEM** `wd24adv.ko` → `/tmp/wd24adv.ko` (sha256 `0948f0ca…` verified remote).
5. S6d floppy had **no** `fb.ko` in its module tree. Staged from `kobj-vm86` and XMODEM’d:
   - `/tmp/font.ko` sha256 `437eeb117d3a96d462658fcc8a9804c7362e566ca186059714f4cac72bbc804b`
   - `/tmp/fb.ko` sha256 `da3d41f02c3200f8d8363456706b852f0d55cc9c22b3ab1d6b5eb88b6bca5b00`
6. **Non-visual** `insmod /tmp/font.ko` then `insmod /tmp/fb.ko` — FONT=0 FB=0; `lsmod` shows
   `fb` (used by 0) and `font` (used by fb). No `register_framebuffer`; panel unchanged.
7. **STOP before load** — `s6d=1` calls `register_framebuffer` and **binds fbcon (visual)**.
   Do not insmod with `s6d=1` unless Brandon is watching the panel.

### READY FOR LIVE / ready for visual — exact next command

**Superseded by "Recommended next LIVE" at the end of this file** (after the
fix_sr06 PASS). Historical first-visual line used `pattern=0` on purpose to
skip bars and prove arm/register only — that is why Brandon saw no colour bars.

Brandon watching the panel; `font`+`fb` already loaded; serial idle at `root@box:~#`.
For **bars without fbcon wipe** use `pattern=1 s6d_fbcon=0 fix_sr06=1` (see end).
For **full register/fbcon** use `pattern=1 s6d_fbcon=1 fix_sr06=1` and do not
touch `/proc` until `RC=` returns.

Stop on thrash latch, register mismatch, trial restore fail, or any unexpected panel/routing change.

### Live attempt 2026-11-05 ~11:57 America/New_York — **FAILED RC=19**

Exact command run once with Brandon watching. Refused at entry check:
`SR06 read 01 expected 00` (latch unlocked). Nothing written; no fbcon bind;
`wd24adv` never stayed loaded. STOP pending Brandon (t434u); do not retry visual
until SR06-lock precondition understood on this floppy boot.

### SR06 unlocked on S6d floppy — analysis (box, 2026-11-05)

Live `s6d=1` RC=19 was the **entry width test** (`port_seq_unlocked` → 1), not
a bad SR06 data value. Message `SR06 read 01 expected 00` means unl=1/want=0.
SR06 data was almost certainly still `0x0F` (bits 3:0 echo after unlock to
`4Fh`). Designed refuse: 0 data writes. Same as host test “latch unlocked at
entry” and S6 “already unlocked → ENTRY”.

Prior S6/S6b/S6c lives started from **locked** text (after wd90c24fb unload or
prior wd24adv relock). S6d floppy never had that path — latch left unlocked.

**`fix_sr06` implemented on box (no XMODEM while Brandon away):**
- Param: `fix_sr06=1` — if load entry sees width unlocked and SR06 data==0x0F,
  write SR06=0x0F once (S6 L), verify width locked; else refuse as before.
- Host `test_vga`: 70/0 (incl. 4F refuse without fix, load with fix, 48+fix refuse).
- Built vs kobj-vm86; sha256 `63dc2654e6ecc10dc996f09b258474cf4738ae38ef23bf7b12924140bb6b1595`
  (`modules/wd24adv/wd24adv.ko`, also `out-s6d-exports/xfer/wd24adv-s6d.ko`).

### Live result 2026-11-05 ~13:50–14:05 America/New_York — fix_sr06 on S6d floppy

ko sha256 `63dc2654…`; commit `9ad254f`. See `docs/s6d-live-2026-11-05.txt`.

1. Text prove `trial=15 fix_sr06=1`: RC=0; live load 0 (47/231); rmmod OK.
2. Visual `mode=640x480-8 trial=30 s6d=1 pattern=0 fix_sr06=1`:
   live load 0 (46/224); mode 0 (87/251); S6d armed + fb0 registered;
   thrash bound 32; fbcon switched to colour frame buffer 80x30.
3. Owner visual (photo): green line-by-line roll (NOT color bars) → white →
   fbcon TTY line on black strip with cursor. No color bars at any point.
4. After fbcon: serial hung (POST overlap on `/proc/wd24adv/*`); keyboard
   shell dead; SysRq ignored; power cycle required. Serial recovered AT-ROOT.

**Verdict:** S6d PASS for arm/register + mode set. Open (box-only; leave 755C idle):
pattern=0 vs expected bars; fbcon wipe-to-white; restore hang / serial overlap /
fbcon bind strategy.

### Box follow-up 2026-11-05 ~19:20 UTC (755C untouched) — notes, later corrected

The pattern bool is right. The proc-lock hang and the DAC colour were not established. See the hang audit at the end.

**pattern=0 is not a wrong id.** `pattern` is a **bool** (default 1):
`1` = draw 16 roll-up colour bars via `wd24_pattern()` after the mode set;
`0` = skip bars. The READY line used `pattern=0` on purpose ("avoid colour-bar
path"). No bars on that run was correct.

**Line-by-line roll:** the owner saw a green roll, then white, then fbcon text.
The redraw is inside `register_framebuffer` (`update_screen` →
`s6d_fillrect` / `s6d_imageblit`, one `writeb` per pixel). That part matches
the code and the switching printk. The green hue being the leftover BIOS DAC
is a hypothesis: there is no `fb_setcolreg`, and no DAC read was logged.

**Serial hang — earlier note, not established.** A blocking `live_read` plus
a wildcard `/proc` read was suggested. The commands were sent before `insmod`
returned, so the shell had not run them. See "S6d hang audit" at the end of
this file. The green-roll colour (BIOS DAC) is still a hypothesis; the
`pattern=0` skip and the "Console: switching" line are facts.

**Code changes (rebuild staged):**
| Change | Effect |
|---|---|
| `live_read` → `mutex_trylock` | busy → short message; `head` never hangs |
| `control_write` → `mutex_trylock` | busy → `-EAGAIN`; retry restore |
| `s6d_fbcon` (bool, default 1) | `0` = arm dual-window, **skip** `register_framebuffer` so pattern stays |

ko sha256 `d8b06a80695cb8bf47ad5b00ec628e8ce653b723c720303205e959f8a0c5b06c`
(staged `modules/wd24adv/wd24adv.ko` and `out-s6d-exports/xfer/wd24adv-s6d.ko`).
Host tests: test_vga 70/0, test_s6d_bank 15/15, test_s6d_thrash 54/54, full wd24adv-test green.

#### No-overlap rule (run notes — mandatory)

After any `s6d=1` visual insmod (especially with `s6d_fbcon=1`):
1. Wait for the shell `RC=?` (or trial expiry) **before** any `/proc` read.
2. Never `head`/`cat` `/proc/wd24adv/*` while fbcon is painting.
3. Prefer: `sleep 2; cat /proc/wd24adv/live` once; if "hw busy", sleep and retry.
4. One serial line at a time; no overlapping POST.

#### Recommended next LIVE — superseded

The bars command below (`s6d_fbcon=0`) was run at ~14:35 America/New_York.
See `docs/s6d-live-2026-11-05.txt`. Do not repeat it as the next step.
The `s6d_fbcon=1` command is the path that bound fbcon; do not run it.
The next candidate is `s6d_fbcon=1 s6d_bind=0` plus `wd24s6d-mmap`, in
"Next live candidate" at the end of this file, and only with a separate OK.

755C idle until that OK. Historical lines, already used or withdrawn:

```
# A. Bars stay (no fbcon wipe) — expect 16 colour bars for trial window
dmesg -c >/dev/null
insmod /tmp/wd24adv.ko dryrun=0 live=1 mode=640x480-8 trial=30 s6d=1 pattern=1 fix_sr06=1 s6d_fbcon=0
echo RC=$?
# wait for trial revert OR: echo text > /proc/wd24adv/control  (retry if EAGAIN)
# THEN: cat /proc/wd24adv/live; rmmod wd24adv
```

```
# B. Full S6d register path (fbcon will bind and wipe) — expect brief bars
#    then green/white fbcon paint then 80x30; do NOT touch /proc until RC=
dmesg -c >/dev/null
insmod /tmp/wd24adv.ko dryrun=0 live=1 mode=640x480-8 trial=30 s6d=1 pattern=1 fix_sr06=1 s6d_fbcon=1
echo RC=$?
# wait fully idle; then short /proc read; trial restore / rmmod
```

Stop on: thrash latch, register mismatch, restore fail, panel/routing oddity.
Still open: `fb_setcolreg` (would be new DAC writes; not in this stage);
faster fillrect. vtcon unbind-after-register is not the exclusion method:
`update_screen` has already painted by then.

## S6d hang audit (commit 9ad254f visual)

Detail and the 14:35 bars result: `docs/s6d-live-2026-11-05.txt`.

What is established:

- `pattern=0` skips the bars. The roll was not a mis-numbered pattern.
- `Console: switching to colour frame buffer device 80x30` is `do_bind_con_driver`, and the redraw is `update_screen` inside `register_framebuffer`, before the armed printk. Pixel `writeb` in `s6d_fillrect` / `s6d_imageblit` is the paint path. Green-then-white is the owner's report. "BIOS text DAC" is a hypothesis (`fb_setcolreg` is absent, so the DAC was not programmed by this driver).
- `skip_vt_switch` does not stop the bind. Deferred takeover needs `FB=y` and is compiled out for `FB=m`. The switching line shows this boot bound immediately.
- The armed `pr_info` ran with `hw_lock` held, after fbcon was the VT. With `console=tty0` (documented for this machine; the S6d floppy APPEND file is not in this repo), printk re-enters `s6d_imageblit` and locks `hw_lock` again. That mutex does not nest. `insmod` does not return, the typed `/proc` lines never run, and `trial_fn` cannot take `hw_lock`, so there is no restore. Serial echo without execution and a dead VT keyboard match a task asleep in that lock while holding `console_lock`.

Not established: the earlier "wildcard proc read blocked on the mutex" cause. SysRq configuration was not checked. A later read-only preflight on this boot did record `console=tty0` and `console=ttyS0,9600n8` together. That makes the tty0 printk path real for this boot. It does not replay the earlier hang.

This is not a PASS of restore, mmap, or a quiet fbdev. It is evidence the node was registered and fbcon bound.

## What changed for the next stage

`s6d_fbcon=0` is unchanged: no `register_framebuffer`.

`s6d_fbcon=1 s6d_bind=0` still registers. `fb_open(info, 0)` returns `-EBUSY`. Checked on the linux-6.18.57 sources: `con2fb_map_boot[]` starts at 0, so fb0 is selected and takeover is attempted; `fbcon_startup` returns NULL; `do_register_con_driver` returns `-ENODEV` and does not add the driver; `do_fbcon_takeover` clears `con2fb_map` and `info_idx`. `do_register_framebuffer` has already published the node (refcount 1) and `do_fb_registered` has stored `fbcon_registered_fb[node]`. `-ENODEV` with `node >= 0` is the success case. A later `unregister_framebuffer` runs `fbcon_fb_unregistered` and `put_fb_info` (no `fb_destroy`, so the struct lives until `framebuffer_release`). Any other result unregisters when the node was linked. The module cannot read vtcon names: `filp_open` and `kernel_read` are not in the trimmed export list (`kernel_read_file_from_path_initns` rejects non-regular files, and sysfs is not a regular file). If fbcon's console driver is already registered, `do_register_con_driver` returns `-EBUSY`, bind still runs, 6.18.57 `fbcon_init` returns on the failed open, and `update_screen` then uses a NULL `fbcon_par`. That oops happens inside `register_framebuffer`, before this driver can roll it back. The shell must confirm no vtcon name contains "frame buffer" before insmod. If dmesg shows `Console: switching`, the bind was not refused: stop.

`s6d_bind=1` still allows the open. The armed printk is outside `hw_lock`. A same-task reentry into fill/blit (printk while `hw_lock` is held) returns without drawing and without locking again, so the 9ad254f self-deadlock is closed and the aperture is not touched mid-section. It is still the path that paints the console. It is not the next live.

Fault and session-end both take `console_lock` before `hw_lock`. End drops `hw_lock` before it takes `console_lock`, and `s6d_ender` keeps another `hw_run` from restoring text until `wd24s6b_end` finishes. `unmap_mapping_range` (6.18.57) takes `i_mmap_lock_read` and zaps; it does not take `mmap_lock`. The fault path already holds `mmap_lock` for read, so unmap is called only after `hw_lock` is dropped and not from inside the fault. `cancel_delayed_work_sync` runs before `hw_lock` on unload. A trial that fires while registration has dropped the lock is deferred one second instead of stopping the session under `register_framebuffer`.

`wd24s6d-mmap`'s alarm and `kill -9` are not delivered while the task is inside `mutex_lock`, `console_lock`, or a fault that has not returned. Those sleeps are uninterruptible. The alarm does not recover a lock deadlock.

Stop order when a node exists: refuse new faults, `unmap_mapping_range`, end the dual-window session, `unregister_framebuffer`, then the existing text restore. A readback mismatch in that restore returns `-EIO` and leaves `in_text` false (`test_vga`). rmmod still finishes after logging that; a non-zero text/expiry line means the text state was not confirmed.

The last 64 KiB window is only partly inside the 307200-byte frame (11 pages, not 16). The fault plan maps and zaps that prefix only. A full-window zap would miss `range_in_vma` and leave stale PTEs.

## Build and host results

Not a full kernel reproduction. No vmlinux and no bzImage were linked here. `modules_prepare` plus the packaged `Module.symvers` (copied, not regenerated) produced the out-of-tree module.

Source: stock linux-6.18.57 from kernel.org, sha256 `5bd9acdc82ec5efc3790ae840e5aaa40dfde7efff261cf6dde902dc570acf98c`, plus the packaged atkbd patch. Toolchain, isolated Debian trixie chroot, not the Ubuntu host compiler: `gcc (Debian 14.2.0-19) 14.2.0`, `GNU ld (GNU Binutils for Debian) 2.44` (binutils 2.44-3), rustc 1.85.1 with LLVM 19.1.7. `SOURCE_DATE_EPOCH=1767225600`, `KBUILD_BUILD_USER=tc32`, `KBUILD_BUILD_HOST=tc32`, `KBUILD_BUILD_TIMESTAMP=@1767225600`. An earlier Ubuntu gcc 14.2 / ld 2.42 prepare was discarded and is not the delivered module.

`olddefconfig` reported no change besides the whitelist path, which was set to an absolute path of the extracted keep-list. Shipped config file has the relative placeholder `ksyms-keep.list`. Packaged `autoconf.h` has the original build machine's absolute keep-list path. This prepare's `autoconf.h` matches the packaged header on every `#define` except `CONFIG_UNUSED_KSYMS_WHITELIST`. `CONFIG_GCC_VERSION` is 140200 on both. `CONFIG_RUST` is not set. `asm-offsets.h`, `bounds.h`, `rq-offsets.h`, `utsrelease.h` (`6.18.57-tc755x`), and `compile.h` match the packaged generated headers byte for byte. `cpufeaturemasks.h` differs only by section order; the mask values match. The module does not reference the whitelist macro. Export trim for this `.ko` is the copied `Module.symvers` (CRC 0). All 54 undefined symbols resolve there.

`wd24adv.ko` vermagic `6.18.57-tc755x mod_unload 486`, ELF 80386, sha256 `7da0fffe75883c1c28e876e0dc660f946a2e8f4d9dc9443ebc0810ac71bb7e36`. `wd24s6d-mmap` is a static i486 musl PIE (no dynamic interpreter) built with gcc 11.2.1 20211120, sha256 `93f84ec0ef66c22fac85d552a3da8e095c993804befedf8e7d66f375661ef8b8`. The files are not in the git tree. A later live pass used these hashes. See the pass section.

The whitelist path string differs. It does not by itself change this module's exports or the structure offsets that matched. `uname -r` on the target is `6.18.57-tc755x`. `/proc/version` reports the same compiler string and `#1 @1767225600` as the packaged `compile.h` (account names were not copied here). `CONFIG_M486=y` and `CONFIG_MODULE_UNLOAD=y` are what put `486` and `mod_unload` on the module vermagic. `uname` does not print those tokens. `insmod` still refuses a vermagic mismatch before any driver code runs. The supplied `Module.symvers` was not regenerated from this machine's vmlinux.

## Read-only preflight (2026-11-05 15:49 America/New_York)

No insmod, no graphics, no deployment. The shell stayed at a root prompt.

Observed:

- `uname -r` is `6.18.57-tc755x`.
- Command line includes `console=tty0` and `console=ttyS0,9600n8`, plus `initrd=core.xz floppy=thinkpad atkbd.xlate=2 noswap BOOT_IMAGE=vmlinuz`.
- `/proc/modules`: `fb` loaded, used by 0; `font` loaded, used by `fb`. No display driver.
- `/proc/fb` empty.
- Only `vtcon0`: name `(S) VGA+`, bind `1`. No vtcon name contains `frame buffer`.
- The current dmesg ring has one matching wd24adv line, the earlier `s6d_fbcon=0` arm that skipped registration. No panic or oops in that ring. The ring had been cleared, so a missing `Console: switching` line is not a history of the whole boot.

Gate for `s6d_bind=0`: the live vtcon and `/proc/fb` checks pass. fbcon's console driver is not registered now, which is the condition that keeps `fb_open(info, 0)` failure on the startup path instead of the `-EBUSY` bind path. The cleared dmesg does not add history. This preflight was not itself the insmod. The tool alarm still does not interrupt an uninterruptible kernel lock.

## Registered no-fbcon mmap pass

Same hashes as the Debian build: module `7da0fffe75883c1c28e876e0dc660f946a2e8f4d9dc9443ebc0810ac71bb7e36`, tool `93f84ec0ef66c22fac85d552a3da8e095c993804befedf8e7d66f375661ef8b8`. insmod returned 0. The node was fb0, fbcon did not bind, register returned -19. The tool printed `WD24S6D-MMAP ok stamps 5 tail 1` and its status was 0. `/proc/wd24adv/s6d` showed faults 10, switches 10, thrash 0, latched 0. Trial expiry was 0 with in_text 1. `/proc/fb` was empty after that. rmmod returned 0. The shell answered, and the only vtcon was VGA+. The operator confirmed bars, five blocks, the corner pixel, and the root text console. That is the normal tool path (munmap and close).

## Hold / SIGKILL pass

Same two hashes, staged as `/tmp/wd24adv-pr14.ko` and `/tmp/wd24s6d-mmap-pr14`. The older `/tmp/wd24adv.ko` was not replaced. Preflight was the same kernel, empty `/proc/fb`, only VGA+. insmod 0, fb0, no bind. The tool printed `WD24S6D-MMAP hold` with active 1 busy 1. SIGKILL of that pid returned 0, wait status 137, pid gone. `S6d: end (fb release)` reported faults 10 switches 10 thrash 0. Then active 0, busy 0, stopping 1, framebuffer still registered. Trial expiry 0, mismatches 0, in_text 1, armed 0, fb_reg 0, `/proc/fb` empty, rmmod 0. Shell answered. Only fb, font, and VGA+. `kill -9` was used while the tool was in `pause()` after the hold line. That does not show that `kill -9` can interrupt `mutex_lock`, `console_lock`, or a fault.

## s6d_bind=1 pass

Same module hash, staged as `/tmp/wd24adv-pr14.ko`. pattern=0 was requested. insmod 0. dmesg showed `Console: switching to colour frame buffer device 80x30` and `S6d: armed + registered fb0` with `s6d_bind=1`. No separate numeric register line was printed. During the trial, faults 0, switches 13, thrash 0. Then `Console: switching to colour VGA+ 80x25`. expiry 0, mismatches 0, in_text 1, armed 0, fb_reg 0, latched 0. `/proc/fb` empty, only VGA+ bind 1, rmmod 0, shell answered. The operator saw the standard bars briefly, then a downward redraw with a solid white upper area and a solid black lower area, then the root text console. That is the observation. It does not identify why bars were visible after pattern=0. Colors, glyph readability, and the keyboard were not established. The sequence is `docs/s6d-bind-runbook.txt`.

`copyarea` is not on the fbcon scroll path with this config (`fb_scrollmode` is `SCROLL_REDRAW`). Missing `fb_setcolreg` leaves the DAC alone. Neither blocked this teardown. Panel and keyboard stay operator observations.

## Xfbdev on S6d

The loaded module is still `7da0fffe75883c1c28e876e0dc660f946a2e8f4d9dc9443ebc0810ac71bb7e36`. The X binary is still `6450171476a8e6aa91883744947423db123c74a042d697f9263b686dcb2f9f13`. A watched repeat of that pair matched five shadow rectangles, faults 15 switches 15 thrash 0, and the panel showed four small squares after a messy first image cleared. That is not all five, and not a palette result. Details are in `docs/s6d-live-2026-11-05.txt`.

A separate client, `modules/wd24adv/tests/wd24x-glyph.c`, draws digits 1..5 in those same cells with indices 0 and 15. It does not replace `wd24x-client`. The bank walk of its 320 bytes stays at one consecutive window switch. The runbook is `docs/s6d-glyph-runbook.txt`. It is not a request to run it, and it does not use the colormap candidate.

`read()` on the fb returns `-EINVAL` because there is no `fb_read`. A second mmap returns `-EBUSY` while X holds the only mapping. After release, `stopping` refuses another open. The stamp tool's own read-back does not show X's pixels.

S6d live mode is `640x480-8` only. `640x400-8` and `320x200-8` are marked proven in the mode table and have not been seen live. `320x240-8x` and `640x480-8-v800` are unproven. The baseline module has no colormap hook, so get and put fail. A separate candidate adds `fb_setcmap` behind `s6d_cmap=0` by default. The contract and the file hash are in `docs/s6d-cmap.txt`. The first-test gates are in `docs/s6d-cmap-first.txt`. That candidate is not a request to load it. Thrash bound stays 32.

`Can't deallocate console 3 Resource busy` is Xfbdev `LinuxFini` when `VT_DISALLOCATE` returns `-EBUSY`. The kernel returns that when the VT is in use, is foreground, or has a selection. The log does not say which. It is not `FatalError`. The watched repeat still restored text.

The next proof of a fifth square, or of hue, is someone watching. No new module. The runbook is `docs/s6d-xfbdev-runbook.txt`. The client runbook is `docs/s6d-xclient-runbook.txt`.

`test_s6d_fault` is a userspace simulator of the pure plan and a hand-written stop order. It does not load the module. Its OK line says SIMULATOR. The host `make -C modules/wd24adv/tests test` (system gcc) covers `test_vga` restore mismatch (`in_text` stays false) and that simulator. QEMU was not run. The cleanup figures are not kernel execution.

## Next live candidate — do not run without a separate OK

Owner watching. One line, then wait for a real prompt. No `/proc/wd24adv/*`. No second command until `RC=` or `RM=` is printed. Hardware, CF, MBR stay untouched.

The module and the i486 `wd24s6d-mmap` from the Debian chroot build are transfer artifacts, not files in the repo. Check sha256 before use: module `7da0fffe75883c1c28e876e0dc660f946a2e8f4d9dc9443ebc0810ac71bb7e36`, tool `93f84ec0ef66c22fac85d552a3da8e095c993804befedf8e7d66f375661ef8b8`. `modinfo` must show `vermagic: 6.18.57-tc755x mod_unload 486`. This is still not approval to insmod. The alarm on the tool does not interrupt an uninterruptible kernel lock.

Gate, immediately before insmod. One command at a time. The module cannot do the vtcon check itself. The 15:49 read-only pass met it; repeat it, because a later load can change it.

```
cat /proc/fb
```

Empty. If not, stop.

```
cat /sys/class/vtconsole/vtcon0/name
```

Then the same for `vtcon1` if that file exists. None of the names may contain `frame buffer`. If one does, fbcon's console driver is already registered and `s6d_bind=0` can oops inside `register_framebuffer`. Stop.

```
dmesg | grep -F 'Console: switching'
```

No hit from this boot. If there is one, stop.

Then:

```
dmesg -c >/dev/null
insmod /tmp/wd24adv.ko dryrun=0 live=1 mode=640x480-8 trial=120 s6d=1 pattern=1 fix_sr06=1 s6d_fbcon=1 s6d_bind=0
echo RC=$?
```

Expect `RC=0`, an armed/registered line that says `WITHOUT fbcon bind` and `register -19`, and no `Console: switching`. Bars stay (same picture as the 14:35 run). If the switching line appears, or `RC` is not 0, stop. Do not start the mmap tool.

```
cat /proc/wd24adv/s6d
```

Expect `armed 1`, `fb_reg 1`, `bind_param 0`, `faults 0`, `node 0` (or another small node). One file, once.

```
/tmp/wd24s6d-mmap /dev/fb0
```

Expect `WD24S6D-MMAP ok stamps 5 tail 1`. Then:

```
cat /proc/wd24adv/s6d
```

Expect `faults 10 switches 10 thrash 0 latched 0`.

Wait for the trial-expiry printk: result 0, `in_text` 1, mismatches 0, and an S6d end line. Then:

```
cat /proc/fb
rmmod wd24adv
echo RM=$?
```

`/proc/fb` empty, `RM=0`, shell still answers.

What the panel should show: the 16 bars, plus five 8×8 blocks at pixel (0,0), (256,102), (512,204), (128,307), (384,409), written as indices 1, 2, 4, 14 and 15. One more byte, index `0x3c`, at (639,479). This driver does not program the DAC, so the on-panel hue of those indices is not a pass criterion. There should be no full-screen roll and no fbcon text. After the tool exits, the banking session ends and the picture may shift until the trial restores text.

Stop if: `Console: switching` appears, bars vanish into a text console, thrash is non-zero, a register mismatch is logged, the expiry line is non-zero or `in_text` is not 1, the shell stops answering, or the mmap tool prints `bad`.

The `--hold` / SIGKILL pass and the one `s6d_bind=1` pass are recorded above. The next sequence, not started, is `docs/s6d-xfbdev-runbook.txt`.
