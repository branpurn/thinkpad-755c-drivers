#!/bin/sh
# Model log, footage samples, probe plan, and the ash/dash script.
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
VAL=$ROOT/tools/wd24-validate
BIN=${WD24X_VALIDATE_BIN:-/tmp/wd24-run}
fail=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

work=$(mktemp -d)
mkdir -p "$work/ppm"
log=$work/log.txt
if WD24X_VALIDATE_MODEL=1 WD24X_VALIDATE_BIN="$BIN" \
	WD24X_VALIDATE_PPM="$work/ppm" \
	WD24X_VALIDATE_SAMPLES="$work/samples.txt" \
	dash "$VAL/validate.sh" > "$log"; then
	ok
else
	bad "model run"
fi
if grep -q 'VAL SUMMARY pass=9 fail=0' "$log"; then ok; else bad "summary"; fi
if grep -q 'fps=70.086' "$log"; then ok; else bad "field rate"; fi
if python3 "$VAL/check_log.py" "$log" > "$work/table.txt"; then ok; else bad "check_log"; fi
if grep -q 'page2' "$work/table.txt"; then ok; else bad "table row"; fi
if python3 "$VAL/check_footage.py" "$work/samples.txt" "$work/ppm" > "$work/foot.txt"; then
	ok
else
	bad "footage"
fi
if grep -q 'fail=0' "$work/foot.txt"; then ok; else bad "footage fail"; fi

# A torn row must not pass.
torn=$(awk '/^1 / {print $1; exit}' "$work/samples.txt")
if [ -n "$torn" ]; then
	src="$work/ppm/f$(printf '%05d' "$torn").ppm"
	cp "$src" "$work/torn.ppm"
	python3 - "$work/torn.ppm" << 'PY'
import sys
p = sys.argv[1]
b = open(p, "rb").read()
i = b.rfind(b"\n")
raw = bytearray(b[i+1:])
for col in range(0, 320):
    o = (48 * 640 + col) * 3
    raw[o] = 0
    raw[o+1] = 255
    raw[o+2] = 0
open(p, "wb").write(b[:i+1] + raw)
PY
	mkdir -p "$work/badppm"
	cp "$work/torn.ppm" "$work/badppm/f$(printf '%05d' "$torn").ppm"
	printf 'STEP 1\n%s\n' "$(grep "^$torn " "$work/samples.txt" | head -n 1)" > "$work/bad.txt"
	if python3 "$VAL/check_footage.py" "$work/bad.txt" "$work/badppm" > "$work/bad.out"; then
		bad "tear accepted"
	else
		ok
	fi
	if grep -q torn "$work/bad.out"; then ok; else bad "tear label"; fi
else
	bad "no sample"
fi

cc -O2 -std=gnu11 -o "$work/probe" "$VAL/wd24_probe.c"
if WD24X_PROBE_DRY=1 WD24X_PROBE_BIN="$work/probe" dash "$VAL/probe.sh" | grep -q 'index 3d4 only'; then
	ok
else
	bad "probe dry"
fi
long=0
while IFS= read -r line; do
	n=$(printf '%s' "$line" | wc -c)
	if [ "$n" -gt 480 ]; then
		long=1
	fi
done < "$VAL/probe.sh"
if [ "$long" = 0 ]; then ok; else bad "probe line length"; fi

if grep -n 'pout(0x' "$VAL/wd24_probe.c" | grep -v '0x3d4'; then
	bad "probe data write"
else
	ok
fi

if command -v busybox >/dev/null 2>&1; then
	if WD24X_VALIDATE_MODEL=1 WD24X_VALIDATE_BIN="$BIN" \
		busybox ash "$VAL/validate.sh" | grep -q 'fail=0'; then
		ok
	else
		bad "ash"
	fi
else
	ok
fi

rm -rf "$work"
echo "test_validate: $pass passed, $fail failed"
[ "$fail" = 0 ]
