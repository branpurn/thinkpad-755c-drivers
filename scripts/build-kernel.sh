#!/bin/sh
# Build the module-capable tc32 kernel + all in-tree driver modules.
#   core fragments:  $TC32_DIR/config/kernel-{base,hdd,tce}.fragment
#   + config/kernel-modules.fragment   (CONFIG_MODULES=y: the only core change)
#   + config/kernel-drivers.fragment   (755C drivers, all =m)
# Output: build/kernel-obj (= KDIR for out-of-tree modules), out/bzImage,
#         out/modules/lib/modules/<kver>/... (stripped), out/kernel.config
. "$(dirname "$0")/common.sh"
FRAGS=${KERNEL_FRAGMENTS:-base hdd tce}
ADDON=${ADDON_FRAGMENTS:-modules drivers}
if [ ! -f "$LINUX_SRC/Makefile" ]; then
    LINUX_SRC=$BUILD/linux-$LINUX_VER
    if [ ! -f "$LINUX_SRC/Makefile" ]; then
        tb=$(fetch "$LINUX_URL" "$LINUX_SHA256"); log "unpacking $(basename "$tb")"; tar -C "$BUILD" -xf "$tb"
    fi
fi
KO=$KDIR; mkdir -p "$KO"
set --
for f in $FRAGS; do set -- "$@" "$TC32_DIR/config/kernel-$f.fragment"; done
for f in $ADDON; do set -- "$@" "$TOP/config/kernel-$f.fragment"; done
# TRIM_KSYMS=1: drop every EXPORT_SYMBOL no module uses (saves ~30 KiB of the
# floppy kernel) but out-of-tree modules may then only use the symbols listed
# in config/ksyms-keep.txt -- adding one means rebuilding the core kernel.
if [ "${TRIM_KSYMS:-1}" = 1 ]; then
    grep -vE '^\s*(#|$)' "$TOP/config/ksyms-keep.txt" > "$BUILD/ksyms-keep.list"
    printf 'CONFIG_TRIM_UNUSED_KSYMS=y\nCONFIG_UNUSED_KSYMS_WHITELIST="%s"\n' "$BUILD/ksyms-keep.list" > "$BUILD/ksyms.fragment"
    set -- "$@" "$BUILD/ksyms.fragment"
fi
cd "$LINUX_SRC"
log "configuring linux $LINUX_VER (tinyconfig + core[$FRAGS] + addon[$ADDON]) in $KO"
make -s O="$KO" ARCH=i386 tinyconfig
KCONFIG_CONFIG="$KO/.config" scripts/kconfig/merge_config.sh -m -O "$KO" "$KO/.config" "$@" >/dev/null
make -s O="$KO" ARCH=i386 olddefconfig
# Every option we ask for must survive dependency resolution.
bad=0
for frag in $(for f in $ADDON; do echo "$TOP/config/kernel-$f.fragment"; done); do
    for line in $(grep -E '^CONFIG_' "$frag"); do
        grep -qxF "$line" "$KO/.config" || { echo "  kconfig mismatch: want $line, got: $(grep -E "^(# )?${line%%=*}[= ]" "$KO/.config" || echo unset)" >&2; bad=1; }
    done
done
[ $bad -eq 0 ] || die "kconfig mismatch (see above)"
log "building bzImage + modules (-j$JOBS)"
make -s O="$KO" ARCH=i386 -j"$JOBS" bzImage modules
rm -rf "$OUT/modules"
make -s O="$KO" ARCH=i386 INSTALL_MOD_PATH="$OUT/modules" INSTALL_MOD_STRIP=1 DEPMOD=true modules_install
cp "$KO/arch/x86/boot/bzImage" "$OUT/bzImage"; cp "$KO/.config" "$OUT/kernel.config"
log "bzImage: $(stat -c %s "$OUT/bzImage") bytes (core tc32: $(stat -c %s "$TC32_DIR/out/bzImage" 2>/dev/null || echo '?') bytes), kernel $(kver)"
