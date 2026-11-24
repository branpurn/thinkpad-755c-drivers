#!/bin/sh
# M4. One known fill, accelerated, compared with the CPU copy, then put back.
# Not the in-flight desktop/Doom run. Do not start until that run is done.
# No X. No fbcon bind. Trial 180. Stop on the first failure.
# Do not set WD24X_MOCK on the target.
P=${WD24X_MOCK:-}
owned=
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
	echo "WD24X-ACCEL-RUN stop $1"
	exit 1
}

own_cleanup() {
	if [ -n "$finishing" ]; then
		return "$cleanup_rc"
	fi
	finishing=1
	if [ -r "$P/proc/wd24adv/live" ]; then
		echo "WD24X-ACCEL-RUN live"
		cat "$P/proc/wd24adv/live"
	fi
	if [ -r "$P/proc/wd24adv/s6d" ]; then
		echo "WD24X-ACCEL-RUN s6d"
		cat "$P/proc/wd24adv/s6d"
	fi
	if [ "$owned" = 1 ] && grep -q '^wd24adv ' "$P/proc/modules" 2>/dev/null; then
		rmmod wd24adv
		rmst=$?
		echo "RM=$rmst"
		if [ "$rmst" != 0 ]; then
			cleanup_rc=1
		fi
	fi
	return "$cleanup_rc"
}

fail() {
	echo "WD24X-ACCEL-RUN stop $1"
	own_cleanup || echo "WD24X-ACCEL-RUN executor finish rmmod"
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

if [ -z "${WD24X_CANDIDATE_ENV:-}" ]; then
	WD24X_CANDIDATE_ENV=$(dirname "$0")/s6d-accel.env
fi
[ -f "$WD24X_CANDIDATE_ENV" ] || stop "candidate env"
. "$WD24X_CANDIDATE_ENV"
pinned "${WD24X_CANDIDATE_SHA:-}" || stop "candidate unpinned"
need_tmp "${WD24X_CANDIDATE_KO:-}" || stop "candidate path"
case "${WD24X_CANDIDATE_KO:-}" in
*pr14*) stop "baseline" ;;
esac
KO="$P$WD24X_CANDIDATE_KO"
[ -f "$KO" ] || stop "module missing"
hash_ok "$KO" "$WD24X_CANDIDATE_SHA" || stop "module hash"
console_ok || stop "console"
if grep -q '^wd24adv ' "$P/proc/modules" 2>/dev/null; then
	stop "wd24adv loaded"
fi
if [ -S "$P/tmp/.X11-unix/X1" ] || [ -f "$P/tmp/.X1-lock" ]; then
	stop "stale X1"
fi

insmod "$KO" dryrun=0 live=1 mode=640x480-8 trial=180 s6d=1 pattern=0 \
	fix_sr06=1 s6d_fbcon=1 s6d_bind=0 accel=1 accel_gate=1
rc=$?
echo "RC=$rc"
[ "$rc" = "0" ] || stop "insmod"
owned=1
dmesg -c >/dev/null
sleep 5
dmesg | grep -q "session ON" || fail "no session"
s6d=$(cat "$P/proc/wd24adv/s6d" 2>/dev/null) || fail "s6d"
echo "$s6d"
case "$s6d" in
*"gate 2 "*)
	echo "WD24X-ACCEL-RUN stop timeout"
	echo "WD24X-ACCEL-RUN power cycle: rmmod not attempted"
	exit 1
	;;
esac
case "$s6d" in
*"accel 1 gate 1 match 1024"*) ;;
*) fail "match" ;;
esac
case "$s6d" in
*" thrash 0 latched 0 "*) ;;
*) fail "thrash" ;;
esac
echo "WD24X-ACCEL-RUN match 1024"
sleep 180
if ! live_ok || ! console_ok; then
	echo "WD24X-ACCEL-RUN stop expiry"
	own_cleanup || echo "WD24X-ACCEL-RUN executor finish rmmod"
	exit 1
fi
own_cleanup || exit 1
echo "WD24X-ACCEL-RUN done"
