#!/bin/sh
# Drives docs/s6d-browser-run.sh. No hardware.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
SCRIPT=$ROOT/docs/s6d-browser-run.sh
fail=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

setup() {
	M=$1
	rm -rf "$M"
	mkdir -p "$M/bin" "$M/proc/wd24adv" "$M/sys/class/vtconsole/vtcon0" \
		"$M/tmp/nsfb/nsres" "$M/tmp/nsfb/pages" "$M/dev"
	printf '%s\n' '(S) VGA+' > "$M/sys/class/vtconsole/vtcon0/name"
	printf '%s\n' 1 > "$M/sys/class/vtconsole/vtcon0/bind"
	: > "$M/proc/fb"
	printf '%s\n' 'font 1 - Live' > "$M/proc/modules"
	printf '%s\n' 'wd24adv live: trial PENDING' > "$M/proc/wd24adv/live"
	printf '%s\n' 's6d: armed 1 active 1 stopping 0 busy 0 fb_reg 1 node 0' \
		'faults 0 switches 0 thrash 0 latched 0 mapped_base 0x0' \
		'present frames 0 last 0 total 0 ns 0 copy_ns 0 mode letter' \
		> "$M/proc/wd24adv/s6d"
	dd if=/dev/zero of="$M/dev/fb0" bs=307200 count=1 status=none
	: > "$M/dev/tty0"
	printf '%s\n' 'Messages' > "$M/tmp/nsfb/nsres/Messages"
	printf '%s\n' '<html></html>' > "$M/tmp/nsfb/pages/canvas.html"
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
echo "wd24adv live text: 0 (port writes mismatches 0)"
EOF
	cat > "$M/bin/sleep" <<'EOF'
#!/bin/sh
exit 0
EOF
	cat > "$M/tmp/nsfb/nsfb" <<'EOF'
#!/bin/sh
echo "NETSURFRES=$NETSURFRES" >> "$WD24X_MOCK/nsfb.args"
echo "$*" >> "$WD24X_MOCK/nsfb.args"
exec /bin/sleep 60
EOF
	echo x > "$M/tmp/wd24adv-desktop.ko"
	chmod 755 "$M/bin/"* "$M/tmp/nsfb/nsfb"
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

setup "$ROOT/modules/wd24adv/tests/.browser-run-ok"
rc=$(run "$ROOT/modules/wd24adv/tests/.browser-run-ok")
M=$ROOT/modules/wd24adv/tests/.browser-run-ok
grep -q "WD24X-BROWSER-RUN done" "$M/out" && ok || bad "done"
[ "$rc" = 0 ] && ok || bad "rc"
grep -q "s6d_bind=0" "$M/insmod.args" && ok || bad "bind"
grep -q "s6d_cmap=1" "$M/insmod.args" && ok || bad "cmap"
grep -q "mode=640x480-8" "$M/insmod.args" && ok || bad "mode"
grep -q "NETSURFRES=$M/tmp/nsfb/nsres/" "$M/nsfb.args" && ok || bad "nsres"
grep -q -- "-f linux -b 8 -w 640 -h 480" "$M/nsfb.args" && ok || bad "launch"
grep -q "file://$M/tmp/nsfb/pages/canvas.html" "$M/nsfb.args" && ok || bad "page"
grep -q "faults 0 " "$M/out" && ok || bad "faults"
grep -q "restore mismatches 0" "$M/out" && ok || bad "restore"
[ -f "$M/rmmod.args" ] && ok || bad "rmmod"

setup "$ROOT/modules/wd24adv/tests/.browser-run-miss"
rm -f "$ROOT/modules/wd24adv/tests/.browser-run-miss/tmp/nsfb/nsfb"
rc=$(run "$ROOT/modules/wd24adv/tests/.browser-run-miss")
M=$ROOT/modules/wd24adv/tests/.browser-run-miss
grep -q "stop nsfb missing" "$M/out" && ok || bad "missing"
[ ! -f "$M/insmod.args" ] && ok || bad "missing insmod"
[ "$rc" != 0 ] && ok || bad "missing rc"

setup "$ROOT/modules/wd24adv/tests/.browser-run-fault"
printf '%s\n' 's6d: armed 1 active 1 stopping 0 busy 1 fb_reg 1 node 0' \
	'faults 1 switches 0 thrash 0 latched 0 mapped_base 0x0' \
	'present frames 0 last 0 total 0 ns 0 copy_ns 0 mode letter' \
	> "$ROOT/modules/wd24adv/tests/.browser-run-fault/proc/wd24adv/s6d"
rc=$(run "$ROOT/modules/wd24adv/tests/.browser-run-fault")
M=$ROOT/modules/wd24adv/tests/.browser-run-fault
grep -q "stop faults" "$M/out" && ok || bad "faults stop"
grep -q "WD24X-BROWSER-RUN done" "$M/out" && bad "faults done" || ok
[ -f "$M/rmmod.args" ] && ok || bad "faults rmmod"
[ "$rc" != 0 ] && ok || bad "faults rc"

rm -rf "$ROOT/modules/wd24adv/tests/.browser-run-ok" \
	"$ROOT/modules/wd24adv/tests/.browser-run-miss" \
	"$ROOT/modules/wd24adv/tests/.browser-run-fault"
echo "test_browser_run: $pass passed, $fail failed"
[ "$fail" = 0 ]
