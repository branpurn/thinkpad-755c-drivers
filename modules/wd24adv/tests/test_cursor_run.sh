#!/bin/sh
# Drives docs/s6d-cursor-run.sh. No hardware.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
SCRIPT=$ROOT/docs/s6d-cursor-run.sh
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
		'cursor 1 show 1 regs 8 vram 256 restored 1' \
		> "$M/proc/wd24adv/s6d"
	cat > "$M/bin/sha256sum" <<'EOF'
#!/bin/sh
b=$(basename "$1")
case "$b" in
wd24adv-cursor.ko) h=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa ;;
*) h=000 ;;
esac
echo "$h  $1"
EOF
	cat > "$M/bin/insmod" <<'EOF'
#!/bin/sh
echo "$*" >> "$WD24X_MOCK/insmod.args"
printf '%s\n' 'wd24adv 1 - Live' >> "$WD24X_MOCK/proc/modules"
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
echo "wd24adv: session ON"
EOF
	cat > "$M/bin/sleep" <<'EOF'
#!/bin/sh
if [ "${1:-}" = 180 ]; then
	printf '%s\n' 'wd24adv live: trial expiry -> 0' \
		'port writes: data 1, index 1; readback mismatches 0; in_text 1; trial -' \
		> "$WD24X_MOCK/proc/wd24adv/live"
	: > "$WD24X_MOCK/proc/fb"
fi
exit 0
EOF
	echo x > "$M/tmp/wd24adv-cursor.ko"
	chmod 755 "$M/bin/"*
	cat > "$M/module.env" <<'EOF'
WD24X_CANDIDATE_KO=/tmp/wd24adv-cursor.ko
WD24X_CANDIDATE_SHA=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
EOF
}

run() {
	M=$1
	WD24X_MOCK=$M WD24X_CANDIDATE_ENV=$M/module.env \
		PATH="$M/bin:/bin:/usr/bin" dash "$SCRIPT" >"$M/out" 2>"$M/err"
	echo $?
}

setup "$ROOT/modules/wd24adv/tests/.cursor-ok"
rc=$(run "$ROOT/modules/wd24adv/tests/.cursor-ok")
M=$ROOT/modules/wd24adv/tests/.cursor-ok
grep -q "WD24X-CURSOR-RUN done" "$M/out" && ok || bad "done"
grep -q "WD24X-CURSOR-RUN match" "$M/out" && ok || bad "match"
[ "$rc" = 0 ] && ok || bad "rc"
grep -q "cursor_gate=1" "$M/insmod.args" && ok || bad "param"
grep -q "s6d_bind=0" "$M/insmod.args" && ok || bad "bind"

setup "$ROOT/modules/wd24adv/tests/.cursor-restore"
M=$ROOT/modules/wd24adv/tests/.cursor-restore
printf '%s\n' 's6d: armed 1 active 1 stopping 0 busy 0 fb_reg 1 node 0' \
	'faults 0 switches 0 thrash 0 latched 0 mapped_base 0x0' \
	'cursor 1 show 1 regs 8 vram 256 restored 0' \
	> "$M/proc/wd24adv/s6d"
rc=$(run "$M")
grep -q "stop restore" "$M/out" && ok || bad "restore"
grep -q "power cycle" "$M/out" && ok || bad "power"
[ "$rc" != 0 ] && ok || bad "restore rc"
[ ! -e "$M/rmmod.args" ] && ok || bad "restore rmmod"

echo "test_cursor_run: $pass passed, $fail failed"
[ "$fail" = 0 ]
