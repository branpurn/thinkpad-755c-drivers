#!/bin/sh
# Drives docs/s6d-doom-run.sh through WD24X_MOCK. No hardware.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
SCRIPT=$ROOT/docs/s6d-doom-run.sh
fail=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

setup() {
	M=$1
	rm -rf "$M"
	mkdir -p "$M/bin" "$M/proc/wd24adv" "$M/sys/class/vtconsole/vtcon0" \
		"$M/tmp/.X11-unix"
	printf '%s\n' '(S) VGA+' > "$M/sys/class/vtconsole/vtcon0/name"
	printf '%s\n' 1 > "$M/sys/class/vtconsole/vtcon0/bind"
	: > "$M/proc/fb"
	printf '%s\n' 'font 1 - Live' > "$M/proc/modules"
	printf '%s\n' 'wd24adv live: trial PENDING' > "$M/proc/wd24adv/live"
	printf '%s\n' 's6d: armed 1 active 1 stopping 0 busy 1 fb_reg 1 node 0' \
		'faults 0 switches 0 thrash 0 latched 0 mapped_base 0x0' \
		> "$M/proc/wd24adv/s6d"
	cat > "$M/bin/sha256sum" <<'EOF'
#!/bin/sh
b=$(basename "$1")
case "$b" in
wd24adv-cmap-fdf30c77.ko) h=fdf30c7743df6149cc0750c96118d50cfcd55c4c8104e88a2b5b9240da7e6d63 ;;
wd24x-doom) h=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc ;;
doom1.wad) h=1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771 ;;
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
printf '%s\n' 'wd24adv live: trial expiry -> 0' \
	'port writes: data 1, index 1; readback mismatches 0; in_text 1; trial -' \
	> "$WD24X_MOCK/proc/wd24adv/live"
printf '%s\n' 's6d: armed 0 active 0 stopping 0 busy 0 fb_reg 0 node -1' \
	'faults 0 switches 0 thrash 0 latched 0 mapped_base 0xffffffff' \
	> "$WD24X_MOCK/proc/wd24adv/s6d"
exit 0
EOF
	cat > "$M/tmp/wd24x-doom" <<'EOF'
#!/bin/sh
echo "INPUT=${WD24X_DOOM_INPUT-unset}" >> "$WD24X_MOCK/doom.args"
echo "$*" >> "$WD24X_MOCK/doom.args"
echo "WD24X-DOOM fb mmap 640 480 index"
echo "WD24X-DOOM present letterbox"
echo "WD24X-DOOM blit letterbox dword"
echo "WD24X-DOOM palette put 256"
case " $* " in
*" -timedemo "*)
	echo "WD24X-DOOM input none"
	echo "timed 2134 gametics in 700 realtics" >&2
	exit 255
	;;
esac
echo "WD24X-DOOM input tty"
exit 0
EOF
	echo x > "$M/tmp/wd24adv-cmap-fdf30c77.ko"
	dd if=/dev/zero of="$M/tmp/doom1.wad" bs=4196020 count=1 status=none
	chmod 755 "$M/bin/"* "$M/tmp/wd24x-doom"
	cat > "$M/doom.env" <<'EOF'
WD24X_DOOM_BIN=/tmp/wd24x-doom
WD24X_DOOM_SHA=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
WD24X_WAD=/tmp/doom1.wad
WD24X_WAD_SHA=1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771
WD24X_WAD_SIZE=4196020
EOF
}

run() {
	M=$1
	shift
	WD24X_MOCK=$M WD24X_DOOM_ENV=$M/doom.env \
		WD24X_CANDIDATE_ENV=$ROOT/docs/s6d-candidate.env \
		PATH="$M/bin:/bin:/usr/bin" dash "$SCRIPT" "$@" >"$M/out" 2>"$M/err"
	echo $?
}

setup "$ROOT/modules/wd24adv/tests/.doom-ok"
rc=$(run "$ROOT/modules/wd24adv/tests/.doom-ok" demo1)
M=$ROOT/modules/wd24adv/tests/.doom-ok
grep -q "WD24X-DOOM-RUN done" "$M/out" && ok || bad "done"
[ "$rc" = 0 ] && ok || bad "rc"
grep -q "trial=600" "$M/insmod.args" && ok || bad "trial"
grep -q "s6d_cmap=1" "$M/insmod.args" && ok || bad "cmap"
grep -q "wd24adv-cmap-fdf30c77.ko" "$M/insmod.args" && ok || bad "ko"
grep -q "pr14" "$M/insmod.args" && bad "baseline" || ok
grep -q "WD24X-DOOM-FPS gametics 2134 realtics 700 fps_x100 10670" "$M/out" && ok || bad "fps"
grep -q -- "-timedemo demo1" "$M/doom.args" && ok || bad "demo arg"
grep -q -- "-nosound" "$M/doom.args" && ok || bad "nosound"
grep -q "INPUT=$" "$M/doom.args" && ok || bad "input off"
grep -q "INPUT=tty" "$M/doom.args" && bad "input grabbed" || ok

setup "$ROOT/modules/wd24adv/tests/.doom-eyes"
rc=$(run "$ROOT/modules/wd24adv/tests/.doom-eyes" eyes)
M=$ROOT/modules/wd24adv/tests/.doom-eyes
grep -q "WD24X-DOOM-RUN done" "$M/out" && ok || bad "eyes done"
grep -q "INPUT=tty" "$M/doom.args" && ok || bad "eyes input"
grep -q -- "-timedemo" "$M/doom.args" && bad "eyes demo" || ok
[ "$rc" = 0 ] && ok || bad "eyes rc"

setup "$ROOT/modules/wd24adv/tests/.doom-cfg"
M=$ROOT/modules/wd24adv/tests/.doom-cfg
echo x > "$M/tmp/default.cfg"
rc=$(run "$M" demo1)
grep -q "stop config" "$M/out" && ok || bad "config"
[ ! -f "$M/insmod.args" ] && ok || bad "config insmod"

rc=$(PATH="/bin:/usr/bin" dash "$SCRIPT" >"$ROOT/modules/wd24adv/tests/.doom-mode" 2>&1)
grep -q "stop mode" "$ROOT/modules/wd24adv/tests/.doom-mode" && ok || bad "mode"
[ "$rc" != 0 ] && ok || bad "mode rc"

rc=$(PATH="/bin:/usr/bin" WD24X_DOOM_ENV=$ROOT/docs/s6d-doom.env \
	WD24X_CANDIDATE_ENV=$ROOT/docs/s6d-candidate.env \
	dash "$SCRIPT" demo1 >"$ROOT/modules/wd24adv/tests/.doom-unpinned" 2>&1)
grep -q "stop doom unpinned" "$ROOT/modules/wd24adv/tests/.doom-unpinned" && ok || bad "unpinned"

echo "test_doom_run: $pass passed, $fail failed"
[ "$fail" = 0 ]
