#!/bin/sh
# Regenerate config/ksyms-keep.txt from the out-of-tree modules' undefined
# symbols (needed with TRIM_KSYMS=1), then rebuild: make kernel oot
. "$(dirname "$0")/common.sh"
f="$TOP/config/ksyms-keep.txt"; hdr=$(grep '^#' "$f")
syms=$(for k in "$TOP"/modules/*/*.ko; do nm -u "$k" | awk '{print $2}'; done | grep -v '^__fentry__$' | sort -u)
printf '%s\n%s\n' "$hdr" "$syms" > "$f"
log "$(echo "$syms" | wc -l) symbols in $f; now run: make kernel oot"
