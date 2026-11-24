#!/usr/bin/env python3
"""Parse VAL lines and print a verdict table. Exit 1 on any FAIL."""
import sys


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else None
    lines = open(path, encoding="utf-8").read().splitlines() if path else sys.stdin.read().splitlines()
    rows = []
    summary = None
    skipped = 0
    for line in lines:
        if not line.startswith("VAL "):
            continue
        parts = line.split(None, 3)
        if len(parts) < 3:
            continue
        if parts[1] == "SUMMARY":
            summary = line
            continue
        ev = parts[3] if len(parts) > 3 else ""
        rows.append((parts[1], parts[2], ev))
    print(f"{'check':<12} {'result':<6} evidence")
    fail = 0
    for name, result, ev in rows:
        print(f"{name:<12} {result:<6} {ev}")
        if result == "SKIP":
            skipped += 1
        elif result != "PASS":
            fail += 1
    if summary:
        print(summary)
    else:
        print(f"VAL SUMMARY pass={len(rows) - fail - skipped} fail={fail} skip={skipped}")
        fail += 1
    if skipped:
        print(f"SKIP {skipped}")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
