# wd24adv: first live test plan (NOT DONE)

Status: **never run on a 755C.** Everything below is a plan. It needs the
owner's explicit approval before step 3. The live backend has been tested
only against a host port model (`make wd24adv-test`) and against QEMU's
standard VGA, where it is refused by the entry check with zero data writes
(`make wd24adv-qemu`). QEMU has no WD90C24, so no live mode set has ever
executed outside the host model.

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

Still not verified in the text state: the SR06 3C4 index-width "locked"
test and AR10 b0 = 0 (the refusal came before both). They stay strict.

## Prerequisites

| # | step | evidence | risk |
|---|---|---|---|
| P1 | Build from the `wd24adv` branch; `make wd24adv-test` (92 + 18 + 52) and `make wd24adv-qemu` (26/26) pass. | H, Q | none |
| P2 | Serial cable to a second machine with a capture program at the console's speed; log everything to a file. | – | none |
| P3 | Serial login on ttyS0 (getty) so the test can be driven while the panel shows graphics. **U**: a serial getty on the 755C image is not verified; if absent, drive from the local keyboard and rely on the trial auto-revert (see "Without serial"). | U | low |
| P4 | Owner's explicit go-ahead for the live load. | – | – |

## Steps

| # | command (as root) | expected | evidence | risk |
|---|---|---|---|---|
| 1 | Stop X: do not start it, or quit it and `pkill Xorg`; check `pidof Xorg` prints nothing. | no X | – | none |
| 2 | If wd90c24fb is loaded: `echo 1 > /sys/module/wd90c24fb/parameters/text`, wait 2 s, `rmmod wd90c24fb`. Check `grep -c wd90c24fb /proc/modules` = 0 and `cat /proc/fb` is empty. Also no vga16fb. | text console back via vgacon | **Q only** (fake=3, `make wd24adv-qemu`); `text=1` on the 755C is **U** (the rmmod restore path itself is what wd90c24fb always runs on unload) | low-medium: if the console does not come back, cold boot and start at step 3 without loading wd90c24fb at all |
| 3 | `dmesg -n 1; printf '\033[9;0]' > /dev/tty1` (no console blanking), then `insmod wd24adv.ko dryrun=0 live=1; dmesg \| tail -20; cat /proc/wd24adv/live` | `LIVE: text state saved (4 planes, DAC, VGA + ext regs); trial 15 s`, `live load: 0 (port data writes 46, index writes 224, mismatches 0)` (model counts; real counts may differ slightly). The panel may blink once (screen off during the plane copy). If the chip is not in the expected state: `entry check: ...` and `REFUSED by the entry check: no data register written`, insmod fails `No such device` (twice in dmesg: busybox insmod retries). | H (model identical after load); Q (refusal, zero data writes); the load's data writes are value-identical idioms + the vgastate plane-copy sequence that wd90c24fb's load already runs on the 755C (L) | low |
| 4 | `rmmod wd24adv; dmesg \| tail` | `live text: 0 (... mismatches 0)`; text console unchanged | H | low |
| 5 | Load again (step 3), then `echo "mode 640x480-8" > /proc/wd24adv/control` and **wait**, typing nothing. | 16 colour bars at the top of the panel for 15 s, then text returns by itself: `live trial expiry: 0`. Redraw the text with `clear` or Alt+F2/Alt+F1 if the screen shows stale content. | H (round trip identical, trial revert); register values identical to wd90c24fb 640x480x8 (xcheck), which runs live (L); the standard-VGA/plane restore on real hardware is **U** | medium |
| 6 | `echo "mode 640x480-8" > /proc/wd24adv/control; sleep 3; echo confirm > /proc/wd24adv/control`, look, then `echo text > /proc/wd24adv/control` | bars stay after 15 s; `text` restores; `cat /proc/wd24adv/live` shows `-> 0` | H | medium |
| 7 | `rmmod wd24adv` | text restored (no-op if already text) | H | low |
| 8 | Later, separate sessions: `640x400-8`, then `320x200-8` (13h; no pattern, shows whatever is in memory). | centred picture for 15 s, then revert | H only; both never seen live (**U**) | medium |

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
  unloads (L). In a fresh BIOS text state (wd90c24fb never loaded) it is
  **U**; if it differs, the load is refused with zero writes.
- **Readback masks** (`rb_mask()` in `wd24adv_core.c`): only CR34, SR35 and
  the SR06 latch readbacks are proven live. Others are assumptions; a wrong
  mask shows up as a `MISMATCH` and the plan stops and restores (fail-safe,
  but it would block the test).
- **SR06 latch** index-width test: from live probes (L); the SR06 value
  reads 0Fh in the text state after wd90c24fb unloads (L), but the
  index-width test there is **U** (same check refuses if different).
- **wd90c24fb `text=1` + rmmod** before the test: QEMU (fake=3) and live
  (L, see "Live observations").
- **Serial getty**: a root shell on ttyS0 drove the first attempt (L).
