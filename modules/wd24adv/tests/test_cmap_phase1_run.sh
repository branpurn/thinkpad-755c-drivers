#!/bin/sh
# Drives docs/s6d-cmap-phase1-run.sh through WD24X_MOCK. No insmod, no /dev/fb0.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
SCRIPT=$ROOT/docs/s6d-cmap-phase1-run.sh
fail=0
pass=0
ok() {
	pass=$((pass + 1))
}
bad() {
	fail=$((fail + 1))
	echo "FAIL $1"
}

setup() {
	M=$1
	rm -rf "$M"
	mkdir -p "$M/bin" "$M/proc/wd24adv" "$M/sys/class/vtconsole/vtcon0" \
		"$M/tmp/wd24-xfbdev" "$M/tmp/xfb-prefix/bin" "$M/tmp/.X11-unix"
	printf '%s\n' '(S) VGA+' > "$M/sys/class/vtconsole/vtcon0/name"
	printf '%s\n' 1 > "$M/sys/class/vtconsole/vtcon0/bind"
	: > "$M/proc/fb"
	printf '%s\n' 'font 1 - Live' > "$M/proc/modules"
	printf '%s\n' 'wd24adv live: trial expiry -> 0' \
		'port writes: data 1, index 1; readback mismatches 0; in_text 1; trial -' \
		> "$M/proc/wd24adv/live"
	printf '%s\n' 's6d: armed 0 active 0 stopping 0 busy 0 fb_reg 0 node -1' \
		'fbcon_param 1 bind_param 0' \
		'faults 15 switches 15 thrash 0 latched 0 mapped_base 0xffffffff' \
		'why -' \
		'cmap_param 0 published 0 frozen 1' > "$M/proc/wd24adv/s6d"
	cat > "$M/bin/sha256sum" <<'EOF'
#!/bin/sh
b=$(basename "$1")
case "$b" in
wd24x-glyph) h=c632750382b3def78912a6b5013ef53e74969e596ed39d1391b283555f01da67 ;;
wd24adv-cmap-92591b1e.ko) h=92591b1e26c3888868285a4a50d602cf969dfa9bdd53c9f868f21fef03d6a8ae ;;
wd24adv-pr14.ko) h=7da0fffe75883c1c28e876e0dc660f946a2e8f4d9dc9443ebc0810ac71bb7e36 ;;
Xfbdev) h=6450171476a8e6aa91883744947423db123c74a042d697f9263b686dcb2f9f13 ;;
xkbcomp) h=5ebf28e8823a5165efb0a2a0b997d2218ebbaa24ddb5abd76160643a939a5f62 ;;
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
if [ "${1:-}" = "-c" ]; then
	exit 0
fi
echo "wd24adv: session ON"
EOF
	cat > "$M/bin/sleep" <<'EOF'
#!/bin/sh
# Let a just-started client exit, and leave the X stand-in alive.
if [ "${1:-}" = 15 ] || [ "${1:-}" = 20 ]; then
	/bin/sleep 0.2
fi
exit 0
EOF
	cat > "$M/tmp/wd24-xfbdev/Xfbdev" <<'EOF'
#!/bin/sh
echo "$*" >> "$WD24X_MOCK/x.args"
exec /bin/sleep 60
EOF
	cat > "$M/tmp/xfb-prefix/bin/xkbcomp" <<'EOF'
#!/bin/sh
exit 0
EOF
	cat > "$M/tmp/wd24x-glyph" <<'EOF'
#!/bin/sh
echo "WD24X-GLYPH shadow 640x480x8 marks 5 match 5 ctrl 1" > "$WD24X_MOCK/tmp/wd24x-glyph.out"
exit 0
EOF
	for f in sha256sum insmod rmmod dmesg sleep; do
		chmod 755 "$M/bin/$f"
	done
	chmod 755 "$M/tmp/wd24-xfbdev/Xfbdev" "$M/tmp/xfb-prefix/bin/xkbcomp" \
		"$M/tmp/wd24x-glyph"
	: > "$M/tmp/busybox-wd24"
	chmod 755 "$M/tmp/busybox-wd24"
	echo x > "$M/tmp/wd24-xfbdev-i486.tgz"
	echo x > "$M/tmp/wd24-xkb.tgz"
	echo x > "$M/tmp/wd24adv-cmap-92591b1e.ko"
}

