#!/bin/sh
# Mock gates for phase 2, phase 3, and the unarmed batch. No /dev/fb0.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
P2=$ROOT/docs/s6d-cmap-p2-run.sh
P3=$ROOT/docs/s6d-cmap-p3-run.sh
BATCH=$ROOT/docs/s6d-cmap-batch-run.sh
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
	printf '%s\n' 'wd24adv live: trial expiry -> 0' \
		'port writes: data 1, index 1; readback mismatches 0; in_text 1; trial -' \
		> "$M/proc/wd24adv/live"
	printf '%s\n' 's6d: armed 0 active 0 stopping 0 busy 0 fb_reg 0 node -1' \
		'fbcon_param 1 bind_param 0' \
		'faults 4 switches 4 thrash 0 latched 0 mapped_base 0xffffffff' \
		'why -' \
		'cmap_param 1 published 1 frozen 1' > "$M/proc/wd24adv/s6d"
	cat > "$M/bin/sha256sum" <<'EOF'
#!/bin/sh
b=$(basename "$1")
case "$b" in
wd24x-cmap-get) h=159c93efff1142ebf31632c7f1ccf1f403cfc53019214c414b9fe048ce3f2857 ;;
wd24x-cmap-set) h=1a26c1c92e6c36d50175cbfce164506482495d3a1f2bc5a81192f5d1044a8c1f ;;
wd24adv-cmap-92591b1e.ko) h=92591b1e26c3888868285a4a50d602cf969dfa9bdd53c9f868f21fef03d6a8ae ;;
wd24adv-pr14.ko) h=7da0fffe75883c1c28e876e0dc660f946a2e8f4d9dc9443ebc0810ac71bb7e36 ;;
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
if [ "${1:-}" = 5 ] || [ "${1:-}" = 15 ]; then /bin/sleep 0.2; fi
exit 0
EOF
	cat > "$M/tmp/wd24x-cmap-get" <<'EOF'
#!/bin/sh
echo "WD24X-CMAP-GET entries 256 shift10 1" > "$WD24X_MOCK/tmp/wd24x-cmap-p2.out"
exit 0
EOF
	cat > "$M/tmp/wd24x-cmap-set" <<'EOF'
#!/bin/sh
echo "WD24X-CMAP-SET index 7 put 1 cache 1" > "$WD24X_MOCK/tmp/wd24x-cmap-p3.out"
exit 0
EOF
	chmod 755 "$M/bin/"* "$M/tmp/wd24x-cmap-get" "$M/tmp/wd24x-cmap-set"
	echo x > "$M/tmp/wd24adv-cmap-92591b1e.ko"
}

run() {
	M=$1
	S=$2
	WD24X_MOCK=$M PATH="$M/bin:/bin:/usr/bin" dash "$S" >"$M/out" 2>"$M/err"
	echo $?
}

setup "$ROOT/modules/wd24adv/tests/.c23-ok2"
rc=$(run "$ROOT/modules/wd24adv/tests/.c23-ok2" "$P2")
M=$ROOT/modules/wd24adv/tests/.c23-ok2
grep -q "WD24X-CMAP-P2-RUN done" "$M/out" && ok || bad "p2 done"
[ "$rc" = 0 ] && ok || bad "p2 rc"
grep -q "s6d_cmap=1" "$M/insmod.args" && ok || bad "p2 cmap arg"
grep -q "wd24adv-cmap-92591b1e.ko" "$M/insmod.args" && ok || bad "p2 ko"
grep -q "wd24adv-pr14.ko" "$M/insmod.args" && bad "p2 baseline" || ok
[ -f "$M/tmp/wd24x-cmap-p2.out" ] && ok || bad "p2 log"
[ -f "$M/tmp/wd24adv-cmap-92591b1e.ko" ] && ok || bad "p2 ko kept"

setup "$ROOT/modules/wd24adv/tests/.c23-hash"
M=$ROOT/modules/wd24adv/tests/.c23-hash
echo nope > "$M/tmp/wd24adv-cmap-92591b1e.ko"
cat > "$M/bin/sha256sum" <<'EOF'
#!/bin/sh
exec /usr/bin/sha256sum "$@"
EOF
chmod 755 "$M/bin/sha256sum"
rc=$(run "$M" "$P2")
grep -q "stop module hash" "$M/out" && ok || bad "hash"
[ ! -f "$M/rmmod.args" ] && ok || bad "hash rmmod"
[ ! -f "$M/insmod.args" ] && ok || bad "hash insmod"

