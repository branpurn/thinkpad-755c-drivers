# tc755: ThinkPad 755C drivers for tc32 (tinycore-32-ram) as modules / .tcz
# Needs a built tinycore-32-ram checkout next to this one (or TC32_DIR=...).
#   make            kernel + out-of-tree modules + .tcz + test floppy/disk
#   make tools      static musl user-space tools (tools/*/, e.g. apm) into out/bin
#   make unit       host unit tests of the WD90C24 register layer (mock chip)
#   make test       unit + QEMU (-M isapc -cpu 486 + cs4231a): load every .tcz, 32 + 16 MB
#   make test-core  run tinycore-32-ram's own test suite on the module-capable floppy
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

scan:
	scripts/scan-public.sh

clean:
	scripts/clean-oot.sh; rm -rf out build/kernel-obj build/tcz-stage build/tcz-disk

distclean:
	scripts/clean-oot.sh; rm -rf out build dl

.PHONY: all kernel oot modutils tools tcz floppy disk sizes unit test test-core ksyms-keep scan clean distclean