run() {
	M=$1
	WD24X_MOCK=$M PATH="$M/bin:/bin:/usr/bin" dash "$SCRIPT" >"$M/out" 2>"$M/err"
	echo $?
}

setup "$ROOT/modules/wd24adv/tests/.cmap1-mock-ok"
rc=$(run "$ROOT/modules/wd24adv/tests/.cmap1-mock-ok")
M=$ROOT/modules/wd24adv/tests/.cmap1-mock-ok
grep -q "WD24X-GLYPH-RUN done" "$M/out" && ok || bad "success done"
[ "$rc" = 0 ] && ok || bad "success rc $rc"
grep -q "dryrun=0 live=1 mode=640x480-8 trial=180 s6d=1 pattern=0 fix_sr06=1 s6d_fbcon=1 s6d_bind=0" "$M/insmod.args" && ok || bad "insmod args"
grep -q "wd24adv-cmap-92591b1e.ko" "$M/insmod.args" && ok || bad "candidate path"
grep -q "s6d_cmap" "$M/insmod.args" && bad "s6d_cmap passed" || ok
grep -q "wd24adv-pr14.ko" "$M/insmod.args" && bad "baseline insmod" || ok
grep -q ":1 -nolisten tcp -noreset -fb /dev/fb0" "$M/x.args" && ok || bad "X args"
grep -q -- "-ac" "$M/x.args" && bad "X -ac" || ok
grep -q "RM=$?" "$M/out" || true
grep -q "^RM=" "$M/out" && ok || bad "rmmod echoed"
[ -f "$M/tmp/wd24x-glyph" ] && ok || bad "glyph kept"
[ -f "$M/tmp/wd24adv-cmap-92591b1e.ko" ] && ok || bad "module kept"
[ -f "$M/tmp/wd24x-glyph.out" ] && ok || bad "stdout kept"
[ ! -e "$M/tmp/.X11-unix/X1" ] && ok || bad "socket left"

setup "$ROOT/modules/wd24adv/tests/.cmap1-mock-hash"
M=$ROOT/modules/wd24adv/tests/.cmap1-mock-hash
echo nope > "$M/tmp/wd24adv-cmap-92591b1e.ko"
echo nope > "$M/tmp/wd24x-glyph"
chmod 755 "$M/tmp/wd24x-glyph"
cat > "$M/bin/sha256sum" <<'EOF'
#!/bin/sh
exec /usr/bin/sha256sum "$@"
EOF
chmod 755 "$M/bin/sha256sum"
rc=$(run "$M")
grep -q "stop module hash" "$M/out" && ok || bad "bad hash"
[ ! -f "$M/insmod.args" ] && ok || bad "hash still insmod"
[ "$rc" != 0 ] && ok || bad "hash rc"

setup "$ROOT/modules/wd24adv/tests/.cmap1-mock-fb"
printf '%s\n' '0 wd24' > "$ROOT/modules/wd24adv/tests/.cmap1-mock-fb/proc/fb"
rc=$(run "$ROOT/modules/wd24adv/tests/.cmap1-mock-fb")
grep -q "stop console" "$ROOT/modules/wd24adv/tests/.cmap1-mock-fb/out" && ok || bad "fb"
[ ! -f "$ROOT/modules/wd24adv/tests/.cmap1-mock-fb/insmod.args" ] && ok || bad "fb insmod"

