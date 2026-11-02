#!/usr/bin/env python3
"""Decode `vromdump-save.sh -serial` output from a serial log into files.
usage: serial2bin.py LOG OUTDIR   -> OUTDIR/{info.txt,vrom.bin,sysbios.bin,lowmem.bin}
Checks each file's length and sha256 against the BEGIN line."""
import hashlib, os, re, sys

log, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
text = open(log, "rb").read().decode("latin-1").replace("\r", "")
ok = True
m = re.search(r"### vromdump BEGIN info\n(.*?)### vromdump END info", text, re.S)
if m:
    open(os.path.join(out, "info.txt"), "w").write(m.group(1))
for m in re.finditer(r"### vromdump BEGIN (\w+) (\d+) ([0-9a-f]{64})\n(.*?)### vromdump END \1", text, re.S):
    name, size, sha, body = m.group(1), int(m.group(2)), m.group(3), m.group(4)
    data = bytes.fromhex("".join(l.strip() for l in body.splitlines() if re.fullmatch(r"[0-9a-f]+", l.strip())))[:size]
    good = len(data) == size and hashlib.sha256(data).hexdigest() == sha
    ok &= good
    open(os.path.join(out, name + ".bin"), "wb").write(data)
    print(f"{name}.bin: {len(data)} bytes, sha256 {'OK' if good else 'MISMATCH (serial noise? re-run)'}")
sys.exit(0 if ok else 1)
