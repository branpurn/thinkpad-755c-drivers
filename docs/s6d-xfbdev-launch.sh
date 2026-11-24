#!/bin/sh
# Launch the Tiny Core Xfbdev against the already-registered fb.
# Does not insmod. Does not use Xorg or modesetting.
#   sh docs/s6d-xfbdev-launch.sh
set -eu
P=${WD24X_MOCK:-}
BIN=${WD24X_XFBDEV:-/tmp/wd24-xfbdev/Xfbdev}
[ -x "$P$BIN" ] || {
	echo "WD24X-XFBDEV-LAUNCH stop binary"
	exit 1
}
case "$BIN" in
*Xorg*|*modesetting*)
	echo "WD24X-XFBDEV-LAUNCH stop server"
	exit 1
	;;
esac
echo "WD24X-XFBDEV-LAUNCH $BIN :1 -nolisten tcp -noreset -fb /dev/fb0"
if [ -n "${WD24X_XFBDEV_EXEC:-}" ]; then
	"$P$BIN" :1 -nolisten tcp -noreset -fb /dev/fb0
fi
