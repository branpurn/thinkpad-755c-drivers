#!/bin/sh
# Drives docs/s6d-scroll-run.sh. No hardware.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
SCRIPT=$ROOT/docs/s6d-scroll-run.sh
SRC=$ROOT/modules/wd24adv/wd24adv_main.c
fail=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

grep -q 'ypanstep = 0' "$SRC" && ok || bad "ypan"
grep -q 'FBINFO_HWACCEL_YPAN' "$SRC" && bad "ypan flag" || ok
grep -q 'FBINFO_HWACCEL_COPYAREA' "$SRC" && ok || bad "copy flag"

setup() {
	M=$1
	rm -rf "$M"
	mkdir -p "$M/bin" "$M/proc/wd24adv" "$M/sys/class/vtconsole/vtcon0" \
		"$M/tmp/.X11-unix" "$M/dev"
	printf '%s\n' '(S) VGA+' > "$M/sys/class/vtconsole/vtcon0/name"
	printf '%s\n' 1 > "$M/sys/class/vtconsole/vtcon0/bind"
	: > "$M/proc/fb"
	printf '%s\n' 'font 1 - Live' > "$M/proc/modules"
	printf '%s\n' 'wd24adv live: trial PENDING' > "$M/proc/wd24adv/live"
	printf '%s\n' 's6d: armed 1 active 1 stopping 0 busy 0 fb_reg 1 node 0' \
		'faults 0 switches 0 thrash 0 latched 0 mapped_base 0x0' \
		'accel 1 gate 1 match 1024' \
		> "$M/proc/wd24adv/s6d"
	: > "$M/dev/tty0"
	cat > "$M/bin/sha256sum" <<'EOF'
#!/bin/sh
b=$(basename "$1")
case "$b" in
wd24adv-accel.ko) h=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa ;;
*) h=000 ;;
esac
echo "$h  $1"
EOF
	cat > "$M/bin/insmod" <<'EOF'
#!/bin/sh
echo "$*" >> "$WD24X_MOCK/insmod.args"
printf '%s\n' 'wd24adv 1 - Live' >> "$WD24X_MOCK/proc/modules"
mkdir -p "$WD24X_MOCK/sys/class/vtconsole/vtcon1"
printf '%s\n' 'frame buffer device' > "$WD24X_MOCK/sys/class/vtconsole/vtcon1/name"
printf '%s\n' 1 > "$WD24X_MOCK/sys/class/vtconsole/vtcon1/bind"
printf '%s\n' '0 wd24adv' > "$WD24X_MOCK/proc/fb"
exit 0
EOF
	cat > "$M/bin/rmmod" <<'EOF'
#!/bin/sh
echo "$*" >> "$WD24X_MOCK/rmmod.args"
exit 0
EOF
	cat > "$M/bin/dmesg" <<'EOF'
#!/bin/sh
if [ "${1:-}" = "-c" ]; then exit 0; fi
echo "Console: switching to colour frame buffer device 80x30"
echo "wd24adv: S6d: armed + registered fb0 (s6d_bind=1; fbcon may bind)"
EOF
	cat > "$M/bin/sleep" <<'EOF'
#!/bin/sh
if [ "${1:-}" = 180 ]; then
	printf '%s\n' 'wd24adv live: trial expiry -> 0' \
		'port writes: data 1, index 1; readback mismatches 0; in_text 1; trial -' \
		> "$WD24X_MOCK/proc/wd24adv/live"
	: > "$WD24X_MOCK/proc/fb"
	rm -rf "$WD24X_MOCK/sys/class/vtconsole/vtcon1"
fi
exit 0
EOF
	echo x > "$M/tmp/wd24adv-accel.ko"
	chmod 755 "$M/bin/"*
	cat > "$M/module.env" <<'EOF'
WD24X_CANDIDATE_KO=/tmp/wd24adv-accel.ko
WD24X_CANDIDATE_SHA=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
EOF
}

run() {
	M=$1
	WD24X_MOCK=$M WD24X_CANDIDATE_ENV=$M/module.env \
		PATH="$M/bin:/bin:/usr/bin" dash "$SCRIPT" >"$M/out" 2>"$M/err"
	echo $?
}

setup "$ROOT/modules/wd24adv/tests/.scroll-ok"
rc=$(run "$ROOT/modules/wd24adv/tests/.scroll-ok")
M=$ROOT/modules/wd24adv/tests/.scroll-ok
grep -q "WD24X-SCROLL-RUN done" "$M/out" && ok || bad "done"
[ "$rc" = 0 ] && ok || bad "rc"
grep -q "s6d_bind=1" "$M/insmod.args" && ok || bad "bind"
grep -q "accel=1" "$M/insmod.args" && ok || bad "accel"
grep -q "WD24X-SCROLL line" "$M/dev/tty0" && ok || bad "tty"
grep -q "Xfbdev" "$M/out" && bad "x" || ok

setup "$ROOT/modules/wd24adv/tests/.scroll-timeout"
M=$ROOT/modules/wd24adv/tests/.scroll-timeout
printf '%s\n' 's6d: armed 1 active 1 stopping 0 busy 0 fb_reg 1 node 0' \
	'faults 0 switches 0 thrash 0 latched 0 mapped_base 0x0' \
	'accel 1 gate 2 match 0' \
	> "$M/proc/wd24adv/s6d"
rc=$(run "$M")
grep -q "stop timeout" "$M/out" && ok || bad "timeout"
[ "$rc" != 0 ] && ok || bad "timeout rc"
[ ! -e "$M/rmmod.args" ] && ok || bad "timeout rmmod"
grep -q "WD24X-SCROLL-RUN unbind vtcon1" "$M/out" && ok || bad "timeout unbind"
[ "$(cat "$M/sys/class/vtconsole/vtcon1/bind")" = 0 ] && ok || bad "timeout bind"
grep -q "WD24X-SCROLL line" "$M/dev/tty0" && bad "tty after timeout" || ok

echo "test_scroll_run: $pass passed, $fail failed"
[ "$fail" = 0 ]
