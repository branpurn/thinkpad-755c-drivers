#!/bin/sh
# Size budget: floppy cost of module support + every .tcz / module.
. "$(dirname "$0")/common.sh"
k=$(stat -c %s "$OUT/bzImage"); c=$(stat -c %s "$TC32_DIR/out/bzImage" 2>/dev/null || echo 0)
echo; printf '  %-34s %9s bytes\n' "core tc32 vmlinuz" "$c" "module-capable vmlinuz" "$k" "  delta (on the floppy)" "$((k - c))" \
  "free on tc755.img FAT" "$(cat "$OUT/floppy-free.txt" 2>/dev/null || echo '?')"
echo; printf '  %-24s %8s %10s\n' "extension" ".tcz" "unpacked"
for t in "$OUT"/tcz/*.tcz; do
    n=$(basename "$t" .tcz)
    printf '  %-24s %8s %10s\n' "$n" "$(stat -c %s "$t")" "$(du -sb "$BUILD/tcz-stage/$n" 2>/dev/null | cut -f1)"
done
