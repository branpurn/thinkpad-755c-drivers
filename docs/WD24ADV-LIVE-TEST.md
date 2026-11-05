# wd24adv: first live test plan (steps 1-5 done live; 640x480-8, 640x400-8, 320x200-8 PROVEN live, 2026-11-02)

Status: steps 1-5 (load, unload, one `640x480-8` trial with auto-revert)
have run on the 755C: step 5 three times, the last with the full-frame
pattern (ccfb5a9), confirmed on the panel by the owner. **`640x480-8` is
PROVEN live**, including the full 300 KiB frame and scan-out above 256 KiB.
**`640x400-8` is PROVEN live** too (one run with auto-revert; owner: bars,
letterboxed). **`320x200-8` is PROVEN live** (one run with auto-revert, 5f275b4:
counts = model, 0 mismatches, clean revert; owner: "16 bars letterboxed
looks good"). Step 6
(`confirm`) has not run. Each live
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

Step 8, `320x200-8` (13h) once with auto-revert (module 5f275b4, sha256
7861b185..., 47368 bytes, sha verified on the 755C after XMODEM; owner
watching):
- New in 5f275b4: the 13h pattern, 16 bars (colours 0-15 via the text AR
  map, 20 px each) over all 64000 bytes of the A0000 window, chain-4,
  no PR0A/bank write and no register write (refused if BIOS PR0A != 0).
  Model: mode 90/258, pattern 64000 memory writes and 0 port writes,
  revert 896/336, model identical after revert. **H**, **Q**
- Pre-check (wd24probe3): 3CC=67 GR0F=00 GR0E=40 SR06=0F CR11=AE CR29=85,
  indexes 0F/0F/01, RC=19, trace=20. Load: `live load: 0 (46, 224,
  mismatches 0)`. **L**
- Mode write ~19:18:27 UTC: `live mode 320x200-8: 0 (port data writes 90,
  index writes 258, mismatches 0)`, exactly the model's counts. **L**
- Auto-revert ~19:18:42 UTC (15 s, no confirm): `live trial expiry: 0
  (896, 336, mismatches 0)`; rmmod `live text: 0 (896, 336, 0)`. **L**
- After: registers and indexes identical to the pre-check; tty1, no
  wd24/wd90/vga16 module loaded, no X. **L**
- Panel (owner): "16 bars letterboxed looks good": the 13h frame is
  shown whole, double-scanned and centred with black bands (BIOS PR19
  centring, no stretch). **320x200-8 PROVEN live.** **L**

## Prerequisites

| # | step | evidence | risk |
|---|---|---|---|
| P1 | Build from the `wd24adv` branch; `make wd24adv-test` (92 + 18 + 64 + 153 + 67) and `make wd24adv-qemu` (26/26) pass. | H, Q | none |
| P2 | Serial cable to a second machine with a capture program at the console's speed; log everything to a file. | – | none |
| P3 | Serial login on ttyS0 (getty) so the test can be driven while the panel shows graphics. A root shell on ttyS0 drove all live steps so far (L). | L | low |
| P4 | Owner's explicit go-ahead for the live load. | – | – |

## Steps

| # | command (as root) | expected | evidence | risk |
|---|---|---|---|---|
| 1 | Stop X: do not start it, or quit it: `pidof Xfbdev` gives its PID, then `kill <that PID>` (the exact PID; never `pkill`/`pkill -f`); check `pidof Xfbdev` prints nothing. | no X | – | none |
| 2 | If wd90c24fb is loaded: `echo 1 > /sys/module/wd90c24fb/parameters/text`, wait 2 s, `rmmod wd90c24fb`. Check `grep -c wd90c24fb /proc/modules` = 0 and `cat /proc/fb` is empty. Also no vga16fb. | text console back via vgacon | Q (fake=3, `make wd24adv-qemu`); **L** (2026-11-02) | low-medium: if the console does not come back, cold boot and start at step 3 without loading wd90c24fb at all |
| 3 | `dmesg -n 1; printf '\033[9;0]' > /dev/tty1` (no console blanking), then `insmod wd24adv.ko dryrun=0 live=1; dmesg \| tail -20; cat /proc/wd24adv/live` | `LIVE: text state saved (4 planes, DAC, VGA + ext regs); trial 15 s`, `live load: 0 (port data writes 46, index writes 224, mismatches 0)` (model counts; seen exactly live, **L**). The panel may blink once (screen off during the plane copy). If the chip is not in the expected state: `entry check: ...` and `REFUSED by the entry check: no data register written`, insmod fails `No such device` (twice in dmesg: busybox insmod retries). | H (model identical after load); Q (refusal, zero data writes); the load's data writes are value-identical idioms + the vgastate plane-copy sequence that wd90c24fb's load already runs on the 755C (L) | low |
| 4 | `rmmod wd24adv; dmesg \| tail` | `live text: 0 (... mismatches 0)`; text console unchanged | H, **L** | low |
| 5 | Load again (step 3), then `echo "mode 640x480-8" > /proc/wd24adv/control` and **wait**, typing nothing. | 16 colour bars over the whole panel for 15 s (from 256 KiB, about line 410 down, in reverse order), `live mode 640x480-8: 0 (port data writes 93, index writes 263, mismatches 0)` (model; 87/251 with the bank-0-only pattern), then text returns by itself: `live trial expiry: 0`. Redraw the text with `clear` or Alt+F2/Alt+F1 if the screen shows stale content. | **L, PROVEN live**: full-frame pattern (ccfb5a9) seen on the panel, 93/263 and 896/336, 0 mismatches, registers identical (2026-11-02); earlier bank-0-only runs (301080b) 87/251 | medium |
| 6 | `echo "mode 640x480-8" > /proc/wd24adv/control; sleep 3; echo confirm > /proc/wd24adv/control`, look, then `echo text > /proc/wd24adv/control` | bars stay after 15 s; `text` restores; `cat /proc/wd24adv/live` shows `-> 0` | H | medium |
| 7 | `rmmod wd24adv` | text restored (no-op if already text) | H | low |
| 8 | Later, separate sessions: `640x400-8`, then `320x200-8` (13h; pattern: 16 bars of 20 px over all 64000 bytes of the A0000 window, no register write; `live mode 320x200-8: 0 (port data writes 90, index writes 258, mismatches 0)` (model), revert 896/336). | centred picture for 15 s (320x200 pixel-doubled to 640x400, letterboxed), then revert | 640x400-8: **L, PROVEN live** (counts = model, 0 mismatches, owner saw 16 bars letterboxed, 2026-11-02); 320x200-8: **L, PROVEN live** (counts = model, 0 mismatches, owner saw 16 bars letterboxed, 2026-11-02) | medium |

After every step: `dmesg | grep wd24adv` and `cat /proc/wd24adv/live`
into the serial log.

## S1: read-only 23C0h probe (run 2026-11-02: OK)

Module `wd24adv.ko` from the `wd24adv` branch, `s1=1`, **built against the
running 6.18.57-tc755x kernel tree** (vermagic `6.18.57-tc755x mod_unload 486`;
a `build/kernel-obj` build is 6.18.57-tc755 and is refused, RC=8). Text state, no fb
driver (wd90c24fb `text=1` + rmmod first, or never loaded), no X.

Port writes, exactly, in order (host model; QEMU stops at CR34):
1. entry check (`wd24_entry_check`, shared with the live load), 14 VGA
   **index** writes, 0 data writes: 3D4<-34, 3D4<-34, 3D4<-31, 3D4<-32,
   3CE<-0F, 3D4<-29, 3C4<-06, 3C4<-11, 3C4<-06 (SR06 latch test), 3C0<-10,
   3C0<-20 (AR index, PAS kept), then the saved 3C4/3CE/3D4 indices
   (model: 01, 06, 0E). If it refuses, S1 does not run (0 23Cxh accesses).
2. 51 16-bit writes to 23C0h, nothing else: 1001 1101 ... 1F01, 1002
   1102 ... 1802 (pass 1), the same 25 (pass 2), then saved & 1FFFh.
   On a stop: the selects so far + the restore.

Reads only 23C0h/23C2h (16-bit) and 3C0h/3C1h. Never 23C2h/23C4h writes,
CNTRL1 start, or panel/PM/routing registers.

| # | command (as root, ttyS0) | expected |
|---|---|---|
| 1 | copy `wd24adv.ko` over; `sha256sum wd24adv.ko` | the recorded sha |
| 2 | `grep -cE 'wd90c24fb\|vga16fb' /proc/modules; cat /proc/fb; pidof Xfbdev` | 0, empty, nothing |
| 3 | `insmod wd24adv.ko dryrun=0 s1=1` | returns 0 (the result is held, not the insmod code) |
| 4 | `cat /proc/wd24adv/s1; dmesg \| grep wd24adv` | `S1 result: OK` (or REFUSED/STOPPED/UNSTABLE/RESTORE_BAD with the reason) |
| 5 | `rmmod wd24adv` | no port access |
| 6 | `insmod wd24adv.ko dryrun=0 s1=1; cat /proc/wd24adv/s1; rmmod wd24adv` | only if 4 was OK: same S at entry, same values |

Any result other than OK: stop, save the log, cold boot if the text
console looks wrong.

## S2: write + readback, start bit never set (snapshot design; run OK 2026-11-04, docs/s2-live-2026-11-04-run2.txt)

Module `wd24adv.ko`, `s2=1 dryrun=0`, tc755x build (vermagic `6.18.57-tc755x
mod_unload 486`). Same state as S1: text, no fb driver, no X. Runs once
inside insmod and puts everything back before insmod returns (nothing stays
changed; rmmod touches no port). Deviation from the design's S2 row
("640x480-8 under trial"): run in the text state, like S1.

Why a snapshot: the first live S2 run (docs/s2-live-2026-11-04-run.txt)
stopped NOT REFERENCE at SRC_LO (2800, S1 had 210b): the register file is
not the same across boots. S2 now restores the values it read in the same
load instead of the S1 reference.

Sequence (`wd24adv_s2.[ch]`; exact order checked by `tests/test_s2.c` and QEMU):
1. entry check (as S1; 0 data writes). Refused: 0 23Cxh accesses.
2. R 23C0h twice, 3C0h/3C1h (S1 rules) and 23C0h must be 1002: else no write.
3. snapshot: 25 selects + reads (block 1 0-F, block 2 0-8), twice. Refused
   (SNAPSHOT REFUSED, selects only, no 23C2h write, 23C0h back to 1002) if
   the passes differ, a word does not echo its index, CNTRL1 has ACT (0800)
   set or cursor control has enable (0800) set. Snapshot logged in /proc.
4. per register in the table: W 23C0h sel, R == sel; W 23C2h test, R 23C0h
   == sel, R 23C2h == test; W 23C2h snapshot value, R == snapshot; W 23C0h
   1001 / 1002, R, R 23C2h == snapshot CNTRL1 / cursor control.
5. post-pass: all 25 == snapshot, then W 23C0h 1002, R == 1002.

Any mismatch: stop; write the snapshot value to every touched register (one
write each, read back); post-pass; 23C0h = 1002; result MISMATCH or
RESTORE_BAD. No retries. Totals: 110 23C0h + 34 23C2h writes.

Guard: a 23C2h write is allowed only if it is the table's test word for the
selected register, or (after a good snapshot in this same load) exactly that
register's snapshot value. The old S1-reference allowance is removed.

| # | reg | sel | test write | restore (S1 ref, for info) | why the test value is safe |
|---|---|---|---|---|---|
| 1 | SRC_LO | 1201 | 2000 | 210b | address only; no start |
| 2 | SRC_HI | 1301 | 304c | 3004 | 4C000h scratch (guard 0-ff) |
| 3 | DST_LO | 1401 | 4000 | 410b | address only |
| 4 | DST_HI | 1501 | 504c | 5004 | scratch 4C000h |
| 5 | DIM_X | 1601 | 6008 | 61e4 | 8 px (guard 1..2048) |
| 6 | DIM_Y | 1701 | 7008 | 713c | 8 lines |
| 7 | PITCH | 1801 | 8280 | 8280 | guard set {280}: same value, weak proof |
| 8 | ROP | 1901 | 9600 | 9300 | guard set (copy 300 / xor 600) |
| 9 | FG | 1A01 | a05a | a000 | colour latch |
| 10 | BG | 1B01 | b05a | b0a5 | colour latch |
| 11 | MASK | 1E01 | e0ff | e0ff | guard set {ff}: same value, weak proof |
| 12 | CNTRL2 | 1101 | 1008 | 1000 | guard set {000,008}; b7 quick start never |
| 13 | CUR primary | 1302 | 305a | 3001 | colour; cursor off |
| 14 | CUR secondary | 1402 | 40a5 | 4000 | colour; cursor off |
| 15 | CUR origin | 1502 | 5000 | 5041 | hotspot, mask 7df; cursor off |
| 16 | CUR pos X | 1602 | 6010 | 617a | 0..639; cursor off |
| 17 | CUR pos Y | 1702 | 7010 | 7047 | 0..479; cursor off |

Never written: CNTRL1 (1001, holds ACT/start), block 1 C/D/F, cursor control
(1002, holds enable), cursor pattern address (1102/1202: references 000/014
are outside the guard set {400}/{13}), block 2 idx 8; 23C4h never accessed.
CNTRL1 and cursor control are re-read after every register.

| # | command (as root, ttyS0) | expected |
|---|---|---|
| 1 | copy over; `sha256sum /tmp/wd24adv.ko` | the recorded sha |
| 2 | `pidof Xfbdev`, then `kill <PID>`; wd90c24fb `text=1` + rmmod | no X, no fb |
| 3 | gate line, then `insmod /tmp/wd24adv.ko dryrun=0 s2=1` | RC=0 |
| 4 | `cat /proc/wd24adv/s2 > /tmp/s2run1.txt; rmmod wd24adv` | `S2 result: OK`, snapshot logged, restore read 1002 |
| 5 | wd90c24fb back, startx; wd24probe3 baseline | RC=19, same values as before |

Build: commit 8f9fcc6 (code), tc755x tree (kobj-vm86), sha256 `f55ee02fbcd3d513006ee92b1996d13e07c507567678d201d87a98d36c6fa89c`.
Exact serial lines (each logged with plog.sh first; no retries):

```
pidof Xfbdev; ps | grep -E "startx|Xfbdev|jwm|rxvt"
kill <the Xfbdev PID printed above>
echo 1 > /sys/module/wd90c24fb/parameters/text; sleep 2; rmmod wd90c24fb
S=$(sha256sum /tmp/wd24adv.ko|cut -c1-8); N=$(grep -cE "^(wd90c24fb|vga16fb|wd24adv) " /proc/modules); F=$(wc -l </proc/fb); pidof Xfbdev >/dev/null; X=$?; echo G=$S$N$F$X; [ $S$N$F$X = f55ee02f001 ] && { dmesg -c >/dev/null; insmod /tmp/wd24adv.ko dryrun=0 s2=1; echo RC=$?; }
cat /proc/wd24adv/s2 > /tmp/s2run1.txt; grep -E "S2 result|snapshot|restore:|writes|stopped" /tmp/s2run1.txt; dmesg | grep wd24adv
rmmod wd24adv; echo RM=$?
insmod /usr/local/lib/modules/6.18.57-tc755x/extra/wd90c24fb.ko; echo RC=$?
( PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; HOME=/root; export PATH HOME; ( sleep 5; exec startx ) </dev/null >/tmp/xautostart.log 2>&1 & )
dmesg -c >/dev/null; insmod /tmp/wd24probe3.ko; echo RC=$?; dmesg | grep wd24probe3 | grep -v " tr " | sort -u
```

Any result other than OK: stop, save the log, cold boot if the text
console looks wrong.

## S3: first BITBLT, off screen (prepared 2026-11-04, not run)

`insmod wd24adv.ko dryrun=0 live=1 mode=640x480-8 s3=1`: after the PROVEN
640x480-8 trial mode set and test pattern (15 s trial timer already queued),
inside the same insmod, three BITBLT operations in the off-screen scratch
bank 60000h-6FFFFh (PR0A 60h), each verified by reading video memory back:

| op | what | CNTRL1 |
|---|---|---|
| 1 | FILL 8x8 at 60000h, colour 3Ch | 0908 |
| 2 | FILL 32x32 at 60200h, colour C3h | 0908 |
| 3 | COPY 32x32 60200h -> 60100h (forward, dst < src) | 0900 |

Why 60000h: it is the area wd90c24fb's own BITBLT self-test fills and copies
on every load (wd_blt_selftest: 64x4 at 60000h -> 61000h, pitch 640), above
the 640x480x8 frame (0-4AFFFh, CRTC start 0) and the hardware cursor pattern
(50000h, snapshot 014h << 14), and at or above 256 KiB, outside the text
state and outside the four 64 KiB planes wd24adv saves/restores. Its
contents are overwritten by every wd90c24fb load, so they are not restored.
The design's 4C000h slot (4 KiB) is too small for 32x32 at pitch 640.

Entry: 23C0h read twice == 1002h, snapshot of the 25 words read twice
(agree, echo, ACT=0, cursor enable=0). Banks 0-5 checksummed before and
after (must be equal); bank 6 read twice (must agree); after each op the
whole of bank 6 must equal the model, and the op must change >= 1 byte.

Exact 23C2h writes (46, block 1 only):

```
op1  2000 3060 4000 5060 8280 6008 7008 e0ff 9300 1000 a03c 0908
op2  2200 3060 4200 5060 8280 6020 7020 e0ff 9300 1000 a0c3 0908
op3  2200 3060 4100 5060 8280 6020 7020 e0ff 9300 1000 0900
rest 1000 210b 3004 410b 5004 61e4 713c 8280 9300 a000 e0ff
```

CNTRL1 (with ACT) is the only new register and is written last, once per op,
after ACT=0 was read. 23C0h: selects 1001h..1F01h/1002h..1802h (block 2
read only) and a final 1002h. GR09 (PR0A): 00h..60h for readback, then 00h.
Never written: block 2, BG, C, D, F, 23C4h; no abort.

Timeout: <= 200000 polls (~0.2 s, as wd90c24fb) for ACT=0. On timeout S3
stops: no further write of any kind (no abort, no restore, no PR0A, no
23C0h) and no video memory access; /proc/wd24adv/s3 shows TIMEOUT/DEAD.
Then, still holding 23C0-23C3, one read-only idle re-check: 23C0h must
still read 1001h, then <= 200000 reads of 23C2h.

- Seen idle (ACT clear): video memory unlocked; the trial expiry and rmmod
  do the normal full text restore (planes, font, DAC, registers); rmmod OK.
- Still busy (or 23C0h changed): LOCKED, sticky until power-off. The trial
  expiry restores registers only (ext, std, DAC, relock, screen on): no plane
  select, no plane/font/text memory read or write, no memset, nothing on
  23C2h/23C4h. The text font/content is NOT restored (the screen may show
  garbage). Every later video memory path is refused with "power cycle
  required": control text/confirm/mode, mode set, pattern, plane save. The
  module pins itself: rmmod gives EBUSY, a reload gives EEXIST. Do not load
  wd90c24fb (its BITBLT self-test would touch the engine): power off.

Build: commit d9c0884 (code), tc755x tree (kobj-vm86), vermagic
`6.18.57-tc755x mod_unload 486`, sha256
`9c77fe90e8b393cdb1e510ffd967e834794ccfd3538e69076370f53e7f7460a1`.
Same preparation as S2 (no X, wd90c24fb text=1 + rmmod). Exact serial
lines (each logged with plog.sh first; no retries):

```
pidof Xfbdev; ps | grep -E "startx|Xfbdev|jwm|rxvt"
kill <the Xfbdev PID printed above>
echo 1 > /sys/module/wd90c24fb/parameters/text; sleep 2; rmmod wd90c24fb
S=$(sha256sum /tmp/wd24adv.ko|cut -c1-8); N=$(grep -cE "^(wd90c24fb|vga16fb|wd24adv) " /proc/modules); F=$(wc -l </proc/fb); pidof Xfbdev >/dev/null; X=$?; echo G=$S$N$F$X; [ $S$N$F$X = 9c77fe90001 ] && { dmesg -c >/dev/null; insmod /tmp/wd24adv.ko dryrun=0 live=1 mode=640x480-8 s3=1; echo RC=$?; }
sleep 20; cat /proc/wd24adv/s3 > /tmp/s3run1.txt; cat /proc/wd24adv/live > /tmp/s3live1.txt; grep -E "S3 result|op [123]|DEAD|idle|restore:|writes" /tmp/s3run1.txt; dmesg | grep wd24adv
rmmod wd24adv; echo RM=$?
insmod /usr/local/lib/modules/6.18.57-tc755x/extra/wd90c24fb.ko; echo RC=$?
```

Expected: RC=0, the test pattern for ~15 s (the screen is not touched by
S3), `S3 result: OK`, three ops VERIFIED, restore read 1002, text back at
expiry, RM=0. Any other result: stop, save the files, cold boot if the
text console looks wrong.
If the result is TIMEOUT, read the "idle re-check" line. IDLE seen: carry on
(rmmod, wd90c24fb back). STILL BUSY / LOCKED: stop; do not rmmod, do not load
wd90c24fb; save the files and power off.

## S4: on-screen BITBLT fills (all X ROPs) and overlapping copies (prepared 2026-11-05 UTC, not run)

Design doc section 7, stage S4. `insmod wd24adv.ko dryrun=0 live=1 mode=640x480-8 s4=1`:
after the PROVEN 640x480-8 trial mode set and test pattern (15 s trial timer
already queued), inside the same insmod, 11 BITBLT operations on the visible
frame in bank 1 (10000h-1FFFFh, PR0A 10h, rows 102.4-204.8). Each op is
followed by a full bank-1 readback compared against the model. This is the
same S3 machinery (entry checks, 25-word snapshot read twice, ACT polls,
~0.2 s timeout latch with the idle re-check / LOCKED path, restore to the
same-load snapshot), driven by a second op table. Bank 1 is inside the 256 KiB
plane save, so the trial expiry restores the text screen as usual.

| op | what | ROP | CNTRL1 |
|---|---|---|---|
| 1 | FILL 64x16 (16,112) c=3Ch | SRC 300 | 0908 |
| 2 | FILL 64x16 (96,112) c=3Fh | XOR 600 | 0908 |
| 3 | FILL 64x16 (176,112) c=30h | AND 100 | 0908 |
| 4 | FILL 64x16 (256,112) c=38h | OR 700 | 0908 |
| 5 | FILL 64x16 (336,112) | INVERT A00 | 0908 |
| 6 | FILL 64x16 (416,112) | CLEAR 000 | 0908 |
| 7 | FILL 64x16 (496,112) | SET F00 | 0908 |
| 8 | COPY 128x24 (48,150)->(40,146), overlap, forward | SRC | 0900 |
| 9 | COPY 128x24 (300,150)->(308,154), overlap, backward | SRC | 0D00 |
| 10 | COPY 120x16 (460,184)->(468,184), same rows, backward | SRC | 0D00 |
| 11 | COPY 120x16 (48,184)->(40,184), same rows, forward | SRC | 0900 |

The kernel picks the direction (dst > src: backward, with src/dst at the
bottom-right pixel, as wd_blt_copy_lin). Every rectangle is checked to lie
inside the 640x480 frame and inside bank 1 (rows 110-203); every op must
change >= 1 byte. Banks 0 and 2-6 are checksummed before and after (must be
equal). Exact 23C2h writes: 139 (op table in tests/qemu-wd24adv-s4.py,
checked byte-for-byte in QEMU). CNTRL1 is only 0908/0900/0D00, written last.
Never written: block 2, BG, C, D, F, 23C4h; no abort.

What the owner should see, for ~15 s: the normal pattern, with seven 64x16
blocks in a row near y=112 (solid, XOR, AND, OR, inverted, black, white),
and two bands near y=146-178 and y=184-200 where the bars are shifted 8 px
left (forward copies) or right/down (backward copies), with no smearing. A
smeared or repeated band would mean the wrong direction; the readback would
also report VRAM_BAD.

Build: commit (this one), tc755x tree (kobj-vm86), vermagic
`6.18.57-tc755x mod_unload 486`, sha256
`7b33b083c012e43a8fde302a28cfb4b4d18fb590f5ff30ec3d4e735d65894607`.
Tests: host test_s4 299/0 (independent engine model, plus faults: BACK
ignored, widened rect, stray write, timeout at op 9, entry refusals,
readback flip); QEMU qemu-wd24adv-s4.py 35/35; S3 QEMU 34/34 unchanged.

Exact serial lines (each logged with plog.sh first; each sent only after
the previous returned; no retries):

```
pidof Xfbdev; ps | grep -E "startx|Xfbdev|jwm|rxvt"
kill <the Xfbdev PID printed above>
echo 1 > /sys/module/wd90c24fb/parameters/text; sleep 2; rmmod wd90c24fb; echo RM=$?
dmesg -n 1; printf '\033[9;0]' > /dev/tty1; echo Q=$?
S=$(sha256sum /tmp/wd24adv.ko|cut -c1-8); N=$(grep -cE "^(wd90c24fb|vga16fb|wd24adv) " /proc/modules); F=$(wc -l </proc/fb); pidof Xfbdev >/dev/null; X=$?; echo G=$S$N$F$X; [ $S$N$F$X = 7b33b083001 ] && { dmesg -c >/dev/null; insmod /tmp/wd24adv.ko dryrun=0 live=1 mode=640x480-8 s4=1; echo RC=$?; }
sleep 20; cat /proc/wd24adv/s4 > /tmp/s4run1.txt; cat /proc/wd24adv/live > /tmp/s4live1.txt; grep -E "S4 result|op [0-9]+ |DEAD|idle|restore:|writes" /tmp/s4run1.txt; dmesg | grep wd24adv
rmmod wd24adv; echo RM=$?
insmod /usr/local/lib/modules/6.18.57-tc755x/extra/wd90c24fb.ko; echo RC=$?
( PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; HOME=/root; export PATH HOME; ( sleep 5; exec startx ) </dev/null >/tmp/xautostart.log 2>&1 & )
i=0; while ! pidof Xfbdev >/dev/null && [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done; sleep 5; pidof Xfbdev && echo X-UP || echo NO-X
```

Expected: RC=0, the pattern with the S4 changes for ~15 s, `S4 result: OK`,
11 ops VERIFIED, restore read 1002, text back at expiry, RM=0, X-UP. Any
other result: stop and save the files. TIMEOUT: read the idle re-check line.
IDLE seen: carry on. STILL BUSY / LOCKED: stop; do not rmmod, do not load
wd90c24fb; save the files and power off.

## S5: on-screen host-data BITBLT, MONO expansion (s5=1 run 2026-11-05 UTC: OK 8/8; X + baseline OK, see docs/s5-live-2026-11-05.txt)

Design doc section 7, stage S5. `insmod wd24adv.ko dryrun=0 live=1 mode=640x480-8 s5=1`:
after the PROVEN 640x480-8 trial mode set and test pattern (15 s trial timer
queued), inside the same insmod, 8 host-data colour-expansion BITBLTs on the
visible frame in bank 1, each followed by a full bank-1 readback against the
model. Same S3/S4 machinery (entry checks, snapshot read twice, ~0.2 s timeout
latch with the idle re-check / LOCKED path, restore to the same-load
snapshot), plus the host port 23C4h (6 ports 23C0h-23C5h requested).

| op | what | CNTRL2 | FG/BG | pattern |
|---|---|---|---|---|
| 1 | MONO 8x8 (16,112) opaque | 0 | 3C/01 | (r^c)&1 |
| 2 | MONO 13x9 (40,112) opaque | 0 | 3F/00 | (r+c)%3==0 |
| 3 | MONO 32x16 (64,112) opaque | 0 | 30/3E | ((5r+3c)>>2)&1 |
| 4 | MONO 64x64 (112,120) opaque | 0 | 3C/00 | ((r*r+c*c)>>4)&1 |
| 5-8 | same four, transparent, at x=200/224/248/296 | 8 | 3F/38/3C/3F | same |

Format (wd90c24fb cexp variant 0): CNTRL1 090E, ROP SRC, 4 px per word in
bits 3:0, bit 3 = leftmost, ceil(w/4) words per row. Each word is written only
after a 23C2h read of CNTRL1 shows ACT (0800h); ACT clear before all words are
sent = HOST DATA SHORT (stop, restore). After the last word: idle poll; busy =
TIMEOUT (no drain). The 23C4h guard allows only the running op's next owed
word, value-checked. Exact counts: 116 writes to 23C2h, 2408 to 23C4h, no
23C4h read. BG (idx B) is writable only in host programs. Never written: block 2,
C, D, F; no abort.

s5=2 (HOSTCOL; now hardened with a probe op, see the s5=2 section below; CNTRL1 0902, 2336 words, 2*ceil(w/4) words per row,
low byte = left pixel) is UNPROVEN: built and QEMU-tested, but a separate live
gate, only after s5=1 is OK.

What to see for ~15 s: the normal pattern with four small checker/stripe/
ring patches near y=112-184 on solid boxes (opaque, x=16-176) and the same four
patterns drawn over the bars with no box (transparent, x=200-360).

Build: tc755x tree (kobj-vm86), vermagic `6.18.57-tc755x mod_unload 486`, sha256
`f2ac6eee0554263133054b69fead93e4af1fe36a306b670efd4f5f5070c3540c`.
Tests: host test_s5 406/0 (independent engine; faults: host short, host hang,
reversed bits, swapped colours, stray write, start ignored, readback flip,
entry refusals); all other host tests unchanged; QEMU qemu-wd24adv-s5.py 46/46
(exact 23C2h/23C4h lists, ACT before each word, fake_hang=3 SHORT, fake_hang=4
TIMEOUT/LOCKED pinned); S3 QEMU 34/34, S4 QEMU 35/35.

Exact serial lines (plog.sh each first; send each only after the previous
returned to the prompt; no retries):

```
pidof Xfbdev; ps | grep -E "startx|Xfbdev|jwm|rxvt"
kill <the Xfbdev PID printed above>
echo 1 > /sys/module/wd90c24fb/parameters/text; sleep 2; rmmod wd90c24fb; echo RM=$?
dmesg -n 1; printf '\033[9;0]' > /dev/tty1; echo Q=$?
S=$(sha256sum /tmp/wd24adv.ko|cut -c1-8); N=$(grep -cE "^(wd90c24fb|vga16fb|wd24adv) " /proc/modules); F=$(wc -l </proc/fb); pidof Xfbdev >/dev/null; X=$?; echo G=$S$N$F$X; [ $S$N$F$X = f2ac6eee001 ] && { dmesg -c >/dev/null; insmod /tmp/wd24adv.ko dryrun=0 live=1 mode=640x480-8 s5=1; echo RC=$?; }
sleep 20; cat /proc/wd24adv/s5 > /tmp/s5run1.txt; cat /proc/wd24adv/live > /tmp/s5live1.txt; grep -E "S5 result|op [0-9]+ |host|DEAD|idle|restore:|writes" /tmp/s5run1.txt; dmesg | grep wd24adv
rmmod wd24adv; echo RM=$?
insmod /usr/local/lib/modules/6.18.57-tc755x/extra/wd90c24fb.ko; echo RC=$?
( PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; HOME=/root; export PATH HOME; ( sleep 5; exec startx ) </dev/null >/tmp/xautostart.log 2>&1 & )
i=0; while ! pidof Xfbdev >/dev/null && [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done; sleep 5; pidof Xfbdev && echo X-UP || echo NO-X
```

Expected: RC=0, `S5 result: OK`, 8 ops VERIFIED with host words n/n, 2408
words to 23C4h, restore read 1002, text back at expiry, RM=0, X-UP. Anything
else: stop and save the files. HOST DATA SHORT: stop, save, do not continue to
s5=2. TIMEOUT: read the idle re-check line; IDLE seen: carry on. STILL BUSY /
LOCKED: stop; no rmmod, no wd90c24fb load; save the files and power off.

## S5 s5=2: on-screen host-data BITBLT, HOSTCOL (run 2026-11-05 UTC: OK 4/4, probe alt 0; X + baseline OK, see docs/s5-2-live-2026-11-05.txt)

Design doc section 7, stage S5 second half. Same load, machinery and safety
model as s5=1 above (`insmod wd24adv.ko dryrun=0 live=1 mode=640x480-8 s5=2`),
CNTRL1 0902 (host source, colour, ROP SRC), 2*ceil(w/4) words per row, low
byte = left pixel: UNPROVEN.

| op | what | pixels |
|---|---|---|
| 1 | PROBE 4x2 at (560,112), 4 words 4140 4342 4544 4746 | 40h + 4r + c |
| 2 | HOSTCOL 8x8 at (400,112) | 38h + ((r+c)&7) |
| 3 | HOSTCOL 32x16 at (424,112) | 30h + ((r^c)&15) |
| 4 | HOSTCOL 64x64 at (472,120) | (4r+c)&FFh |

Hardening (the word count is the live risk: an engine wanting more words
would stall into LOCKED): op 1 is a 4x2 probe. If ACT is still 1 after its 4
words, the module feeds the planned 1 px/word words (pixel n in both bytes:
4444 4545 4646 4747), each only after an ACT=1 read; the guard allows them
only during that feed. Idle at the end = `HOSTCOL 1 PX/WORD` (stop, restore,
not dead); idle part way = HOST DATA SHORT; still busy = TIMEOUT. On a VRAM
mismatch the report names the packing seen (`HOSTCOL readback class`: as
modelled / bytes swapped / unchanged / high byte only / other). Exact counts:
2340 words (+ up to 4 probe words), no 23C4h read; BG (idx B) never written.

What to see for ~15 s: the normal pattern with three colour patches on the
bars at x=400-536, y=112-184 (diagonal stripes, a xor checker, a gradient),
and a tiny 4x2 dot at (560,112).

Build: tc755x tree (kobj-vm86), vermagic `6.18.57-tc755x mod_unload 486`, sha256
`1c7b21b099dee110bcb8a913d495abeea722cc993257ed0b049bb4127c1885f1`.
Tests: host test_s5 455/0 (adds probe 1 px/word -> HCOL_ALT, 1 px/word never
idle -> TIMEOUT after 8 words, idle one early -> SHORT, classes swapped/high
byte only, guard refuses probe words outside the feed); all other host tests
unchanged; QEMU qemu-wd24adv-s5.py 55/55 (exact lists with the probe,
fake_hang=5 HCOL_ALT restored + rmmod, fake_swap=1 VRAM BAD "bytes swapped"
restored + rmmod); S3 QEMU 34/34, S4 QEMU 35/35.

Exact serial lines (plog.sh each first; send each only after the previous
returned to the prompt; never overlap; no retries):

```
pidof Xfbdev; ps | grep -E "startx|Xfbdev|jwm|rxvt"
kill <the Xfbdev PID printed above>
echo 1 > /sys/module/wd90c24fb/parameters/text; sleep 2; rmmod wd90c24fb; echo RM=$?
dmesg -n 1; printf '\033[9;0]' > /dev/tty1; echo Q=$?
S=$(sha256sum /tmp/wd24adv.ko|cut -c1-8); N=$(grep -cE "^(wd90c24fb|vga16fb|wd24adv) " /proc/modules); F=$(wc -l </proc/fb); pidof Xfbdev >/dev/null; X=$?; echo G=$S$N$F$X; [ $S$N$F$X = 1c7b21b0001 ] && { dmesg -c >/dev/null; insmod /tmp/wd24adv.ko dryrun=0 live=1 mode=640x480-8 s5=2; echo RC=$?; }
sleep 20; cat /proc/wd24adv/s5 > /tmp/s5run2.txt; cat /proc/wd24adv/live > /tmp/s5live2.txt; grep -E "S5 HOSTCOL|result|op [0-9]+ |host|probe|class|DEAD|idle|restore:|writes" /tmp/s5run2.txt; dmesg | grep wd24adv
rmmod wd24adv; echo RM=$?
insmod /usr/local/lib/modules/6.18.57-tc755x/extra/wd90c24fb.ko; echo RC=$?
( PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; HOME=/root; export PATH HOME; ( sleep 5; exec startx ) </dev/null >/tmp/xautostart.log 2>&1 & )
i=0; while ! pidof Xfbdev >/dev/null && [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done; sleep 5; pidof Xfbdev && echo X-UP || echo NO-X
```

Expected: RC=0, `S5 HOSTCOL result: OK`, 4 ops VERIFIED, probe alternative
words 0, restore read 1002, text back at expiry, RM=0, X-UP. HOSTCOL 1
PX/WORD, HOST DATA SHORT or VRAM BAD: stop after L6, save the files, carry on
with L7-L10 (restored, not dead); the format is then redesigned on the box.
TIMEOUT: read the idle re-check line; IDLE seen: carry on. STILL BUSY / LOCKED:
stop; no rmmod, no wd90c24fb load; save the files and power off.

## S6: dual window, PR0A read / PR0B write (run 2026-11-04: REGISTER READBACK at P3, PR31 locked in trial mode; PR0B + PR1 b3 split read OK; restored, X + baseline OK, see docs/s6-live-2026-11-04.txt)

Design doc section 7, stage S6 (the RAM-shadow S6 is parked). One load:
`insmod wd24adv.ko dryrun=0 live=1 mode=640x480-8 s6=1`. Right after the
PROVEN 640x480-8 trial mode set, under hw_lock + console_lock, the program in
`wd24adv_s6.c` runs once:

### S6c (run 2026-11-05: OK, b7 held and effective while locked, writes 33, X-UP, baseline ok; docs/s6c-live-2026-11-05.txt): PR31 b7 held across SR06 relock

Question: does PR31 b7 (a) survive the SR06 relock and (b) still steer reads
to PR0A / writes to PR0B while SR06 is locked? S6 v2 proved b7 only while
unlocked. Load option `s6c=1` (with s6=1); everything up to P3 is S6 v2.

| step | what | writes |
|---|---|---|
| E ... P3 | as S6 v2 (P3 must read back dd, else REGISTER READBACK) | as v2 |
| L1 | SR06 = 0Fh with b7 set; must read 0f + width locked, else RESTORE FAIL; PR31 locked read logged (expect 00) | SR06 |
| D2 | the v2 copy test, run LOCKED | memory |
| U2 | SR06 = 4Fh (read back + width unlocked; fail -> relock, RESTORE FAIL, stop); read PR31: dd = held, 5d = cleared | SR06 |
| P4, L, V2, restore | as S6 v2 | as v2 |

Results: OK = held and effective locked; B7 NOT HELD LOCKED ("relock cleared
PR31 b7" or "PR31 b7 kept but no effect while locked"; restored by design);
RESTORE FAIL as v2. 33 register writes when OK. Extra dmesg line `S6c: held
SR06 0f/0 PR31(locked) .., re-unlock PR31 dd (b7 held); D2 rd 0 noecho 0 V2 0/0/0`.

Proven: everything through P3 (S6 v2 live). Assumed (what S6c tests): b7
retention and effect while locked; that the re-unlock works with b7 set (if
not: RESTORE FAIL with b7 possibly set, stop rule).

Build: tc755x (kobj-vm86), vermagic `6.18.57-tc755x mod_unload 486`, sha256
`96a6b5273454187db8002c2db55d0ee318363b5f57d82cbfc713a583583af479`.
Tests: host test_s6 140/0 (v2 cases unchanged; S6c lock-hold OK; lock-clear,
kept-no-effect, FFh variants -> NOT HELD; re-unlock refused -> RESTORE FAIL,
relocked, guard closed; PR31 b7 ignored -> not OK; v2 on a lock-clear chip
still OK); other host suites unchanged; QEMU qemu-wd24adv-s6.py 24/24 (v2 19 + S6c dry-run, s6c-without-s6 refusal, real .ko zero data writes, fake OK, rmmod).

Copy route (as v2): box -> grok-bot-server -> Pop ~/wd24adv-s6c.ko, check
sha on Pop, then `~/755c-install/xkit/send1.sh ~/wd24adv-s6c.ko /tmp/wd24adv.ko`,
then on the 755C `sha256sum /tmp/wd24adv.ko` must start 96a6b527.

Lines (separate approval; plog each; one per call, wait for the prompt; no
overlap, no retries; interrupted -> check commands.log, never re-send):

```
pidof Xfbdev; ps | grep -E "startx|Xfbdev|jwm|rxvt"
kill <the Xfbdev PID printed above>
echo 1 > /sys/module/wd90c24fb/parameters/text; sleep 2; rmmod wd90c24fb; echo RM=$?
dmesg -n 1; printf '\033[9;0]' > /dev/tty1; echo Q=$?
S=$(sha256sum /tmp/wd24adv.ko|cut -c1-8); N=$(grep -cE "^(wd90c24fb|vga16fb|wd24adv) " /proc/modules); F=$(wc -l </proc/fb); pidof Xfbdev >/dev/null; X=$?; echo G=$S$N$F$X; [ $S$N$F$X = 96a6b527001 ] && { dmesg -c >/dev/null; insmod /tmp/wd24adv.ko dryrun=0 live=1 mode=640x480-8 s6=1 s6c=1; echo RC=$?; }
sleep 20; cat /proc/wd24adv/s6 > /tmp/s6crun.txt; cat /proc/wd24adv/live > /tmp/s6clive.txt; grep -vE "^  write" /tmp/s6crun.txt; dmesg | grep wd24adv
rmmod wd24adv; echo RM=$?
insmod /usr/local/lib/modules/6.18.57-tc755x/extra/wd90c24fb.ko; echo RC=$?
( PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; HOME=/root; export PATH HOME; ( sleep 5; exec startx ) </dev/null >/tmp/xautostart.log 2>&1 & )
i=0; while ! pidof Xfbdev >/dev/null && [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done; sleep 5; pidof Xfbdev && echo X-UP || echo NO-X
pidof Xfbdev >/dev/null && { dmesg -c >/dev/null; insmod /tmp/wd24probe3.ko; echo RC=$?; dmesg | grep wd24probe3 | grep -v " tr " | sort -u; }
```

Expected: G=96a6b527001, RC=0, v2 lines through P3 dd, `L1 held: SR06 0f
locked`, then either `U2 re-unlock: PR31 dd (b7 held)` + D2/V2 all 0 +
`result: OK`, or `B7 NOT HELD LOCKED` (both are answers); P4 5d, final
`SR06 readback 0f, index width: locked`, `regions restored: 0 bytes differ`,
screen unchanged, text at expiry, RM=0, RC=0, X-UP, baseline RC=19 result=ok
restore_fail=0x00 SR06=0f SR11=5d.

Stop rules: G mismatch -> send none of the S6c lines, go to the wd90c24fb/X
lines. ENTRY / REGISTER READBACK / MEMORY / NOT HELD / SPLIT ONLY: restored
by design, continue with rmmod and X, report. RESTORE FAIL (incl. "did not
relock with PR31 b7 set", "re-unlock failed"), any SR06 readback other than
0f or width not locked after L, mismatch in /tmp/s6clive.txt, TIMEOUT, STILL
BUSY, LOCKED, garbled screen not back to text at expiry, or no prompt: stop;
no rmmod, no reload; power off.

### S6b (run 2026-11-05 America/New_York: OK 18/18; trial=300 after TRIAL_MAX=600; X-UP; baseline skipped — wd24probe3.ko missing): dual-window mmap + PR31 b7 copy

`s6b=1` with `dryrun=0 live=1 mode=640x480-8 trial=300`; then the user tool
`wd24s6b-test` opens /dev/wd24s6b. See WD24ADV-NEXT-STAGE.md for the design.
Trial range is 5..600 (cannot be disabled). Use 300 so serial typing of the
arm+run line cannot eat the window (the first live attempt expired at 60 s
before `run`; a second attempt with trial=300 was refused by the old clamp).

| step | what | writes |
|---|---|---|
| begin (open) | entry: PR1 b3 off, PR1 b7:6 = 11, GR06 64K at A0000, PR0A 0, SR06 locked; else refused, no write | PR0A, PR0B = 0, PR1 b3 on |
| SETWIN | PR0B = w0, PR0A = w1-8 (read back) | PR0A, PR0B |
| COPY | PR1 = BIOS; unlock 4F; PR31 |= 80; relock 0F; 4K chunks; unlock; PR31 must still have b7; PR31 base; relock; PR1 b3 on; windows back | 2 unlocks per COPY |
| end (close / kill -9 / text / expiry / rmmod) | PR31 base if b7 may be set, PR1, PR0A, PR0B = entry; SR06 locked check | as listed |

The tool touches only 50000-7FFFF off screen and the visible frame (scrolls
up 16 lines once, then restores it): the screen shows bars, a brief
16-line jump, then bars again. Expected `S6B-RESULT OK 18/18 steps; state 1
copies 5`.

Proven: PR1 b3 split, PR0B, PR31 b7 effective with SR06 locked in PR1 = c5
(S6, S6c). Assumed: PR0A/PR0B writes while b7 is set (COPY moves banks
with b7 on); PR0A/PR0B reach above 50h/70h (to 78h); UC user mapping of
A0000 behaves as the kernel ioremap; a b7 that reads back but has no effect
is only caught by the data checks (`S6B FAIL kernel COPY`).

Build: tc755x (kobj-vm86), vermagic `6.18.57-tc755x mod_unload 486`, module
sha256 `f73f8e1c728e16124357d8c7303efde725a31de5fb29a1d8914e3b47dcc1c822`,
tool sha256 `de6a9b34c9855fec53508a2e3859d0c77c1296c2dfad549942b408e6690f44f7`.
Tests: host test_s6b + all suites; QEMU qemu-wd24adv-s6b.py,
qemu-wd24adv.py (trial=601 refused), qemu-wd24adv-s6.py, qemu-wd24adv-s3.py.

Copy route: box -> grok-bot-server -> Pop ~/wd24adv-s6b.ko and
~/wd24s6b-test, check both shas on Pop, then
`~/755c-install/xkit/send1.sh ~/wd24adv-s6b.ko /tmp/wd24adv.ko` and
`~/755c-install/xkit/send1.sh ~/wd24s6b-test /tmp/wd24s6b-test`; on the
755C `sha256sum /tmp/wd24adv.ko /tmp/wd24s6b-test` must start f73f8e1c /
de6a9b34.

Lines (separate approval; plog each; one per call, wait for the prompt; no
overlap, no retries; interrupted -> check commands.log, never re-send).
L5 is the G gate only. L6 is one combined arm+run line (<400 chars) so the
trial clock starts only when run is about to execute. Send L6 only if GATE=OK:

```
pidof Xfbdev; ps | grep -E "startx|Xfbdev|jwm|rxvt"
kill <the Xfbdev PID printed above>
echo 1 > /sys/module/wd90c24fb/parameters/text; sleep 2; rmmod wd90c24fb; echo RM=$?
dmesg -n 1; printf '\033[9;0]' > /dev/tty1; chmod 755 /tmp/wd24s6b-test; echo Q=$?
S=$(sha256sum /tmp/wd24adv.ko|cut -c1-8); T=$(sha256sum /tmp/wd24s6b-test|cut -c1-8); N=$(grep -cE "^(wd90c24fb|vga16fb|wd24adv) " /proc/modules); F=$(wc -l </proc/fb); pidof Xfbdev >/dev/null; X=$?; echo G=$S$T$N$F$X; [ $S$T$N$F$X = f73f8e1cde6a9b34001 ] && echo GATE=OK || echo GATE=FAIL
dmesg -c >/dev/null; insmod /tmp/wd24adv.ko dryrun=0 live=1 mode=640x480-8 s6b=1 trial=300; echo RC=$?; [ -c /dev/wd24s6b ] || mknod /dev/wd24s6b c $(sed -n 's/.*major \([0-9]*\).*/\1/p' /proc/wd24adv/s6b | head -1) 0; /tmp/wd24s6b-test run | grep -vE "^S6B ok"; echo T=$?
/tmp/wd24s6b-test hold & sleep 3; kill -9 $!; sleep 2; tail -6 /proc/wd24adv/s6b
sleep 300; cat /proc/wd24adv/s6b > /tmp/s6brun.txt; cat /proc/wd24adv/live > /tmp/s6blive.txt; tail -4 /tmp/s6brun.txt; dmesg | grep wd24adv | tail -6
rmmod wd24adv; echo RM=$?
insmod /usr/local/lib/modules/6.18.57-tc755x/extra/wd90c24fb.ko; echo RC=$?
( PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; HOME=/root; export PATH HOME; ( sleep 5; exec startx ) </dev/null >/tmp/xautostart.log 2>&1 & )
i=0; while ! pidof Xfbdev >/dev/null && [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done; sleep 5; pidof Xfbdev && echo X-UP || echo NO-X
pidof Xfbdev >/dev/null && { dmesg -c >/dev/null; insmod /tmp/wd24probe3.ko; echo RC=$?; dmesg | grep wd24probe3 | grep -v " tr " | sort -u; }
```

WAIT: the arm+run line up to 60 s after RC=0 (a 486 with UC ISA VRAM; expect
under 20 s for `run`); the `sleep 300` line up to 320 s; others as S6c.

Expected: G=f73f8e1cde6a9b34001 and GATE=OK, RC=0, `S6b: armed`, the run
prints only `S6B copy ...: err 0` x4 and `S6B-RESULT OK 18/18 ...`, T=0; hold
line `S6B-HOLD setwin 0`, then `release (close / munmap / exit) -> 0; state OFF`,
`restore: PR1 c5 PR0A 00 PR0B 00 SR06 0f locked, PR31 b7 clear, 0 bad`,
`restore-bad 0`; after expiry text is back, the log ends with
`trial expiry` and SR06 locked; RM=0, RC=0, X-UP, baseline RC=19 result=ok
restore_fail=0x00 SR06=0f SR11=5d.

Stop rules: GATE=FAIL / G mismatch -> send none of the S6b lines, go to the
wd90c24fb/X lines. `S6B-RESULT FAIL` with state 1 (data only, session still
ON) or open refused (`No such device`, entry refused, no write): continue
with the sleep/expiry line, rmmod and X, report. Session DEAD (dmesg `S6b:
session DEAD`, INFO state 2), `restore-bad` not 0, `SR06 ... not locked` in
the log, a restore line other than `SR06 0f locked, PR31 b7 clear, 0 bad`, a
run line that does not return within 60 s, kill -9 not followed by
`release ... -> 0`, mismatch in /tmp/s6blive.txt, TIMEOUT, STILL BUSY,
LOCKED, RESTORE FAIL, garbled screen not back to text at expiry, or no
prompt: stop; no rmmod, no reload; power off.


### S6 v2 (run 2026-11-04: OK, writes 31, SR06 relocked 0f; L7-L8 overlapped by operator error, stopped with wd24adv loaded in text, X not restored, baseline not run; see docs/s6v2-live-2026-11-04.txt)

The lock: PR31 = SR11 is a Paradise extended register. Data book 14.1: PR20
(SR06) must be x1x01xxx to read/write SR07+; locked, the 3C4 index is 3 bits
wide (unlocked 6); table 6-8: bits 7:4 RO, 3:0 RW. 86Box vid_paradise.c: SR
index > 7 only when SR06 == 48h exactly, else reads FFh and writes are
ignored. The trial mode ends with SR06 = BIOS value 0Fh (locked), so the v1
P3 write of DDh was ignored and read back 00h. wd24probe3 proved live that
SR06 = 4Fh ((0Fh & EFh) | 48h) unlocks (SR11 read 5Dh, index width 6 bits),
that SR06 reads back 0Fh (bits 3:0 only) and that writing 0Fh relocks.

| step | what | writes |
|---|---|---|
| E | read PR0A PR0B PR1 GR06 SR06, index width, PR31 (locked read, logged); need PR1 b3=0, b7:6=11, GR06 b3:2=01, PR0A=0, width = locked; else ENTRY (nothing written) | none |
| E2 | SR06 = 4Fh (read back 0Fh + width unlocked), read PR31, SR06 = 0Fh (read back + width locked). PR31 must be 5Dh else ENTRY (only this pair written); relock fail -> RESTORE FAIL | SR06 x2 |
| save | 7 x 4 KiB off screen via PR0A (proven) | PR0A |
| prefill | 4 patterns, single window, read back (else MEM) | PR0A |
| P1 | PR0B = 70h, read back | PR0B |
| P2 | PR1 = C5h -> CDh, read back | PR1 b3 |
| D1 | A0000 reads Y+0, A8000 reads X+8000; new patterns written through both halves | memory |
| P2' + V1 | PR1 back to C5h (read back), then single window: Y+0' and X+8000' verified BEFORE any PR31 write | PR1, PR0A |
| U | SR06 = 4Fh, read back (0Fh + width unlocked) | SR06 |
| P3 | PR31 = 5Dh -> DDh, read back | PR31 b7 |
| D2 | read X+1000; write Y+1000, re-read (still X); copy X+1000 -> Y+2000 | memory |
| P4 | PR31 = 5Dh, read back (still unlocked) | PR31 |
| L | SR06 = 0Fh, read back (0Fh + width locked); fail -> RESTORE FAIL | SR06 |
| V2 | single window: Y+1000 C, Y+2000 copy, X+1000 kept | PR0A |
| restore | 7 regions back and compared, PR0B and PR0A back | PR0A PR0B |

Every error after an unlock goes through P4 (if PR31 was touched) and L.
Guard while S6 runs (`s6_active`): SR06 only the saved BIOS value, or exactly
(BIOS & EFh) | 48h while the program's lock step is open (`s6_lock_open`);
PR31 b7 only with `dual_open` and `s6_lock_open`. Outside S6 the proven SR06
and PR31 rules are unchanged. Flags cleared after the run. 31 register writes
when OK.

Build: tc755x tree (kobj-vm86), vermagic `6.18.57-tc755x mod_unload 486`, sha256
`08c12db4b650c06aa81bb0d290f289819b9d7e356bc7cb6ba24a8f880c0e7118`.
Tests: host test_s6 88/0: v1 cases plus lock variants: locked SR07+ read FFh
(86Box) -> OK; unlock only on exactly 48h (86Box) -> REGISTER READBACK after
the E2 pair; SR06 writes ignored -> REGISTER READBACK; PR31 5Ch -> ENTRY with
2 writes; relock ignored -> RESTORE FAIL; SR06 already unlocked -> ENTRY, zero
writes; guard: 4F refused in S6 outside the lock step, 48/5F refused in it,
PR31 b7 needs both flags. In every non-relock-fault case all registers (SR06
= 0Fh) and all 1 MiB restored. Host suites unchanged; QEMU
qemu-wd24adv-s6.py 19/19.

Lines (only after a separate approval of a new live run, and after the new
.ko is copied to /tmp/wd24adv.ko on the 755C by the usual send1.sh route):
the v1 list below, with the G line expecting `08c12db4001`:

```
S=$(sha256sum /tmp/wd24adv.ko|cut -c1-8); N=$(grep -cE "^(wd90c24fb|vga16fb|wd24adv) " /proc/modules); F=$(wc -l </proc/fb); pidof Xfbdev >/dev/null; X=$?; echo G=$S$N$F$X; [ $S$N$F$X = 08c12db4001 ] && { dmesg -c >/dev/null; insmod /tmp/wd24adv.ko dryrun=0 live=1 mode=640x480-8 s6=1; echo RC=$?; }
```

Expected v2: E line `SR06 0f lock locked PR31(locked) 00`, `E2 PR31
(unlocked) 5d`, every `SR06 readback 0f`, width unlocked after U / locked
after L, P2 cd, `V1 single: PR1 c5 Y+0000'=0 X+8000'=0 bad -> split yes`,
P3 dd, P4 5d, V2 all 0, `regions restored: 0 bytes differ`, `result: OK`.
Extra v2 stop rules: `SR06 did not relock`, any SR06 readback other than 0f,
or width `unlocked`/`no answer` after L: RESTORE FAIL rule (no rmmod, power
off). ENTRY `PR31 not 5d` or REGISTER READBACK at E2/U: restored by design,
carry on with the text/X lines, report.

### S6 v1 (run 2026-11-04)

| step | what | writes |
|---|---|---|
| E | read PR0A PR0B PR1 PR31 GR06; need PR1 b3=0, PR1 b7:6=11, PR31 b7=0, GR06 b3:2=01, PR0A=0, else ENTRY (nothing written) | none |
| save | 7 x 4 KiB off screen via PR0A (proven): Y=70000h +0/+1000/+2000, X=50000h +8000/+1000/+0/+2000 | PR0A |
| prefill | 4 patterns, single window, read back (else MEM) | PR0A |
| P1 | PR0B = 70h, read back | PR0B |
| P2 | PR1 = PR1 or 08h (C5h -> CDh), read back | PR1 b3 |
| D1 | A0000 must read Y+0, A8000 must read X+8000; write new patterns through both halves | memory |
| P3 | PR31 = PR31 or 80h (5Dh -> DDh), read back | PR31 b7 |
| D2 | read X+1000; write Y+1000 and re-read (still X); copy X+1000 -> Y+2000 | memory |
| P4 | PR31 back, PR1 back, each read back (else RESTORE FAIL) | PR31, PR1 |
| V | single window: verify all five results | PR0A |
| restore | 7 regions written back and compared, PR0B and PR0A back | PR0A PR0B |

The guard allows PR1 b3 and PR31 b7 only while this program runs
(`dual_open`); the PR1 aperture bits b5:4 stay refused. 25 register writes.
Results: OK (both), SPLIT ONLY, RW ONLY, NO DUAL WINDOW, ENTRY CHECK, REGISTER
READBACK, MEMORY, RESTORE FAIL. The text restore at trial expiry also restores
PR0B/PR1/PR31.

What to see for ~15 s: the normal 640x480-8 pattern, with NO change (every
access is off screen, above 4B000h); then text.

Build: tc755x tree (kobj-vm86), vermagic `6.18.57-tc755x mod_unload 486`, sha256
`e84364a8f5f8d2263bac6d15efb426fa78b3d52186eda4e12e36e03299891a11`.
Tests: host test_s6 42/0 (86Box semantics OK; chip ignoring PR31 b7 -> SPLIT
ONLY, ignoring PR1 b3 -> RW ONLY, ignoring both -> NONE, PR1 b3 not read back
-> REGISTER READBACK, refused -> REGISTER READBACK, 5 entry refusals with zero
writes; in every case all registers and all 1 MiB restored); host 94/0 (PR1 b3
/ PR31 b7 refused outside S6); QEMU qemu-wd24adv-s6.py 19/19; S1-S5 and base
QEMU unchanged.

Exact serial lines (plog.sh each first; send each only after the previous
returned to the prompt; never overlap; no retries):

```
pidof Xfbdev; ps | grep -E "startx|Xfbdev|jwm|rxvt"
kill <the Xfbdev PID printed above>
echo 1 > /sys/module/wd90c24fb/parameters/text; sleep 2; rmmod wd90c24fb; echo RM=$?
dmesg -n 1; printf '\033[9;0]' > /dev/tty1; echo Q=$?
S=$(sha256sum /tmp/wd24adv.ko|cut -c1-8); N=$(grep -cE "^(wd90c24fb|vga16fb|wd24adv) " /proc/modules); F=$(wc -l </proc/fb); pidof Xfbdev >/dev/null; X=$?; echo G=$S$N$F$X; [ $S$N$F$X = e84364a8001 ] && { dmesg -c >/dev/null; insmod /tmp/wd24adv.ko dryrun=0 live=1 mode=640x480-8 s6=1; echo RC=$?; }
sleep 20; cat /proc/wd24adv/s6 > /tmp/s6run.txt; cat /proc/wd24adv/live > /tmp/s6live.txt; grep -vE "^  write" /tmp/s6run.txt; dmesg | grep wd24adv
rmmod wd24adv; echo RM=$?
insmod /usr/local/lib/modules/6.18.57-tc755x/extra/wd90c24fb.ko; echo RC=$?
( PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; HOME=/root; export PATH HOME; ( sleep 5; exec startx ) </dev/null >/tmp/xautostart.log 2>&1 & )
i=0; while ! pidof Xfbdev >/dev/null && [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done; sleep 5; pidof Xfbdev && echo X-UP || echo NO-X
```

Expected: G=e84364a8001, RC=0, `result: OK`, P1 PR0B readback 70, P2 PR1
readback cd, P3 PR31 readback dd, P4 restore PR31 5d PR1 c5, all `0 bad`,
`regions restored: 0 bytes differ`, screen unchanged, text back at expiry,
RM=0, X-UP.

Stop rules:
- G line not matching: stop, send nothing more of S6 (L6-L7 not sent); L8-L10 to get X back.
- SPLIT ONLY / RW ONLY / NO DUAL WINDOW / ENTRY CHECK / REGISTER READBACK / MEMORY: restored by design; save the files, carry on with L7-L10, report; redesign on the box.
- RESTORE FAIL, any `DIFFERENT`/mismatch in /tmp/s6live.txt, a garbled screen that does not return to text at expiry, or a shell that does not return: stop; no rmmod, no wd90c24fb load, no reload; power off.
- Interrupted call: check commands.log first; never re-send.

## Roll-up visual test (prepared 2026-11-04, run live 2026-11-04)

One sequence, each PROVEN mode as its own load with the 15 s auto-revert
(no confirm), then optional S3 last. Any non-OK result touches /tmp/ru.stop
and every later step prints SKIP/STOPPED. Same preparation (no X,
wd90c24fb text=1 + rmmod, then `dmesg -n 1` and no console blanking as in step 3
of the main plan; added after the 2026-11-04 run, which ran without it), build d9c0884 / sha above. Log each line with
plog.sh first. Pause ~10 s on the text console between steps.

What to see (pattern=1, wd24adv_core.c wd24_pattern/pattern_direct; panel
results as recorded live on 2026-11-02):

| step | mode | on screen during the ~15 s trial |
|---|---|---|
| 1 | 640x480-8 | 16 vertical bars of 40 px over the whole frame, no black band; from line ~410 (256 KiB) down the bar order is reversed (as recorded: "bars over the whole frame") |
| 2 | 640x400-8 | 16 bars of 40 px, letterboxed (black bands top and bottom), no reversed part (frame < 256 KiB) |
| 3 | 320x200-8 | 16 bars of 20 px (pixel-doubled), letterboxed, as recorded with 5f275b4/5767c74 |
| 4 (optional) | 640x480-8 + S3 | as step 1 (S3 is off screen) |

After each: the text console comes back unchanged, then STEPn-OK.

```
pidof Xfbdev; ps | grep -E "startx|Xfbdev|jwm|rxvt"
kill <the Xfbdev PID printed above>
echo 1 > /sys/module/wd90c24fb/parameters/text; sleep 2; rmmod wd90c24fb; rm -f /tmp/ru.stop
dmesg -n 1; printf '\033[9;0]' > /dev/tty1
gt(){ S=$(sha256sum /tmp/wd24adv.ko|cut -c1-8); N=$(grep -cE "^(wd90c24fb|vga16fb|wd24adv) " /proc/modules); F=$(wc -l </proc/fb); pidof Xfbdev >/dev/null; X=$?; echo G=$S$N$F$X; [ $S$N$F$X = 9c77fe90001 ]; }
ru(){ [ -e /tmp/ru.stop ] && { echo SKIP; return 1; }; gt && dmesg -c >/dev/null && insmod /tmp/wd24adv.ko dryrun=0 live=1 mode=$1 $2 && sleep 20 && grep -q "mismatches 0; in_text 1" /proc/wd24adv/live && echo OK-$1 $2 || { touch /tmp/ru.stop; echo STOP-$1; dmesg | grep wd24adv; return 1; }; }
ru 640x480-8 && { rmmod wd24adv && echo STEP1-OK || touch /tmp/ru.stop; }
ru 640x400-8 && { rmmod wd24adv && echo STEP2-OK || touch /tmp/ru.stop; }
ru 320x200-8 && { rmmod wd24adv && echo STEP3-OK || touch /tmp/ru.stop; }
ru 640x480-8 s3=1; cat /proc/wd24adv/s3 > /tmp/rus3.txt; grep -E "S3 result|DEAD|idle" /tmp/rus3.txt; grep -q "S3 result: OK" /tmp/rus3.txt || touch /tmp/ru.stop
[ -e /tmp/ru.stop ] && echo "STOPPED: check before rmmod" || { rmmod wd24adv && echo STEP4-OK || touch /tmp/ru.stop; }
[ -e /tmp/ru.stop ] && echo "STOPPED: do not load wd90c24fb until checked" || { insmod /usr/local/lib/modules/6.18.57-tc755x/extra/wd90c24fb.ko; echo RC=$?; }
( PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; HOME=/root; export PATH HOME; ( sleep 5; exec startx ) </dev/null >/tmp/xautostart.log 2>&1 & )
i=0; while ! pidof Xfbdev >/dev/null && [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done; sleep 5; pidof Xfbdev && echo X-UP || echo NO-X
pidof Xfbdev >/dev/null && { dmesg -c >/dev/null; insmod /tmp/wd24probe3.ko; echo RC=$?; dmesg | grep wd24probe3 | grep -v " tr " | sort -u; }
```

To skip S3, do not send its two lines. Expected end: wd90c24fb RC=0, X-UP, then
the X baseline (as in docs/s2-live-2026-11-04-run2.txt): insmod RC=19 (the
probe always unloads itself) with `result=ok`, 3CC=E3 GR0F=05 GR0E=01
CR11=8C CR29=85 CR34=A0 PR18=C7 PR19=54 SR06=0F. (3CC=67 GR0F=00 is the
text state, not this one.) NO-X: stop, no probe.
STOP-<mode>: that step failed and the module may still be loaded; read dmesg
and /proc/wd24adv/live and go no further; cold boot if the text console
looks wrong. S3 TIMEOUT: as in the S3 section (STILL BUSY: power off,
nothing else).

### Roll-up live run 2026-11-04: all four steps OK, S3 OK; visual artifacts open

Build d9c0884 (sha256 `9c77fe90…`), sequence exactly as above. Raw log:
[rollup-live-2026-11-04.txt](rollup-live-2026-11-04.txt). Times are UTC (when the
line was sent from the serial host; the 755C starts it a few seconds later).

| step | sent (UTC) | log result |
|---|---|---|
| 0 state check | 22:41:41 | as expected: wd90c24fb loaded, fb0 WD90C24, Xfbdev 597, shas 9c77fe90 / eb0a36e2 |
| prep | 22:42:43 | kill 597, text=1, rmmod wd90c24fb RM=0, no fb |
| 1 640x480-8 | 22:46:06 | mode 93/263, expiry 896/336, mismatches 0, OK, STEP1-OK |
| 2 640x400-8 | 22:47:32 | mode 92/261, expiry 896/336, mismatches 0, OK, STEP2-OK |
| 3 320x200-8 | 22:48:59 | mode 90/258, expiry 896/336, mismatches 0, OK, STEP3-OK |
| 4 640x480-8 s3=1 | 22:50:27 | OK; `S3 result: OK`, 3 ops verified, 23C2h writes 46, PR0A 15, op2 busy 21 polls, op3 busy 75 polls, no timeout |
| restore line | 22:52:02 | interrupted while being typed; a following check was typed into it (garbled line, `>` prompt); nothing in it ran |
| Ctrl-C | 23:48:57 | one 0x03: normal prompt; check: wd24adv loaded, no ru.stop, no fb, no X |
| rmmod wd24adv | 23:49:54 | RM=0, `live text: 0 (896, 336, mismatches 0)` |
| insmod wd90c24fb | 23:50:17 | RC=0, BITBLT self-test passed, fb0 WD90C24 |
| startx, wait | 23:50:52 | X-UP (Xfbdev 727) |
| wd24probe3 | 23:53:04 | RC=19, result=ok, 3CC=E3 GR0F=05 GR0E=01 CR11=8C CR29=85 CR34=A0 PR18=C7 PR19=54 SR06=0F (X baseline) |

Panel (owner, verbatim as relayed; with his correction):
- #1: "looks pretty good, just a few (3?) pixel high row of artifacting at very
  bottom". Correction: #1 DID show the bar-order flip near the bottom.
- #2: "same bars but no bar order flipping near bottom; letterboxed".
- #3: "two sets of bars, small row of artifacting between the two sets of bars,
  letterboxed".
- #4: "more artifacting at bottom than in first one, maybe 10ish pixels row
  across" (S3 OK in the log).
- Between tests: "some strange output on TTY between tests (weird spacing
  gobbledy gook)".

So the bar flip (#1 yes, #2 no) matches the design. Open: bottom rows in
#1/#4, the doubled picture in #3, the tty between tests. Analysis (box only,
no device experiment):

- **Not in this sequence: `dmesg -n 1`.** Step 3 of the main plan sets it;
  the roll-up does not. The 755C boots with `console=tty0 console=ttyS0`
  and the serial capture shows every KERN_INFO wd24adv line on the console,
  so 4-6 printk lines reached vgacon during each graphics trial. vgacon
  hard-scrolls by writing CR0C/0D (start address) and the cursor CR0E/0F;
  its text writes go to B8000h, which GR06=05 does not decode. **L** (log,
  cmdline) / **U** (that vgacon scrolled during the trials).
- **#1/#4 bottom rows: UNKNOWN; leading hypothesis vgacon start-address
  scroll.** The pattern fills the whole 307200-byte frame (banks 0-4, PR0A in
  4 KiB units, flip from 256 KiB = line ~409; owner saw the flip, so no CRTC
  wrap). S3 ops start at 60000h/60100h/60200h (384 KiB+; 32 rows end below 68000h for any pitch <= 1 KiB), beyond the frame end at
  4B000h (300 KiB), so by address the S3 ops are not on visible rows. A
  start address moved up by vgacon would show memory past 4B000h (never
  written by the pattern) at the bottom; #4 printed one more long line (the
  S3 summary), consistent with "more" but not proven.
- **#3 two sets + row between: UNKNOWN; geometry fits "no effective double
  scan + 64 KiB wrap".** The pattern writes rows of 320 bytes, 64000 bytes.
  If each of the 400 scan lines fetched a new row, rows 0-199 are the bars,
  rows 200-204.8 the 1536 unwritten bytes 64000-65535 (the artifact row),
  then the plane address wraps at 64 KiB and rows 205-399 repeat the bars.
  The mode writes std 13h CR09=41h (2 lines/row) and read back 0 mismatches;
  why it did not double here (5f275b4 on 2026-11-02, separate session, was
  fine) is unknown: a WD ext bit left by 640x400-8, or the vgacon scroll.
  Need from owner: were the two sets stacked vertically?
- **TTY garble: UNKNOWN; leading hypothesis vgacon out of sync.** `mismatches
  0` covers register readback (rb_mask), DAC (768 bytes) and all four planes
  (64 KiB each, byte compare): font (plane 2), characters and attributes are
  restored exactly to the load-time save, CR0A-0F to saved values. Not
  covered: vgacon's own origin/cursor/screen buffer. Lines printed during a
  trial never reached memory and a hard-scrolled origin is set back by the
  restore, so vgacon's state no longer matches the screen.

Safe next fixes (box first, then one approved live run):
1. Add `dmesg -n 1` (and `printf '\033[9;0]' > /dev/tty1`) to the roll-up
   preparation; it changes no driver code. Repeat #1 and #3 only.
2. In wd24adv, block console output during a mode (console_lock across the
   trial or `vt` KD_GRAPHICS via do_blank / con_is_visible) and redraw tty1
   after restore (`redraw_screen`/`update_screen`): QEMU model first.
3. pattern: also clear memory from the frame end to 512 KiB so a start shift
   shows black, and in 13h clear 64000-65535; add CR0C/0D/CR09 readback to
   /proc/wd24adv/live at expiry so the next log shows whether they moved.

### Re-run of #1 and #3 with quiet console, 2026-11-05 UTC

Build d9c0884 (wd24adv.ko 9c77fe90, wd24probe3.ko eb0a36e2), prep from
1c40faf (adds `dmesg -n 1; printf '\033[9;0]' > /dev/tty1`). Raw log:
docs/rerun-1-3-live-2026-11-05.txt. One line at a time, each sent only after
the previous returned to the prompt.

| Step | UTC | Result |
|---|---|---|
| read-only check | 01:01:43 | Xfbdev 727, wd90c24fb only, no ru.stop |
| kill 727 | 01:02:07 | X gone (usual `apm: set display` line) |
| text=1 + rmmod wd90c24fb | 01:02:24 | RM=0, Console VGA+ 80x25 |
| dmesg -n 1 + tty1 blank-off | 01:02:51 | Q=0 |
| #1 ru 640x480-8 + rmmod | 01:04:47 | G=9c77fe90001, OK-640x480-8, STEP1-OK |
| #3 ru 320x200-8 + rmmod | 01:05:49 | G=9c77fe90001, OK-320x200-8, STEP3-OK |
| insmod wd90c24fb | 01:06:41 | RC=0 |
| startx, wait | 01:07:31 / 01:08:06 | X-UP (Xfbdev 798) |
| wd24probe3 | 01:09:43 | RC=19, result=ok, 3CC=E3 GR0F=05 GR0E=01 CR11=8C CR29=85 CR34=A0 PR18=C7 PR19=54 SR06=0F (X baseline) |

The agent's Shell call for the insmod line was interrupted after the line had
already returned (RC=0 and the prompt are in the log). commands.log was
checked before the next line was sent.

Owner observations (verbatim): "#1, two sets of bars, almost imperceptible
defect where on bottom orange bar and rightward, bottom bars shifted up maybe
1 pixel ... #2 [= the 320x200 step], 16 bars, no second set on bottom,
letterboxed top and bottom". "Really looked very good."

**1-px shift (#1): this is how the test pattern is drawn, not a hardware fault.** wd24_pattern()
(wd24adv_core.c 1074-1110) picks the bar line per chunk with
`line[L >= 4 * WD24_PLANE]`, so the switch to the reversed set falls at
byte 262144 exactly, not at a row start. 262144 / 640 = 409.6: row 409 is
normal order for x < 384 and reversed for x >= 384. x = 384 is inside
reversed bar 15-9 = 6, which is AR06 (brown, which looks orange). So from the
orange bar rightward the bottom set starts one row higher (row 409, not 410).
That matches the observation exactly. The CRTC did not wrap at 256 KiB:
normal-order bars at the bottom would have meant a wrap.
#3 is as designed: 16 bars of 20 px, 320x200 doubled to 400 lines on the
480-line panel, which gives the letterbox.

**tty1 garble photo (after the re-run): from the 2026-11-04 run, not this
one.** The photo shows two garbled blocks, each directly above a `wd24adv:
live text: 0 (896, 336, mismatches 0)` line. Wide-spaced characters, roughly
every other one missing. Everything after (wd90c24fb found ..., text=1,
Console switching to colour VGA+ 80x25) is clean. Evidence:
- `live text` is pr_info (level 6, wd24adv_main.c 496). In this re-run
  `dmesg -n 1` was set at 01:02:51, before either insmod, so no wd24adv line
  could reach the console. The last level-6 lines printed to tty1 before the
  quieting are the `text=1` and `Console: switching` lines at 01:02:24. Those
  are the clean lines below the garble.
- The 2026-11-04 roll-up ran without quieting. There, the lines directly above
  each `live text: 0` are `live mode ...`, `... set: write "confirm" ... 15 s`
  and `live trial expiry: 0`. Those were printed while the chip was in the
  packed graphics mode (docs/rollup-live-2026-11-04.txt 39-44). The
  `confirm`/`control` text fits the fragments.
- Likely mechanism (inferred from the code; not tested on the device): vgacon writes
  char/attr words to B8000 while the chip is in chain-4/packed mode. There is
  no odd/even, so the bytes go to the wrong planes and offsets. Plane 0
  (chars) and plane 1 (attrs) end up interleaved, which shows as every other
  character. wd24adv's text restore rewrites the 4 saved planes, but vgacon
  lines printed during the trial are drawn over the saved image after the
  save. vgacon writes straight into VRAM, so the bad cells stay there. When
  fbcon (wd90c24fb) takes over and later hands back, the VRAM content is copied
  to and from the vc buffer, so the garble carries over to later switches. The
  exact point where it was baked in (during the trial vs. at restore) is
  UNKNOWN.
- Consequence: with `dmesg -n 1` there is no new garble. A fresh screen (clear
  tty1, or reboot) is needed to confirm. Not done (no device experiments).

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

### S1 live attempt 2026-11-02: not run (vermagic)

The 945c255 module (sha `3d700867…`) was refused at load: `version magic
'6.18.57-tc755 mod_unload 486 ' should be '6.18.57-tc755x mod_unload 486 '`,
insmod RC=8. S1 made no port accesses. The run stopped there; wd90c24fb and X
were restored, and the wd24probe3 baseline matched all nine registers.
Raw log: [s1-live-2026-11-02.txt](s1-live-2026-11-02.txt).
Live modules must be built against the **6.18.57-tc755x** tree, not
`build/kernel-obj` (6.18.57-tc755). A tc755x rebuild of the same source has
sha256 `d5973eae…` (needs its own approval). The 755C line editor truncates
input at about 500 characters, so keep each command line under ~400.

### S1 live run 2026-11-02: OK (tc755x build)

Build `d5973eae…` (945c255 source, tc755x tree). Two runs, both `S1 result:
OK`, `/proc/wd24adv/s1` byte-identical (md5 `ebd7b296…`). Entry check passed
(14 index writes, 0 data); 23C0h at entry 1002/1002; 3C1h:3C0h = 3f20; strict
echo 1; 51 23C0h writes, 0 23C2h/23C4h writes, 0 guard refusals, 105 reads;
restore wrote 1002, read 1002. Every select echoed in 23C0h. 23C2h:

| idx | blk 1 | blk 2 |
|---|---|---|
| 0 | 0108 | 0000 |
| 1 | 1000 | 1000 |
| 2 | 210b | 2014 |
| 3 | 3004 | 3001 |
| 4 | 410b | 4000 |
| 5 | 5004 | 5041 |
| 6 | 61e4 | 617a |
| 7 | 713c | 7047 |
| 8 | 8280 | 8000 |
| 9 | 9300 | |
| A | a000 | |
| B | b0a5 | |
| C | c000 | |
| D | d000 | |
| E | e0ff | |
| F | f0ff | |

Pass 2 equals pass 1. Restore (wd90c24fb + X) and the wd24probe3 baseline
matched all nine registers. Raw log: [s1-live-2026-11-02-run.txt](s1-live-2026-11-02-run.txt).
