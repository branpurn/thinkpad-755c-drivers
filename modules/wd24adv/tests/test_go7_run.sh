#!/bin/sh
# Dry-run docs/s6d-go7-run.sh under dash and busybox ash. No hardware.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
SCRIPT=$ROOT/docs/s6d-go7-run.sh
fail=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

setup() {
	M=$1
	rm -rf "$M"
	mkdir -p "$M/bin" "$M/proc/wd24adv" "$M/sys/class/vtconsole/vtcon0" \
		"$M/tmp"
	printf '%s\n' '(S) VGA+' > "$M/sys/class/vtconsole/vtcon0/name"
	printf '%s\n' 1 > "$M/sys/class/vtconsole/vtcon0/bind"
	: > "$M/proc/fb"
	printf '%s\n' 'font 1 - Live' > "$M/proc/modules"
	printf '%s\n' 'wd24adv live: trial PENDING' > "$M/proc/wd24adv/live"
	cat > "$M/bin/sha256sum" <<'EOF'
#!/bin/sh
b=$(basename "$1")
case "$b" in
wd24adv-desktop.ko) h=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa ;;
wd24x-doom) h=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc ;;
doom1.wad) h=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb ;;
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
printf '%s\n' 'wd24adv live text: 0 mismatches 0' > "$WD24X_MOCK/proc/wd24adv/live"
exit 0
EOF
	cat > "$M/tmp/wd24x-doom" <<'EOF'
#!/bin/sh
echo "$*" >> "$WD24X_MOCK/doom.args"
echo "FRAMESTATS=${WD24X_DOOM_FRAMESTATS-}" >> "$WD24X_MOCK/doom.args"
echo "FLIP=${WD24X_DOOM_FLIP-}" >> "$WD24X_MOCK/doom.args"
echo "INPUT=${WD24X_DOOM_INPUT-}" >> "$WD24X_MOCK/doom.args"
case " $* " in
*" -warp "*)
	echo "WD24X-DOOM framestats frames 2 intervals 1 min 40 med 40 p95 40 p99 40 max 40 over1 1 over2 0 render_ms 30 copy_ms 8 flip_ms 0"
	echo "WD24X-DOOM framestats tics 0:0 1:2 2:0 3:0 4:0 5:0 6:0 7:0"
	echo "WD24X-DOOM framestats hist <5:0 <10:0 <15:0 <20:0 <29:0 <40:0 <58:1 <80:0 <114:0 <160:0 <240:0 >=240:0"
	exit 0
	;;
esac
echo "WD24X-DOOM framestats frames 4 intervals 3 min 50 med 52 p95 60 p99 60 max 60 over1 3 over2 1 render_ms 40 copy_ms 9 flip_ms 0" >&2
echo "WD24X-DOOM framestats tics 0:0 1:4 2:0 3:0 4:0 5:0 6:0 7:0" >&2
echo "WD24X-DOOM framestats hist <5:0 <10:0 <15:0 <20:0 <29:0 <40:0 <58:2 <80:1 <114:0 <160:0 <240:0 >=240:0" >&2
echo "timed 5026 gametics in 9000 realtics (19.545555 fps)" >&2
exit 255
EOF
	chmod 755 "$M/bin/sha256sum" "$M/bin/insmod" "$M/bin/rmmod" \
		"$M/tmp/wd24x-doom"
	printf 'xxxx' > "$M/tmp/doom1.wad"
	: > "$M/tmp/wd24adv-desktop.ko"
	cat > "$M/candidate.env" <<'EOF'
WD24X_CANDIDATE_KO=/tmp/wd24adv-desktop.ko
WD24X_CANDIDATE_SHA=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
EOF
	cat > "$M/doom.env" <<'EOF'
WD24X_DOOM_BIN=/tmp/wd24x-doom
WD24X_DOOM_SHA=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
WD24X_WAD=/tmp/doom1.wad
WD24X_WAD_SHA=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
WD24X_WAD_SIZE=4
EOF
}

run() {
	shell=$1
	M=$2
	PATH="$M/bin:$PATH" WD24X_MOCK="$M" WD24X_GO7_PLAY_SEC=0 \
		WD24X_CANDIDATE_ENV="$M/candidate.env" \
		WD24X_DOOM_ENV="$M/doom.env" \
		"$shell" "$SCRIPT" >"$M/out" 2>"$M/err"
	echo "$?"
}

