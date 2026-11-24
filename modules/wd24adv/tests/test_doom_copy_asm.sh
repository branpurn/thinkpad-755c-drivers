#!/bin/sh
# The 64000-byte aperture copy stores with movl, four dwords a trip.
# 64000 is 0xfa00. The unrolled trip stops at 64000-16 (0xf9f0).
# rep movs is not used. The rotated frame was CR0C, not this loop.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
SRC=/tmp/doom-src/doomgeneric/doomgeneric
OBJ=/tmp/wd24-doom-copy-asm.o
fail=0
if [ ! -f "$SRC/doomgeneric.h" ]; then
	echo "test_doom_copy_asm: skip (doomgeneric pin not checked out)"
	exit 0
fi
if [ -x /tmp/src/i486-linux-musl-cross/bin/i486-linux-musl-gcc ]; then
	CC=/tmp/src/i486-linux-musl-cross/bin/i486-linux-musl-gcc
	OBJDUMP=/tmp/src/i486-linux-musl-cross/bin/i486-linux-musl-objdump
	ARCH="-march=i486 -mtune=i486"
else
	CC=gcc
	OBJDUMP=objdump
	ARCH="-m32 -march=i486 -mtune=i486"
fi
if ! $CC -O2 $ARCH -fomit-frame-pointer -DNDEBUG -std=gnu11 \
	-DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
	-DWD24X_DRAW_DIRECT \
	-I"$SRC" -I"$ROOT/tools/wd24-doom" -I"$ROOT/modules/wd24adv" \
	-c "$ROOT/tools/wd24-doom/doomgeneric_wd24.c" -o "$OBJ"; then
	echo "test_doom_copy_asm: failed to compile"
	exit 1
fi
dump=$($OBJDUMP -d --no-show-raw-insn "$OBJ")
# 64000 is the lea 0xfa00 that bounds the trip. The trip is four movl
# and add $0x10. A rep in that window means the stores were joined.
set -- $(printf '%s\n' "$dump" | awk '
/^[[:xdigit:]]+ <DG_DrawFrame>/ {p=1}
/^[[:xdigit:]]+ </ && !/DG_DrawFrame/ {if (p) exit}
p && /lea[[:space:]]+0xfa00\(/ { cap=18; bad=0; adds=0; movs=0; on=1 }
on && cap > 0 {
	if (/rep[[:space:]]+mov/) bad=1
	if (/add[[:space:]]+\$0x10,/) adds++
	if (/mov[[:space:]]+%e/) movs++
	cap--
	if (cap == 0) {
		if (adds && movs >= 4 && !bad) ok++
		else if (adds) fail++
		on=0
	}
}
END { print ok+0, fail+0 }
')
ok=$1
fail=$2
if [ "$ok" -lt 2 ] || [ "$fail" -ne 0 ]; then
	echo "test_doom_copy_asm: FAIL trips=$ok bad=$fail"
	exit 1
fi
echo "test_doom_copy_asm: $ok dword trips, no rep movs"
