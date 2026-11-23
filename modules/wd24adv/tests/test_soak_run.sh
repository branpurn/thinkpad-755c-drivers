#!/bin/sh
# docs/s6d-soak-run.sh. The hour is not started.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
SCRIPT=$ROOT/docs/s6d-soak-run.sh
fail=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

M=$ROOT/modules/wd24adv/tests/.soak
rm -rf "$M"
mkdir -p "$M"

dash "$SCRIPT" >"$M/plan"
grep -q "WD24X-SOAK plan only" "$M/plan" && ok || bad "plan"
grep -q "timedemo13hmap" "$M/plan" && ok || bad "doom"
grep -q "s6d-fbcon-run.sh" "$M/plan" && ok || bad "fbcon"

printf '%s\n' 'one true' 'two false' > "$M/cmds"
WD24X_SOAK_RUN=1 WD24X_SOAK_CMDS=$M/cmds WD24X_SOAK_LOG=$M/log \
	dash "$SCRIPT" >"$M/out"
rc=$?
grep -q "stop two" "$M/out" && ok || bad "stop"
grep -q "WD24X-SOAK ok one" "$M/log" && ok || bad "ok one"
grep -q "WD24X-SOAK done" "$M/out" && bad "done" || ok
[ "$rc" != 0 ] && ok || bad "rc"

echo "test_soak_run: $pass passed, $fail failed"
[ "$fail" = 0 ]
