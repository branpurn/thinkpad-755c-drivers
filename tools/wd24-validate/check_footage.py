#!/usr/bin/env python3
"""Compare captured fields to the sample manifest.

A row that the manifest marks solid must be one color. A missing field
number between the first and the last is a dropped field. A pixel that
is not the expected color is noise. Two colors on a solid row is a tear.
"""
import sys
from pathlib import Path


def load_ppm(path: Path):
    data = path.read_bytes()
    if not data.startswith(b"P6\n"):
        raise SystemExit(f"not a ppm {path}")
    nl = data.find(b"\n", 3)
    size = data[3:nl].split()
    w, h = int(size[0]), int(size[1])
    nl2 = data.find(b"\n", nl + 1)
    raw = data[nl2 + 1:]
    return w, h, raw


def pixel(raw, w, x, y):
    o = (y * w + x) * 3
    return raw[o], raw[o + 1], raw[o + 2]


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: check_footage.py samples.txt ppm_dir", file=sys.stderr)
        return 2
    samples = Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
    ppm_dir = Path(sys.argv[2])
    cache = {}
    fields = set()
    step = 1
    fail = 0
    torn = 0
    noise = 0
    print(f"{'field':<8} {'phase':<10} {'result':<6} evidence")
    for line in samples:
        if not line.strip():
            continue
        if line.startswith("STEP "):
            step = int(line.split()[1])
            continue
        n, x, y, r, g, b, solid, phase = line.split()
        n, x, y = int(n), int(x), int(y)
        r, g, b, solid = int(r), int(g), int(b), int(solid)
        fields.add(n)
        path = ppm_dir / f"f{n:05d}.ppm"
        if n not in cache:
            if not path.is_file():
                print(f"{n:<8} {phase:<10} FAIL   missing {path.name}")
                fail += 1
                continue
            cache[n] = load_ppm(path)
        w, h, raw = cache[n]
        got = pixel(raw, w, x, y)
        if got != (r, g, b):
            print(f"{n:<8} {phase:<10} FAIL   noise got {got} want {(r, g, b)}")
            noise += 1
            fail += 1
            continue
        if solid:
            colors = set()
            row = y * w * 3
            for col in range(w):
                o = row + col * 3
                colors.add((raw[o], raw[o + 1], raw[o + 2]))
            if len(colors) != 1:
                print(f"{n:<8} {phase:<10} FAIL   torn colors={len(colors)}")
                torn += 1
                fail += 1
                continue
        print(f"{n:<8} {phase:<10} PASS   {x},{y}")
    dropped = 0
    if fields:
        ordered = sorted(fields)
        expect = ordered[0]
        for n in ordered:
            if n != expect:
                print(f"{expect:<8} {'?':<10} FAIL   dropped")
                dropped += 1
                fail += 1
            expect = n + step
    print(f"FOOTAGE fields={len(fields)} noise={noise} torn={torn} dropped={dropped} fail={fail}")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
