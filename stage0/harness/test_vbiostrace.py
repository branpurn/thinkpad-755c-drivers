#!/usr/bin/env python3
"""Self-test of vbiostrace on a freely available VGA BIOS (SeaVGABIOS,
LGPLv3, Debian package 'seabios': /usr/share/seabios/vgabios-isavga.bin).
Checks that the traced register values for modes 03h/12h/13h are the
standard IBM VGA tables, and that the --lowmem path (INT 10h entered via a
dumped IVT, as for a BIOS-integrated video ROM) gives the same trace.
  python3 test_vbiostrace.py [ROM]"""
import os, subprocess, sys, tempfile, json

HERE = os.path.dirname(os.path.abspath(__file__))
ROM = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/seabios/vgabios-isavga.bin"

# IBM VGA standard values (VGA BIOS parameter table) for the regs we check
STD = {
    0x03: {"MISC": 0x67, "CR": [0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F], "SR": [0x03, 0x00, 0x03, 0x00, 0x02]},
    0x12: {"MISC": 0xE3, "CR": [0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0x00, 0x40], "SR": [0x03, 0x01, 0x0F, 0x00, 0x06]},
    0x13: {"MISC": 0x63, "CR": [0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41], "SR": [0x03, 0x01, 0x0F, 0x00, 0x0E]},
}
fails = 0


def check(desc, ok, detail=""):
    global fails
    fails += not ok
    print(f"  [{'PASS' if ok else 'FAIL'}] {desc}" + (f"  ({detail})" if detail and not ok else ""))


def first(c, kind, n):
    """value of the first write to kind00..n-1 in a call (the mode table itself;
    the BIOS may change e.g. SR04 again afterwards for the font load)."""
    seen = {}
    for e in c["events"]:
        r = e.get("reg") or ""
        if e["op"] == "out" and r.startswith(kind) and len(r) >= 4 and r[2:4].isalnum():
            i = int(r[2:4], 16)
            seen.setdefault(i, e["val"])
    return [seen.get(i) for i in range(n)]


def run(*args):
    with tempfile.TemporaryDirectory() as d:
        j = os.path.join(d, "t.json")
        p = subprocess.run([sys.executable, os.path.join(HERE, "vbiostrace.py"), *args, "-o", os.path.join(d, "t.txt"),
                            "--json", j], capture_output=True, text=True)
        return p.returncode, json.load(open(j)), open(os.path.join(d, "t.txt")).read()


if not os.path.exists(ROM):
    print(f"SKIP: {ROM} not found (apt install seabios)"); sys.exit(0)
print(f"== vbiostrace self-test on {ROM}")
rc, js, txt = run("--rom", ROM, "--calls", "0003,0012,0013")
check("POST + 3 mode sets return (no hang, no CPU exception)", rc == 0 and all(c["ok"] for c in js["calls"]))
calls = {c["label"]: c for c in js["calls"]}
for mode, std in STD.items():
    c = calls[f"INT 10h AX={mode:04X} BX=0000"]
    a = c["after"]
    check(f"mode {mode:02X}h: BDA 0449h = {mode:02X}h", c["bda_mode"] == mode, hex(c["bda_mode"]))
    check(f"mode {mode:02X}h: MISC = {std['MISC']:02X}h", a["MISC"] == std["MISC"], hex(a["MISC"]))
    check(f"mode {mode:02X}h: CR00-09 = IBM table", a["CR"][:10] == std["CR"], " ".join(f"{x:02X}" for x in a["CR"][:10]))
    sr = first(c, "SR", 5)
    check(f"mode {mode:02X}h: SR00-04 table write = IBM table", sr == std["SR"], str(sr))
    check(f"mode {mode:02X}h: palette loaded and VRAM cleared", any(e["reg"] == "DAC" for e in c["events"]) and c["vram_writes"] > 0)
    check(f"mode {mode:02X}h: every port write logged with CS:IP", all(":" in e["at"] for e in c["events"]))

# --lowmem path: capture IVT/BDA after POST, then enter INT 10h via that vector only
sys.path.insert(0, HERE)
import vbiostrace as vt
import argparse
a = argparse.Namespace(rom=ROM, sysbios=None, lowmem=None, real_ints="10,6d,42,1f,43", chip="wd90c24", vram=1024,
                       max_insns=20_000_000, reads=False, raw=False)
e = vt.Emu(a); e.post()
with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
    f.write(bytes(e.mu.mem_read(0, 0x500))); low = f.name
seg, off = e.vec(0x10)
check("POST hooked INT 10h into the ROM", seg == 0xC000, f"{seg:04X}:{off:04X}")
rc2, js2, _ = run("--rom", ROM, "--lowmem", low, "--no-post", "--calls", "0013")
rc1, js1, _ = run("--rom", ROM, "--calls", "0013")
os.unlink(low)
w1 = [(e["port"], e["val"]) for e in js1["calls"][1]["events"] if e["op"] == "out" and e["port"] != 0x402]
w2 = [(e["port"], e["val"]) for e in js2["calls"][0]["events"] if e["op"] == "out" and e["port"] != 0x402]
check("--lowmem/--no-post: mode 13h via dumped IVT gives the same port writes", rc2 == 0 and w1 == w2, f"{len(w1)} vs {len(w2)}")
rc3, js3, txt3 = run("--rom", ROM, "--calls", "0013", "--chip", "generic")
check("--chip generic also runs", rc3 == 0)
print(f"== {'PASS' if not fails else 'FAIL'}: {fails} failure(s)")
sys.exit(1 if fails else 0)
