#!/bin/sh
# Browser gate, after the eight morning steps. One insmod.
# The launch is the console command in docs/s6d-browser-plan.txt.
# Keys are the console tty. Stop on the first miss.
# Do not set WD24X_MOCK on the target.
P=${WD24X_MOCK:-}
owned=
npid=
finishing=
cleanup_rc=0
NSFB=${WD24X_NSFB:-$P/tmp/nsfb/nsfb}
NSRES=${WD24X_NSRES:-$P/tmp/nsfb/nsres}
PAGE_PATH=${WD24X_NSFB_PAGE_PATH:-$P/tmp/nsfb/pages/canvas.html}
SETTLE=${WD24X_NSFB_SETTLE:-8}
HASH=$(CDPATH= cd -- "$(dirname "$0")/../tools/wd24-browser" && pwd)/fbhash.sh

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
	echo "WD24X-BROWSER-RUN stop $1"
	exit 1
}

own_cleanup() {
	if [ -n "$finishing" ]; then
		return "$cleanup_rc"
	fi
	finishing=1
	if [ -n "$npid" ]; then
		kill "$npid" 2>/dev/null
		wait "$npid" 2>/dev/null
		npid=
	fi
	if [ -r "$P/proc/wd24adv/live" ]; then
		echo "WD24X-BROWSER-RUN live"
		cat "$P/proc/wd24adv/live"
	fi
	if [ -r "$P/proc/wd24adv/s6d" ]; then
		echo "WD24X-BROWSER-RUN s6d"
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
	dmesg > "$P/tmp/wd24x-browser-dmesg.txt" 2>/dev/null
	return "$cleanup_rc"
}

fail() {
	echo "WD24X-BROWSER-RUN stop $1"
	own_cleanup || echo "WD24X-BROWSER-RUN executor finish rmmod"
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

text_ok() {
	log=$P/tmp/wd24x-browser-dmesg.txt
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
[ -x "$NSFB" ] || stop "nsfb missing"
[ -r "$NSRES/Messages" ] || stop "messages missing"
[ -r "$PAGE_PATH" ] || stop "page missing"
[ -c "$P/dev/tty0" ] || [ -f "$P/dev/tty0" ] || stop "tty0"
console_ok || stop "console"
if grep -q '^wd24adv ' "$P/proc/modules" 2>/dev/null; then
	stop "wd24adv loaded"
fi

insmod "$KO" dryrun=0 live=1 mode=640x480-8 trial=180 s6d=1 s6d_cmap=1 pattern=0 fix_sr06=1 s6d_fbcon=1 s6d_bind=0
rc=$?
echo "RC=$rc"
[ "$rc" = "0" ] || stop "insmod"
owned=1
dmesg -c >/dev/null

# Same command as the plan. stdin is the console, not this script.
NETSURFRES=$NSRES/ "$NSFB" -f linux -b 8 -w 640 -h 480 \
	"file://$PAGE_PATH" <"$P/dev/tty0" >"$P/tmp/nsfb.log" 2>&1 &
npid=$!
echo "NPID=$npid"
sleep "$SETTLE"
if ! kill -0 "$npid" 2>/dev/null; then
	fail "nsfb exited"
fi
dmesg | grep -q "session ON" || fail "no session"

hash_out=$(sh "$HASH") || fail "fbhash"
echo "$hash_out"
echo "$hash_out" | grep -q 'WD24X-BROWSER fb ' || fail "fb hash"
echo "$hash_out" | grep -q 'faults 0 ' || fail "faults"
echo "$hash_out" | grep -q 'thrash 0 ' || fail "thrash"
echo "$hash_out" | grep -q 'mode letter' || fail "mode"
echo "$hash_out" | grep -q 'WD24X-BROWSER done' || fail "fbhash done"

kill "$npid" 2>/dev/null
wait "$npid" 2>/dev/null
npid=
own_cleanup || fail "rmmod"
if ! text_ok; then
	echo "WD24X-BROWSER-RUN stop restore"
	exit 1
fi
echo "WD24X-BROWSER-RUN restore mismatches 0"
echo "WD24X-BROWSER-RUN done"
