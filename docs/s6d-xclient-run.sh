#!/bin/sh
# One shot. Trial clock starts at insmod. No -ac. X uses -noreset
# so the last client does not start a second server generation.
xpid=
cpid=
finishing=
finish() {
	if [ -n "$finishing" ]; then
		return
	fi
	finishing=1
	if [ -n "$cpid" ]; then
		st=$(state "$cpid")
		if [ -n "$st" ] && [ "$st" != "Z" ]; then
			kill -TERM "$cpid" 2>/dev/null
			echo "CTERM=$?"
			sleep 2
		fi
		reap_hard "$cpid"
		cpid=
	fi
	if [ -n "$xpid" ]; then
		st=$(state "$xpid")
		if [ -n "$st" ] && [ "$st" != "Z" ]; then
			kill -TERM "$xpid" 2>/dev/null
			echo "TERM=$?"
			sleep 10
		fi
		reap_hard "$xpid"
		xpid=
	fi
	rm -f /tmp/.X1-lock /tmp/.X11-unix/X1
	if grep -q '^wd24adv ' /proc/modules 2>/dev/null; then
		rmmod wd24adv
		echo "RM=$?"
	fi
	rm -rf /tmp/wd24-xfbdev /tmp/xfb-prefix
	rm -f /tmp/busybox-wd24 /tmp/wd24x-client /tmp/wd24x-run.sh
}
fail() {
	echo "WD24X-RUN stop $1"
	finish
	exit 1
}
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
	echo "WD24X-RUN stop pid $pid still $st"
	return 1
}
hash_ok() {
	case "$(sha256sum "$1")" in
	"$2"*) return 0 ;;
	*) return 1 ;;
	esac
}

[ -x /tmp/busybox-wd24 ] || fail "extractor missing"
[ -f /tmp/wd24-xfbdev-i486.tgz ] || fail "X archive missing"
[ -f /tmp/wd24-xkb.tgz ] || fail "xkb archive missing"
[ -x /tmp/wd24x-client ] || fail "client missing"
hash_ok /tmp/wd24x-client d846aa0153c59017072118e142d075502cd0332d720527b0255f4a422a021179 || fail "client hash"
[ -z "$(cat /proc/fb)" ] || fail "fb not empty"

if [ ! -x /tmp/wd24-xfbdev/Xfbdev ]; then
	/tmp/busybox-wd24 tar -C /tmp -xzof /tmp/wd24-xfbdev-i486.tgz || fail "extract X"
fi
if [ ! -x /tmp/xfb-prefix/bin/xkbcomp ]; then
	/tmp/busybox-wd24 tar -C /tmp -xzof /tmp/wd24-xkb.tgz || fail "extract xkb"
fi
hash_ok /tmp/xfb-prefix/bin/xkbcomp 5ebf28e8823a5165efb0a2a0b997d2218ebbaa24ddb5abd76160643a939a5f62 || fail "xkbcomp hash"

dmesg -c >/dev/null
insmod /tmp/wd24adv-pr14.ko dryrun=0 live=1 mode=640x480-8 trial=180 s6d=1 pattern=0 fix_sr06=1 s6d_fbcon=1 s6d_bind=0
rc=$?
echo "RC=$rc"
[ "$rc" = "0" ] || fail "insmod"

/tmp/wd24-xfbdev/Xfbdev :1 -nolisten tcp -noreset -fb /dev/fb0 >/tmp/wd24-xfbdev/x.log 2>&1 &
xpid=$!
echo "XPID=$xpid"
sleep 15
st=$(state "$xpid")
if [ -z "$st" ] || [ "$st" = "Z" ]; then
	xpid=
	fail "X not alive"
fi
if grep -q "Failed to compile keymap" /tmp/wd24-xfbdev/x.log; then
	fail "keymap"
fi
if grep -q "Fatal server error" /tmp/wd24-xfbdev/x.log; then
	fail "fatal"
fi
dmesg | grep -q "session ON" || fail "no session"

/tmp/wd24x-client :1 >/tmp/wd24x-client.out 2>/tmp/wd24x-client.err &
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
cat /tmp/wd24x-client.out
cat /proc/wd24adv/s6d
cpid=
if [ "$cl" != "0" ] || ! grep -q "rects 5 match 5" /tmp/wd24x-client.out; then
	fail "client"
fi

finish
echo "WD24X-RUN done"
