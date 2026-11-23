#!/bin/sh
# docs/s6d-xfbdev-launch.sh. No X server is started.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
SCRIPT=$ROOT/docs/s6d-xfbdev-launch.sh
fail=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

M=$ROOT/modules/wd24adv/tests/.xlaunch
rm -rf "$M"
mkdir -p "$M/tmp/wd24-xfbdev"
printf '%s\n' '#!/bin/sh' 'echo ran "$@"' > "$M/tmp/wd24-xfbdev/Xfbdev"
chmod 755 "$M/tmp/wd24-xfbdev/Xfbdev"

WD24X_MOCK=$M dash "$SCRIPT" >"$M/out"
rc=$?
grep -q "WD24X-XFBDEV-LAUNCH /tmp/wd24-xfbdev/Xfbdev :1" "$M/out" && ok || bad "line"
grep -q "modesetting" "$M/out" && bad "modesetting" || ok
[ "$rc" = 0 ] && ok || bad "rc"

rm -rf "$M/tmp"
rc=$(WD24X_MOCK=$M dash "$SCRIPT" >/dev/null; echo $?)
[ "$rc" != 0 ] && ok || bad "missing"

echo "test_xfbdev_launch: $pass passed, $fail failed"
[ "$fail" = 0 ]
