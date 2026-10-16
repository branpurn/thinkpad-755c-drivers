#!/bin/sh
# Build the out-of-tree modules in modules/*/ against KDIR (default: the
# module-capable tc32 kernel from scripts/build-kernel.sh). Any kernel build
# dir with the same .config works:  KDIR=/path/to/kernel-obj make oot
. "$(dirname "$0")/common.sh"
[ -f "$KDIR/Module.symvers" ] || die "KDIR=$KDIR has no Module.symvers: run 'make kernel' (or point KDIR at a built tree with CONFIG_MODULES=y)"
grep -qx 'CONFIG_MODULES=y' "$KDIR/.config" || die "KDIR=$KDIR was built without CONFIG_MODULES"
KV=$(kver); dst="$OUT/modules/lib/modules/$KV/extra"; mkdir -p "$dst"
for d in "$TOP"/modules/*/; do
    [ -f "$d/Kbuild" ] || continue
    n=$(basename "$d"); log "out-of-tree module: $n"
    make -s -C "$KDIR" ARCH=i386 M="$d" -j"$JOBS" modules
    for k in "$d"/*.ko; do strip --strip-debug -o "$dst/$(basename "$k")" "$k"; done
done
ls -l "$dst" >&2