setup "$ROOT/modules/wd24adv/tests/.cmap1-mock-vt"
printf '%s\n' 'frame buffer' > "$ROOT/modules/wd24adv/tests/.cmap1-mock-vt/sys/class/vtconsole/vtcon0/name"
rc=$(run "$ROOT/modules/wd24adv/tests/.cmap1-mock-vt")
grep -q "stop console" "$ROOT/modules/wd24adv/tests/.cmap1-mock-vt/out" && ok || bad "vtcon"

setup "$ROOT/modules/wd24adv/tests/.cmap1-mock-x1"
echo 1 > "$ROOT/modules/wd24adv/tests/.cmap1-mock-x1/tmp/.X1-lock"
rc=$(run "$ROOT/modules/wd24adv/tests/.cmap1-mock-x1")
grep -q "stop stale X1" "$ROOT/modules/wd24adv/tests/.cmap1-mock-x1/out" && ok || bad "stale"
[ -f "$ROOT/modules/wd24adv/tests/.cmap1-mock-x1/tmp/.X1-lock" ] && ok || bad "stale removed"
[ ! -f "$ROOT/modules/wd24adv/tests/.cmap1-mock-x1/insmod.args" ] && ok || bad "stale insmod"

setup "$ROOT/modules/wd24adv/tests/.cmap1-mock-mark"
cat > "$ROOT/modules/wd24adv/tests/.cmap1-mock-mark/tmp/wd24x-glyph" <<'EOF'
#!/bin/sh
echo "WD24X-GLYPH shadow 640x480x8 marks 5 match 5 ctrl 0" > "$WD24X_MOCK/tmp/wd24x-glyph.out"
exit 0
EOF
chmod 755 "$ROOT/modules/wd24adv/tests/.cmap1-mock-mark/tmp/wd24x-glyph"
rc=$(run "$ROOT/modules/wd24adv/tests/.cmap1-mock-mark")
M=$ROOT/modules/wd24adv/tests/.cmap1-mock-mark
grep -q "stop client" "$M/out" && ok || bad "ctrl"
grep -q "WD24X-GLYPH-RUN done" "$M/out" && bad "ctrl done" || ok
[ -f "$M/rmmod.args" ] && ok || bad "ctrl rmmod"
[ -f "$M/tmp/wd24x-glyph.out" ] && ok || bad "ctrl log kept"

setup "$ROOT/modules/wd24adv/tests/.cmap1-mock-time"
cat > "$ROOT/modules/wd24adv/tests/.cmap1-mock-time/tmp/wd24x-glyph" <<'EOF'
#!/bin/sh
exec /bin/sleep 30
EOF
chmod 755 "$ROOT/modules/wd24adv/tests/.cmap1-mock-time/tmp/wd24x-glyph"
rc=$(run "$ROOT/modules/wd24adv/tests/.cmap1-mock-time")
M=$ROOT/modules/wd24adv/tests/.cmap1-mock-time
grep -q "stop client timeout" "$M/out" && ok || bad "timeout"
grep -q "WD24X-GLYPH-RUN done" "$M/out" && bad "timeout done" || ok
[ -f "$M/rmmod.args" ] && ok || bad "timeout rmmod"

setup "$ROOT/modules/wd24adv/tests/.cmap1-mock-pre"
printf '%s\n' 'wd24adv 1 - Live' >> "$ROOT/modules/wd24adv/tests/.cmap1-mock-pre/proc/modules"
rc=$(run "$ROOT/modules/wd24adv/tests/.cmap1-mock-pre")
M=$ROOT/modules/wd24adv/tests/.cmap1-mock-pre
grep -q "stop wd24adv loaded" "$M/out" && ok || bad "preexisting"
[ ! -f "$M/rmmod.args" ] && ok || bad "preexisting rmmod"
[ ! -f "$M/insmod.args" ] && ok || bad "preexisting insmod"
grep -q "WD24X-GLYPH-RUN done" "$M/out" && bad "preexisting done" || ok

