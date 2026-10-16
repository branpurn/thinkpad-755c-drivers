#!/bin/sh
# Usage: tests/run.sh [MEM_MB ...]   (default: 32 16)
set -u
TOP=$(cd "$(dirname "$0")/.." && pwd); cd "$TOP"
for f in out/tc755.img out/tcz-disk.img; do [ -f "$f" ] || { echo "$f missing: run 'make'"; exit 2; }; done
command -v qemu-system-i386 >/dev/null || { echo "qemu-system-i386 not found"; exit 2; }
mkdir -p out/test-logs; : > out/test-logs/summary.txt
rc=0
for m in ${*:-32 16}; do
    python3 tests/harness.py --mem "$m" | tee "out/test-logs/console-m$m.txt"
    [ "$(tail -1 "out/test-logs/console-m$m.txt" | grep -c ': PASS')" = 1 ] || rc=1
    tail -1 "out/test-logs/console-m$m.txt" >> out/test-logs/summary.txt
done
echo; echo "===== tc755 test summary ====="; cat out/test-logs/summary.txt
exit $rc
