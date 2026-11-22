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
wd24adv-desktop.ko) h=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa ;;
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
echo "FB=${WD24X_DOOM_FB-unset}" >> "$WD24X_MOCK/doom.args"
echo "PRESENT=${WD24X_DOOM_PRESENT-unset}" >> "$WD24X_MOCK/doom.args"
echo "$*" >> "$WD24X_MOCK/doom.args"
echo "WD24X-DOOM fb ioctl 320 200"
echo "WD24X-DOOM palette put 256"
if [ "${WD24X_DOOM_PRESENT:-}" = "13h" ]; then
	echo "WD24X-DOOM present 13h"
	echo "WD24X-DOOM blit direct 64000"
	echo "WD24X-DOOM present switches 0"
else
	echo "WD24X-DOOM present letterbox"
	echo "WD24X-DOOM blit letterbox dword"
	echo "WD24X-DOOM present switches 5"
fi
case " $* " in
*" -timedemo "*)
	echo "WD24X-DOOM input none"
	if [ -n "${WD24X_DOOM_FRAMES:-}" ]; then
		echo "WD24X-DOOM time frames ${WD24X_DOOM_FRAMES} render_ms 1 present_ms 2 palette_ms 0"
		echo "FRAMES=${WD24X_DOOM_FRAMES}" >> "$WD24X_MOCK/doom.args"
		echo "TIME=${WD24X_DOOM_TIME-}" >> "$WD24X_MOCK/doom.args"
		exit 0
	fi
	echo "timed 2134 gametics in 700 realtics" >&2
	exit 255
	;;
esac
echo "WD24X-DOOM input tty"
exit 0
EOF
	echo x > "$M/tmp/wd24adv-desktop.ko"
	dd if=/dev/zero of="$M/tmp/doom1.wad" bs=4196020 count=1 status=none
	chmod 755 "$M/bin/"* "$M/tmp/wd24x-doom"
	cat > "$M/module.env" <<'EOF'
WD24X_CANDIDATE_KO=/tmp/wd24adv-desktop.ko
WD24X_CANDIDATE_SHA=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
EOF
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
		WD24X_CANDIDATE_ENV=$M/module.env \
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
grep -q "wd24adv-desktop.ko" "$M/insmod.args" && ok || bad "ko"
grep -q "fdf30c77" "$M/insmod.args" && bad "old module" || ok
grep -q "FB=ioctl" "$M/doom.args" && ok || bad "fb ioctl"
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
	dash "$SCRIPT" demo1 >"$ROOT/modules/wd24adv/tests/.doom-unpinned" 2>&1)
grep -q "stop candidate unpinned" "$ROOT/modules/wd24adv/tests/.doom-unpinned" && ok || bad "unpinned"

setup "$ROOT/modules/wd24adv/tests/.doom-measure"
rc=$(run "$ROOT/modules/wd24adv/tests/.doom-measure" measure)
M=$ROOT/modules/wd24adv/tests/.doom-measure
grep -q "WD24X-DOOM-RUN done" "$M/out" && ok || bad "measure done"
grep -q "trial=180" "$M/insmod.args" && ok || bad "measure trial"
grep -q "FRAMES=60" "$M/doom.args" && ok || bad "frames"
grep -q "TIME=30" "$M/doom.args" && ok || bad "time env"
grep -q "WD24X-DOOM-RUN measure frames 60" "$M/out" && ok || bad "measure line"
grep -q "timedemo" "$M/out" && bad "measure wants gametics" || ok
grep -q "mode=640x480-8" "$M/insmod.args" && ok || bad "measure mode"
grep -q "present_mode=13h" "$M/insmod.args" && bad "measure 13h param" || ok
[ "$rc" = 0 ] && ok || bad "measure rc"

setup "$ROOT/modules/wd24adv/tests/.doom-13h"
rc=$(run "$ROOT/modules/wd24adv/tests/.doom-13h" measure13h)
M=$ROOT/modules/wd24adv/tests/.doom-13h
grep -q "WD24X-DOOM-RUN done" "$M/out" && ok || bad "13h done"
grep -q "trial=180" "$M/insmod.args" && ok || bad "13h trial"
grep -q "mode=320x200-8" "$M/insmod.args" && ok || bad "13h mode"
grep -q "present_mode=13h" "$M/insmod.args" && ok || bad "13h param"
grep -q "mode=640x480-8" "$M/insmod.args" && bad "13h still 640" || ok
grep -q "PRESENT=13h" "$M/doom.args" && ok || bad "13h env"
grep -q "FRAMES=60" "$M/doom.args" && ok || bad "13h frames"
grep -q "WD24X-DOOM-RUN measure13h frames 60" "$M/out" && ok || bad "13h line"
grep -q "present 13h refused" "$M/out" && bad "13h refused" || ok
[ "$rc" = 0 ] && ok || bad "13h rc"

echo "test_doom_run: $pass passed, $fail failed"
[ "$fail" = 0 ]
