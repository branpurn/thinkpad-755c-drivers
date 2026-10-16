#!/bin/sh
# Regression: the core's own QEMU suite must still pass with the
# module-capable kernel on the floppy (same initramfs, same boot menu).
. "$(dirname "$0")/common.sh"
[ -f "$TC32_DIR/out/test-disk.img" ] || sh "$TC32_DIR/tests/mk-test-disk.sh"
mkdir -p "$OUT/test-logs/core"
python3 "$TC32_DIR/tests/harness.py" --img "$OUT/tc755.img" --disk "$TC32_DIR/out/test-disk.img" --logdir "$OUT/test-logs/core" --mem "${MEM:-32}"