setup "$ROOT/modules/wd24adv/tests/.cmap1-mock-race"
cat > "$ROOT/modules/wd24adv/tests/.cmap1-mock-race/bin/insmod" <<'EOF'
#!/bin/sh
echo "$*" >> "$WD24X_MOCK/insmod.args"
printf '%s\n' 'wd24adv 1 - Live' >> "$WD24X_MOCK/proc/modules"
exit 1
EOF
chmod 755 "$ROOT/modules/wd24adv/tests/.cmap1-mock-race/bin/insmod"
rc=$(run "$ROOT/modules/wd24adv/tests/.cmap1-mock-race")
M=$ROOT/modules/wd24adv/tests/.cmap1-mock-race
grep -q "stop insmod" "$M/out" && ok || bad "insmod race"
grep -q "^RC=1" "$M/out" && ok || bad "insmod rc"
[ ! -f "$M/rmmod.args" ] && ok || bad "insmod race rmmod"
grep -q "WD24X-GLYPH-RUN done" "$M/out" && bad "insmod race done" || ok
grep -q '^wd24adv ' "$M/proc/modules" && ok || bad "unrelated module kept"

setup "$ROOT/modules/wd24adv/tests/.cmap1-mock-rm"
cat > "$ROOT/modules/wd24adv/tests/.cmap1-mock-rm/bin/rmmod" <<'EOF'
#!/bin/sh
echo "$*" >> "$WD24X_MOCK/rmmod.args"
exit 1
EOF
chmod 755 "$ROOT/modules/wd24adv/tests/.cmap1-mock-rm/bin/rmmod"
rc=$(run "$ROOT/modules/wd24adv/tests/.cmap1-mock-rm")
M=$ROOT/modules/wd24adv/tests/.cmap1-mock-rm
grep -q "^RM=1" "$M/out" && ok || bad "rmmod status"
grep -q "WD24X-GLYPH-RUN done" "$M/out" && bad "rmmod done" || ok
[ "$rc" != 0 ] && ok || bad "rmmod rc"

setup "$ROOT/modules/wd24adv/tests/.cmap1-mock-exp"
printf '%s\n' 'wd24adv live: trial expiry -> 5' \
	'port writes: data 1, index 1; readback mismatches 0; in_text 1; trial -' \
	> "$ROOT/modules/wd24adv/tests/.cmap1-mock-exp/proc/wd24adv/live"
rc=$(run "$ROOT/modules/wd24adv/tests/.cmap1-mock-exp")
M=$ROOT/modules/wd24adv/tests/.cmap1-mock-exp
grep -q "stop expiry" "$M/out" && ok || bad "expiry code"
grep -q "WD24X-GLYPH-RUN done" "$M/out" && bad "expiry done" || ok
[ -f "$M/rmmod.args" ] && ok || bad "expiry still unloads owned"

setup "$ROOT/modules/wd24adv/tests/.cmap1-mock-thr"
printf '%s\n' 's6d: armed 0 active 0 stopping 0 busy 0 fb_reg 0 node -1' \
	'fbcon_param 1 bind_param 0' \
	'faults 15 switches 15 thrash 0 latched 1 mapped_base 0xffffffff' \
	'why -' \
	'cmap_param 0 published 0 frozen 1' \
	> "$ROOT/modules/wd24adv/tests/.cmap1-mock-thr/proc/wd24adv/s6d"
rc=$(run "$ROOT/modules/wd24adv/tests/.cmap1-mock-thr")
M=$ROOT/modules/wd24adv/tests/.cmap1-mock-thr
grep -q "stop expiry" "$M/out" && ok || bad "latched"
grep -q "WD24X-GLYPH-RUN done" "$M/out" && bad "latched done" || ok

setup "$ROOT/modules/wd24adv/tests/.cmap1-mock-on"
printf '%s\n' 's6d: armed 1 active 1 stopping 0 busy 1 fb_reg 1 node 0' \
	'fbcon_param 1 bind_param 0' \
	'faults 1 switches 1 thrash 0 latched 0 mapped_base 0x0' \
	'why -' \
	'cmap_param 1 published 0 frozen 0' \
	> "$ROOT/modules/wd24adv/tests/.cmap1-mock-on/proc/wd24adv/s6d"