setup "$ROOT/modules/wd24adv/tests/.c23-pre"
printf '%s\n' 'wd24adv 1 - Live' >> "$ROOT/modules/wd24adv/tests/.c23-pre/proc/modules"
rc=$(run "$ROOT/modules/wd24adv/tests/.c23-pre" "$P2")
M=$ROOT/modules/wd24adv/tests/.c23-pre
grep -q "stop wd24adv loaded" "$M/out" && ok || bad "pre"
[ ! -f "$M/rmmod.args" ] && ok || bad "pre rmmod"

setup "$ROOT/modules/wd24adv/tests/.c23-race"
cat > "$ROOT/modules/wd24adv/tests/.c23-race/bin/insmod" <<'EOF'
#!/bin/sh
echo "$*" >> "$WD24X_MOCK/insmod.args"
printf '%s\n' 'wd24adv 9 - Live' >> "$WD24X_MOCK/proc/modules"
exit 1
EOF
chmod 755 "$ROOT/modules/wd24adv/tests/.c23-race/bin/insmod"
rc=$(run "$ROOT/modules/wd24adv/tests/.c23-race" "$P2")
M=$ROOT/modules/wd24adv/tests/.c23-race
grep -q "stop insmod" "$M/out" && ok || bad "race"
[ ! -f "$M/rmmod.args" ] && ok || bad "race rmmod"
grep -q '^wd24adv ' "$M/proc/modules" && ok || bad "race kept"

setup "$ROOT/modules/wd24adv/tests/.c23-rm"
cat > "$ROOT/modules/wd24adv/tests/.c23-rm/bin/rmmod" <<'EOF'
#!/bin/sh
echo "$*" >> "$WD24X_MOCK/rmmod.args"
exit 1
EOF
chmod 755 "$ROOT/modules/wd24adv/tests/.c23-rm/bin/rmmod"
rc=$(run "$ROOT/modules/wd24adv/tests/.c23-rm" "$P2")
M=$ROOT/modules/wd24adv/tests/.c23-rm
grep -q "^RM=1" "$M/out" && ok || bad "rm status"
grep -q "WD24X-CMAP-P2-RUN done" "$M/out" && bad "rm done" || ok
[ "$rc" != 0 ] && ok || bad "rm rc"

setup "$ROOT/modules/wd24adv/tests/.c23-pub"
printf '%s\n' 'cmap_param 0 published 0 frozen 0' > "$ROOT/modules/wd24adv/tests/.c23-pub/proc/wd24adv/s6d"
rc=$(run "$ROOT/modules/wd24adv/tests/.c23-pub" "$P2")
M=$ROOT/modules/wd24adv/tests/.c23-pub
grep -q "stop cmap publish" "$M/out" && ok || bad "publish"
grep -q "done" "$M/out" && bad "publish done" || ok
[ -f "$M/rmmod.args" ] && ok || bad "publish owned rmmod"

setup "$ROOT/modules/wd24adv/tests/.c23-time"
cat > "$ROOT/modules/wd24adv/tests/.c23-time/tmp/wd24x-cmap-get" <<'EOF'
#!/bin/sh
exec /bin/sleep 30
EOF
chmod 755 "$ROOT/modules/wd24adv/tests/.c23-time/tmp/wd24x-cmap-get"
rc=$(run "$ROOT/modules/wd24adv/tests/.c23-time" "$P2")
M=$ROOT/modules/wd24adv/tests/.c23-time
grep -q "stop client timeout" "$M/out" && ok || bad "timeout"
grep -q "done" "$M/out" && bad "timeout done" || ok
[ -f "$M/rmmod.args" ] && ok || bad "timeout rmmod"

setup "$ROOT/modules/wd24adv/tests/.c23-pre"
printf '%s\n' 'wd24adv 1 - Live' >> "$ROOT/modules/wd24adv/tests/.c23-pre/proc/modules"
rc=$(run "$ROOT/modules/wd24adv/tests/.c23-pre" "$P2")
M=$ROOT/modules/wd24adv/tests/.c23-pre
grep -q "stop wd24adv loaded" "$M/out" && ok || bad "pre"
[ ! -f "$M/rmmod.args" ] && ok || bad "pre rmmod"

