#!/bin/sh
# Fail if a wd24adv.ko names an undefined symbol that is not in the
# floppy export allowlist. Symbols only in that file. No comments.
#   scripts/check-ko-exports.sh path/to/wd24adv.ko
#   scripts/check-ko-exports.sh --undef path/to/symbol-list
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
LIST=$ROOT/modules/wd24adv/floppy-exports.txt
MODE=ko
SRC=
if [ "${1:-}" = "--undef" ]; then
	MODE=list
	SRC=${2:-}
else
	SRC=${1:-}
fi
if [ -z "$SRC" ] || [ ! -f "$SRC" ] || [ ! -f "$LIST" ]; then
	echo "check-ko-exports: need a .ko or --undef list, and $LIST" >&2
	exit 2
fi
if grep -q '__kmalloc_large_noprof' "$LIST"; then
	echo "check-ko-exports: allowlist names __kmalloc_large_noprof" >&2
	exit 1
fi
fail=0
if [ "$MODE" = "ko" ]; then
	syms=$(nm -u "$SRC" | awk '{print $NF}')
else
	syms=$(sed -n '/^[A-Za-z_.]/p' "$SRC")
fi
for s in $syms; do
	if ! grep -qx "$s" "$LIST"; then
		echo "check-ko-exports: $s is not in the floppy allowlist" >&2
		fail=1
	fi
done
exit "$fail"
