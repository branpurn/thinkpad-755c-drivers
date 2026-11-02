# tc755: ThinkPad 755C drivers for tc32 (tinycore-32-ram) as modules / .tcz
# Needs a built tinycore-32-ram checkout next to this one (or TC32_DIR=...).
#   make            kernel + out-of-tree modules + .tcz + test floppy/disk
#   make tools      static musl user-space tools (tools/*/, e.g. apm) into out/bin
#   make unit       host unit tests of the WD90C24 register layer (mock chip)
#   make test       unit + QEMU (-M isapc -cpu 486 + cs4231a): load every .tcz, 32 + 16 MB
#   make test-core  run tinycore-32-ram's own test suite on the module-capable floppy
#   make sdl        SDL2 + wdfb video backend, static i486 (sdl/), out/sdl
#   make test-sdl   QEMU test of the wdfb backend (needs kernel, oot, sdl)
#   make doom       doomgeneric for wd90c24fb, no SDL, static i486 (doom/), out/doom
#   make test-doom  QEMU test + timedemos of doom-wdfb: IWAD=/path/DOOM.WAD (never committed)
#   make scan       public-repo hygiene scan (run before every push)
.POSIX:
SHELL = /bin/sh

all: tcz floppy disk sizes

kernel:
	scripts/build-kernel.sh

oot: kernel
	scripts/build-oot.sh

modutils:
	scripts/build-modutils.sh

tools:
	scripts/build-tools.sh

tcz: oot modutils tools
	scripts/mktcz.sh

floppy: kernel
	scripts/mkfloppy.sh

disk: tcz
	tests/mk-test-disk.sh

sizes:
	@scripts/sizes.sh

unit:
	$(MAKE) -C tests/unit

test: unit all
	tests/run.sh

test-core: floppy
	scripts/test-core.sh

ksyms-keep: oot
	scripts/ksyms-keep.sh

sdl:
	sdl/build.sh

test-sdl: kernel oot modutils sdl
	sdl/tests/qemu-wdfb.py

doom:
	doom/build.sh

test-doom: kernel oot modutils doom
	@test -n "$(IWAD)" || { echo 'usage: make test-doom IWAD=/path/to/DOOM.WAD [IWAD2=freedoom1.wad]' >&2; exit 2; }
	i2="$(IWAD2)"; doom/tests/qemu-doom.py --iwad "$(IWAD)" $${i2:+--iwad "$$i2"}

scan:
	scripts/scan-public.sh

clean:
	scripts/clean-oot.sh; rm -rf out build/kernel-obj build/tcz-stage build/tcz-disk

distclean:
	scripts/clean-oot.sh; rm -rf out build dl

.PHONY: all kernel oot modutils tools tcz floppy disk sizes unit test test-core ksyms-keep sdl test-sdl doom test-doom scan clean distclean
