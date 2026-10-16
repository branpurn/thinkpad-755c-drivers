#!/bin/sh
# Shared helpers. Source from other scripts: . "$(dirname "$0")/common.sh"
# This repo builds ON TOP of a tinycore-32-ram checkout (the "core"):
#   TC32_DIR   path to the tinycore-32-ram tree (default: ../tinycore-32-ram)
#   LINUX_SRC  kernel source (default: $TC32_DIR/build/linux-$LINUX_VER, used
#              read-only via O=; unpacked here if the core has not built yet)
#   KDIR       kernel build dir that modules are built against
#              (default: build/kernel-obj = core config + modules fragments)
set -eu
TOP=$(cd "$(dirname "$0")/.." && pwd)
TC32_DIR=${TC32_DIR:-$(cd "$TOP/.." && pwd)/tinycore-32-ram}
[ -f "$TC32_DIR/versions.env" ] || { echo "ERROR: TC32_DIR=$TC32_DIR is not a tinycore-32-ram checkout" >&2; exit 2; }
TC32_DIR=$(cd "$TC32_DIR" && pwd)
. "$TC32_DIR/versions.env"
DL="$TOP/dl"; BUILD="$TOP/build"; OUT="$TOP/out"
KDIR=${KDIR:-$BUILD/kernel-obj}
LINUX_SRC=${LINUX_SRC:-$TC32_DIR/build/linux-$LINUX_VER}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}
export PATH="$TC32_DIR/build/host/bin:$TC32_DIR/build/toolchain/i486-linux-musl-cross/bin:$PATH"
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1767225600}   # 2026-01-01T00:00:00Z
export KBUILD_BUILD_VERSION=1 KBUILD_BUILD_USER=tc32 KBUILD_BUILD_HOST=tc32 KBUILD_BUILD_TIMESTAMP="@${SOURCE_DATE_EPOCH}"
export MTOOLS_SKIP_CHECK=1
mkdir -p "$DL" "$BUILD" "$OUT"

log() { printf '==> %s\n' "$*" >&2; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

# fetch URL SHA256 -> local path (reuses the core's dl/ cache when present)
fetch() {
    url=$1 sum=$2 b=$(basename "$1") f="$DL/$(basename "$1")"
    [ -f "$f" ] || { [ -f "$TC32_DIR/dl/$b" ] && cp "$TC32_DIR/dl/$b" "$f"; } || true
    if [ ! -f "$f" ]; then
        log "downloading $url"
        curl -fL --retry 3 -o "$f.part" "$url" && mv "$f.part" "$f"
    fi
    echo "$sum  $f" | sha256sum -c --quiet - >&2 || die "checksum mismatch: $f"
    echo "$f"
}

kver() { cat "$KDIR/include/config/kernel.release" 2>/dev/null || die "no kernel in $KDIR: run 'make kernel'"; }