setup "$ROOT/modules/wd24adv/tests/.c23-race"
cat > "$ROOT/modules/wd24adv/tests/.c23-race/bin/insmod" <<'EOF'
#!/bin/sh
echo "$*" >> "$WD24X_MOCK/insmod.args"
printf '%s\n' 'wd24adv 9 - Live' >> "$WD24X_MOCK/proc/modules"
exit 1
EOF
chmod 755 "$ROOT/modules/wd24adv/tests/.c23-race/bin/insmod"
rc=$(run "$ROOT/modules/wd24adv/tests/.c23-race" "$P2")
M=$ROOT/modules/wd24adv/tests/.c23-race
grep -q "stop insmod" "$M/out" && ok || bad "race"
[ ! -f "$M/rmmod.args" ] && ok || bad "race rmmod"

setup "$ROOT/modules/wd24adv/tests/.c23-pub"
printf '%s\n' 'cmap_param 0 published 0 frozen 0' > "$ROOT/modules/wd24adv/tests/.c23-pub/proc/wd24adv/s6d"
rc=$(run "$ROOT/modules/wd24adv/tests/.c23-pub" "$P2")
M=$ROOT/modules/wd24adv/tests/.c23-pub
grep -q "stop cmap publish" "$M/out" && ok || bad "publish"
grep -q "done" "$M/out" && bad "publish done" || ok
[ -f "$M/rmmod.args" ] && ok || bad "publish rmmod"

setup "$ROOT/modules/wd24adv/tests/.c23-time"
cat > "$ROOT/modules/wd24adv/tests/.c23-time/tmp/wd24x-cmap-get" <<'EOF'
#!/bin/sh
exec /bin/sleep 30
EOF
chmod 755 "$ROOT/modules/wd24adv/tests/.c23-time/tmp/wd24x-cmap-get"
rc=$(run "$ROOT/modules/wd24adv/tests/.c23-time" "$P2")
M=$ROOT/modules/wd24adv/tests/.c23-time
grep -q "stop client timeout" "$M/out" && ok || bad "timeout"
grep -q "done" "$M/out" && bad "timeout done" || ok
[ -f "$M/rmmod.args" ] && ok || bad "timeout rmmod"

setup "$ROOT/modules/wd24adv/tests/.c23-ok3"
rc=$(run "$ROOT/modules/wd24adv/tests/.c23-ok3" "$P3")
M=$ROOT/modules/wd24adv/tests/.c23-ok3
grep -q "WD24X-CMAP-P3-RUN done" "$M/out" && ok || bad "p3 done"
[ "$rc" = 0 ] && ok || bad "p3 rc"
grep -q "s6d_cmap=1" "$M/insmod.args" && ok || bad "p3 cmap"

setup "$ROOT/modules/wd24adv/tests/.c23-exp"
printf '%s\n' 'wd24adv live: trial expiry -> 4' \
	'port writes: data 1, index 1; readback mismatches 2; in_text 0; trial -' \
	> "$ROOT/modules/wd24adv/tests/.c23-exp/proc/wd24adv/live"
rc=$(run "$ROOT/modules/wd24adv/tests/.c23-exp" "$P3")
M=$ROOT/modules/wd24adv/tests/.c23-exp
grep -q "stop expiry" "$M/out" && ok || bad "expiry"
grep -q "done" "$M/out" && bad "expiry done" || ok
[ -f "$M/rmmod.args" ] && ok || bad "expiry rmmod"

setup "$ROOT/modules/wd24adv/tests/.c23-p10"
printf '%s\n' 'cmap_param 1 published 10 frozen 0' > "$ROOT/modules/wd24adv/tests/.c23-p10/proc/wd24adv/s6d"
rc=$(run "$ROOT/modules/wd24adv/tests/.c23-p10" "$P2")
M=$ROOT/modules/wd24adv/tests/.c23-p10
grep -q "stop cmap publish" "$M/out" && ok || bad "published 10"
grep -q "done" "$M/out" && bad "published 10 done" || ok

setup "$ROOT/modules/wd24adv/tests/.c23-z0"
printf '%s\n' 's6d: armed 0 active 0 stopping 0 busy 0 fb_reg 0 node -1' \
	'fbcon_param 1 bind_param 0' \
	'faults 4 switches 4 thrash 0 latched 0 mapped_base 0xffffffff' \
	'why -' \
	'cmap_param 1 published 1 frozen 0' > "$ROOT/modules/wd24adv/tests/.c23-z0/proc/wd24adv/s6d"
