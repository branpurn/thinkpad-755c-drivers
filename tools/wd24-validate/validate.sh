#!/bin/sh
# On-device and in-VM checks. One line per result:
#   VAL <name> PASS|FAIL <evidence>
#   VAL SUMMARY pass=N fail=N
# WD24X_VALIDATE_MODEL=1 runs the CRTC model (no /dev/fb0).
# WD24X_VALIDATE_BIN is that program. WD24X_VALIDATE_FULL=1 records
# every latched field instead of the short hold.
# On the device, run this script in its own insmod and rmmod before
# Doom. A second /dev/fb0 open after Doom returns -ENODEV (-19).
# Busybox ash standalone applets are not used; this file only calls
# the program named above, cat, and grep.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
BIN=${WD24X_VALIDATE_BIN:-$ROOT/wd24_run}
MODEL=${WD24X_VALIDATE_MODEL:-0}
PASS=0
FAIL=0

count_line() {
	case "$1" in
	VAL\ SUMMARY*) ;;
	VAL\ *\ PASS\ *) PASS=$((PASS + 1)) ;;
	VAL\ *\ FAIL\ *) FAIL=$((FAIL + 1)) ;;
	esac
}

if [ "$MODEL" = 1 ]; then
	if [ ! -x "$BIN" ]; then
		echo "VAL model FAIL bin"
		echo "VAL SUMMARY pass=0 fail=1"
		exit 1
	fi
	set --
	if [ "${WD24X_VALIDATE_FULL:-0}" = 1 ]; then
		set -- --full
	fi
	if [ -n "${WD24X_VALIDATE_MP4:-}" ]; then
		set -- "$@" --mp4 "$WD24X_VALIDATE_MP4"
	fi
	if [ -n "${WD24X_VALIDATE_PPM:-}" ]; then
		set -- "$@" --ppm "$WD24X_VALIDATE_PPM"
	fi
	if [ -n "${WD24X_VALIDATE_SAMPLES:-}" ]; then
		set -- "$@" --samples "$WD24X_VALIDATE_SAMPLES"
	fi
	exec "$BIN" "$@"
fi

if [ ! -x "$BIN" ]; then
	echo "VAL page0 FAIL bin"
	echo "VAL SUMMARY pass=0 fail=1"
	exit 1
fi
LOG=${WD24X_VALIDATE_LOG:-/tmp/wd24-val.log}
"$BIN" --device > "$LOG" 2>/tmp/wd24-val.err || true
while IFS= read -r line; do
	count_line "$line"
	echo "$line"
done < "$LOG"

if [ -n "${WD24X_DOOM_LOG:-}" ] && [ -f "$WD24X_DOOM_LOG" ]; then
	rows=$(grep 'vram cmp' "$WD24X_DOOM_LOG" | tail -n 1 || true)
	case "$rows" in
	*"rows_bad 0"*) echo "VAL hud PASS $rows"; PASS=$((PASS + 1)) ;;
	"") echo "VAL hud FAIL no-vram-cmp"; FAIL=$((FAIL + 1)) ;;
	*) echo "VAL hud FAIL $rows"; FAIL=$((FAIL + 1)) ;;
	esac
	case "$rows" in
	*"frame_bad 0"*) echo "VAL ghost PASS $rows"; PASS=$((PASS + 1)) ;;
	"") echo "VAL ghost FAIL no-vram-cmp"; FAIL=$((FAIL + 1)) ;;
	*) echo "VAL ghost FAIL $rows"; FAIL=$((FAIL + 1)) ;;
	esac
	fps=$(grep 'timed ' "$WD24X_DOOM_LOG" | tail -n 1 || true)
	lt=$(grep 'framestats flip' "$WD24X_DOOM_LOG" | tail -n 1 || true)
	case "$fps" in
	*"fps"*)
		case "$lt" in
		*"latch_to 0"*) echo "VAL perf PASS $fps $lt"; PASS=$((PASS + 1)) ;;
		*) echo "VAL perf FAIL $fps $lt"; FAIL=$((FAIL + 1)) ;;
		esac
		;;
	*) echo "VAL perf FAIL no-timed-line"; FAIL=$((FAIL + 1)) ;;
	esac
else
	echo "VAL hud FAIL no-doom-log"
	echo "VAL ghost FAIL no-doom-log"
	echo "VAL perf FAIL no-doom-log"
	FAIL=$((FAIL + 3))
fi

P=${WD24X_MOCK:-}
mis=$(grep 'mismatches' "$P/proc/wd24adv/live" 2>/dev/null | tail -n 1 || true)
case "$mis" in
*"mismatches 0"*) echo "VAL restore PASS $mis rmmod=see-shell console=see-shell"; PASS=$((PASS + 1)) ;;
"") echo "VAL restore FAIL no-live"; FAIL=$((FAIL + 1)) ;;
*) echo "VAL restore FAIL $mis"; FAIL=$((FAIL + 1)) ;;
esac
echo "VAL SUMMARY pass=$PASS fail=$FAIL"
[ "$FAIL" = 0 ]
