#!/bin/sh
# The 64000-byte aperture copy must be one movl per dword. rep movsl
# into the chain-4 window is what rotated the frame on the 755C.
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
loops=$(printf '%s\n' "$dump" | awk '
/^[[:xdigit:]]+ <DG_DrawFrame>/ {p=1}
/^[[:xdigit:]]+ </ && !/DG_DrawFrame/ {if (p) exit}
p && /cmp[[:space:]]+\$0xfa00/ {
	for (i = NR - 6; i <= NR; i++) if (i > 0) want[i] = 1
}
{ line[NR] = $0 }
END {
	for (i = 1; i <= NR; i++) if (want[i]) print line[i]
}
')
n=$(printf '%s\n' "$loops" | grep -c 'cmp[[:space:]]\+\$0xfa00' || true)
rep=$(printf '%s\n' "$loops" | grep -c 'rep movs' || true)
if [ "$n" -lt 1 ] || [ "$rep" -ne 0 ]; then
	echo "test_doom_copy_asm: FAIL loops=$n rep=$rep"
	printf '%s\n' "$loops"
	exit 1
fi
echo "test_doom_copy_asm: $n dword loops, no rep movs"
