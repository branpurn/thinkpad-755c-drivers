#!/bin/sh
# Host/QEMU check. Not a 755C run and not an FPS claim.
# WD24X_DOOM_HOST=1 is required. WD24X_DOOM_FB is cleared.
set -eu
if [ "${WD24X_DOOM_HOST:-}" != "1" ]; then
	echo "WD24X-DOOM-BENCH stop not a host run"
	exit 1
fi
BIN=${WD24X_DOOM_BIN:-/tmp/wd24x-doom.s}
WAD=${WD24X_DOOM_WAD:-/tmp/doom-wad/freedoom1.wad}
BIN_SHA=e735b3fdb770780bc8ac4c97f7239cafb63103aae310f5b43d5c0e6bbd8a3164
WAD_SHA=7323bcc168c5a45ff10749b339960e98314740a734c30d4b9f3337001f9e703d
[ -x "$BIN" ] || {
	echo "WD24X-DOOM-BENCH stop binary"
	exit 1
}
[ -f "$WAD" ] || {
	echo "WD24X-DOOM-BENCH stop wad"
	exit 1
}
bh=$(sha256sum "$BIN" | awk '{print $1}')
wh=$(sha256sum "$WAD" | awk '{print $1}')
[ "$bh" = "$BIN_SHA" ] || {
	echo "WD24X-DOOM-BENCH stop binary hash"
	exit 1
}
[ "$wh" = "$WAD_SHA" ] || {
	echo "WD24X-DOOM-BENCH stop wad hash"
	exit 1
}
mode=${1:-smoke}
cd /tmp
case "$mode" in
smoke)
	out=$(WD24X_DOOM_FRAMES=2 WD24X_DOOM_FB= WD24X_DOOM_PALETTE= \
		WD24X_DOOM_INPUT= \
		qemu-i386-static -cpu 486 "$BIN" \
		-iwad "$WAD" -timedemo demo1 -nosound 2>&1)
	printf '%s\n' "$out" | grep -q 'WD24X-DOOM display file'
	printf '%s\n' "$out" | grep -q 'WD24X-DOOM input none'
	printf '%s\n' "$out" | grep -q 'WD24X-DOOM render 320 200 index'
	printf '%s\n' "$out" | grep -q 'WD24X-DOOM present letterbox'
	printf '%s\n' "$out" | grep -q 'WD24X-DOOM frames 2 index 0x00e988bb present 0x807cd9bd'
	printf '%s\n' "$out" | grep -q 'WD24X-DOOM playpal0 1 1 1'
	printf '%s\n' "$out" | grep -q 'luminance\|fb mmap' && exit 1
	echo "WD24X-DOOM-BENCH smoke ok"
	;;
bench)
	out=$(WD24X_DOOM_FB= WD24X_DOOM_PALETTE= WD24X_DOOM_INPUT= \
		qemu-i386-static -cpu 486 "$BIN" \
		-iwad "$WAD" -timedemo demo1 -nosound 2>&1 || true)
	printf '%s\n' "$out" | grep -q 'timed 7117 gametics'
	printf '%s\n' "$out" | grep -q 'WD24X-DOOM frames 7156 index 0x3ac75f8d present 0x506f4d2d'
	printf '%s\n' "$out" | grep -q 'WD24X-DOOM playpal0 1 32 1'
	echo "WD24X-DOOM-BENCH qemu-i486 gametics 7117"
	echo "WD24X-DOOM-BENCH not the 755C"
	;;
*)
	echo "WD24X-DOOM-BENCH stop mode"
	exit 1
	;;
esac
