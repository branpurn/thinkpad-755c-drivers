#!/bin/sh
# Frame-stats timedemo, then about a minute of E1M1.
# There is no go6 script in this tree. This file does not edit
# docs/s6d-doom-run.sh. The line below is the script's own status.
# A host that expands the status first is the go5 bug. Send this
# instead, so the device shell expands it:
#   echo 'S6D-GO7 GO7_RC=$?'
# WD24X_MOCK prefixes paths for the host dry-run.
# WD24X_GO7_PLAY_SEC defaults to 60. 0 runs the hand-play in the
# foreground and lets that process exit itself.
P=${WD24X_MOCK:-}
rc=1
owned=0
sec=${WD24X_GO7_PLAY_SEC:-60}
set -u

finish() {
	echo "S6D-GO7 GO7_RC=$rc"
}
trap finish EXIT

# Busybox ash with standalone applets ignores PATH and runs its own
# sha256sum, insmod, and rmmod. A function wins over that applet.
# The device leaves WD24X_MOCK empty, so these stay the real commands.
if [ -n "$P" ] && [ -x "$P/bin/sha256sum" ]; then
	sha256sum() { "$P/bin/sha256sum" "$@"; }
fi
if [ -n "$P" ] && [ -x "$P/bin/insmod" ]; then
	insmod() { "$P/bin/insmod" "$@"; }
fi
if [ -n "$P" ] && [ -x "$P/bin/rmmod" ]; then
	rmmod() { "$P/bin/rmmod" "$@"; }
fi

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
	echo "S6D-GO7 stop $1"
	if [ "$owned" = 1 ]; then
		rmmod wd24adv || true
		owned=0
	fi
	rc=1
	exit 1
}

console_ok() {
	fb=$(cat "$P/proc/fb" 2>/dev/null || echo "")
	[ -z "$fb" ] || return 1
	okb=0
	for c in "$P"/sys/class/vtconsole/vtcon*; do
		[ -e "$c" ] || continue
		name=$(cat "$c/name" 2>/dev/null || echo "")
		b=$(cat "$c/bind" 2>/dev/null || echo "")
		case "$name" in
		*VGA*) [ "$b" = 1 ] && okb=1 ;;
		esac
	done
	[ "$okb" = 1 ]
}

have_line() {
	grep -q "$2" "$1" 2>/dev/null
}

if [ -z "${WD24X_CANDIDATE_ENV:-}" ]; then
	WD24X_CANDIDATE_ENV=$(dirname "$0")/s6d-desktop.env
fi
if [ -z "${WD24X_DOOM_ENV:-}" ]; then
	WD24X_DOOM_ENV=$(dirname "$0")/s6d-doom.env
fi
[ -f "$WD24X_CANDIDATE_ENV" ] || stop "candidate env"
[ -f "$WD24X_DOOM_ENV" ] || stop "doom env"
# shellcheck disable=SC1090
. "$WD24X_CANDIDATE_ENV"
# shellcheck disable=SC1090
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
[ "$sz" = "${WD24X_WAD_SIZE:-}" ] || stop "wad size"
hash_ok "$WAD" "$WD24X_WAD_SHA" || stop "wad hash"
console_ok || stop "console"
if grep -q '^wd24adv ' "$P/proc/modules" 2>/dev/null; then
	stop "wd24adv loaded"
fi

insmod "$KO" dryrun=0 live=1 mode=320x200-8 trial=600 s6d=1 s6d_cmap=1 \
	pattern=0 fix_sr06=1 s6d_fbcon=1 s6d_bind=0 present_mode=13h
[ "$?" = 0 ] || stop "insmod"
owned=1
echo "S6D-GO7 loaded"

WD24X_DOOM_FB=aperture WD24X_DOOM_PALETTE=put WD24X_DOOM_PRESENT=13h \
	WD24X_DOOM_INPUT= WD24X_DOOM_FLIP=1 WD24X_DOOM_FRAMESTATS=1 \
	"$BIN" -iwad "$WAD" -timedemo demo1 -nosound -nogui \
	>"$P/tmp/wd24x-go7-demo.out" 2>"$P/tmp/wd24x-go7-demo.err"
demo_rc=$?
echo "S6D-GO7 demo_rc=$demo_rc"
have_line "$P/tmp/wd24x-go7-demo.err" "WD24X-DOOM framestats frames " \
	|| have_line "$P/tmp/wd24x-go7-demo.out" "WD24X-DOOM framestats frames " \
	|| stop "demo stats"
have_line "$P/tmp/wd24x-go7-demo.err" "WD24X-DOOM framestats hist" \
	|| have_line "$P/tmp/wd24x-go7-demo.out" "WD24X-DOOM framestats hist" \
	|| stop "demo hist"
have_line "$P/tmp/wd24x-go7-demo.err" "timed " \
	|| have_line "$P/tmp/wd24x-go7-demo.out" "timed " \
	|| stop "demo timed"
if [ "$demo_rc" != 0 ] && [ "$demo_rc" != 255 ]; then
	stop "demo rc"
fi

if [ "$sec" = 0 ]; then
	WD24X_DOOM_FB=aperture WD24X_DOOM_PALETTE=put WD24X_DOOM_PRESENT=13h \
		WD24X_DOOM_INPUT=tty WD24X_DOOM_FLIP=1 WD24X_DOOM_FRAMESTATS=1 \
		"$BIN" -iwad "$WAD" -warp 1 1 -nosound -nogui \
		>"$P/tmp/wd24x-go7-play.out" 2>"$P/tmp/wd24x-go7-play.err"
	play_rc=$?
else
	WD24X_DOOM_FB=aperture WD24X_DOOM_PALETTE=put WD24X_DOOM_PRESENT=13h \
		WD24X_DOOM_INPUT=tty WD24X_DOOM_FLIP=1 WD24X_DOOM_FRAMESTATS=1 \
		"$BIN" -iwad "$WAD" -warp 1 1 -nosound -nogui \
		>"$P/tmp/wd24x-go7-play.out" 2>"$P/tmp/wd24x-go7-play.err" &
	child=$!
	i=0
	while [ "$i" -lt "$sec" ]; do
		if ! kill -0 "$child" 2>/dev/null; then
			break
		fi
		sleep 1
		i=$((i + 1))
	done
	if kill -0 "$child" 2>/dev/null; then
		kill -TERM "$child" 2>/dev/null
	fi
	wait "$child"
	play_rc=$?
fi
echo "S6D-GO7 play_rc=$play_rc"
have_line "$P/tmp/wd24x-go7-play.err" "WD24X-DOOM framestats frames " \
	|| have_line "$P/tmp/wd24x-go7-play.out" "WD24X-DOOM framestats frames " \
	|| stop "play stats"
have_line "$P/tmp/wd24x-go7-play.err" "WD24X-DOOM framestats hist" \
	|| have_line "$P/tmp/wd24x-go7-play.out" "WD24X-DOOM framestats hist" \
	|| stop "play hist"

if [ "$owned" = 1 ]; then
	rmmod wd24adv || stop "rmmod"
	owned=0
fi
live=$(cat "$P/proc/wd24adv/live" 2>/dev/null || echo "")
case "$live" in
*mismatches\ 0*) ;;
*) stop "restore" ;;
esac
echo "S6D-GO7 done"
rc=0
exit 0
