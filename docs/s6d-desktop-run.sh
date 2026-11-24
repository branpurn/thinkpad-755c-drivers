#!/bin/sh
# Bounded X on the colormap module. One shot. Does not start by being copied.
# Do not set WD24X_MOCK on the target. An empty value uses /proc and /tmp.
# Trial clock starts at insmod. No -ac. X uses -noreset. No long-lived server.
# Glyph draws indices 0 and 15. xcmp only QueryColors. Neither opens /dev/fb0
# a second time: that open is -EBUSY, and the close would drop X's pages.
# The DAC check is /proc/wd24adv/cmap, the cache FBIOGETCMAP would return
# after fb_setcmap's 3C9 readback. This script does not write VGA registers.
#
# owned=1 only after this process's insmod returns 0. A module already
# present, or one that appears after a failed insmod, is not unloaded.
P=${WD24X_MOCK:-}
xpid=
cpid=
xstarted=
xextracted=
xkbextracted=
owned=
finishing=
cleanup_rc=0

state() {
	if [ ! -f "/proc/$1/stat" ]; then
		return 0
	fi
	read _ _ st _ < "/proc/$1/stat"
	printf '%s' "$st"
}

reap_hard() {
	pid=$1
	st=$(state "$pid")
	if [ -z "$st" ] || [ "$st" = "Z" ]; then
		wait "$pid"
		echo "WAIT=$?"
		return 0
	fi
	kill -9 "$pid"
	echo "KILL9=$?"
	sleep 5
	st=$(state "$pid")
	if [ -z "$st" ] || [ "$st" = "Z" ]; then
		wait "$pid"
		echo "WAIT=$?"
		return 0
	fi
	echo "WD24X-DESKTOP-RUN stop pid $pid still $st"
	return 1
}

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
	echo "WD24X-DESKTOP-RUN stop $1"
	if [ -n "$xextracted" ]; then
		rm -rf "$P/tmp/wd24-xfbdev"
	fi
	if [ -n "$xkbextracted" ]; then
		rm -rf "$P/tmp/xfb-prefix"
	fi
	exit 1
}

own_cleanup() {
	rmst=0
	if [ -n "$finishing" ]; then
		return "$cleanup_rc"
	fi
	finishing=1
	if [ -n "$cpid" ]; then
		st=$(state "$cpid")
		if [ -n "$st" ] && [ "$st" != "Z" ]; then
			kill -TERM "$cpid" 2>/dev/null
			echo "CTERM=$?"
			sleep 2
		fi
		reap_hard "$cpid" || cleanup_rc=1
		cpid=
	fi
	if [ "$cleanup_rc" != 0 ]; then
		echo "WD24X-DESKTOP-RUN executor reap then rmmod"
		return 1
	fi
	if [ -n "$xpid" ]; then
		st=$(state "$xpid")
		if [ -n "$st" ] && [ "$st" != "Z" ]; then
			kill -TERM "$xpid" 2>/dev/null
			echo "TERM=$?"
			sleep 10
		fi
		if ! reap_hard "$xpid"; then
			cleanup_rc=1
			echo "WD24X-DESKTOP-RUN executor reap $xpid then rmmod"
			return 1
		fi
		xpid=
	fi
	if [ -n "$xstarted" ] && [ "$cleanup_rc" = 0 ]; then
		rm -f "$P/tmp/.X1-lock" "$P/tmp/.X11-unix/X1"
	fi
	if [ -n "$xextracted" ]; then
		rm -rf "$P/tmp/wd24-xfbdev"
	fi
	if [ -n "$xkbextracted" ]; then
		rm -rf "$P/tmp/xfb-prefix"
	fi
	if [ -r "$P/proc/wd24adv/live" ]; then
		echo "WD24X-DESKTOP-RUN live"
		cat "$P/proc/wd24adv/live"
	fi
	if [ -r "$P/proc/wd24adv/s6d" ]; then
		echo "WD24X-DESKTOP-RUN s6d"
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
	dmesg > "$P/tmp/wd24x-desktop-dmesg.txt" 2>/dev/null
	return "$cleanup_rc"
}

fail() {
	echo "WD24X-DESKTOP-RUN stop $1"
	own_cleanup || echo "WD24X-DESKTOP-RUN executor finish rmmod and X1"
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
	*" fb_reg 0 node "*) ;;
	*) return 1 ;;
	esac
	case "$s6d" in
	*" thrash 0 latched 0 mapped_base "*) ;;
	*) return 1 ;;
	esac
	return 0
}

