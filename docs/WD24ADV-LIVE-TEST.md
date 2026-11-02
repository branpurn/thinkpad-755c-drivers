# wd24adv: first live test plan (steps 1-5 done live; 640x480-8 PROVEN live, 2026-11-02)

Status: steps 1-5 (load, unload, one `640x480-8` trial with auto-revert)
have run on the 755C: step 5 three times, the last with the full-frame
pattern (ccfb5a9), confirmed on the panel by the owner. **`640x480-8` is
PROVEN live**, including the full 300 KiB frame and scan-out above 256 KiB.
**`640x400-8` is PROVEN live** too (one run with auto-revert; owner: bars,
letterboxed). Step 6 (`confirm`) and `320x200-8` have not run. Each live
step needs the owner's explicit approval. Off the 755C the live backend is
tested against a host port model (`make wd24adv-test`) and QEMU's standard
VGA, where the entry check refuses with zero data writes
(`make wd24adv-qemu`; QEMU has no WD90C24).

Legend for evidence: **H** = host model test, **Q** = QEMU test, **L** =
seen live on the 755C (earlier probes / wd90c24fb), **U** = unproven.

## What can go wrong, and why recovery is a cold boot at worst

wd24adv never writes CMOS, PM ports 24h-27h, 0D00h/0D01h, PR18/PR19 data,
CR35/CR3A, the shadow CRTC (CR34=06), panel power or the aperture; the
guard refuses these (H, xcheck). Every register it writes is volatile and
reprogrammed by the BIOS at POST. So the worst case is a wrong picture or
a hung machine until a **cold boot** (power off, wait 10 s, power on);
nothing persists. A blank or garbled panel for a few seconds is the
expected failure mode, not panel damage: the panel route (PR18/PR19) and
panel power are not touched.

## Live observations so far (755C, 2026-11-02)

First attempt at steps 1-3 (load only, no mode command):

- Step 1: X on this image is `Xfbdev` (started by `startx`, not respawned);
  stopping it by its PID left tty1 active. **L**
- Step 2: `text=1` then `rmmod wd90c24fb` worked: `text=1: back to text
  mode`, `Console: switching to colour VGA+ 80x25`, no fb left. **L**
- Text state after step 2 (wd24probe3 header reads): 3CC=67, GR0F=00,
  GR0E=40, CR11=AE, CR29=85, SR06=0F (latch register value). **L**
  (wd24probe3 gates on the wd90c24fb-live GR0F=05 (`wd24p3_seq.h` 140-142)
  and stops at `not-live-refused` before any CR34 or 3C4-width access, so
  its `CR34 ... =00`, `3C4test ... =00` and `final ... =00` fields in this
  state are the static struct's zeros, not reads; trace=20 = exactly the
  entry, gate and index-restore accesses.)
  So wd90c24fb's unload relock leaves GR0F/CR29 = **00/85**, not the 00/00
  of the POST trace.
- Step 3: the entry check refused that state (`GR0F/CR29 ... not 00/00 or
  05/85`), `live load: -19 (port data writes 0, index writes 9)`, insmod
  `No such device`, nothing written, registers unchanged afterwards. **L**
  Because the check got as far as GR0F/CR29, CR34 read A0h twice and PR18 =
  C7h, PR19 = 54h in this text state too. **L**
- Fix: the load entry check now also accepts 00/85 (host tests: core
  self-test and the port-model round trip from 3CC=67 GR0F=00 GR0E=40
  CR11=AE CR29=85). **H**

Retry with the fix (301080b), steps 3-4: entry check passed in this text
state, including the SR06 3C4 index-width "locked" test and AR10 b0 = 0;
`live load: 0 (port data writes 46, index writes 224, mismatches 0)`
(= model); rmmod `live text: 0`, 0 mismatches; wd24probe3 header
identical before/after. **L**