rc=$(run "$ROOT/modules/wd24adv/tests/.c23-z0" "$P2")
M=$ROOT/modules/wd24adv/tests/.c23-z0
grep -q "WD24X-CMAP-P2-RUN done" "$M/out" && ok || bad "frozen 0 done"
[ "$rc" = 0 ] && ok || bad "frozen 0 rc"

setup "$ROOT/modules/wd24adv/tests/.c23-e01"
printf '%s\n' 'wd24adv live: trial expiry -> 01' \
	'port writes: data 1, index 1; readback mismatches 0; in_text 1; trial -' \
	> "$ROOT/modules/wd24adv/tests/.c23-e01/proc/wd24adv/live"
rc=$(run "$ROOT/modules/wd24adv/tests/.c23-e01" "$P3")
M=$ROOT/modules/wd24adv/tests/.c23-e01
grep -q "stop expiry" "$M/out" && ok || bad "expiry 01"
grep -q "done" "$M/out" && bad "expiry 01 done" || ok

# batch
STAMP=$ROOT/modules/wd24adv/tests/.c23-stamp
rm -rf "$STAMP"
mkdir -p "$STAMP"
cat > "$STAMP/p2" <<'EOF'
#!/bin/sh
echo p2 >> "$WD24X_STAMP"
exit 0
EOF
cat > "$STAMP/p2bad" <<'EOF'
#!/bin/sh
echo p2 >> "$WD24X_STAMP"
exit 1
EOF
cat > "$STAMP/p3" <<'EOF'
#!/bin/sh
echo p3 >> "$WD24X_STAMP"
exit 0
EOF
chmod 755 "$STAMP/p2" "$STAMP/p2bad" "$STAMP/p3"
WD24X_STAMP=$STAMP/ran WD24X_P2_SCRIPT=$STAMP/p2 WD24X_P3_SCRIPT=$STAMP/p3 \
	dash "$BATCH" >"$STAMP/un.out" 2>"$STAMP/un.err"
rc=$?
grep -q "stop not armed" "$STAMP/un.out" && ok || bad "batch unarmed"
[ "$rc" != 0 ] && ok || bad "batch unarmed rc"
[ ! -f "$STAMP/ran" ] && ok || bad "batch unarmed ran"
WD24X_BATCH_ARM=phase2-and-phase3 WD24X_STAMP=$STAMP/ran2 \
	WD24X_P2_SCRIPT=$STAMP/p2bad WD24X_P3_SCRIPT=$STAMP/p3 \
	dash "$BATCH" >"$STAMP/bad.out"
rc=$?
grep -q "stop phase 2" "$STAMP/bad.out" && ok || bad "batch p2 stop"
grep -q "phase 3" "$STAMP/bad.out" && bad "batch p3 after fail" || ok
[ "$rc" != 0 ] && ok || bad "batch p2 rc"
WD24X_BATCH_ARM=phase2-and-phase3 WD24X_STAMP=$STAMP/ran3 \
	WD24X_P2_SCRIPT=$STAMP/p2 WD24X_P3_SCRIPT=$STAMP/p3 \
	dash "$BATCH" >"$STAMP/ok.out"
rc=$?
grep -q "WD24X-CMAP-BATCH done" "$STAMP/ok.out" && ok || bad "batch done"
[ "$rc" = 0 ] && ok || bad "batch rc"
grep -q p3 "$STAMP/ran3" && ok || bad "batch p3"

rm -rf "$ROOT/modules/wd24adv/tests/.c23-ok2" \
	"$ROOT/modules/wd24adv/tests/.c23-hash" \
	"$ROOT/modules/wd24adv/tests/.c23-pre" \
	"$ROOT/modules/wd24adv/tests/.c23-race" \
	"$ROOT/modules/wd24adv/tests/.c23-rm" \
	"$ROOT/modules/wd24adv/tests/.c23-pub" \
	"$ROOT/modules/wd24adv/tests/.c23-time" \
	"$ROOT/modules/wd24adv/tests/.c23-ok3" \
	"$ROOT/modules/wd24adv/tests/.c23-exp" \
	"$ROOT/modules/wd24adv/tests/.c23-p10" \
	"$ROOT/modules/wd24adv/tests/.c23-z0" \
	"$ROOT/modules/wd24adv/tests/.c23-e01" \
	"$STAMP"
echo "test_cmap_p23_run: $pass passed, $fail failed"
[ "$fail" = 0 ]
