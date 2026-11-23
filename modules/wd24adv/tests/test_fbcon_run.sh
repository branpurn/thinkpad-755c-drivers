#!/bin/sh
# Drives docs/s6d-fbcon-run.sh. No hardware.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
SCRIPT=$ROOT/docs/s6d-fbcon-run.sh
fail=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

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
		> "$M/proc/wd24adv/s6d"
	: > "$M/dev/tty0"
	cat > "$M/bin/sha256sum" <<'EOF'
#!/bin/sh
b=$(basename "$1")
case "$b" in
wd24adv-desktop.ko) h=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa ;;
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
	echo x > "$M/tmp/wd24adv-desktop.ko"
	chmod 755 "$M/bin/"*
	cat > "$M/module.env" <<'EOF'
WD24X_CANDIDATE_KO=/tmp/wd24adv-desktop.ko
WD24X_CANDIDATE_SHA=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
EOF
}

run() {
	M=$1
	WD24X_MOCK=$M WD24X_CANDIDATE_ENV=$M/module.env \
		PATH="$M/bin:/bin:/usr/bin" dash "$SCRIPT" >"$M/out" 2>"$M/err"
	echo $?
}

setup "$ROOT/modules/wd24adv/tests/.fbcon-ok"
rc=$(run "$ROOT/modules/wd24adv/tests/.fbcon-ok")
M=$ROOT/modules/wd24adv/tests/.fbcon-ok
grep -q "WD24X-FBCON-RUN done" "$M/out" && ok || bad "done"
grep -q "WD24X-FBCON-RUN after FB=\[0 wd24adv\]" "$M/out" && ok || bad "after fb"
grep -q "session ON" "$M/out" && bad "session line" || ok
[ "$rc" = 0 ] && ok || bad "rc"
grep -q "s6d_bind=1" "$M/insmod.args" && ok || bad "bind"
grep -q "s6d_cmap=1" "$M/insmod.args" && bad "cmap" || ok
grep -q "Xfbdev" "$M/out" && bad "x" || ok
grep -q "WD24X-FBCON line" "$M/dev/tty0" && ok || bad "tty"
grep -q "pr14" "$M/insmod.args" && bad "baseline" || ok

setup "$ROOT/modules/wd24adv/tests/.fbcon-thrash"
M=$ROOT/modules/wd24adv/tests/.fbcon-thrash
printf '%s\n' 's6d: armed 1 active 1 stopping 0 busy 0 fb_reg 1 node 0' \
	'faults 32 switches 32 thrash 1 latched 1 mapped_base 0x10' \
	> "$M/proc/wd24adv/s6d"
rc=$(run "$M")
grep -q "stop thrash" "$M/out" && ok || bad "thrash"
[ "$rc" != 0 ] && ok || bad "thrash rc"
grep -q "WD24X-FBCON-RUN unbind vtcon1" "$M/out" && ok || bad "unbind"
[ "$(cat "$M/sys/class/vtconsole/vtcon1/bind")" = 0 ] && ok || bad "bind cleared"
grep -q "wd24adv" "$M/rmmod.args" && ok || bad "thrash rmmod"

setup "$ROOT/modules/wd24adv/tests/.fbcon-nosession"
M=$ROOT/modules/wd24adv/tests/.fbcon-nosession
cat > "$M/bin/dmesg" <<'EOF'
#!/bin/sh
if [ "${1:-}" = "-c" ]; then exit 0; fi
echo "wd24adv: session ON"
EOF
chmod 755 "$M/bin/dmesg"
rc=$(run "$M")
grep -q "stop no bind" "$M/out" && ok || bad "no bind"
grep -q "stop no session" "$M/out" && bad "old session gate" || ok
[ "$rc" != 0 ] && ok || bad "no bind rc"
grep -q "WD24X-FBCON-RUN unbind vtcon1" "$M/out" && ok || bad "no bind unbind"

echo "test_fbcon_run: $pass passed, $fail failed"
[ "$fail" = 0 ]
