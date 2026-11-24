#!/bin/sh
# Stage a Tiny Core extension for wd24adv. The .ko must already exist
# and its vermagic must be exactly the floppy kernel's. This script
# does not build the module.
#   sh scripts/wd24adv-tcz.sh /path/wd24adv.ko /path/out
# Needs mksquashfs for the .tcz. Without it, the tree, list, dep, and
# info are still written and the script exits 2.
set -eu
KO=${1:-}
OUT=${2:-}
WANT="6.18.57-tc755x mod_unload 486"
KVER=6.18.57-tc755x
if [ -z "$KO" ] || [ -z "$OUT" ] || [ ! -f "$KO" ]; then
	echo "WD24X-TCZ stop args"
	exit 1
fi
MODINFO=${WD24X_MODINFO:-modinfo}
got=$("$MODINFO" -F vermagic "$KO" | head -n 1)
case "$got" in
"$WANT") ;;
*)
	echo "WD24X-TCZ stop vermagic"
	exit 1
	;;
esac
stage=$OUT/stage
rm -rf "$stage"
moddir=$stage/usr/local/lib/modules/$KVER/kernel/drivers/video/fbdev
mkdir -p "$moddir" "$stage/usr/local/tce.installed" "$OUT"
cp "$KO" "$moddir/wd24adv.ko"
inst=$stage/usr/local/tce.installed/wd24adv
cat > "$inst" << EOF
#!/bin/sh
# Load dry-run. A live mode set is a separate command, not onboot.
insmod /usr/local/lib/modules/$KVER/kernel/drivers/video/fbdev/wd24adv.ko dryrun=1
EOF
chmod 755 "$inst"
{
	echo "Title:          wd24adv.tcz"
	echo "Description:    WD90C24 fbdev, dry-run until a live command"
	echo "Version:        6.18.57-tc755x"
	echo "Author:         wd24adv"
	echo "Original-site:  https://github.com/branpurn/thinkpad-755c-drivers"
	echo "Copying-policy: GPL v2"
	echo "Size:           0"
	echo "Extension_by:   wd24adv"
} > "$OUT/wd24adv.tcz.info"
printf '%s\n' modutils.tcz > "$OUT/wd24adv.tcz.dep"
(
	cd "$stage" && find . ! -type d | sed 's|^\./|/|' | sort
) > "$OUT/wd24adv.tcz.list"
if ! command -v mksquashfs >/dev/null 2>&1; then
	echo "WD24X-TCZ staged no squashfs"
	exit 2
fi
rm -f "$OUT/wd24adv.tcz"
mksquashfs "$stage" "$OUT/wd24adv.tcz" -comp xz -noappend -all-root >/dev/null
md5sum "$OUT/wd24adv.tcz" > "$OUT/wd24adv.tcz.md5.txt"
echo "WD24X-TCZ done"
