#!/bin/sh
# Host checks for the read-only order tool and the unarmed key test.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
fail=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }
gcc -O2 -Wall -Wextra -Werror -std=gnu11 -o /tmp/wd24x-order \
	"$ROOT/modules/wd24adv/tests/wd24x-order.c"
FIX=/tmp/wd24-order-fix.txt
cat > "$FIX" <<'EOF'
wd24adv live: mode 640x480-8 -> 0
noise
S6d: session ON (fb open)
S6d: end (fb release): faults 15 switches 15 thrash 0
wd24adv live: trial expiry -> 0
Can't deallocate console 3 Resource busy
EOF
got=$(/tmp/wd24x-order "$FIX")
printf '%s\n' "$got" | grep -q '^WD24X-ORDER begin$' && ok || bad "begin"
printf '%s\n' "$got" | grep -q '^1 wd24adv live: mode 640x480-8 -> 0$' && ok || bad "mode"
printf '%s\n' "$got" | grep -q '^2 S6d: session ON (fb open)$' && ok || bad "session"
printf '%s\n' "$got" | grep -q '^5 Can'"'"'t deallocate console 3 Resource busy$' && ok || bad "dealloc"
printf '%s\n' "$got" | grep -q '^WD24X-ORDER count 5$' && ok || bad "count"
printf '%s\n' "$got" | grep -qi 'cause\|stripe\|palette' && bad "diagnosis" || ok

M=/tmp/wd24-tty-mock
rm -rf "$M"
mkdir -p "$M/tmp"
cat > "$M/tmp/wd24x-tty-read" <<'EOF'
#!/bin/sh
echo "WD24X-TTY-READ kd 0 fg 1 name (S) VGA+ bind 1"
EOF
chmod 755 "$M/tmp/wd24x-tty-read"
WD24X_MOCK=$M WD24X_TTY_BIN=$M/tmp/wd24x-tty-read \
	dash "$ROOT/docs/s6d-tty-accept.sh" >"$M/un.out"
rc=$?
grep -q "stop not armed" "$M/un.out" && ok || bad "unarmed"
[ "$rc" != 0 ] && ok || bad "unarmed rc"
[ ! -f "$M/tmp/wd24x-tty-before.out" ] && ok || bad "unarmed ran"
grep -v '^#' "$ROOT/docs/s6d-tty-accept.sh" | grep -q KDSETMODE && bad "kdset" || ok
grep -v '^#' "$ROOT/docs/s6d-tty-accept.sh" | grep -q VT_ACTIVATE && bad "activate" || ok
WD24X_TTY_ACCEPT=keystroke WD24X_MOCK=$M WD24X_TTY_BIN=$M/tmp/wd24x-tty-read \
	WD24X_TTY_KEY=x dash "$ROOT/docs/s6d-tty-accept.sh" >"$M/ok.out"
rc=$?
grep -q "WD24X-TTY-ACCEPT done" "$M/ok.out" && ok || bad "armed done"
grep -q "WD24X-TTY-ACCEPT key 1" "$M/ok.out" && ok || bad "key"
grep -q "kd 0 fg 1" "$M/ok.out" && ok || bad "snapshot"
[ "$rc" = 0 ] && ok || bad "armed rc"
cat > "$M/tmp/wd24x-tty-key" <<'EOF'
#!/bin/sh
echo "WD24X-TTY-KEY timeout"
exit 1
EOF
chmod 755 "$M/tmp/wd24x-tty-key"
# Timeout keeps the after snapshot and does not print done.
WD24X_TTY_ACCEPT=keystroke WD24X_MOCK=$M WD24X_TTY_BIN=$M/tmp/wd24x-tty-read \
	WD24X_TTY_KEYBIN=$M/tmp/wd24x-tty-key WD24X_TTY_KEY= \
	dash "$ROOT/docs/s6d-tty-accept.sh" >"$M/nk.out"
rc=$?
grep -q "stop no key" "$M/nk.out" && ok || bad "no key"
grep -q "done" "$M/nk.out" && bad "no key done" || ok
[ -f "$M/tmp/wd24x-tty-before.out" ] && ok || bad "timeout before kept"
[ -f "$M/tmp/wd24x-tty-after.out" ] && ok || bad "timeout after kept"
grep -v '^#' "$ROOT/docs/s6d-tty-accept.sh" | grep -q 'dd ' && bad "dd" || ok
rm -rf "$M" /tmp/wd24x-order "$FIX"
echo "test_tty_order: $pass passed, $fail failed"
[ "$fail" = 0 ]
