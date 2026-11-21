#!/bin/sh
# The checker, plus a source scan so a 64 KiB kmalloc cannot return quietly.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
CHECK=$ROOT/scripts/check-ko-exports.sh
MAIN=$ROOT/modules/wd24adv/wd24adv_main.c
LIST=$ROOT/modules/wd24adv/floppy-exports.txt
fail=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

tmp=$(mktemp -d)
printf '%s\n' vfree vmalloc_noprof > "$tmp/good"
printf '%s\n' vfree __kmalloc_large_noprof > "$tmp/bad"
if sh "$CHECK" --undef "$tmp/good"; then ok; else bad "allowed"; fi
if sh "$CHECK" --undef "$tmp/bad"; then bad "large allowed"; else ok; fi
if grep -q '__kmalloc_large_noprof' "$LIST"; then bad "list has large"; else ok; fi
if grep -q 'kmalloc(WD24S6D_PRESENT_SRC' "$MAIN"; then bad "present kmalloc"; else ok; fi
if grep -E 'kmalloc\((8192|64000|65536)[,)]' "$MAIN"; then bad "big kmalloc"; else ok; fi
if grep -q 'vmalloc(WD24S6D_PRESENT_SRC)' "$MAIN"; then ok; else bad "present vmalloc"; fi
if grep -q -- '-Werror=frame-larger-than' "$ROOT/modules/wd24adv/Kbuild"; then ok; else bad "frame flag"; fi
rm -rf "$tmp"
echo "test_ko_exports: $pass passed, $fail failed"
[ "$fail" = 0 ]
