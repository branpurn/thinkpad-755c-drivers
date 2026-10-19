# Real-hardware verification checklist (ThinkPad 755C / 755CS)

Everything below passed in QEMU (`make test`: 486, ISA, 32 and 16 MB) and
in the host unit tests (`make unit`, WD90C24 register model), but QEMU has
no WD90C24, no Ricoh PC Card bridge, no CS4248 at 0x4E30 and a SeaBIOS
APM that returns from "suspend" at once. Each item here needs a real 755C.
Record results (paste the serial log) in an issue per section.

## 0. Setup: serial console

The serial console is the safe channel: if the panel goes blank or shows
garbage, keep working over COM1 and restore text from there.

1. Null-modem cable from the 755C's COM1 (9-pin, or the port replicator) to
   a host. On the host: `picocom -b 9600 /dev/ttyUSB0` (or `screen
   /dev/ttyUSB0 9600`), 8N1, no flow control. Log everything:
   `picocom -b 9600 --logfile 755c-$(date +%F).log /dev/ttyUSB0`.
2. In ThinkPad Features / Easy-Setup: COM1 enabled at 0x3F8 / IRQ 4. Note
   (do not change yet) the audio settings and the display mode (LCD, CRT,
   both) as the BIOS reports them.
3. Write `out/tc755.img` to a 1.44 MB floppy; copy `out/tcz/*.tcz` (+
   `.dep`) to `tce/optional/` on the IDE disk or a CF card (ext2 or FAT).
4. Boot the default `tc32` label (console on VGA and COM1, 9600 8N1).
   The `boot:` prompt is mirrored on COM1; type options there if needed,
   e.g. `tc32 wd90c24fb.trial=60`.
5. Basic checks, all from the serial shell:
   - [ ] `uname -r` prints `6.18.57-tc755`.
   - [ ] `cat /proc/cmdline` contains `floppy=thinkpad`; swap floppies and
     `ls /mnt/fd0` twice: the second disk's contents show (disk-change line
     handled). If not, reboot with `floppy=0,thinkpad` and note it.
   - [ ] `cat /proc/meminfo | head -3` matches the installed RAM (4-36 MB).
   - [ ] `dmesg | grep -iE 'apm|ata|i8042|serial'`: APM BIOS found, IDE disk
     on `pata_legacy`, i8042 KBD + AUX ports, ttyS0 16550A.
   - [ ] `tce-load -i /mnt/sda1/tce/optional/modutils.tcz && lsmod` works.

Collect for every later section: `dmesg | tail -40` after each step.

## 1. WD90C24A video (wd90c24fb)

### 1.1 Detect (stage 1)
- [ ] `tce-load -i .../wd90c24-probe.tcz; dmesg | grep wd90c24fb`
  - names `WD90C24A/A2` (or plain `WD90C24`), signature `"24"`;
  - memory `1024 KiB` (755C) and no "dual-scan rule" on the 755C TFT
    (the 755CS DSTN may report dual scan: note it);
  - `panel: PR18=0x.. PR19=0x..`: LCD on / CRT off when booted on the panel
    only; repeat after Fn+F7 to CRT and to both, record all three PR19
    values;
  - "extended register blocks present" (A/A2); absent means a plain C24:
    then the BITBLT items below must report "CPU drawing only".
- [ ] The text screen is unchanged after the probe (it must touch nothing).

### 1.2 Mode set and fail-safe (stage 2)
For a first test on a new machine, load with a long trial, so text
comes back on its own:
`insmod /usr/local/lib/modules/6.18.57-tc755/extra/wd90c24fb.ko trial=60`
(`tce-load -i fb-wd90c24.tcz` uses no trial; boot with
`wd90c24fb.trial=60` to get one).
- [ ] dmesg: `banking self-test` did not fail; `fb0: WD90C24..., 640x480x8`.
- [ ] Panel: picture fills the 640x480 TFT, not shifted, no wrap or
  doubled lines, no flicker; the console text is readable (fbcon 80x30).
- [ ] Colours: `cat /dev/urandom > /dev/fb0` shows 256-colour noise; the
  16 console colours look right (no channel swapped; 6-bit DAC).
- [ ] `/proc/fb` shows `0 WD90C24`; `cat /sys/class/graphics/fb0/bits_per_pixel` = 8.
- [ ] After the trial (no `keep`): text mode is back by itself. Check
  fonts, colours, cursor, and that the BIOS panel stretch/centring
  is as before. `cat /proc/fb` is empty; `rmmod wd90c24fb` then works.
- [ ] Load again, `echo 1 > /sys/module/wd90c24fb/parameters/keep`, wait
  past the trial: stays in 640x480x8.
