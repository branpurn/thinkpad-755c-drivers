#!/bin/sh
# out/tcz-disk.img: IDE disk with sda1 = ext2 holding tce/optional/*.tcz
# (all extensions from out/tcz). No root needed.
. "$(dirname "$0")/../scripts/common.sh"
PATH=$PATH:/usr/sbin:/sbin
T="$BUILD/tcz-disk"; rm -rf "$T"; mkdir -p "$T/ext/tce/optional"
cp "$OUT"/tcz/*.tcz "$OUT"/tcz/*.tcz.dep "$T/ext/tce/optional/"
dd if=/dev/zero of="$T/p1" bs=1M count=15 status=none
mke2fs -q -t ext2 -O ^resize_inode,^dir_index,^ext_attr -b 1024 -d "$T/ext" -E root_owner=0:0 "$T/p1"
IMG="$OUT/tcz-disk.img"
dd if=/dev/zero of="$IMG" bs=1M count=16 status=none
printf 'label: dos\nstart=2048, size=30720, type=83\n' | sfdisk -q "$IMG"
dd if="$T/p1" of="$IMG" bs=512 seek=2048 conv=notrunc status=none
log "test disk: $IMG"
