#!/bin/sh
# M2. fbcon bind, still under the trial. Not the in-flight desktop/Doom run.
# Do not start this until that run has printed its done lines.
# No X. One insmod. s6d_bind=1. Trial 180. Stop on the first failure.
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
	echo "WD24X-FBCON-RUN stop $1"
	exit 1
}

own_cleanup() {
	if [ -n "$finishing" ]; then
		return "$cleanup_rc"
	fi
	finishing=1
	if [ -r "$P/proc/wd24adv/live" ]; then
		echo "WD24X-FBCON-RUN live"
		cat "$P/proc/wd24adv/live"
	fi
	if [ -r "$P/proc/wd24adv/s6d" ]; then
		echo "WD24X-FBCON-RUN s6d"
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
	echo "WD24X-FBCON-RUN stop $1"
	own_cleanup || echo "WD24X-FBCON-RUN executor finish rmmod"
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

quiet_ok() {
	s6d=$(cat "$P/proc/wd24adv/s6d" 2>/dev/null) || return 1
	echo "$s6d"
	case "$s6d" in
	*" thrash 0 latched 0 "*) ;;
	*) return 1 ;;
	esac
	case "$s6d" in
	*"fb_reg 1 "*) ;;
	*) return 1 ;;
	esac
	return 0
}

if [ -z "${WD24X_CANDIDATE_ENV:-}" ]; then
	WD24X_CANDIDATE_ENV=$(dirname "$0")/s6d-desktop.env
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

insmod "$KO" dryrun=0 live=1 mode=640x480-8 trial=180 s6d=1 s6d_cmap=1 pattern=0 fix_sr06=1 s6d_fbcon=1 s6d_bind=1
rc=$?
echo "RC=$rc"
[ "$rc" = "0" ] || stop "insmod"
owned=1
dmesg -c >/dev/null
sleep 5
dmesg | grep -q "session ON" || fail "no session"
if ! quiet_ok; then
	fail "thrash"
fi
if [ ! -e "$P/dev/tty0" ]; then
	fail "tty0"
fi
printf 'WD24X-FBCON line\n' > "$P/dev/tty0" || fail "tty0"
echo "WD24X-FBCON-RUN wrote tty0"
# 5s already slept. 180s more passes trial=180.
sleep 180
if ! live_ok || ! console_ok; then
	echo "WD24X-FBCON-RUN stop expiry"
	own_cleanup || echo "WD24X-FBCON-RUN executor finish rmmod"
	exit 1
fi
own_cleanup || exit 1
echo "WD24X-FBCON-RUN done"