Step 5, twice (17:35 and 17:49 UTC; the second time watched on the panel):
- `live mode 640x480-8: 0 (port data writes 87, index writes 251,
  mismatches 0)`: exactly the host model's counts. **L**
- `live trial expiry: 0 (port data writes 896, index writes 336,
  mismatches 0)` after 15 s, no confirm; rmmod `live text: 0 (896, 336,
  0)`; 3CC/GR0F/GR0E/SR06/CR11/CR29 and indexes identical before/after;
  the normal text console came back by itself. **L**
- Panel: 16 colour bars over the top ~1/5 (the pattern fills only bank 0:
  65280 bytes = 102 lines), so chain-4 + doubleword scan-out, CR13 pitch
  640, start address 0 and the 8-bit PEL path are as modelled. **L**
- Below the bars: the old wd90c24fb fbcon picture (stale video memory,
  not text-mode data), mostly legible. Lines up to ~409 lie in the 4 x 64
  KiB planar window; wd90c24fb's unload `restore_vga()` rewrote plane 3
  offsets 0-64K, plane 2 0-32K and plane 1 16K-24K there, i.e. every
  4th pixel column (x mod 4 = 3, and = 2 / = 1 in part of it), which
  explains "mostly". The bottom ~1/5 of that text (from about line 410 =
  linear 256 KiB, which no text-mode, vgastate or wd24adv access can
  reach) was garbled: the owner saw it as **shifted text**, and there
  were **no bars at the bottom** (so the CRTC did not wrap to 0 at
  256 KiB). Shifted text suggests stale fbcon content (e.g. from an
  earlier scroll position) rather than lost memory; the cause is **U**
  (not a write by any of this code); the full-frame run below overwrites
  that memory, so it no longer matters.

Step 5 with the full-frame pattern (ccfb5a9, module sha256 af7e3f19...,
watched on the panel by the owner):
- Pre-check (wd24probe3): 3CC=67 GR0F=00 GR0E=40 SR06=0F CR11=AE CR29=85,
  indexes 0F/0F/01. Load: `live load: 0 (46, 224, mismatches 0)`. **L**
- Mode write ~18:28:08 UTC: `live mode 640x480-8: 0 (port data writes 93,
  index writes 263, mismatches 0)`, exactly the model's counts (87/251 +
  6 PR0A data writes + 12 index writes for the banked pattern). **L**
- Auto-revert ~18:28:23 UTC (15 s, no confirm): `live trial expiry: 0
  (896, 336, mismatches 0)`; rmmod `live text: 0 (896, 336, 0)`. **L**
- After: 3CC/GR0F/GR0E/SR06/CR11/CR29 and indexes identical to the
  pre-check; text console on tty1, no wd24/wd90/vga16 module loaded. **L**
- Panel (owner): "Panel looked great": bars over the whole frame, clean
  revert. So PR0A banking in chain-4 (banks 0-4) and scan-out of video
  memory above 256 KiB (lines ~410-479) work as modelled; no wrap. **L**

Step 8, `640x400-8` once with auto-revert (same ccfb5a9 module, sha256
af7e3f19..., owner watching):
- Box check first: the plan writes exactly the 640x480-8 register set;
  the only differences are MISC 63 (E3 for 640x480; 63 = same 25 MHz clock,
  mode 13h sync polarity) and the standard 400-line verticals CR06 BF,
  CR07 1F, CR10 9C, CR11 8E, CR12 8F, CR15 96, CR16 B9, plus one PR0A
  bank write fewer (the 256000-byte frame is banks 0-3, all inside the
  planar window that is saved and restored). No PR18/PR19, shadow CRTC,
  PR41/PR64 or other panel register is written: centring is the BIOS
  PR19 = 54h as-is. Model: mode 92/261, revert 896/336, 0 mismatches.
- Pre-check (wd24probe3): 3CC=67 GR0F=00 GR0E=40 SR06=0F CR11=AE CR29=85,
  indexes 0F/0F/01. Load: `live load: 0 (46, 224, mismatches 0)`. **L**
