# Validation harness

The picture is not the result. A run is good only when the log's
`VAL` lines pass and, for a VM recording, `check_footage.py` agrees
with those fields. The 755C is the confirmation machine. It is not
where the first look happens.

## VM flow

1. Build the model runner on the host:

```
cc -O2 -std=gnu11 -o /tmp/wd24-run \
  tools/wd24-validate/wd24_run.c tools/wd24-validate/wd24_chip.c
```

2. Short pass (host tests use this):

```
WD24X_VALIDATE_MODEL=1 WD24X_VALIDATE_BIN=/tmp/wd24-run \
  sh tools/wd24-validate/validate.sh
```

3. Field recording. `WD24X_VALIDATE_FULL=1` holds each phase card for
   2 seconds at the computed field rate and holds each strobe page for
   35 fields (about half a second). ffmpeg writes the MP4. The sample
   list is what the footage checker reads.

```
mkdir -p /tmp/wd24-val-ppm
WD24X_VALIDATE_MODEL=1 WD24X_VALIDATE_BIN=/tmp/wd24-run \
WD24X_VALIDATE_FULL=1 \
WD24X_VALIDATE_MP4=/tmp/wd24-validate.mp4 \
WD24X_VALIDATE_PPM=/tmp/wd24-val-ppm \
WD24X_VALIDATE_SAMPLES=/tmp/wd24-val-samples.txt \
  sh tools/wd24-validate/validate.sh
python3 tools/wd24-validate/check_log.py
python3 tools/wd24-validate/check_footage.py \
  /tmp/wd24-val-samples.txt /tmp/wd24-val-ppm
```

`check_log.py` prints the verdict table. `check_footage.py` prints one
row per sampled pixel: noise, a torn solid row, or a dropped field
number.

The guest boot, when the image exists, is `tools/wd24-validate/qemu/run-qemu.sh`.
QEMU is pinned at 9.2.x. The device is the same `wd24_chip.c`. The
command is `-cpu 486 -m 36 -icount shift=4,sleep=off`. This VM has no
`qemu-system-i386` and no tc755x vmlinuz or initrd, so that boot is
not a result here. The MP4 is the model at the CRTC field rate, not a
grab of a booted guest.

### What a reviewer looks for

| phase | must be seen | fail |
| --- | --- | --- |
| card 1..5 | full-frame color, one large digit, 2 seconds, field counter in the top band | wrong color, missing digit, a tear through the row |
| page 0..2 | red, green, blue, latched `CR0C` `00` / `40` / `80`, readback sum 0 | checksum mismatch, latch timeout, start not the page |
| hud | rows 168..199 hash matches the CPU-window readback | `rows_bad` not 0 |
| ghost | full 64000-byte readback matches the engine buffer on the sampled frame | `frame_bad` not 0 |
| strobe | three pages, 35 fields each, digit is the page number | torn row, skipped field number, latch timeout |
| perf | a fps figure, `latch_to` 0, no interval above twice the median | a timeout, a missing fps, an over-2x interval |
| restore | latched start back at 0 | a non-zero mismatch |

`thresholds.conf` holds those perf limits. A Windows 98 fps is not one
of them. That comparison needs the same demo.

## Hardware confirmation

Do not treat the VM as the panel. On the 755C, with the module loaded:

```
sh tools/wd24-validate/validate.sh
```

That path needs `/dev/fb0`. Page patterns, the five cards (2 seconds
each) and the strobe use `WD24S6D_IOC_FLIP` and then read the window
back. HUD, ghost, and perf need a Doom log produced with
`WD24X_DOOM_VALIDATE=1` and `WD24X_DOOM_FRAMESTATS=1`. Point
`WD24X_DOOM_LOG` at that capture. Restore reads `mismatches` from
`/proc/wd24adv/live`. rmmod and the text console are still a person
at the machine: the script does not claim them from the model.

Calibration probe, later, not from this VM. Each line of
`tools/wd24-validate/probe.sh` is under 480 characters. The program
selects a CRTC index and reads. It does not write a data register.
`WD24X_PROBE_DRY=1` prints the plan. The interesting line is
`PROBE 3da high N low N edges N`, plus `PROBE cr00` through `cr18`
and `PROBE misc`. Those numbers replace the assumed 3DA bit 0 duty.

## Fidelity

| behavior | class | basis |
| --- | --- | --- |
| 75 MHz 486DX4, type 9545, no TSC (`-cpu 486`) | cited | `docs/s6d-kernel-speed.txt`; IBM ThinkPad 755C specifications |
| `-icount shift=4` (16 ns/insn, 62.5e6 insn/s, ~0.83 IPC) | assumed | clock is cited; the IPC is not measured |
| 36 MB RAM | cited | `docs/WD24ADV-DESKTOP-DESIGN.md` |
| PIT, HZ=250, periodic ticks, no TSC in the guest config | cited | `docs/s6d-kernel-speed.txt` audit of the tc755x config |
| 1 MiB VRAM, 64 KiB window at A0000 | cited | desktop design; VGADOC PR0A; mode table GR06=05 |
| PR5=05h unlocks PR0A; other values refused and logged | cited | `wd24adv_core.h`, hw.c 187 |
| PR0A bits 0-7 added at CPU address bit 12 | cited | VGADOC paradise.txt; `wd24adv_flip.h` |
| CR0C/CR0D load on 3DA bit 3 rising edge | cited | datasheet 10.12 and 15.8 |
| Field rate from CR00/CR06/CR07 and the 25.175 MHz dot clock | cited | `std_13h` in `wd24adv_modes.c`. std_13h computes 70.086 Hz (70086 millihertz) |
| 3DA bit 3 set for the CR10/CR11 retrace window | cited | datasheet 15.8; IBM VGA retrace-end nibble on those CR bytes |
| 3DA bit 0 set outside the CR01/CR12 display window | assumed | display-enable, not the blank-start registers. The probe's high/low counts are the measurement |
| 640x480 panel, 320x200 doubled by AR10 bit 6, 400 lines centred | cited | `wd24adv_modes.c` (PR19 centring, live value 54h, CR09=0x41) |
| DAC channels kept at 6 bits | cited | `wd24adv_cmap_cache.h` |
| 6-bit value expanded as v*255/63 in the recording | assumed | the file is 8-bit RGB; the panel analog map is not claimed |
| VRAM write stall default 0 | assumed | no datasheet latency is applied. `wd24_chip_set_stall_ns` logs a stall if one is set. An older flush note of about 8.6 MB/s is not this path |
| Extended registers other than the unlock and PR0A | unmodeled | the write is stored for readback, counted, and logged. It does not move the picture |
| Guest boot of 6.18.57-tc755x | not run | no qemu-system-i386, vmlinuz, or initrd on this machine |
| Real TFT gamma, BIOS font, and rmmod console | not proven | confirmation run only |

Unknown port writes increment a counter and are logged. A run with a
non-zero `unmodeled` or `unknown` count is not a clean model run.
The std_13h program leaves both at 0.

## What the VM cannot prove

The recording shows the latched start, the bank the CPU wrote, and the
scale the mode table describes. It does not show the 755C panel, the
BIOS PR19 value drifting, VRAM wait states, or a Doom timedemo on that
CPU. Sprite noise that exists only in the glass, with a matching
readback, will not appear in the model. That is why the hardware pass
still exists.