session_live() {
	# Xfbdev still holds /dev/fb0. s6d_fb_open set busy. Release has
	# not run, so frozen is 0. xcmp does not open /dev/fb0.
	s6d=$(cat "$P/proc/wd24adv/s6d" 2>/dev/null) || return 1
	echo "$s6d"
	case "$s6d" in
	*" busy 1 "*) ;;
	*) return 1 ;;
	esac
	case "$s6d" in
	*"cmap_param 1 published 1 frozen 0"*) ;;
	*) return 1 ;;
	esac
	case "$s6d" in
	*" thrash 0 latched 0 "*) ;;
	*) return 1 ;;
	esac
	return 0
}

session_closed() {
	# After X exits, s6d_fb_release(user != 0) sets frozen and clears
	# busy. It does not clear published. This is before rmmod.
	s6d=$(cat "$P/proc/wd24adv/s6d" 2>/dev/null) || return 1
	echo "$s6d"
	case "$s6d" in
	*" busy 0 "*) ;;
	*) return 1 ;;
	esac
	case "$s6d" in
	*"cmap_param 1 published 1 frozen 1"*) ;;
	*) return 1 ;;
	esac
	case "$s6d" in
	*" thrash 0 latched 0 "*) ;;
	*) return 1 ;;
	esac
	return 0
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

cmap_match() {
	cfile=$1
	xfile=$2
	exec 3<"$cfile"
	exec 4<"$xfile"
	i=0
	while IFS= read -r a <&3; do
		case "$a" in
		[0-9]*) ;;
		*) continue ;;
		esac
		b=
		while IFS= read -r b <&4; do
			case "$b" in
			[0-9]*) break ;;
			*) b= ;;
			esac
		done
		[ -n "$b" ] || {
			exec 3<&-
			exec 4<&-
			return 1
		}
		[ "$a" = "$b" ] || {
			exec 3<&-
			exec 4<&-
			return 1
		}
		i=$((i + 1))
	done
	while IFS= read -r b <&4; do
		case "$b" in
		[0-9]*)
			exec 3<&-
			exec 4<&-
			return 1
			;;
		esac
	done
	exec 3<&-
	exec 4<&-
	[ "$i" = 256 ]
}

if [ -z "${WD24X_DESKTOP_ENV:-}" ]; then
	WD24X_DESKTOP_ENV=$(dirname "$0")/s6d-desktop.env
fi
[ -f "$WD24X_DESKTOP_ENV" ] || stop "desktop env"
. "$WD24X_DESKTOP_ENV"
pinned "${WD24X_CANDIDATE_SHA:-}" || stop "candidate unpinned"
pinned "${WD24X_XCMP_SHA:-}" || stop "xcmp unpinned"
need_tmp "${WD24X_CANDIDATE_KO:-}" || stop "candidate path"
need_tmp "${WD24X_XCMP:-}" || stop "xcmp path"
need_tmp "${WD24X_GLYPH:-}" || stop "glyph path"
case "${WD24X_CANDIDATE_KO:-}" in
*pr14*) stop "baseline" ;;
esac
KO="$P$WD24X_CANDIDATE_KO"
GLYPH="$P$WD24X_GLYPH"
XCMP="$P$WD24X_XCMP"

[ -x "$P$WD24X_BUSYBOX" ] || stop "extractor missing"
[ -f "$P$WD24X_XFBDEV_TGZ" ] || stop "X archive missing"
[ -f "$P$WD24X_XKB_TGZ" ] || stop "xkb archive missing"
[ -f "$KO" ] || stop "module missing"
[ -x "$GLYPH" ] || stop "glyph missing"
[ -x "$XCMP" ] || stop "xcmp missing"
hash_ok "$P$WD24X_BUSYBOX" "$WD24X_BUSYBOX_SHA" || stop "extractor hash"
hash_ok "$P$WD24X_XFBDEV_TGZ" "$WD24X_XFBDEV_TGZ_SHA" || stop "X archive hash"
hash_ok "$P$WD24X_XKB_TGZ" "$WD24X_XKB_TGZ_SHA" || stop "xkb archive hash"
hash_ok "$KO" "$WD24X_CANDIDATE_SHA" || stop "module hash"
hash_ok "$GLYPH" "$WD24X_GLYPH_SHA" || stop "glyph hash"
hash_ok "$XCMP" "$WD24X_XCMP_SHA" || stop "xcmp hash"
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

if [ ! -x "$P/tmp/wd24-xfbdev/Xfbdev" ]; then
	xextracted=1
	"$P$WD24X_BUSYBOX" tar -C "$P/tmp" -xzof "$P$WD24X_XFBDEV_TGZ" || stop "extract X"
fi
if [ ! -x "$P/tmp/xfb-prefix/bin/xkbcomp" ]; then
	xkbextracted=1
	"$P$WD24X_BUSYBOX" tar -C "$P/tmp" -xzof "$P$WD24X_XKB_TGZ" || stop "extract xkb"
