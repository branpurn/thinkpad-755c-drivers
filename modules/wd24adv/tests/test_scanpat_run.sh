#!/bin/sh
# Drives docs/s6d-scanpat-run.sh through WD24X_MOCK. No hardware.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
SCRIPT=$ROOT/docs/s6d-scanpat-run.sh
fail=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

M=$ROOT/modules/wd24adv/tests/.scanpat
rm -rf "$M"
mkdir -p "$M/bin" "$M/proc/wd24adv" "$M/sys/class/vtconsole/vtcon0" "$M/tmp"
printf '%s\n' '(S) VGA+' > "$M/sys/class/vtconsole/vtcon0/name"
printf '%s\n' 1 > "$M/sys/class/vtconsole/vtcon0/bind"
printf '%s\n' 'font 1 - Live' > "$M/proc/modules"
printf '%s\n' 'wd24adv live: mode 320x200-8 -> 0' > "$M/proc/wd24adv/live"
cat > "$M/bin/insmod" <<'EOF'
#!/bin/sh
echo "$*" >> "$WD24X_MOCK/insmod.args"
printf '%s\n' 'wd24adv 1 - Live' >> "$WD24X_MOCK/proc/modules"
printf '%s\n' 'scan mode pr_locked=1 cr08=00 cr09=41 cr0c=00 cr0d=00 dw_bytes=0' \
	>> "$WD24X_MOCK/proc/wd24adv/live"
exit 0
EOF
cat > "$M/bin/rmmod" <<'EOF'
#!/bin/sh
echo "$*" >> "$WD24X_MOCK/rmmod.args"
exit 0
EOF
cat > "$M/tmp/wd24x-scanpat" <<'EOF'
#!/bin/sh
echo "WD24X-SCANPAT red row 0 cols 8-23"
echo "WD24X-SCANPAT green row 0 cols 304-319"
exit 0
EOF
chmod 755 "$M/bin/insmod" "$M/bin/rmmod" "$M/tmp/wd24x-scanpat"

run() {
	WD24X_MOCK=$M WD24X_CANDIDATE_KO=/tmp/wd24adv-desktop.ko \
		WD24X_SCANPAT=/tmp/wd24x-scanpat \
		PATH="$M/bin:/bin:/usr/bin" dash "$SCRIPT" >"$M/out" 2>"$M/err"
	echo $?
}

rc=$(run)
grep -q "WD24X-SCANPAT-RUN done" "$M/out" && ok || bad "done"
grep -q "WD24X-SCANPAT-RUN start 0" "$M/out" && ok || bad "start"
grep -q "mode=320x200-8" "$M/insmod.args" && ok || bad "mode"
grep -q "s6d_bind=0" "$M/insmod.args" && ok || bad "bind"
grep -q "pattern=0" "$M/insmod.args" && ok || bad "pattern"
grep -q "present_mode=13h" "$M/insmod.args" && ok || bad "present"
grep -q "wd24adv" "$M/rmmod.args" && ok || bad "rmmod"
[ "$rc" = 0 ] && ok || bad "rc"

rm -f "$M/tmp/wd24x-scanpat"
rc=$(run)
grep -q "stop program" "$M/out" && ok || bad "missing"
[ "$rc" != 0 ] && ok || bad "missing rc"

printf '%s\n' 'font 1 - Live' > "$M/proc/modules"
: > "$M/proc/wd24adv/live"
cat > "$M/bin/insmod" <<'EOF'
#!/bin/sh
echo "$*" >> "$WD24X_MOCK/insmod.args"
printf '%s\n' 'wd24adv 1 - Live' >> "$WD24X_MOCK/proc/modules"
printf '%s\n' 'scan mode pr_locked=1 cr08=00 cr09=41 cr0c=23 cr0d=50 dw_bytes=36160' \
	>> "$WD24X_MOCK/proc/wd24adv/live"
exit 0
EOF
chmod 755 "$M/bin/insmod"
cat > "$M/tmp/wd24x-scanpat" <<'EOF'
#!/bin/sh
echo "WD24X-SCANPAT red row 0 cols 8-23"
exit 0
EOF
chmod 755 "$M/tmp/wd24x-scanpat"
rc=$(run)
grep -q "WD24X-SCANPAT-RUN start nonzero" "$M/out" && ok || bad "nonzero text"
grep -q "WD24X-SCANPAT-RUN done" "$M/out" && bad "nonzero done" || ok
[ "$rc" != 0 ] && ok || bad "nonzero rc"

echo "test_scanpat_run: $pass passed, $fail failed"
[ "$fail" = 0 ]
