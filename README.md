# thinkpad-755c-drivers (tc755)

Drivers for the **IBM ThinkPad 755C/755CS** delivered as **loadable kernel
modules inside Tiny Core-style `.tcz` extensions**, for the single-floppy
[tinycore-32-ram](https://github.com/branpurn/tinycore-32-ram) (tc32)
system. The floppy core stays minimal; everything here loads from the
`tce` directory on the IDE disk / CF card with `tce-load -i`.

Status: in-tree drivers packaged and **verified in QEMU** (486, ISA, 32 and
16 MB): framebuffer, CS4248 audio driver, TrackPoint, PC Card core, APM
tool. Native WD90C24A video driver: stages 1-5 (detect, 640x480x8, BITBLT,
LCD/CRT and panel power, APM suspend/resume), unit-tested against a register
model and QEMU-tested for the fail-safe paths. Results from a real 755C
(video, PC Card, ...): [docs/HW-CHECKLIST.md](docs/HW-CHECKLIST.md).

| `.tcz` | contents | size |
|---|---|---|
| `modutils.tcz` | static busybox `insmod rmmod lsmod modprobe depmod modinfo` (load first) | 72 KiB |
| `fb-vga16.tcz` | `fb` + fbcon + `vga16fb` + `tc32-vgafb-dev` shim: 640x480x16, 80x30 console | 40 KiB |
| `snd-cs4248.tcz` | ALSA core + `snd-ad1848` (755C defaults `port=0x4e30 irq=10 dma1=0 thinkpad=1`) + OSS `/dev/dsp` | 84 KiB |
| `snd-cs4231.tcz` | same with `snd-cs4231` (QEMU `cs4231a`, generic WSS cards) | 96 KiB |
| `trackpoint.tcz` | `psmouse` (TrackPoint protocol) + `mousedev` + `evdev` | 20 KiB |
| `pcmcia-755c.tcz` | PC Card core + `i82365` + `pata_pcmcia` (CF) + `serial_cs`; its hook sets the ISA socket resources ([docs/HARDWARE.md](docs/HARDWARE.md#pc-card-in-detail)). Verified on a real 755C | 48 KiB |
| `fb-wd90c24.tcz` | native `wd90c24fb`: 640x480, 256 colours, fbcon; falls back to vga16fb; damage + BITBLT for tc32's Xfbdev ([docs/WD90C24.md](docs/WD90C24.md)) | 52 KiB |
| `apm-tools.tcz` | static `apm`: AC/battery status, `-s` suspend, `-S` standby | 24 KiB |
| `wd90c24-probe.tcz` | `wd90c24fb probe_only=1`: identifies the chip, memory and panel state in dmesg | 40 KiB |

Cost to the floppy: **+20 KiB** of kernel for `CONFIG_MODULES` (with
`TRIM_UNUSED_KSYMS`); 151 KiB stays free. See
[docs/HARDWARE.md](docs/HARDWARE.md) (device survey, sources),
[docs/ROADMAP.md](docs/ROADMAP.md) (priorities, size/RAM per module),
[docs/EMULATORS.md](docs/EMULATORS.md) (what QEMU / 86Box can test).

## Build

Prerequisite: a built tinycore-32-ram checkout (its toolchain, kernel source,
host tools and `out/tc32.img` are reused), by default next to this one:

```sh
git clone https://github.com/branpurn/tinycore-32-ram ../tinycore-32-ram
make -C ../tinycore-32-ram          # once
make                                # kernel, modules, .tcz, test floppy + disk
make test                           # QEMU: load every .tcz at 32 and 16 MB
make test-core                      # tc32's own suite on the module-capable floppy
make scan                           # public-repo hygiene scan before pushing
```

Variables: `TC32_DIR` (core checkout), `LINUX_SRC` (kernel source, used
read-only via `O=`), `KDIR` (kernel build dir modules are built against,
default `build/kernel-obj`), `TRIM_KSYMS=0` (keep all exported symbols,
+33 KiB), `JOBS`.

What `make` does:

1. `scripts/build-kernel.sh`: tinyconfig + the core's `kernel-{base,hdd,tce}.fragment`
   + `config/kernel-modules.fragment` (`CONFIG_MODULES=y`, the only core
   change) + `config/kernel-drivers.fragment` (every 755C driver `=m`).
   Fails if Kconfig drops any requested option.
2. `scripts/build-oot.sh`: out-of-tree modules in `modules/*/` via
   `make -C $KDIR M=...` (any build dir with the same `.config` works).
3. `scripts/build-modutils.sh`: the modutils busybox (core's pinned
   busybox + musl i486 toolchain).
4. `scripts/mktcz.sh`: one `.tcz` per `ext/*.ext`; module dependencies come
   from `modinfo`, load order is generated into
   `/usr/local/tce.installed/<name>`, which tc32's `tce-load` runs once (Tiny Core convention).
5. `scripts/mkfloppy.sh`: `out/tc755.img` = the core floppy with the
   module-capable `vmlinuz` swapped in. `tests/mk-test-disk.sh`: IDE disk
   with all `.tcz` in `tce/optional/`.

## Use (on tc32)

```sh
tce-load -i modutils.tcz      # always first
tce-load -i fb-vga16.tcz      # console switches to the 640x480 framebuffer
tce-load -i snd-cs4248.tcz    # then: cat sound.ub > /dev/dsp  (8 kHz u8 default)
```

Module parameters can be overridden from the boot prompt with the usual
`module.param=value` syntax, e.g.
`tc32 tce=hda1 snd_ad1848.irq=5`. Modules are tied to the exact kernel
build (`vermagic 6.18.57-tc755 mod_unload 486`): boot the matching `vmlinuz`.
The stock tc32 kernel (`6.18.57`) has module support too, but refuses
these modules: the driver config makes some fb/video helpers built-in, so
the two kernels are not ABI-compatible.

## SDL2 (wdfb backend)

`sdl/` holds an SDL2 video driver for wd90c24fb: 8bpp, `FBIOPUTCMAP`, and
one damage ioctl per updated rect. It uses evdev or a raw console keyboard,
no X, and restores the VT on exit. `make sdl` fetches the pinned SDL2
2.32.10 (sha256 checked), patches it, and builds a static i486 `libSDL2.a`
plus `out/sdl/sdltest.i486`. `sdl/tests/qemu-wdfb.py` tests it under
`qemu-system-i386 -cpu 486` with `wd90c24fb fake=3`. See
[sdl/README.md](sdl/README.md).

## Doom (doomgeneric, no SDL)

`doom/` builds `doom-wdfb`: doomgeneric at a pinned commit (sha256 checked),
patched to draw its 8-bit frame straight into the wd90c24fb shadow at pitch
640, with one damage ioctl per frame. Full screen 640x480 by default,
`DOOM_WDFB_SCALE=2` for 640x400 or `=1` for 320x200 centred (fastest); no
sound yet. `make doom` builds it (static, i486);
`make test-doom IWAD=/path/to/DOOM.WAD` tests it under QEMU. No WAD is
included and none may be committed. See [doom/README.md](doom/README.md).

## Adding a driver

* in-tree: add `CONFIG_FOO=m` to `config/kernel-drivers.fragment` and an
  `ext/foo.ext` (see [ext/README.md](ext/README.md)).
* out-of-tree: `modules/foo/{Kbuild,foo.c}`, then `make ksyms-keep` if it
  needs kernel symbols no in-tree module uses, then `make`.

## License

GPL-2.0 (see `LICENSE`), as kernel modules must be.