fi
hash_ok "$P/tmp/wd24-xfbdev/Xfbdev" "$WD24X_XFBDEV_BIN_SHA" || stop "X hash"
hash_ok "$P/tmp/xfb-prefix/bin/xkbcomp" "$WD24X_XKBCOMP_SHA" || stop "xkbcomp hash"

insmod "$KO" dryrun=0 live=1 mode=640x480-8 trial=180 s6d=1 s6d_cmap=1 pattern=0 fix_sr06=1 s6d_fbcon=1 s6d_bind=0
rc=$?
echo "RC=$rc"
[ "$rc" = "0" ] || stop "insmod"
owned=1
dmesg -c >/dev/null

"$P/tmp/wd24-xfbdev/Xfbdev" :1 -nolisten tcp -noreset -fb /dev/fb0 >"$P/tmp/wd24-xfbdev/x.log" 2>&1 &
xpid=$!
xstarted=1
echo "XPID=$xpid"
sleep 15
st=$(state "$xpid")
if [ -z "$st" ] || [ "$st" = "Z" ]; then
	reap_hard "$xpid" || true
	xpid=
	fail "X not alive"
fi
if grep -q "Failed to compile keymap" "$P/tmp/wd24-xfbdev/x.log"; then
	fail "keymap"
fi
if grep -q "Fatal server error" "$P/tmp/wd24-xfbdev/x.log"; then
	fail "fatal"
fi
dmesg | grep -q "session ON" || fail "no session"

"$GLYPH" :1 >"$P/tmp/wd24x-glyph.out" 2>"$P/tmp/wd24x-glyph.err" &
cpid=$!
echo "CPID=$cpid"
sleep 20
st=$(state "$cpid")
if [ -n "$st" ] && [ "$st" != "Z" ]; then
	fail "glyph timeout"
fi
wait "$cpid"
cl=$?
echo "CL=$cl"
cat "$P/tmp/wd24x-glyph.out"
cpid=
if [ "$cl" != "0" ] || ! have_line "$P/tmp/wd24x-glyph.out" "WD24X-GLYPH shadow 640x480x8 marks 5 match 5 ctrl 1"; then
	fail "glyph"
fi

"$XCMP" :1 >"$P/tmp/wd24x-xcmp.out" 2>"$P/tmp/wd24x-xcmp.err"
cl=$?
echo "XCL=$cl"
if [ "$cl" != "0" ] || ! have_line "$P/tmp/wd24x-xcmp.out" "WD24X-XCMAP entries 256 shift10 1"; then
	fail "xcmp"
fi

try=0
while [ "$try" -lt 5 ]; do
	cat "$P/proc/wd24adv/cmap" >"$P/tmp/wd24x-cmap-cache.out" 2>/dev/null || true
	if have_line "$P/tmp/wd24x-cmap-cache.out" "WD24X-CMAP-CACHE busy"; then
		try=$((try + 1))
		sleep 1
		continue
	fi
	break
done
if ! have_line "$P/tmp/wd24x-cmap-cache.out" "WD24X-CMAP-CACHE entries 256 shift10 1"; then
	fail "cmap cache"
fi
if ! session_live; then
	fail "session"
fi
if ! cmap_match "$P/tmp/wd24x-cmap-cache.out" "$P/tmp/wd24x-xcmp.out"; then
	fail "cmap"
fi
echo "WD24X-DESKTOP-RUN cmap match 256"

if [ -n "$xpid" ]; then
	st=$(state "$xpid")
	if [ -n "$st" ] && [ "$st" != "Z" ]; then
		kill -TERM "$xpid" 2>/dev/null
		echo "TERM=$?"
		sleep 10
	fi
	if ! reap_hard "$xpid"; then
		echo "WD24X-DESKTOP-RUN executor reap $xpid then rmmod"
		exit 1
	fi
	xpid=
fi
if [ -n "$xstarted" ]; then
	rm -f "$P/tmp/.X1-lock" "$P/tmp/.X11-unix/X1"
fi
xstarted=
if ! session_closed; then
	fail "closed"
fi

# 15s plus 20s are already slept. 150s more passes trial=180.
sleep 150
if ! live_ok || ! s6d_after || ! console_ok; then
	echo "WD24X-DESKTOP-RUN stop expiry"
	own_cleanup || echo "WD24X-DESKTOP-RUN executor finish rmmod"
	exit 1
fi
own_cleanup || exit 1
console_ok || {
	echo "WD24X-DESKTOP-RUN stop console after rmmod"
	exit 1
}
echo "WD24X-DESKTOP-RUN done"
