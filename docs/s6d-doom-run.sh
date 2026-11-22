#!/bin/sh
# Doom on the desktop-env module. One shot. Does not start by being copied.
# WD24X_DOOM_FB=ioctl. Does not mmap. fdf30c77 has no present ioctl.
# Do not set WD24X_MOCK on the target.
#   sh s6d-doom-run.sh demo1
#   sh s6d-doom-run.sh demo2
#   sh s6d-doom-run.sh demo3
#   sh s6d-doom-run.sh eyes
#   sh s6d-doom-run.sh measure
#   sh s6d-doom-run.sh measure13h
#   sh s6d-doom-run.sh timedemo13h
# demoN is -timedemo with no tty input. eyes opts into WD24X_DOOM_INPUT=tty
# and is the keyboard test, not the unattended run. measure is demo1 for
# 60 frames with WD24X_DOOM_TIME=30, trial 180, so the clock cannot
# expire mid-run. measure and measure13h do not wait for that timer.
# They rmmod, and the pass is the text restore with mismatches 0.
# measure13h is the same 60 frames with present_mode=13h and
# mode=320x200-8. timedemo13h is Doom's own -timedemo demo1 on that
# same 13h present. No default.cfg, so the engine keeps screenblocks
# 10 and detaillevel 0. The command passes -nosound -nogui. That is
# the Win98 match: doom -timedemo demo1, same wad, screenblocks 10,
# detaillevel 0, sound off. Trial stays 600, the module maximum.
# At the measured 45.93 ms/frame the demo ends well inside that, and
# the script rmmods when Doom exits. It writes only the out, err, and
# dmesg files already used here. It does not write a config or a frame.
# The engine's completed timedemo exits 255: I_Error prints
# "timed N gametics in N realtics (N fps)" and then exit(-1). The
# parenthetical fps is part of the line. That 255 is this
# script's success, together with the gametics line. No VGA register writes
# beyond the module's existing mode and colormap path.
P=${WD24X_MOCK:-}
owned=
mode=${1:-}
trial=600
finishing=
cleanup_rc=0

hash_ok() {
	case "$(sha256sum "$1")" in
	"$2"*) return 0 ;;
	*) return 1 ;;
	esac
}

