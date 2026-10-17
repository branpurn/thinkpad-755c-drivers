#!/bin/sh
# Small static i486 musl user-space tools (tools/*/) into out/bin/.
. "$(dirname "$0")/common.sh"
CC="${MUSL_TC_PREFIX}gcc"
command -v "$CC" >/dev/null || die "musl toolchain missing: run 'make tools' (or 'make') in $TC32_DIR"
mkdir -p "$OUT/bin"
for d in "$TOP"/tools/*/; do
    n=$(basename "$d")
    "$CC" -static -march=i486 -mtune=i486 -Os -fno-pie -no-pie -ffunction-sections -fdata-sections -Wl,--gc-sections -Wall -Wextra -Werror \
        -o "$OUT/bin/$n" "$d"/*.c || die "build failed: $n"
    "${MUSL_TC_PREFIX}strip" -s "$OUT/bin/$n"
    log "tool $n: $(stat -c %s "$OUT/bin/$n") bytes"
done