rc=$(run "$ROOT/modules/wd24adv/tests/.cmap1-mock-on")
M=$ROOT/modules/wd24adv/tests/.cmap1-mock-on
grep -q "stop cmap default" "$M/out" && ok || bad "cmap on"
grep -q "WD24X-GLYPH-RUN done" "$M/out" && bad "cmap on done" || ok
[ ! -f "$M/tmp/wd24x-glyph.out" ] && ok || bad "cmap on drew"
[ -f "$M/rmmod.args" ] && ok || bad "cmap on owned rmmod"

setup "$ROOT/modules/wd24adv/tests/.cmap1-mock-base"
echo KEEP > "$ROOT/modules/wd24adv/tests/.cmap1-mock-base/tmp/wd24adv-pr14.ko"
cat > "$ROOT/modules/wd24adv/tests/.cmap1-mock-base/bin/sha256sum" <<'EOF'
#!/bin/sh
b=$(basename "$1")
case "$b" in
wd24adv-pr14.ko) h=000 ;;
*) h=92591b1e26c3888868285a4a50d602cf969dfa9bdd53c9f868f21fef03d6a8ae ;;
esac
if [ "$b" = "wd24x-glyph" ]; then
	h=c632750382b3def78912a6b5013ef53e74969e596ed39d1391b283555f01da67
fi
if [ "$b" = "Xfbdev" ]; then
	h=6450171476a8e6aa91883744947423db123c74a042d697f9263b686dcb2f9f13
fi
if [ "$b" = "xkbcomp" ]; then
	h=5ebf28e8823a5165efb0a2a0b997d2218ebbaa24ddb5abd76160643a939a5f62
fi
if [ "$b" = "wd24adv-cmap-92591b1e.ko" ]; then
	h=92591b1e26c3888868285a4a50d602cf969dfa9bdd53c9f868f21fef03d6a8ae
fi
echo "$h  $1"
EOF
chmod 755 "$ROOT/modules/wd24adv/tests/.cmap1-mock-base/bin/sha256sum"
rc=$(run "$ROOT/modules/wd24adv/tests/.cmap1-mock-base")
M=$ROOT/modules/wd24adv/tests/.cmap1-mock-base
grep -q "stop baseline hash" "$M/out" && ok || bad "baseline hash"
[ ! -f "$M/insmod.args" ] && ok || bad "baseline insmod"
[ ! -f "$M/rmmod.args" ] && ok || bad "baseline rmmod"
[ "$(cat "$M/tmp/wd24adv-pr14.ko")" = KEEP ] && ok || bad "baseline overwritten"

rm -rf "$ROOT/modules/wd24adv/tests/.cmap1-mock-ok" \
	"$ROOT/modules/wd24adv/tests/.cmap1-mock-hash" \
	"$ROOT/modules/wd24adv/tests/.cmap1-mock-fb" \
	"$ROOT/modules/wd24adv/tests/.cmap1-mock-vt" \
	"$ROOT/modules/wd24adv/tests/.cmap1-mock-x1" \
	"$ROOT/modules/wd24adv/tests/.cmap1-mock-mark" \
	"$ROOT/modules/wd24adv/tests/.cmap1-mock-time" \
	"$ROOT/modules/wd24adv/tests/.cmap1-mock-pre" \
	"$ROOT/modules/wd24adv/tests/.cmap1-mock-race" \
	"$ROOT/modules/wd24adv/tests/.cmap1-mock-rm" \
	"$ROOT/modules/wd24adv/tests/.cmap1-mock-exp" \
	"$ROOT/modules/wd24adv/tests/.cmap1-mock-thr" \
	"$ROOT/modules/wd24adv/tests/.cmap1-mock-on" \
	"$ROOT/modules/wd24adv/tests/.cmap1-mock-base"
echo "test_cmap_phase1_run: $pass passed, $fail failed"
[ "$fail" = 0 ]
