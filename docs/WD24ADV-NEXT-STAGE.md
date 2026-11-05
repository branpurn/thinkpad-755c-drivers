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
| Docs + ksyms-keep list + S6d bank host tests; **kernel rebuilt on box** (tc755x kobj-vm86 + tc755) with three exports verified | Install/flash new bzImage on 755C + reboot; then S6d live |
| Later: S6d fbdev code once KDIR has exports | Any S6d live run; any Xfbdev on 755C |
| S7 ioctl surface design (after S6d) | Live ioctl / accel runs |
