# Roadmap

Principle: the tc32 floppy stays *de minimis*. The only thing this track
asks of the core kernel is **loadable module support**; every driver is a
module inside a `.tcz` on the `tce` disk (IDE, CF card, or a second floppy).

All numbers below are measured on this repo's build (Linux 6.18.57, i486,
`-Os`), in QEMU `-M isapc -cpu 486`. "Resident" is the sum of the
page-rounded sizes in `/proc/modules` (x86 forces `STRICT_MODULE_RWX`, so
every module costs at least 3 pages = 12 KiB). "MemFree drop" additionally
counts the loop device, squashfs metadata and page cache for that `.tcz`.

## 0. Module support in the core (done in this PR)

| item | floppy cost | RAM | status |
|---|---|---|---|
| `CONFIG_MODULES` + `MODULE_UNLOAD` + `TRIM_UNUSED_KSYMS` | **+20 KiB** vmlinuz (1,032,704 → 1,053,184 B; 151 KiB still free on the FAT) | -172 KiB free RAM after boot (core suite at 32 MB: 24,324 → 24,152 kB) | built + tested |
| ... without `TRIM_UNUSED_KSYMS` | +53 KiB (all ~thousands of EXPORT_SYMBOLs kept) | | measured; rejected as default |
| `modutils.tcz` (static busybox insmod/rmmod/lsmod/modprobe/depmod/modinfo) | 0 (72 KiB .tcz on tce disk) | ~0.2 MB MemFree | built + tested |

Trade-off of `TRIM_UNUSED_KSYMS`: an out-of-tree module may only use
exported symbols that some in-tree `=m` module uses or that are listed in
`config/ksyms-keep.txt`. A new symbol means rebuilding the core kernel.
`make ksyms-keep` regenerates the list. Turn it off with `TRIM_KSYMS=0`.

Proposed core change (for tinycore-32-ram, not done here): add a
`kernel-modules.fragment` equal to `config/kernel-modules.fragment` and
build with it by default, so module-capable kernels are the one artefact.

## 1. Video: WD90C24A (priority 1)

| stage | what | module size (.ko) | .tcz | resident RAM | status |
|---|---|---|---|---|---|
| 1a | `vga16fb` + fbcon + `tc32-vgafb-dev` shim: 640x480x4bpp planar, 80x30 console | fb 60 K, vga16fb 14 K, helpers ~35 K, shim 3.5 K | `fb-vga16.tcz` **40 KiB** | **152 KiB** (9 modules), MemFree drop ~0.65 MB | **done, QEMU-verified (screenshot)** |
| 1b | `wd90c24fb` stage 1: XFree86-style detect (signature, memory, panel, C24A blocks), restores everything | 6 K | 4 KiB | 12 KiB | done; unit tests on a mock chip + QEMU negative probe; see [WD90C24.md](WD90C24.md) |
| 1c | native `wd90c24fb`: 640x480x8 packed via PR0(A) banking, 300 KiB shadow, save/restore, self-test, `trial`/`keep`, `.tcz` fallback to vga16fb | 12 K | 53 KiB (`fb-wd90c24.tcz`, incl. fb stack) | ~150 KiB modules + 300 KiB shadow | done; unit tests + QEMU fail-safe; panel picture needs hardware |
| 1d | BitBLT engine (C31-style, WD90C24A only) for fbcon copyarea/fillrect: fast scrolling; hardware cursor | +5-10 K | | | new |
| 1e | panel/CRT: LCD only, CRT only (up to 1024x768x8), simultaneous; DSTN (755CS) frame-buffer split; follow Fn+F7 | +5 K | | | new |
| 1f | APM suspend/resume save/restore (**done**, stage 5); MCLK programming 44.3 → 55.0 MHz (**not done**: needs a stable value measured on a real 755C) | +2 K | | | partly done |

Reference material for 1c-1f: XFree86 3.3.6 SVGA `wd` driver sources
(wd90c24 chipset paths), VGADOC Paradise chapter, 86Box `vid_paradise.c`
(WD90C30/C31 behaviour, close to C24 for banking and the C31 blitter), the
755C video BIOS (at E0000) for panel timing. Development loop: build and
unit-check register paths in 86Box with a WD90C30/C31 card, then real 755C
over serial console (see docs/EMULATORS.md).

## 2. Audio: CS4248 (priority 2)