- Mode write ~18:42:06 UTC: `live mode 640x400-8: 0 (port data writes 92,
  index writes 261, mismatches 0)`, exactly the model's counts. **L**
- Auto-revert ~18:42:21 UTC (15 s, no confirm): `live trial expiry: 0
  (896, 336, mismatches 0)`; rmmod `live text: 0 (896, 336, 0)`. **L**
- After: registers and indexes identical to the pre-check; tty1, no
  wd24/wd90/vga16 module loaded, no X. **L**
- Panel (owner): "16 colored bars, just bars, letterboxed": the 400-line
  frame is shown whole, centred with black bands (PR19 = 54h centring, no
  stretch), no stale content. **640x400-8 PROVEN live.** **L**

## Prerequisites

| # | step | evidence | risk |
|---|---|---|---|
| P1 | Build from the `wd24adv` branch; `make wd24adv-test` (92 + 18 + 55) and `make wd24adv-qemu` (26/26) pass. | H, Q | none |
| P2 | Serial cable to a second machine with a capture program at the console's speed; log everything to a file. | – | none |
| P3 | Serial login on ttyS0 (getty) so the test can be driven while the panel shows graphics. A root shell on ttyS0 drove all live steps so far (L). | L | low |
| P4 | Owner's explicit go-ahead for the live load. | – | – |

## Steps

| # | command (as root) | expected | evidence | risk |
|---|---|---|---|---|
| 1 | Stop X: do not start it, or quit it and `pkill Xorg`; check `pidof Xorg` prints nothing. | no X | – | none |
| 2 | If wd90c24fb is loaded: `echo 1 > /sys/module/wd90c24fb/parameters/text`, wait 2 s, `rmmod wd90c24fb`. Check `grep -c wd90c24fb /proc/modules` = 0 and `cat /proc/fb` is empty. Also no vga16fb. | text console back via vgacon | Q (fake=3, `make wd24adv-qemu`); **L** (2026-11-02) | low-medium: if the console does not come back, cold boot and start at step 3 without loading wd90c24fb at all |
| 3 | `dmesg -n 1; printf '\033[9;0]' > /dev/tty1` (no console blanking), then `insmod wd24adv.ko dryrun=0 live=1; dmesg \| tail -20; cat /proc/wd24adv/live` | `LIVE: text state saved (4 planes, DAC, VGA + ext regs); trial 15 s`, `live load: 0 (port data writes 46, index writes 224, mismatches 0)` (model counts; seen exactly live, **L**). The panel may blink once (screen off during the plane copy). If the chip is not in the expected state: `entry check: ...` and `REFUSED by the entry check: no data register written`, insmod fails `No such device` (twice in dmesg: busybox insmod retries). | H (model identical after load); Q (refusal, zero data writes); the load's data writes are value-identical idioms + the vgastate plane-copy sequence that wd90c24fb's load already runs on the 755C (L) | low |
| 4 | `rmmod wd24adv; dmesg \| tail` | `live text: 0 (... mismatches 0)`; text console unchanged | H, **L** | low |
| 5 | Load again (step 3), then `echo "mode 640x480-8" > /proc/wd24adv/control` and **wait**, typing nothing. | 16 colour bars over the whole panel for 15 s (from 256 KiB, about line 410 down, in reverse order), `live mode 640x480-8: 0 (port data writes 93, index writes 263, mismatches 0)` (model; 87/251 with the bank-0-only pattern), then text returns by itself: `live trial expiry: 0`. Redraw the text with `clear` or Alt+F2/Alt+F1 if the screen shows stale content. | **L, PROVEN live**: full-frame pattern (ccfb5a9) seen on the panel, 93/263 and 896/336, 0 mismatches, registers identical (2026-11-02); earlier bank-0-only runs (301080b) 87/251 | medium |
| 6 | `echo "mode 640x480-8" > /proc/wd24adv/control; sleep 3; echo confirm > /proc/wd24adv/control`, look, then `echo text > /proc/wd24adv/control` | bars stay after 15 s; `text` restores; `cat /proc/wd24adv/live` shows `-> 0` | H | medium |
| 7 | `rmmod wd24adv` | text restored (no-op if already text) | H | low |
| 8 | Later, separate sessions: `640x400-8`, then `320x200-8` (13h; pattern: 16 bars of 20 px over all 64000 bytes of the A0000 window, no register write; `live mode 320x200-8: 0 (port data writes 90, index writes 258, mismatches 0)` (model), revert 896/336). | centred picture for 15 s (320x200 pixel-doubled to 640x400, letterboxed), then revert | 640x400-8: **L, PROVEN live** (counts = model, 0 mismatches, owner saw 16 bars letterboxed, 2026-11-02); 320x200-8: H only (**U**) | medium |

