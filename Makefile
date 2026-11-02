# tc755: ThinkPad 755C drivers for tc32 (tinycore-32-ram) as modules / .tcz
# Needs a built tinycore-32-ram checkout next to this one (or TC32_DIR=...).
#   make            kernel + out-of-tree modules + .tcz + test floppy/disk
#   make tools      static musl user-space tools (tools/*/, e.g. apm) into out/bin
#   make unit       host unit tests of the WD90C24 register layer (mock chip)
#   make test       unit + QEMU (-M isapc -cpu 486 + cs4231a): load every .tcz, 32 + 16 MB
#   make test-core  run tinycore-32-ram's own test suite on the module-capable floppy
#   make sdl        SDL2 + wdfb video backend, static i486 (sdl/), out/sdl
#   make test-sdl   QEMU test of the wdfb backend (needs kernel, oot, sdl)
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

scan:
	scripts/scan-public.sh

clean:
	scripts/clean-oot.sh; rm -rf out build/kernel-obj build/tcz-stage build/tcz-disk

distclean:
	scripts/clean-oot.sh; rm -rf out build dl

.PHONY: all kernel oot modutils tools tcz floppy disk sizes unit test test-core ksyms-keep sdl test-sdl scan clean distclean

# --- wd24adv (experimental; separate from wd90c24fb, which is unchanged) ---
#   make wd24adv       build all out-of-tree modules incl. wd24adv + vromdump
#   make wd24adv-test  host tests of the wd24adv core and live backend
#   make wd24adv-qemu  QEMU -vga std test of wd24adv.ko (needs make wd24adv)
#   make stage0-test   self-test of the Stage 0 VGA BIOS trace harness
#                      (needs python3 with unicorn==2.1.4 and SeaVGABIOS)
wd24adv: oot

wd24adv-test:
	$(MAKE) -C modules/wd24adv/tests

wd24adv-qemu:
	python3 modules/wd24adv/tests/qemu-wd24adv.py

stage0-test:
	python3 stage0/harness/test_vbiostrace.py
