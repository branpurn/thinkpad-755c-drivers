#!/bin/sh
# Drives docs/s6d-desktop-run.sh through WD24X_MOCK. No hardware.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
SCRIPT=$ROOT/docs/s6d-desktop-run.sh
fail=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

palette() {
	i=0
	while [ "$i" -lt 256 ]; do
		printf '%s\n' "$i 1 2 3"
		i=$((i + 1))
	done
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
	printf '%s\n' 'wd24adv live: trial PENDING' > "$M/proc/wd24adv/live"
	printf '%s\n' 's6d: armed 1 active 1 stopping 0 busy 1 fb_reg 1 node 0' \
		'fbcon_param 1 bind_param 0' \
		'faults 0 switches 0 thrash 0 latched 0 mapped_base 0x0' \
		'why -' \
		'cmap_param 1 published 1 frozen 0' > "$M/proc/wd24adv/s6d"
	{
		printf '%s\n' 'WD24X-CMAP-CACHE entries 256 shift10 1'
		palette
	} > "$M/proc/wd24adv/cmap"
	cat > "$M/bin/sha256sum" <<'EOF'
#!/bin/sh
b=$(basename "$1")
case "$b" in
wd24adv-desktop.ko) h=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa ;;
wd24x-xcmp) h=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb ;;
wd24x-glyph) h=c632750382b3def78912a6b5013ef53e74969e596ed39d1391b283555f01da67 ;;
busybox-wd24) h=52b264655f25e902098eb797ba72f424cbe3ec0c37caceeaf5f0984835682236 ;;
wd24-xfbdev-i486.tgz) h=af429004d7d279f84d6ebdf2b62f589b81471dd791f6d0949d3d6c67a106c224 ;;
wd24-xkb.tgz) h=5d2203659fffd031f85715e3ce29a7be987c04bc231e36574e6fb748147e9cb2 ;;
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
if [ "${1:-}" = "-c" ]; then exit 0; fi
echo "wd24adv: session ON"
EOF
	cat > "$M/bin/sleep" <<'EOF'
#!/bin/sh
if [ "${1:-}" = 15 ] || [ "${1:-}" = 20 ]; then
	/bin/sleep 0.2
fi
if [ "${1:-}" = 150 ]; then
	printf '%s\n' 'wd24adv live: trial expiry -> 0' \
		'port writes: data 1, index 1; readback mismatches 0; in_text 1; trial -' \
		> "$WD24X_MOCK/proc/wd24adv/live"
	printf '%s\n' 's6d: armed 0 active 0 stopping 0 busy 0 fb_reg 0 node -1' \
		'fbcon_param 1 bind_param 0' \
		'faults 0 switches 0 thrash 0 latched 0 mapped_base 0xffffffff' \
		'why -' \
		'cmap_param 1 published 1 frozen 1' > "$WD24X_MOCK/proc/wd24adv/s6d"
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
echo "WD24X-GLYPH shadow 640x480x8 marks 5 match 5 ctrl 1"
exit 0
EOF
	cat > "$M/tmp/wd24x-xcmp" <<'EOF'
#!/bin/sh
echo "WD24X-XCMAP entries 256 shift10 1"
i=0
while [ "$i" -lt 256 ]; do
	echo "$i 1 2 3"
	i=$((i + 1))
done
exit 0
EOF
	echo x > "$M/tmp/wd24adv-desktop.ko"
	echo x > "$M/tmp/busybox-wd24"
	echo x > "$M/tmp/wd24-xfbdev-i486.tgz"
	echo x > "$M/tmp/wd24-xkb.tgz"
	chmod 755 "$M/bin/"* "$M/tmp/wd24-xfbdev/Xfbdev" \
		"$M/tmp/xfb-prefix/bin/xkbcomp" "$M/tmp/wd24x-glyph" \
		"$M/tmp/wd24x-xcmp" "$M/tmp/busybox-wd24"
	cat > "$M/desktop.env" <<'EOF'
WD24X_CANDIDATE_KO=/tmp/wd24adv-desktop.ko
WD24X_CANDIDATE_SHA=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
WD24X_XCMP=/tmp/wd24x-xcmp
WD24X_XCMP_SHA=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
WD24X_GLYPH=/tmp/wd24x-glyph
WD24X_GLYPH_SHA=c632750382b3def78912a6b5013ef53e74969e596ed39d1391b283555f01da67
WD24X_XFBDEV_TGZ=/tmp/wd24-xfbdev-i486.tgz
WD24X_XFBDEV_TGZ_SHA=af429004d7d279f84d6ebdf2b62f589b81471dd791f6d0949d3d6c67a106c224
WD24X_XKB_TGZ=/tmp/wd24-xkb.tgz
WD24X_XKB_TGZ_SHA=5d2203659fffd031f85715e3ce29a7be987c04bc231e36574e6fb748147e9cb2
WD24X_BUSYBOX=/tmp/busybox-wd24
WD24X_BUSYBOX_SHA=52b264655f25e902098eb797ba72f424cbe3ec0c37caceeaf5f0984835682236
WD24X_XFBDEV_BIN_SHA=6450171476a8e6aa91883744947423db123c74a042d697f9263b686dcb2f9f13
WD24X_XKBCOMP_SHA=5ebf28e8823a5165efb0a2a0b997d2218ebbaa24ddb5abd76160643a939a5f62
EOF
}

run() {
	M=$1
	WD24X_MOCK=$M WD24X_DESKTOP_ENV=$M/desktop.env \
		PATH="$M/bin:/bin:/usr/bin" dash "$SCRIPT" >"$M/out" 2>"$M/err"
	echo $?
}

setup "$ROOT/modules/wd24adv/tests/.desk-ok"
rc=$(run "$ROOT/modules/wd24adv/tests/.desk-ok")
M=$ROOT/modules/wd24adv/tests/.desk-ok
grep -q "WD24X-DESKTOP-RUN done" "$M/out" && ok || bad "done"
[ "$rc" = 0 ] && ok || bad "rc"
grep -q "s6d_cmap=1" "$M/insmod.args" && ok || bad "cmap arg"
grep -q "trial=180" "$M/insmod.args" && ok || bad "trial"
grep -q "wd24adv-desktop.ko" "$M/insmod.args" && ok || bad "ko"
grep -q "pr14" "$M/insmod.args" && bad "baseline" || ok
grep -q "WD24X-DESKTOP-RUN cmap match 256" "$M/out" && ok || bad "match"
grep -q -- "-nolisten tcp" "$M/x.args" && ok || bad "x flags"
grep -q -- "-ac" "$M/x.args" && bad "ac" || ok

setup "$ROOT/modules/wd24adv/tests/.desk-mis"
M=$ROOT/modules/wd24adv/tests/.desk-mis
printf '%s\n' '255 9 9 9' >> "$M/proc/wd24adv/cmap"
rc=$(run "$M")
grep -q "WD24X-DESKTOP-RUN stop cmap" "$M/out" && ok || bad "mismatch"
[ "$rc" != 0 ] && ok || bad "mismatch rc"

rc=$(WD24X_DESKTOP_ENV=$ROOT/docs/s6d-desktop.env PATH="/bin:/usr/bin" \
	dash "$SCRIPT" >"$ROOT/modules/wd24adv/tests/.desk-unpinned" 2>&1)
grep -q "stop candidate unpinned" "$ROOT/modules/wd24adv/tests/.desk-unpinned" && ok || bad "unpinned"
[ "$rc" != 0 ] && ok || bad "unpinned rc"

echo "test_desktop_run: $pass passed, $fail failed"
[ "$fail" = 0 ]
