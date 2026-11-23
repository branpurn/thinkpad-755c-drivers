#!/bin/sh
# One 10-second mode 13h picture. Look at the panel, then the log.
# Red block at the top means the scan starts at byte 0. A band from
# the middle of the picture, with the red block elsewhere, is a
# nonzero CR0C/CR0D. The script redirects the program's stdout so
# those lines are not printed on the VGA console.
#   sh s6d-scanpat-run.sh
#   sh s6d-scanpat-run.sh pages
# pages fills page 0 red, banks the CPU and fills page 1 green while
# the start stays 0, then sets CR0C/CR0D to 0x4000. Red, then still
# red, then green means the start moved the scanout. Red turning
# green before the second line means the CPU bank did not move.
# Staying red after "page 1 shown" means CR0C/CR0D did not move it.
# Trial is 30s. The picture is up for 10s of that, or about 14s for
# pages. pattern=0.
# s6d_bind=0 leaves vtcon0 on vgacon; the module then sets KD_GRAPHICS.
# Stripped i486 static binary from this tree: 26132 bytes,
# sha256 c9a961f5785c11f74ef70b9fdfa7ef47834f4e2c28d8ca8fbd64aa5ca25b51d1.
# Build: i486-linux-musl-gcc -static -march=i486 -mtune=i486 -O2 -s
#   -o /tmp/wd24x-scanpat tools/wd24-scan/scanpat.c
P=${WD24X_MOCK:-}
owned=
finishing=

stop() {
	echo "WD24X-SCANPAT-RUN stop $1"
	exit 1
}

own_cleanup() {
	if [ -n "$finishing" ]; then
		return 0
	fi
	finishing=1
	if [ -r "$P/proc/wd24adv/live" ]; then
		echo "WD24X-SCANPAT-RUN live"
		cat "$P/proc/wd24adv/live"
	fi
	if [ "$owned" = 1 ] && grep -q '^wd24adv ' "$P/proc/modules" 2>/dev/null; then
		rmmod wd24adv
		echo "RM=$?"
	fi
	return 0
}

trap own_cleanup EXIT

BIN=${WD24X_SCANPAT:-/tmp/wd24x-scanpat}
KO=${WD24X_CANDIDATE_KO:-/tmp/wd24adv-desktop.ko}
[ -x "$P$BIN" ] || stop "program"
console_name=$(cat "$P/sys/class/vtconsole/vtcon0/name" 2>/dev/null || echo "")
console_bind=$(cat "$P/sys/class/vtconsole/vtcon0/bind" 2>/dev/null || echo "")
case "$console_name" in
*VGA+*) ;;
*) stop "console" ;;
esac
[ "$console_bind" = "1" ] || stop "console"
[ ! -e "$P/sys/class/vtconsole/vtcon1" ] || stop "console"
if grep -q '^wd24adv ' "$P/proc/modules" 2>/dev/null; then
	stop "loaded"
fi

mode=${1:-picture}
case "$mode" in
picture|pages) ;;
*) stop "mode" ;;
esac

echo "WD24X-SCANPAT-RUN look"
if [ "$mode" = "pages" ]; then
	echo "red, then still red, then green: the start moved"
else
	echo "red at the top, just right of the eight row bits, is start 0"
	echo "green is the top-right corner, yellow the bottom-right"
	echo "a white line every 10 rows"
fi

insmod "$P$KO" dryrun=0 live=1 mode=320x200-8 \
	trial=30 s6d=1 s6d_cmap=1 pattern=0 fix_sr06=1 \
	s6d_fbcon=1 s6d_bind=0 present_mode=13h
rc=$?
echo "RC=$rc"
[ "$rc" = "0" ] || stop "insmod"
owned=1

if [ "$mode" = "pages" ]; then
	"$P$BIN" pages >"$P/tmp/wd24x-scanpat.out" 2>"$P/tmp/wd24x-scanpat.err"
else
	"$P$BIN" >"$P/tmp/wd24x-scanpat.out" 2>"$P/tmp/wd24x-scanpat.err"
fi
dr=$?
echo "DR=$dr"
cat "$P/tmp/wd24x-scanpat.out"
[ "$dr" = "0" ] || stop "program rc"
if [ "$mode" = "pages" ]; then
	grep -q "WD24X-SCANPAT pages red then green" \
		"$P/tmp/wd24x-scanpat.out" || stop "pages"
	grep -q "WD24X-SCANPAT flip show 0 cr0c 00 cr0d 00 pr0a 10" \
		"$P/tmp/wd24x-scanpat.out" || stop "page0"
	grep -q "WD24X-SCANPAT flip show 1 cr0c 40 cr0d 00 pr0a 00" \
		"$P/tmp/wd24x-scanpat.out" || stop "page1"
else
	grep -q "WD24X-SCANPAT red rows 0-15 cols 40-79" \
		"$P/tmp/wd24x-scanpat.out" || stop "legend"
fi

live=$(cat "$P/proc/wd24adv/live" 2>/dev/null) || stop "live"
case "$live" in
*"scan mode "*) ;;
*) stop "scan" ;;
esac
case "$live" in
*"cr0c="*"cr0d="*"dw_bytes="*) ;;
*) stop "scan fields" ;;
esac
case "$live" in
*"cr0c=00 cr0d=00"*) echo "WD24X-SCANPAT-RUN start 0" ;;
*) echo "WD24X-SCANPAT-RUN start nonzero"; stop "start" ;;
esac
echo "WD24X-SCANPAT-RUN done"
