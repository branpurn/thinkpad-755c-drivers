# Stage 0: capture what the 755C's own video BIOS does

Before `wd24adv` (modules/wd24adv) writes any WD90C24 extended register for
real, we want the IBM BIOS's own register sequences for each mode: what it
programs for 03h, 12h, 13h, the Paradise modes 5Ch-5Fh and the panel /
stretch services, including the panel routing (PR18/PR19), shadow CRTC
(CR34) and clock (PR68/PR69) values. Stage 0 has two parts:

1. **`vromdump`** (modules/vromdump, packaged as `vromdump.tcz`): a tiny,
   read-only module that snapshots the firmware into `/proc/vromdump/`.
2. **`harness/vbiostrace.py`**: runs that firmware (on the build host,
   never on the 755C) in a real-mode emulator against a fake VGA register
   file and logs every port write, decoded into WD register names.

The firmware images are IBM's: **dumps and traces of them stay out of
git** (`.gitignore` covers `stage0/dumps/`, `*.rom`, `vrom.bin`,
`sysbios.bin`, `lowmem.bin`). Only register knowledge derived from the
traces goes into the driver and docs. Do not disassemble the ROM into the
repo either.

## 1. Dump the ROM on the 755C

`vromdump.ko` only *reads* memory: at insmod it copies

