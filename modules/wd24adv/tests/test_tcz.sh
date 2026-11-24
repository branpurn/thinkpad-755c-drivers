#!/bin/sh
# scripts/wd24adv-tcz.sh layout. No real module.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
SCRIPT=$ROOT/scripts/wd24adv-tcz.sh
fail=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

M=$ROOT/modules/wd24adv/tests/.tcz
rm -rf "$M"
mkdir -p "$M/bin" "$M/out"
printf '%s\n' ko > "$M/wd24adv.ko"
cat > "$M/bin/modinfo" << 'EOF'
#!/bin/sh
echo "6.18.57-tc755x mod_unload 486"
EOF
chmod 755 "$M/bin/modinfo"
PATH="$M/bin:$PATH" WD24X_MODINFO=$M/bin/modinfo \
	dash "$SCRIPT" "$M/wd24adv.ko" "$M/out" >"$M/log"
rc=$?
grep -q "/usr/local/lib/modules/6.18.57-tc755x/kernel/drivers/video/fbdev/wd24adv.ko" \
	"$M/out/wd24adv.tcz.list" && ok || bad "list"
grep -q "modutils.tcz" "$M/out/wd24adv.tcz.dep" && ok || bad "dep"
grep -q "dryrun=1" "$M/out/stage/usr/local/tce.installed/wd24adv" && ok || bad "startup"
if [ "$rc" = 0 ]; then
	[ -f "$M/out/wd24adv.tcz" ] && ok || bad "tcz"
	[ -f "$M/out/wd24adv.tcz.md5.txt" ] && ok || bad "md5"
elif [ "$rc" = 2 ]; then
	grep -q "staged no squashfs" "$M/log" && ok || bad "staged"
else
	bad "rc $rc"
fi

cat > "$M/bin/modinfo" << 'EOF'
#!/bin/sh
echo "6.18.57 mod_unload 486"
EOF
PATH="$M/bin:$PATH" WD24X_MODINFO=$M/bin/modinfo \
	dash "$SCRIPT" "$M/wd24adv.ko" "$M/bad" >"$M/badlog"
rc=$?
grep -q "stop vermagic" "$M/badlog" && ok || bad "vermagic"
[ "$rc" != 0 ] && ok || bad "vermagic rc"

echo "test_tcz: $pass passed, $fail failed"
[ "$fail" = 0 ]