| item | .ko | .tcz | resident RAM | status |
|---|---|---|---|---|
| `snd-ad1848 thinkpad=1` + ALSA core + OSS PCM emulation (`/dev/dsp`, so busybox-only userland can play raw PCM) | ad1848 8 K, wss-lib 23 K, snd 62 K, pcm 86 K, timer 24 K, pcm-oss 35 K, soundcore 8 K | `snd-cs4248.tcz` **84 KiB** | **~168 KiB** (7 modules), MemFree drop ~1.3 MB at load | **done**; QEMU: binds `cs4231a`, 24 KiB played through DMA/IRQ |
| `snd-cs4231` variant (QEMU / other WSS cards) | | 96 KiB | ~212 KiB | done, QEMU-verified |
| trims: build without `SND_PCM_OSS` once a tiny ALSA player exists; `SND_PCM_TIMER=n`; drop `snd-rawmidi`/`mpu401` from the 755C path | -35..-60 K | | -50..-80 KiB | todo |
| real hardware: confirm I/O 0x4E30/IRQ 10/DMA 0, `thinkpad=1` power-up, APM suspend/resume (wss_lib already handles the ThinkPad power bit) | | | | needs 755C |
| tiny static player (`.wav`/raw → `/dev/dsp`) | | ~10 KiB | | todo |

## 3. PC Card: 2x Ricoh RF5C266 (priority 3)

| item | .ko | .tcz | resident | status |
|---|---|---|---|---|
| `pcmcia_core` + `pcmcia_rsrc` + `i82365` + `pcmcia` (ds) | 17+12+20+37 K | `pcmcia.tcz` **48 KiB** (incl. below) | ~104 KiB (all 7) | built; QEMU has no bridge: core loads, i82365 reports no device |
| `pata_pcmcia`: CompactFlash/ATA cards as a `tce=` disk or root, the cheapest storage upgrade | 18 K | (in pcmcia.tcz) | | built |
| `serial_cs`: PC Card modems / serial | 35 K | (in pcmcia.tcz) | | built |
| userspace: none needed for 16-bit cards on 6.18 (CIS parsed in kernel, `PCMCIA_LOAD_CIS` uses firmware loader only for broken CIS) | | | | |
| **networking (NE2000-class `pcnet_cs`, 3c589, xirc2ps, smc91c92)** needs `CONFIG_NET` + `INET` + `NETDEVICES` in the **core** (bool, cannot be a module): est. **+250-350 KiB** compressed. Does **not** fit the 151 KiB left on the floppy | | | 1-2 MB | blocked on a core decision (open question) |
| out-of-tree copy of `i82365.c` in case the core ever moves past 7.0 (removed in 7.1) | | | | only if needed |

## 4. APM, hotkeys, suspend (priority 4)

| item | cost | status |
|---|---|---|
| APM driver: already **core** (`/proc/apm`, power-off, `/dev/apm_bios`) | 0 | core |
| `apm` CLI (status, `-m`, `-v`, `-s` suspend / `-S` standby via `APM_IOC_*`): static musl tool `tools/apm` in `apm-tools.tcz` | ~24 KiB .tcz (42 KiB static binary) | done (QEMU: status + suspend round trip; real suspend needs 755C) |
| Fn keys: volume, blank, display switch, invert, power mode, suspend are **BIOS/EC functions**; nothing to drive. Make sure drivers survive them: vga16fb/wd90c24fb must re-read state after Fn+F7; snd must re-power after resume | 0 | verify on hardware |
| Suspend-to-RAM (Fn+F4) and suspend-to-disk (Fn+F12, needs a type 0xA0 partition / `PHDISK` file, `tphdisk` port as a 2 KiB static tool) | ~4 KiB | todo |
| `CONFIG_APM_CPU_IDLE` / `idle_threshold` battery tuning, `APM_DISPLAY_BLANK` | core option, ~1 KiB | proposal for core |
| SMAPI (old `thinkpad`/`tpctl` module): only if the 755C BIOS exposes it; port to 6.18 as a char device | est. 10-20 K | research |

## 5. Smaller items

| item | status |
|---|---|
| TrackPoint: `psmouse` (TrackPoint protocol only) + `mousedev` + `evdev`: `trackpoint.tcz` **20 KiB**, ~48 KiB resident. QEMU: binds ImExPS/2 on i8042 AUX | **done** |
| console mouse (gpm-like, static) | todo |
| parallel port: `parport_pc`, `lp`, `pata_parport` (Zip/backpack CD) as `parport.tcz` | todo |
| PC speaker (`pcspkr`) | todo |
| `floppy=thinkpad` on the core's default APPEND line | proposal for core (open question) |

## Totals (all extensions loaded at once, QEMU 32 MB)

27 modules, **516 KiB resident**; MemFree 24.4 MB → 19.3 MB (includes
page cache and 7 loop devices). Same run at **16 MB**: MemFree 8.2 MB →
2.8 MB, every check still passes. Realistic 755C set (fb + sound +
trackpoint + pcmcia) ≈ 470 KiB resident.
