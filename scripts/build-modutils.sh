#!/bin/sh
# Static i486 musl busybox with ONLY the module tools (insmod/rmmod/lsmod/
# modprobe/depmod/modinfo). Shipped as modutils.tcz so the core busybox on
# the floppy does not grow. Uses the core's pinned busybox + musl toolchain.
. "$(dirname "$0")/common.sh"
command -v "${MUSL_TC_PREFIX}gcc" >/dev/null || die "musl toolchain missing: run 'make tools' (or 'make') in $TC32_DIR"
tb=$(fetch "$BUSYBOX_URL" "$BUSYBOX_SHA256")
B="$BUILD/busybox-modutils"
if [ ! -f "$B/Makefile" ]; then
    rm -rf "$B" "$BUILD/busybox-$BUSYBOX_VER"; tar -C "$BUILD" -xf "$tb"; mv "$BUILD/busybox-$BUSYBOX_VER" "$B"
fi
cd "$B"
make -s allnoconfig >/dev/null
grep -vE '^\s*(#|$)' "$TOP/config/busybox-modutils.applets" | while IFS= read -r l; do
    sed -i "/^# CONFIG_$l is not set\$/d; /^CONFIG_$l=/d" .config; echo "CONFIG_$l=y" >> .config
done
sed -i 's|^CONFIG_CROSS_COMPILER_PREFIX=.*|CONFIG_CROSS_COMPILER_PREFIX="'"$MUSL_TC_PREFIX"'"|' .config
sed -i 's|^CONFIG_EXTRA_CFLAGS=.*|CONFIG_EXTRA_CFLAGS="-march=i486 -mtune=i486 -Os -fno-pie"|' .config
sed -i 's|^CONFIG_EXTRA_LDFLAGS=.*|CONFIG_EXTRA_LDFLAGS="-no-pie"|' .config
yes "" | make -s oldconfig >/dev/null 2>&1
grep -vE '^\s*(#|$)' "$TOP/config/busybox-modutils.applets" | while IFS= read -r l; do
    grep -q "^CONFIG_$l=y" .config || die "busybox: CONFIG_$l not applied"
done
make -s -j"$JOBS" busybox >/dev/null
"${MUSL_TC_PREFIX}strip" -s busybox
mkdir -p "$OUT/modutils"; cp busybox "$OUT/modutils/busybox"
log "modutils busybox: $(stat -c %s busybox) bytes: $(./busybox --list 2>/dev/null | tr '\n' ' ' || echo '(cross binary)')"
