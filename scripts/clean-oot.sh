#!/bin/sh
. "$(dirname "$0")/common.sh"
for d in "$TOP"/modules/*/; do [ -f "$d/Kbuild" ] && make -s -C "$KDIR" ARCH=i386 M="$d" clean 2>/dev/null || true; done