setup "$ROOT/modules/wd24adv/tests/.go7-ok"
rc=$(run /usr/bin/dash "$ROOT/modules/wd24adv/tests/.go7-ok")
M=$ROOT/modules/wd24adv/tests/.go7-ok
grep -q "S6D-GO7 GO7_RC=0" "$M/out" && ok || bad "dash rc"
grep -q "S6D-GO7 done" "$M/out" && ok || bad "dash done"
grep -q "FRAMESTATS=1" "$M/doom.args" && ok || bad "dash stats env"
grep -q "FLIP=1" "$M/doom.args" && ok || bad "dash flip"
grep -q -- "-timedemo demo1" "$M/doom.args" && ok || bad "dash demo"
grep -q -- "-warp 1 1" "$M/doom.args" && ok || bad "dash warp"
grep -q "present_mode=13h" "$M/insmod.args" && ok || bad "dash insmod"
grep -q "wd24adv" "$M/rmmod.args" && ok || bad "dash rmmod"
[ "$rc" = 0 ] && ok || bad "dash exit"

setup "$ROOT/modules/wd24adv/tests/.go7-ash"
M=$ROOT/modules/wd24adv/tests/.go7-ash
PATH="$M/bin:$PATH" WD24X_MOCK="$M" WD24X_GO7_PLAY_SEC=0 \
	WD24X_CANDIDATE_ENV="$M/candidate.env" \
	WD24X_DOOM_ENV="$M/doom.env" \
	busybox ash "$SCRIPT" >"$M/out" 2>"$M/err"
rc=$?
grep -q "S6D-GO7 GO7_RC=0" "$M/out" && ok || bad "ash rc"
grep -q -- "-warp 1 1" "$M/doom.args" && ok || bad "ash warp"
grep -q -- "-timedemo demo1" "$M/doom.args" && ok || bad "ash demo"
[ "$rc" = 0 ] && ok || bad "ash exit"

setup "$ROOT/modules/wd24adv/tests/.go7-nostats"
M=$ROOT/modules/wd24adv/tests/.go7-nostats
cat > "$M/tmp/wd24x-doom" <<'EOF'
#!/bin/sh
echo "timed 1 gametics in 1 realtics (35.0 fps)" >&2
exit 255
EOF
chmod 755 "$M/tmp/wd24x-doom"
PATH="$M/bin:$PATH" WD24X_MOCK="$M" WD24X_GO7_PLAY_SEC=0 \
	WD24X_CANDIDATE_ENV="$M/candidate.env" \
	WD24X_DOOM_ENV="$M/doom.env" \
	/usr/bin/dash "$SCRIPT" >"$M/out" 2>"$M/err"
rc=$?
grep -q "S6D-GO7 stop demo stats" "$M/out" && ok || bad "nostats stop"
grep -q "S6D-GO7 GO7_RC=1" "$M/out" && ok || bad "nostats rc line"
grep -q "wd24adv" "$M/rmmod.args" && ok || bad "nostats rmmod"
[ "$rc" != 0 ] && ok || bad "nostats exit"

setup "$ROOT/modules/wd24adv/tests/.go7-pin"
M=$ROOT/modules/wd24adv/tests/.go7-pin
rm -rf "$M"
mkdir -p "$M"
printf '%s\n' 'WD24X_CANDIDATE_KO=/tmp/wd24adv-desktop.ko' \
	'WD24X_CANDIDATE_SHA=UNPINNED' > "$M/candidate.env"
printf '%s\n' 'WD24X_DOOM_BIN=/tmp/wd24x-doom' \
	'WD24X_DOOM_SHA=UNPINNED' > "$M/doom.env"
WD24X_CANDIDATE_ENV="$M/candidate.env" WD24X_DOOM_ENV="$M/doom.env" \
	/usr/bin/dash "$SCRIPT" >"$M/out" 2>"$M/err"
rc=$?
grep -q "S6D-GO7 stop candidate unpinned" "$M/out" && ok || bad "unpinned stop"
grep -q "S6D-GO7 GO7_RC=1" "$M/out" && ok || bad "unpinned rc line"
[ "$rc" != 0 ] && ok || bad "unpinned exit"

echo "test_go7_run: $pass passed, $fail failed"
[ "$fail" = 0 ]
