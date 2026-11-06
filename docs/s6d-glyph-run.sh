#!/bin/sh
# Glyph trial. One shot. Does not start by being copied.
# Do not set WD24X_MOCK on the target. An empty value uses /proc and /tmp.
# Trial clock starts at insmod. No -ac. X uses -noreset.
# The rectangle client /tmp/wd24x-client is not used and is not removed.
P=${WD24X_MOCK:-}
xpid=
cpid=
xstarted=
finishing=

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
	echo "WD24X-GLYPH-RUN stop pid $pid still $st"
	return 1
}

hash_ok() {
	case "$(sha256sum "$1")" in
	"$2"*) return 0 ;;
	*) return 1 ;;
	esac
}

finish() {
	if [ -n "$finishing" ]; then
		return 0
	fi
	finishing=1
	if [ -n "$cpid" ]; then
		st=$(state "$cpid")
		if [ -n "$st" ] && [ "$st" != "Z" ]; then
			kill -TERM "$cpid" 2>/dev/null
			echo "CTERM=$?"
			sleep 2
		fi
		reap_hard "$cpid" || return 1
		cpid=
	fi
	if [ -n "$xpid" ]; then
		st=$(state "$xpid")
		if [ -n "$st" ] && [ "$st" != "Z" ]; then
			kill -TERM "$xpid" 2>/dev/null
			echo "TERM=$?"
			sleep 10
		fi
		reap_hard "$xpid" || return 1
		xpid=
	fi
	if [ -n "$xstarted" ]; then
		rm -f "$P/tmp/.X1-lock" "$P/tmp/.X11-unix/X1"
	fi
	if [ -r "$P/proc/wd24adv/live" ]; then
		echo "WD24X-GLYPH-RUN live"
		cat "$P/proc/wd24adv/live"
	fi
	if [ -r "$P/proc/wd24adv/s6d" ]; then
		echo "WD24X-GLYPH-RUN s6d"
		cat "$P/proc/wd24adv/s6d"
	fi
	if grep -q '^wd24adv ' "$P/proc/modules" 2>/dev/null; then
		rmmod wd24adv
		echo "RM=$?"
	fi
	dmesg > "$P/tmp/wd24x-glyph-dmesg.txt" 2>/dev/null
	return 0
}

fail() {
	echo "WD24X-GLYPH-RUN stop $1"
	finish || echo "WD24X-GLYPH-RUN executor finish rmmod and X1"
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
	*"trial expiry"*) ;;
	*) return 1 ;;
	esac
	case "$live" in
	*"mismatches 0"*) ;;
	*) return 1 ;;
	esac
	case "$live" in
	*"in_text 1"*) ;;
	*) return 1 ;;
	esac
	return 0
}

exact_mark() {
	lines=0
	hit=0
	while IFS= read -r line; do
		lines=$((lines + 1))
		if [ "$line" = "WD24X-GLYPH shadow 640x480x8 marks 5 match 5 ctrl 1" ]; then
			hit=1
		fi
	done < "$P/tmp/wd24x-glyph.out"
	[ "$lines" = 1 ] && [ "$hit" = 1 ]
}

[ -x "$P/tmp/busybox-wd24" ] || fail "extractor missing"
[ -f "$P/tmp/wd24-xfbdev-i486.tgz" ] || fail "X archive missing"
[ -f "$P/tmp/wd24-xkb.tgz" ] || fail "xkb archive missing"
[ -f "$P/tmp/wd24adv-pr14.ko" ] || fail "module missing"
[ -x "$P/tmp/wd24x-glyph" ] || fail "glyph missing"
hash_ok "$P/tmp/wd24adv-pr14.ko" 7da0fffe75883c1c28e876e0dc660f946a2e8f4d9dc9443ebc0810ac71bb7e36 || fail "module hash"
hash_ok "$P/tmp/wd24x-glyph" c632750382b3def78912a6b5013ef53e74969e596ed39d1391b283555f01da67 || fail "glyph hash"
console_ok || fail "console"
if [ -S "$P/tmp/.X11-unix/X1" ] || [ -f "$P/tmp/.X1-lock" ]; then
	fail "stale X1"
fi
if grep -q '^wd24adv ' "$P/proc/modules" 2>/dev/null; then
	fail "wd24adv loaded"
fi
if grep -q '^vga16fb ' "$P/proc/modules" 2>/dev/null; then
	fail "vga16fb"
fi
if grep -q '^wd90c24fb ' "$P/proc/modules" 2>/dev/null; then
	fail "wd90c24fb"
fi

if [ ! -x "$P/tmp/wd24-xfbdev/Xfbdev" ]; then
	"$P/tmp/busybox-wd24" tar -C "$P/tmp" -xzof "$P/tmp/wd24-xfbdev-i486.tgz" || fail "extract X"
fi
if [ ! -x "$P/tmp/xfb-prefix/bin/xkbcomp" ]; then
	"$P/tmp/busybox-wd24" tar -C "$P/tmp" -xzof "$P/tmp/wd24-xkb.tgz" || fail "extract xkb"
fi
hash_ok "$P/tmp/wd24-xfbdev/Xfbdev" 6450171476a8e6aa91883744947423db123c74a042d697f9263b686dcb2f9f13 || fail "X hash"
hash_ok "$P/tmp/xfb-prefix/bin/xkbcomp" 5ebf28e8823a5165efb0a2a0b997d2218ebbaa24ddb5abd76160643a939a5f62 || fail "xkbcomp hash"

dmesg -c >/dev/null
insmod "$P/tmp/wd24adv-pr14.ko" dryrun=0 live=1 mode=640x480-8 trial=180 s6d=1 pattern=0 fix_sr06=1 s6d_fbcon=1 s6d_bind=0
rc=$?
echo "RC=$rc"
[ "$rc" = "0" ] || fail "insmod"

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

"$P/tmp/wd24x-glyph" :1 >"$P/tmp/wd24x-glyph.out" 2>"$P/tmp/wd24x-glyph.err" &
cpid=$!
echo "CPID=$cpid"
sleep 20
st=$(state "$cpid")
if [ -n "$st" ] && [ "$st" != "Z" ]; then
	fail "client timeout"
fi
wait "$cpid"
cl=$?
echo "CL=$cl"
cat "$P/tmp/wd24x-glyph.out"
if [ -r "$P/proc/wd24adv/s6d" ]; then
	cat "$P/proc/wd24adv/s6d"
fi
cpid=
if [ "$cl" != "0" ] || ! exact_mark; then
	fail "client"
fi

if [ -n "$xpid" ]; then
	st=$(state "$xpid")
	if [ -n "$st" ] && [ "$st" != "Z" ]; then
		kill -TERM "$xpid" 2>/dev/null
		echo "TERM=$?"
		sleep 10
	fi
	reap_hard "$xpid" || {
		echo "WD24X-GLYPH-RUN executor reap $xpid then rmmod"
		exit 1
	}
	xpid=
fi
if [ -n "$xstarted" ]; then
	rm -f "$P/tmp/.X1-lock" "$P/tmp/.X11-unix/X1"
fi
xstarted=

# 15s plus 20s are already slept. 150s more passes trial=180.
sleep 150
if ! live_ok || ! console_ok; then
	echo "WD24X-GLYPH-RUN stop expiry"
	finish || echo "WD24X-GLYPH-RUN executor finish rmmod"
	exit 1
fi
finish || {
	echo "WD24X-GLYPH-RUN executor finish rmmod"
	exit 1
}
console_ok || fail "console after rmmod"
echo "WD24X-GLYPH-RUN done"