| file | source | size |
|---|---|---|
| `/proc/vromdump/vrom` | video option ROM at C0000 (55AA header, byte 2 x 512) | e.g. 32 KiB; 32 KiB raw if no 55AA |
| `/proc/vromdump/sysbios` | E0000-FFFFF (the 755C's video BIOS may be folded into the system BIOS) | 128 KiB |
| `/proc/vromdump/lowmem` | 0000-04FF: interrupt vector table + BIOS data area | 1280 B |
| `/proc/vromdump/info` | text: ROM signature/size/checksum, INT 10h/1Fh/42h/43h/6Dh vectors, BDA video state | - |

into kernel memory and serves the copies; it writes no I/O port and no
device register, and it does not touch the WD90C24 or `wd90c24fb`, so it
can be loaded with or without either framebuffer driver. `scan=1` (off by
default) also lists 55AA signatures in C0000-DFFFF (reads only; that range
can hold PC Card memory windows, hence opt-in). `rmmod vromdump` frees
everything.

Build and package (host): `make oot tcz` gives `out/tcz/vromdump.tcz`
(+ `.dep`). Copy it with `modutils.tcz` to `tce/optional/` on the CF card
(as for the other extensions; docs/HW-CHECKLIST.md step 3) together with
`stage0/vromdump-save.sh`. **Do not use the internal disk's Win98
partition for this.**

On the 755C (serial shell, root), with the CF card at `/mnt/sdX1`:

```sh
tce-load -i /mnt/sdX1/tce/optional/modutils.tcz
tce-load -i /mnt/sdX1/tce/optional/vromdump.tcz   # = insmod vromdump.ko
cat /proc/vromdump/info
# either: to a writable FAT/ext2 medium (remount the CF card rw if needed)
mount -o remount,rw /mnt/sdX1
sh /mnt/sdX1/vromdump-save.sh /mnt/sdX1/vromdump   # writes vrom.bin sysbios.bin lowmem.bin + sha256
# or: over the serial console, with picocom --logfile on the host
sh /mnt/sdX1/vromdump-save.sh -serial
rmmod vromdump
```

`-serial` prints each file as hex between `### vromdump BEGIN/END`
markers; on the host: `stage0/harness/serial2bin.py 755c.log stage0/dumps`
rebuilds the files and checks length and sha256.

Checked in QEMU (`-M isapc -cpu 486`, tc755 floppy + test disk):
`tce-load vromdump.tcz` -> `info` shows a 55AA ROM of 38912 bytes with
sum8 0x00, INT 10h -> C000:514E, BDA mode 03h; both save paths work,
and `serial2bin.py` rebuilt all three files from the serial log with
matching sha256. The dumped ROM is QEMU's SeaVGABIOS (`vgabios-cirrus`,
isapc's default card) apart from 647 bytes at 0006h-9739h: SeaVGABIOS
keeps its variables in the shadowed ROM segment, so a dump is the
*running* image, not the pristine file (the 755C's ROM is expected to
behave like a normal read-only ROM; compare two dumps if in doubt).

What to look at in `info`: if `vrom` has `55AA` and INT 10h points into
`C000:xxxx`, run the harness with `--rom`. If there is no option ROM at
C0000 or INT 10h points into `E000`/`F000`, the video BIOS is inside the
system BIOS: use `--sysbios ... --lowmem ...` (step 2).

## 2. Trace the BIOS (host)

```sh
python3 -m venv .venv-vbt && .venv-vbt/bin/pip install -r stage0/harness/requirements.txt
.venv-vbt/bin/python stage0/harness/test_vbiostrace.py      # self-test on SeaVGABIOS (apt install seabios)
# 755C, option ROM at C0000:
.venv-vbt/bin/python stage0/harness/vbiostrace.py --rom stage0/dumps/vrom.bin \
    -o stage0/dumps/trace.txt --json stage0/dumps/trace.json
# 755C, video BIOS inside the system BIOS:
.venv-vbt/bin/python stage0/harness/vbiostrace.py --sysbios stage0/dumps/sysbios.bin \
    --lowmem stage0/dumps/lowmem.bin --rom stage0/dumps/vrom.bin --no-post -o stage0/dumps/trace.txt
```

Default calls: POST (`C000:0003`), INT 10h AX=0003, 0012, 0013, 005C,
005D, 005E, 005F, 007F BH=02 (Paradise status), 1A00. `--calls
AX[:BX],...` adds any other (e.g. the IBM panel/expansion services once
the trace shows which AH values the handler dispatches on), `--vbe`
probes 4F00/4F01/4F02, `--reads` logs port reads (useful to see chip-ID
probes), `--raw` logs index writes and every DAC byte.

Per call the trace shows: return AX/BX/CF, the BDA mode, every port
write as `CS:IP  REG := value` (`SR06/PR20`, `CR32/PR19`, `GR0F/PR5`,
`EXT-IDX` 23C0h, `LB0` 2DF0h, ...), INT 15h/1Ah and other BIOS calls the
ROM makes (stubbed: CF=1 AH=86h, i.e. "unsupported"; the log shows the
arguments so we learn which ThinkPad hooks it wants), and the register
file afterwards with `*` on what the call changed.

The fake VGA (`--chip wd90c24`, default) echoes every indexed register,
toggles 3DAh retrace bits and port 61h refresh, and models the WD90C24
ID readbacks (GR0F PR5 lock status, CR36/CR37 = "24" when CR34=0, GR0B
memory size from `--vram`). If the IBM BIOS probes something else and
takes a different path, `--reads` shows it; add the behaviour to
`FakeVGA` rather than guessing in the driver.

Self-test result on SeaVGABIOS 1.16.3 (Debian `seabios` package,
`vgabios-isavga.bin`): POST + all calls return; modes 03h/12h/13h write
the IBM VGA table values (MISC, CR00-09, SR00-04), load the palette and
clear VRAM; the `--lowmem --no-post` path (INT 10h through a dumped IVT)
reproduces the same port writes. `sample-seavgabios-isavga-mode13.txt`
is the trace for mode 13h.

## Limits

- No real WD90C24 behind it: registers read back what was written (plus
  the ID quirks above). Anything the BIOS *reads* from the panel/EC
  (panel ID straps in PR18, CMOS, EC ports) comes back as written/FFh,
  so the trace is the path for that answer; vary it with `FakeVGA`.
- Timers are faked (port 61h toggles each read, 3DAh toggles); the
  trace has no timing. The datasheet panel power delays (~100 ms) are
  not visible here: the driver uses the documented sequence.
- `sysbios.bin` code that calls SMI/EC/APM interfaces will hit stubs;
  those calls are listed in the trace.
