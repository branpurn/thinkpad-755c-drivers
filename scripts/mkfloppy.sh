#!/bin/sh
# out/tc755.img = the core tc32 floppy with vmlinuz replaced by the
# module-capable kernel. Everything else (syslinux, core.xz, boot menu) is
# byte-for-byte the core's, so this measures exactly what modules cost.
. "$(dirname "$0")/common.sh"
SRC=${TC32_IMG:-$TC32_DIR/out/tc32.img}
[ -f "$SRC" ] || die "missing $SRC: run 'make' in $TC32_DIR first"
[ -f "$OUT/bzImage" ] || die "missing out/bzImage: run 'make kernel'"
IMG="$OUT/tc755.img"
cp "$SRC" "$IMG"
cp "$OUT/bzImage" "$BUILD/vmlinuz"; touch -d "@$SOURCE_DATE_EPOCH" "$BUILD/vmlinuz"
mdel -i "$IMG" ::/vmlinuz
mcopy -m -i "$IMG" "$BUILD/vmlinuz" ::/vmlinuz || die "module-capable kernel does not fit on the floppy"
free=$(mdir -i "$IMG" ::/ | awk '/bytes free/{gsub(/[^0-9]/,""); print}')
log "tc755.img: vmlinuz $(stat -c %s "$OUT/bzImage") bytes (core $(stat -c %s "$TC32_DIR/out/bzImage") bytes); free on FAT: $free bytes"
echo "$free" > "$OUT/floppy-free.txt"