- [ ] `rmmod wd90c24fb` alone is refused ("Resource temporarily
  unavailable", fbcon holds it); `echo 1 > /sys/module/wd90c24fb/parameters/text`
  restores text, then `rmmod` succeeds and text is intact.
- [ ] Parameters: `enable=0` (nothing happens), `probe_only=1`, and with
  `fb-wd90c24.tcz` on a machine that declines (e.g. `wd90c24fb.enable=0`
  on the boot line): the .tcz falls back to vga16fb and the panel is lit.
- [ ] Write/mmap: `dd if=/dev/zero of=/dev/fb0 bs=640 count=240` blanks
  the top half only; an mmap client (any fb test program) updates within
  ~0.1 s.

### 1.3 BITBLT (stage 3, A/A2 only)
- [ ] dmesg: `BITBLT self-test passed: acceleration on`.
- [ ] Scroll test on the panel console:
  `t0=$(date +%s); seq 1 5000 > /dev/tty1; echo $(( $(date +%s) - t0 )) s`.
  Scrolling is clean (no stale lines, no smeared rows, cursor right).
  Compare with `accel=0`: record both times.
- [ ] No `BITBLT engine timed out` in dmesg after 10 minutes of heavy
  scrolling (`while :; do dmesg > /dev/tty1; done` for a while).
- [ ] With `accel=0`: identical picture, slower.

### 1.4 LCD / CRT (stage 4)
Attach an external VGA monitor for the CRT items.
- [ ] `output=auto` keeps what Fn+F7 / the BIOS chose.
- [ ] `output=lcd`, `output=crt`, `output=both`: each shows the picture on
  the right outputs; CRT timing is 640x480 @ 60 Hz (monitor OSD).
- [ ] Blank levels (docs/WD90C24.md "Blank levels"), each followed by
  `echo 0 > /sys/class/graphics/fb0/blank` (picture back after ~200 ms,
  no garbage or flash of noise, colours right):
  - `echo 1 > /sys/class/graphics/fb0/blank`: black picture, backlight
    still lit (look at the panel edge in a dark room);
  - `echo 2`, `echo 3`, `echo 4`: backlight off too (panel dark);
  - `echo 0 > /sys/module/wd90c24fb/parameters/apm_blank`, then `echo 4`:
    backlight off by PNLOFF alone (the register path); `echo 1 >
    .../apm_blank` again. With the old kernel (no
    `CONFIG_APM_DISPLAY_BLANK`) this is the only path.
  - X: `xset dpms force off` (if xset is installed) or wait for the screen
    saver: picture off; a key press brings X back fully redrawn.
- [ ] Console blanking: `printf '\033[9;1]' > /dev/tty1` (blank after
  1 min), wait, then a key press on the 755C keyboard brings it back.
- [ ] After `text=1` / trial end following `output=crt`: the panel comes
  back exactly as the BIOS had it.
- [ ] Fn+F7 while the fb is live: record what happens (BIOS SMI may change
  PR19 behind the driver). Known limitation: restore returns PR19 to the
  load-time setting.
- [ ] 755CS (DSTN): the same items, plus no half-screen ghosting.

### 1.5 Suspend / resume (stage 5)
- [ ] `tce-load -i apm-tools.tcz`, fb live with `keep=1`, then `apm -s`:
  the machine suspends (moon LED), wakes with the lid/Fn key, `apm -s`
  prints `resumed from suspend`, dmesg has
  `suspend: text mode handed back` and `resume: 640x480x8 restored`,
  the picture and colours are back, BITBLT still on.
- [ ] Same with Fn+F4 (BIOS-initiated suspend: APM event path).
- [ ] Resume with the lid closed / on battery / after Fn+F7 change.
- [ ] After resume: `text=1` and `rmmod` restore a correct text console.
- [ ] If resume fails: dmesg `resume: mode set failed`, text mode visible,
  `/proc/fb` empty. Record it.

### 1.6 X speed (stage 6)
- [ ] Xfbdev.log says `wd90c24fb: damage, BITBLT copies, BITBLT fills to
  the driver` (only `damage` on a plain C24 or with `accel=0`).
- [ ] Idle X: `top` shows next to no CPU (before: about a third went to
  the 10 Hz flush); `cat /sys/module/wd90c24fb/parameters/stats` twice,
  10 s apart: `bytes` barely moves, `full` not at all.
- [ ] Bus speed (text console, X stopped): `cat .../stats; for i in 1 2 3
  4 5 6 7 8 9 10; do dd if=/dev/zero of=/dev/fb0 bs=307200 count=1
  2>/dev/null; done; cat .../stats`: KiB/s = (bytes difference / 1024) /
  (flush_us difference / 1e6). Record it: it sets how much the BITBLT
  and damage paths save.
- [ ] rxvt scrolling (`ls -lR /usr`), dragging and resizing windows,
  opening xcalc/xedit: the picture follows smoothly and is right
  (compare with `echo 0 > /sys/module/wd90c24fb/parameters/xaccel` and a
  new startx, which gives the old 10 Hz flush).

### 1.7 86Box bench (before or alongside the 755C)
- [ ] WD90C30/C31 card in 86Box: `wd90c24-probe` names the chip;
  `insmod wd90c24fb.ko force=1 trial=30`: mode set, banking self-test,
  (C31) BITBLT self-test, text back after the trial (see docs/WD90C24.md).

## 2. Audio: CS4248 (snd-cs4248.tcz)
- [ ] `tce-load -i snd-cs4248.tcz` with the 755C defaults
  (`port=0x4e30 irq=10 dma1=0 thinkpad=1`): `cat /proc/asound/cards`
  lists the codec; dmesg has no "codec not found".
- [ ] If Easy-Setup moved the codec, override on the boot line
  (`snd_ad1848.port=... snd_ad1848.irq=...`) and note the values.
- [ ] Playback: `dd if=/dev/urandom of=/dev/dsp bs=1024 count=64` makes
  noise in the speaker; IRQ 10 count grows in `/proc/interrupts`.
- [ ] Volume Fn keys work during playback (BIOS/EC function).
- [ ] After `apm -s` / Fn+F4 resume: playback still works (thinkpad=1 power
  bit re-applied).
- [ ] `rmmod snd_pcm_oss snd_ad1848 snd_wss_lib` succeeds.

## 2b. Hardware cursor probe (wd90c24fb.hwcursor, off by default)

On the framebuffer console, no X running:

    P=/sys/module/wd90c24fb/parameters
    echo 1 > $P/hwcursor
    cat $P/cursor_test        # registers 0-8; 0= is 020 or 000 (cursor off)
    echo 1 > $P/cursor_test   # white arrow, black outline, tip at the centre
    cat $P/cursor_test        # expect 0=820 1=000 2=014 3=00f 4=000 5=000 6=140 7=0f0
    echo 2 > $P/cursor_test   # arrow tip exactly in the top-left corner
    echo 3 > $P/cursor_test   # tip on the bottom-right pixel (arrow mostly off-screen)
    echo 0 > $P/cursor_test   # gone

If the arrow is shifted, set `hwc_xoff`/`hwc_yoff` by the error and repeat.
Garbage instead of an arrow, or nothing: report the `cat` output and leave
hwcursor=0. Then `startx`: /tmp/Xfbdev.log says "wd90c24fb: hardware
cursor" and the X pointer moves without trails; `stats` shows `cursor=`
rising. `TC32X_SOFTCURSOR=1 startx` gives the old software cursor.

## 3. TrackPoint (trackpoint.tcz)
- [ ] `/dev/input/mice` appears; `hexdump -C /dev/input/mice`
  prints bytes when moving the TrackPoint and pressing both buttons.
- [ ] dmesg names `TrackPoint` (protocol detected) or a plain PS/2 mouse:
  record which. External PS/2 mouse: same test.
- [ ] Still works after suspend/resume.

## 4. PC Card (pcmcia.tcz)
- [ ] `tce-load -i pcmcia.tcz`: i82365 finds two Ricoh RF5C266 sockets at
  0x3E0; dmesg shows the socket count and IRQ probing.
- [ ] CF card in a PC Card adapter: `pata_pcmcia` binds, `/dev/sdb` (or
  next) appears, `mount` and read a file.
- [ ] Modem/serial card (if available): `serial_cs` binds, `/dev/ttyS1`.
- [ ] Hot eject and re-insert: no oops, device comes back.
- [ ] After suspend/resume with a card inserted.

## 5. APM (core + apm-tools.tcz)
- [ ] `apm -v`: APM BIOS 1.x version and flags.
- [ ] `apm` on AC and on battery: AC state, battery status and percentage
  change correctly; `apm -m` minutes are plausible or -1.
- [ ] `apm -s` (suspend to RAM) and `apm -S` (standby): enter and leave
  cleanly; serial console works again after resume (UART re-init).
- [ ] Fn+F12 hibernation needs a PHDISK file / 0xA0 partition: not tested
  by this repo; note only whether the BIOS refuses.
- [ ] `poweroff` turns the machine off (APM power-off).

## 6. Memory and size
- [ ] At 8 MB RAM: load every .tcz in turn; `free` after each, no OOM.
- [ ] Note MemFree before/after `fb-wd90c24` (300 KiB shadow + modules).

## Known limitations to confirm or refute
- MCLK is never programmed (BIOS value kept). Faster memory clock
  (44.3 → 55 MHz) is not attempted until a stable value is measured.
- Only one mode: 640x480x8. No hardware cursor, no 16 bpp.
- Fn+F7 while live is not tracked; restore uses the load-time PR19.
- The CRT is driven at the panel's 640x480 timing; no separate CRT modes.
