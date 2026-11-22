#!/bin/sh
# Host check of the browser fixtures and the framebuffer hash script.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
PAGES=$ROOT/tools/wd24-browser/pages
HASH=$ROOT/tools/wd24-browser/fbhash.sh
BUILD=$ROOT/tools/wd24-browser/build.sh
fail=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

for f in canvas.html bands.png quad.gif pixel.gif dot.jpg; do
	if [ -s "$PAGES/$f" ]; then ok; else bad "$f"; fi
done
head -c 8 "$PAGES/bands.png" | grep -q PNG && ok || bad "png"
head -c 6 "$PAGES/quad.gif" | grep -q GIF89a && ok || bad "gif"
jpg=$(od -An -tx1 -N3 "$PAGES/dot.jpg" | tr -d ' \n')
[ "$jpg" = "ffd8ff" ] && ok || bad "jpg"
grep -q 'wd24 canvas' "$PAGES/canvas.html" && ok || bad "html"
grep -q 'scroll marker' "$PAGES/canvas.html" && ok || bad "scroll"

M=$ROOT/modules/wd24adv/tests/.browser
rm -rf "$M"
mkdir -p "$M/dev" "$M/proc/wd24adv"
dd if=/dev/zero of="$M/dev/fb0" bs=307200 count=1 status=none
printf 'x' | dd of="$M/dev/fb0" bs=1 count=1 conv=notrunc status=none
printf '%s\n' 's6d: armed 1 active 1 stopping 0 busy 0 fb_reg 1 node 0' \
	'faults 0 switches 0 thrash 0 latched 0 mapped_base 0x0' \
	'accel 0 gate 0 match 0' \
	'present frames 0 last 0 total 0 ns 0 copy_ns 0 mode letter' \
	> "$M/proc/wd24adv/s6d"
printf '%s\n' 'wd24adv live: trial PENDING' > "$M/proc/wd24adv/live"
out=$(WD24X_MOCK=$M sh "$HASH")
echo "$out" | grep -q 'WD24X-BROWSER fb ' && ok || bad "hash"
echo "$out" | grep -q 'faults 0 switches 0' && ok || bad "faults"
echo "$out" | grep -q 'accel 0 gate 0 match 0' && ok || bad "accel"
echo "$out" | grep -q 'WD24X-BROWSER done' && ok || bad "done"
rm -rf "$M"
out=$(WD24X_MOCK=$M sh "$HASH" >"$ROOT/modules/wd24adv/tests/.browser-out" 2>&1) || rc=$?
rc=${rc:-0}
grep -q 'stop no fb' "$ROOT/modules/wd24adv/tests/.browser-out" && ok || bad "missing"
[ "$rc" != 0 ] && ok || bad "missing rc"
rc=0
WD24X_NS_CHECK=1 sh "$BUILD" >"$ROOT/modules/wd24adv/tests/.browser-build" 2>&1 || rc=$?
grep -q '170d89462442c286f80c0604f30e4c7f2a801265' "$ROOT/modules/wd24adv/tests/.browser-build" && ok || bad "pin"
grep -q 'a287525eeafa4b9e721822670e1e03eeeef6b31c' "$ROOT/modules/wd24adv/tests/.browser-build" && ok || bad "nsfb pin"
grep -q 'libnsfb-linux-fbdev.patch' "$ROOT/modules/wd24adv/tests/.browser-build" && ok || bad "patch"
grep -q FBIOPUTCMAP "$ROOT/tools/wd24-browser/patches/libnsfb-linux-fbdev.patch" && ok || bad "cmap"
grep -q '51d3ee85de73949483af79671fe9075a10e405e3' "$ROOT/tools/wd24-browser/patches/libnsfb-linux-fbdev.patch" && ok || bad "cite"
[ "$rc" = 0 ] && ok || bad "build rc"
rm -f "$ROOT/modules/wd24adv/tests/.browser-out" "$ROOT/modules/wd24adv/tests/.browser-build"

echo "test_browser_pages: $pass passed, $fail failed"
[ "$fail" = 0 ]