After every step: `dmesg | grep wd24adv` and `cat /proc/wd24adv/live`
into the serial log.

## Abort and revert

1. Picture wrong / panel blank after a mode set: **do nothing for 15 s**;
   the trial reverts to text.
2. Still wrong: over serial `echo text > /proc/wd24adv/control`, then
   `rmmod wd24adv` (both restore).
3. If a command returns `-EIO` (readback mismatch) the plan has already
   stopped and restored; `mode` is refused from then on. Save
   `/proc/wd24adv/live` and dmesg, then `rmmod wd24adv`.
4. A load that failed after writing leaves the module loaded but **inert**
   (restored; rmmod writes nothing). Save the log, rmmod.
5. Machine hung or no serial response: **cold boot** (hold power until off,
   wait 10 s, power on). Nothing persistent was written.

## Without serial

Drive from the local keyboard on tty1. Type the whole command line before
pressing Enter, e.g.
`echo "mode 640x480-8" > /proc/wd24adv/control; sleep 20; dmesg | tail -20 > /tmp/wd24.log`.
The trial brings text back after 15 s. Do not use `confirm` without serial.

## Known interactions (unproven)

- **vgacon while in a graphics mode.** Plans run under `console_lock()`,
  but between them vgacon still owns tty1. With `dmesg -n 1`, blanking off
  and no typing on tty1 it should not write; if it does it may touch CR0A-
  CR0F (cursor/start address), which moves or corrupts the picture but not
  the panel route. Its B8000h text writes are not decoded while GR06 maps
  A0000h. The restore writes the saved values in any case. **U**
- **CR34 readback.** The entry check requires CR34 to read A0h with no write
  first: seen in the wd90c24fb state and in the text state after wd90c24fb
  unloads (L: wd24adv's own entry-check reads, `wd24adv_core.c` 397-401,
  at GR0F/CR29/SR06 = 00/85/0F, before any write; wd24probe3 never reads
  CR34 in that state). In a fresh BIOS text state (wd90c24fb never loaded) it is
  **U**; if it differs, the load is refused with zero writes.
- **Readback masks** (`rb_mask()` in `wd24adv_core.c`): only CR34, SR35 and
  the SR06 latch readbacks are proven live. Others are assumptions; a wrong
  mask shows up as a `MISMATCH` and the plan stops and restores (fail-safe,
  but it would block the test).
- **SR06 latch** index-width test: from live probes (L); passed in the
  text state after wd90c24fb unloads (L, 301080b load).
- **Video memory above 256 KiB** (lines ~410-479 at 640x480x8): outside
  the planar window, so never saved or restored (not visible in text
  mode). It showed shifted stale text in the 301080b test (**U** why);
  the full-frame pattern overwrites it and its scan-out is **L**
  (ccfb5a9).
- **wd90c24fb `text=1` + rmmod** before the test: QEMU (fake=3) and live
  (L, see "Live observations").
- **Serial getty**: a root shell on ttyS0 drove the first attempt (L).