need_tmp() {
	case "$1" in
	/tmp/*) return 0 ;;
	*) return 1 ;;
	esac
}

pinned() {
	[ -n "$1" ] && [ "$1" != "UNPINNED" ]
}

stop() {
	echo "WD24X-DOOM-RUN stop $1"
	exit 1
}

own_cleanup() {
	rmst=0
	if [ -n "$finishing" ]; then
		return "$cleanup_rc"
	fi
	finishing=1
	if [ -r "$P/proc/wd24adv/live" ]; then
		echo "WD24X-DOOM-RUN live"
		cat "$P/proc/wd24adv/live"
	fi
	if [ -r "$P/proc/wd24adv/s6d" ]; then
		echo "WD24X-DOOM-RUN s6d"
		cat "$P/proc/wd24adv/s6d"
	fi
	if [ "$owned" = 1 ] && grep -q '^wd24adv ' "$P/proc/modules" 2>/dev/null; then
		rmmod wd24adv
		rmst=$?
		echo "RM=$rmst"
		if [ "$rmst" != 0 ]; then
			cleanup_rc=1
			return 1
		fi
	fi
	dmesg > "$P/tmp/wd24x-doom-dmesg.txt" 2>/dev/null
	return "$cleanup_rc"
}

fail() {
	echo "WD24X-DOOM-RUN stop $1"
	own_cleanup || echo "WD24X-DOOM-RUN executor finish rmmod"
	exit 1
}

console_ok() {
	fb=$(cat "$P/proc/fb" 2>/dev/null)
	name=$(cat "$P/sys/class/vtconsole/vtcon0/name" 2>/dev/null)
	bind=$(cat "$P/sys/class/vtconsole/vtcon0/bind" 2>/dev/null)
	echo "FB=[$fb]"
	echo "VT=$name"
	echo "BIND=$bind"
	[ -z "$fb" ] || return 1
	case "$name" in
	*VGA+*) ;;
	*) return 1 ;;
	esac
	[ "$bind" = "1" ] || return 1
	[ ! -e "$P/sys/class/vtconsole/vtcon1" ] || return 1
	return 0
}

live_ok() {
	live=$(cat "$P/proc/wd24adv/live" 2>/dev/null) || return 1
	echo "$live"
	case "$live" in
	*"wd24adv live: trial expiry -> 0"*) ;;
	*) return 1 ;;
	esac
	case "$live" in
	*"readback mismatches 0;"*) ;;
	*) return 1 ;;
	esac
	case "$live" in
	*"in_text 1;"*) ;;
	*) return 1 ;;
	esac
	return 0
}

s6d_after() {
	s6d=$(cat "$P/proc/wd24adv/s6d" 2>/dev/null) || return 1
	echo "$s6d"
	case "$s6d" in
	*"s6d: armed 0 active "*) ;;
	*) return 1 ;;
	esac
	case "$s6d" in
	*" thrash 0 latched 0 "*) ;;
	*) return 1 ;;
	esac
	return 0
}

s6d_quiet() {
	s6d=$(cat "$P/proc/wd24adv/s6d" 2>/dev/null) || return 1
	echo "$s6d"
	case "$s6d" in
	*"faults 0 "*) ;;
	*) return 1 ;;
	esac
	case "$s6d" in
	*" thrash 0 latched 0 "*) ;;
	*) return 1 ;;
	esac
	return 0
}

text_ok() {
	log=$P/tmp/wd24x-doom-dmesg.txt
	hit=0
	line=
	[ -f "$log" ] || return 1
	while IFS= read -r line; do
		case "$line" in
		*"live text: 0 ("*"mismatches 0)") hit=1 ;;
		esac
	done < "$log"
	[ "$hit" = 1 ] || return 1
	if grep -q "readback:" "$log"; then
		return 1
	fi
	return 0
}

finish_rmmod() {
	if ! s6d_quiet; then
		fail "s6d"
	fi
	echo "WD24X-DOOM-RUN rmmod"
	own_cleanup || fail "rmmod"
	if ! text_ok; then
		echo "WD24X-DOOM-RUN stop restore"
		exit 1
	fi
	echo "WD24X-DOOM-RUN restore mismatches 0"
	console_ok || {
		echo "WD24X-DOOM-RUN stop console after rmmod"
		exit 1
	}
	echo "WD24X-DOOM-RUN done"
	exit 0
}

take_timed() {
	line=$1
	case "$line" in
	"timed "*" gametics in "*" realtics"*) ;;
	*) return 1 ;;
	esac
	rest=${line#timed }
	gt=${rest%% *}
	rest=${rest#* in }
	rt=${rest%% *}
	case "$gt" in
	''|*[!0-9]*) return 1 ;;
	esac
	case "$rt" in
	''|*[!0-9]*) return 1 ;;
	esac
	[ "$rt" -gt 0 ]
}

have_line() {
	hit=0
	while IFS= read -r line; do
		if [ "$line" = "$2" ]; then
			hit=1
		fi
	done < "$1"
	[ "$hit" = 1 ]
}

case "$mode" in
demo1|demo2|demo3|eyes|timedemo13h) ;;
measure|measure13h) trial=180 ;;
*) stop "mode" ;;
esac

if [ -z "${WD24X_CANDIDATE_ENV:-}" ]; then
	WD24X_CANDIDATE_ENV=$(dirname "$0")/s6d-desktop.env
fi
if [ -z "${WD24X_DOOM_ENV:-}" ]; then
	WD24X_DOOM_ENV=$(dirname "$0")/s6d-doom.env
fi
[ -f "$WD24X_CANDIDATE_ENV" ] || stop "candidate env"
[ -f "$WD24X_DOOM_ENV" ] || stop "doom env"
. "$WD24X_CANDIDATE_ENV"
. "$WD24X_DOOM_ENV"
pinned "${WD24X_CANDIDATE_SHA:-}" || stop "candidate unpinned"
pinned "${WD24X_DOOM_SHA:-}" || stop "doom unpinned"
need_tmp "${WD24X_CANDIDATE_KO:-}" || stop "candidate path"
need_tmp "${WD24X_DOOM_BIN:-}" || stop "doom path"
need_tmp "${WD24X_WAD:-}" || stop "wad path"
case "${WD24X_CANDIDATE_KO:-}" in
*pr14*) stop "baseline" ;;
esac
KO="$P$WD24X_CANDIDATE_KO"
BIN="$P$WD24X_DOOM_BIN"
WAD="$P$WD24X_WAD"

[ -f "$KO" ] || stop "module missing"
[ -x "$BIN" ] || stop "doom missing"
[ -f "$WAD" ] || stop "wad missing"
hash_ok "$KO" "$WD24X_CANDIDATE_SHA" || stop "module hash"
hash_ok "$BIN" "$WD24X_DOOM_SHA" || stop "doom hash"
sz=$(wc -c < "$WAD" | tr -d ' ')
[ "$sz" = "$WD24X_WAD_SIZE" ] || stop "wad size"
hash_ok "$WAD" "$WD24X_WAD_SHA" || stop "wad hash"
if [ -f "$P/tmp/default.cfg" ] || [ -f "$P/tmp/doom.cfg" ]; then
	stop "config"
fi
console_ok || stop "console"
if [ -S "$P/tmp/.X11-unix/X1" ] || [ -f "$P/tmp/.X1-lock" ]; then
	stop "stale X1"
fi
if grep -q '^wd24adv ' "$P/proc/modules" 2>/dev/null; then
	stop "wd24adv loaded"
fi
if grep -q '^vga16fb ' "$P/proc/modules" 2>/dev/null; then
	stop "vga16fb"
fi
if grep -q '^wd90c24fb ' "$P/proc/modules" 2>/dev/null; then
	stop "wd90c24fb"
fi

gfx=640x480-8
present=letterbox
extra=
if [ "$mode" = "measure13h" ] || [ "$mode" = "timedemo13h" ]; then
	gfx=320x200-8
	present=13h
	extra=present_mode=13h
fi
insmod "$KO" dryrun=0 live=1 mode=$gfx trial=$trial s6d=1 s6d_cmap=1 pattern=0 fix_sr06=1 s6d_fbcon=1 s6d_bind=0 $extra
rc=$?
echo "RC=$rc"
[ "$rc" = "0" ] || stop "insmod"
owned=1
dmesg -c >/dev/null
start=$(date +%s 2>/dev/null || echo 0)
echo "WD24X-DOOM-RUN mode $mode"

if [ "$mode" = "eyes" ]; then
	WD24X_DOOM_FB=ioctl WD24X_DOOM_PALETTE=put WD24X_DOOM_PRESENT=$present \
		WD24X_DOOM_INPUT=tty WD24X_DOOM_FRAMES= \
		"$BIN" -iwad "$WAD" -nosound -nogui \
		>"$P/tmp/wd24x-doom.out" 2>"$P/tmp/wd24x-doom.err"
elif [ "$mode" = "measure" ] || [ "$mode" = "measure13h" ]; then
	WD24X_DOOM_FB=ioctl WD24X_DOOM_PALETTE=put WD24X_DOOM_PRESENT=$present \
		WD24X_DOOM_INPUT= WD24X_DOOM_FRAMES=60 WD24X_DOOM_TIME=30 \
		"$BIN" -iwad "$WAD" -timedemo demo1 -nosound -nogui \
		>"$P/tmp/wd24x-doom.out" 2>"$P/tmp/wd24x-doom.err"
elif [ "$mode" = "timedemo13h" ]; then
	WD24X_DOOM_FB=ioctl WD24X_DOOM_PALETTE=put WD24X_DOOM_PRESENT=$present \
		WD24X_DOOM_INPUT= WD24X_DOOM_FRAMES= \
		"$BIN" -iwad "$WAD" -timedemo demo1 -nosound -nogui \
		>"$P/tmp/wd24x-doom.out" 2>"$P/tmp/wd24x-doom.err"
else
	WD24X_DOOM_FB=ioctl WD24X_DOOM_PALETTE=put WD24X_DOOM_PRESENT=$present \
		WD24X_DOOM_INPUT= WD24X_DOOM_FRAMES= \
		"$BIN" -iwad "$WAD" -timedemo "$mode" -nosound -nogui \
		>"$P/tmp/wd24x-doom.out" 2>"$P/tmp/wd24x-doom.err"
fi
dr=$?
echo "DR=$dr"
cat "$P/tmp/wd24x-doom.out"
cat "$P/tmp/wd24x-doom.err"
if ! have_line "$P/tmp/wd24x-doom.out" "WD24X-DOOM fb ioctl 320 200"; then
	fail "ioctl"
fi
if ! have_line "$P/tmp/wd24x-doom.out" "WD24X-DOOM palette put 256"; then
	fail "palette"
fi
if [ "$mode" = "measure13h" ] || [ "$mode" = "timedemo13h" ]; then
	if have_line "$P/tmp/wd24x-doom.out" "WD24X-DOOM present 13h refused"; then
		fail "refused"
	fi
	if ! have_line "$P/tmp/wd24x-doom.out" "WD24X-DOOM present 13h"; then
		fail "present"
	fi
	if ! have_line "$P/tmp/wd24x-doom.out" "WD24X-DOOM blit direct 64000"; then
		fail "blit"
	fi
	if ! have_line "$P/tmp/wd24x-doom.out" "WD24X-DOOM present switches 0"; then
		fail "switches"
	fi
	if have_line "$P/tmp/wd24x-doom.out" "WD24X-DOOM blit letterbox dword"; then
		fail "letterbox"
	fi
else
	if ! have_line "$P/tmp/wd24x-doom.out" "WD24X-DOOM present switches 5"; then
		fail "switches"
	fi
	if ! have_line "$P/tmp/wd24x-doom.out" "WD24X-DOOM present letterbox"; then
		fail "present"
	fi
	if ! have_line "$P/tmp/wd24x-doom.out" "WD24X-DOOM blit letterbox dword"; then
		fail "blit"
	fi
fi

if [ "$mode" = "eyes" ]; then
	if ! have_line "$P/tmp/wd24x-doom.out" "WD24X-DOOM input tty"; then
		fail "input"
	fi
elif [ "$mode" = "measure" ] || [ "$mode" = "measure13h" ]; then
	if have_line "$P/tmp/wd24x-doom.out" "WD24X-DOOM input tty"; then
		fail "input"
	fi
	if ! grep -q "^WD24X-DOOM time frames " "$P/tmp/wd24x-doom.out"; then
		fail "time"
	fi
	if [ "$dr" != "0" ]; then
		fail "doom rc"
	fi
	if [ "$mode" = "measure13h" ]; then
		echo "WD24X-DOOM-RUN measure13h frames 60"
	else
		echo "WD24X-DOOM-RUN measure frames 60"
	fi
	finish_rmmod
elif [ "$mode" = "timedemo13h" ]; then
	if have_line "$P/tmp/wd24x-doom.out" "WD24X-DOOM input tty"; then
		fail "input"
	fi
	found=
	for log in "$P/tmp/wd24x-doom.out" "$P/tmp/wd24x-doom.err"; do
		while IFS= read -r line; do
			case "$line" in
			"timed "*" gametics in "*" realtics"*) found=$line ;;
			esac
		done < "$log"
	done
	take_timed "$found" || fail "timedemo"
	if [ "$dr" != "0" ] && [ "$dr" != "255" ]; then
		fail "doom rc"
	fi
	echo "$found"
	echo "WD24X-DOOM-FPS gametics $gt realtics $rt fps_x100 $((gt * 3500 / rt))"
	finish_rmmod
else
	if have_line "$P/tmp/wd24x-doom.out" "WD24X-DOOM input tty"; then
		fail "input"
	fi
	found=
	for log in "$P/tmp/wd24x-doom.out" "$P/tmp/wd24x-doom.err"; do
		while IFS= read -r line; do
			case "$line" in
			"timed "*" gametics in "*" realtics"*) found=$line ;;
			esac
		done < "$log"
	done
	take_timed "$found" || fail "timedemo"
	if [ "$dr" != "0" ] && [ "$dr" != "255" ]; then
		fail "doom rc"
	fi
	echo "WD24X-DOOM-FPS gametics $gt realtics $rt fps_x100 $((gt * 3500 / rt))"
fi

now=$(date +%s 2>/dev/null || echo 0)
left=$trial
if [ "$start" != "0" ] && [ "$now" != "0" ]; then
	used=$((now - start))
	if [ "$used" -lt 0 ]; then
		used=0
	fi
	left=$((trial - used))
fi
if [ "$left" -lt 5 ]; then
	left=5
fi
if [ "$left" -gt "$trial" ]; then
	left=$trial
fi
echo "WD24X-DOOM-RUN wait $left"
sleep "$left"
if ! live_ok || ! s6d_after || ! console_ok; then
	echo "WD24X-DOOM-RUN stop expiry"
	own_cleanup || echo "WD24X-DOOM-RUN executor finish rmmod"
	exit 1
fi
own_cleanup || exit 1
console_ok || {
	echo "WD24X-DOOM-RUN stop console after rmmod"
	exit 1
}
echo "WD24X-DOOM-RUN done"
