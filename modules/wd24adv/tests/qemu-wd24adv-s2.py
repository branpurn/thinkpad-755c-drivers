#!/usr/bin/env python3
"""QEMU system test of wd24adv S2 (s2=1): zero-write refusal + exact sequence.

Same guest and trace as qemu-wd24adv-s1.py.  Both modules are built here
from the working tree against --kdir (the QEMU tc755 kernel), under out/:

  * s2=1 (dry-run): /proc/wd24adv/s2 lists the plan, ZERO port accesses;
  * refusals: s2 with s1=1 / live=1 / mode=: zero accesses;
  * the real wd24adv.ko, s2=1 dryrun=0: the entry check refuses (QEMU is not
    a WD90C24): no VGA data write, ZERO 23Cxh accesses (no 23C2h write);
  * the TEST-ONLY fake build (registers preset to the S1 reference with
    SRC_LO = 2800h, a snapshot that is NOT the S1 reference): the
    exact 23Cxh access sequence (restated from wd24adv_s2.h), 34 23C2h
    writes, only to the closed list, ending with 23C0h = 1002h.

Logs: out/test-logs/qemu-wd24adv-s2-{serial,trace,sequence}.log
"""
import argparse, os, re, shutil, subprocess, sys, time

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import importlib.util
_spec = importlib.util.spec_from_file_location("qwdfb", os.path.join(TOP, "sdl/tests/qemu-wdfb.py"))
qwdfb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(qwdfb)
newc, Guest = qwdfb.newc, qwdfb.Guest
_spec2 = importlib.util.spec_from_file_location("qwd24", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                                      "qemu-wd24adv.py"))
qwd24 = importlib.util.module_from_spec(_spec2)
_spec2.loader.exec_module(qwd24)

RX_VGA = re.compile(r"vga_std_(read|write)_io addr 0x([0-9a-f]+), val 0x([0-9a-f]+)")
RX_IO = re.compile(r"memory_region_ops_(read|write) .*addr 0x([0-9a-f]+) value 0x([0-9a-f]+) size (\d+) name 'io'")
VGA = range(0x3b0, 0x3e0)
EXT = range(0x23c0, 0x23c8)
VGA_INDEX = {0x3c4, 0x3ce, 0x3d4, 0x3b4}
SEL = [0x1001 | (i << 8) for i in range(16)] + [0x1002 | (i << 8) for i in range(9)]


def build_mod(kdir, dst, fake):
    """copy the module sources to dst and build wd24adv(.ko | -qemufake.ko) there"""
    src = os.path.join(TOP, "modules/wd24adv")
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)
    for f in os.listdir(src):
        if f.endswith((".c", ".h")) and not f.startswith("wd24adv_engine"):
            shutil.copy(os.path.join(src, f), dst)
    name = "wd24adv-qemufake" if fake else "wd24adv"
    objs = "wd24adv_main.o wd24adv_core.o wd24adv_modes.o wd24adv_port.o wd24adv_s1.o wd24adv_s2.o"
    with open(os.path.join(dst, "Kbuild"), "w") as f:
        f.write(f"obj-m += {name}.o\n{name}-y := {objs}\n")
        if fake:
            f.write("# QEMU TEST BUILD ONLY: modelled 23C0h/23C2h values, never install\nccflags-y += -DWD24S1_QEMU_FAKE\n")
    subprocess.run(["make", "-s", "-C", kdir, "ARCH=i386", f"M={dst}", "modules"], check=True)
    return os.path.join(dst, name + ".ko")


class Trace:
    """incremental reader of the cpu_in/cpu_out trace, VGA + 23Cxh only"""
    def __init__(self, path):
        self.path, self.off, self.acc, self.part = path, 0, [], ""

    def poll(self):
        if not os.path.exists(self.path):
            return
        with open(self.path, "r", errors="replace") as f:
            f.seek(self.off)
            data = f.read()
            self.off = f.tell()
        data = self.part + data
        lines = data.split("\n")
        self.part = lines.pop()
        for l in lines:
            m = RX_VGA.search(l)
            if m:
                self.acc.append(("W" if m.group(1) == "write" else "R", int(m.group(2), 16), "b",
                                 int(m.group(3), 16)))
                continue
            m = RX_IO.search(l)
            if m and int(m.group(2), 16) in EXT:
                self.acc.append(("W" if m.group(1) == "write" else "R", int(m.group(2), 16),
                                 "w" if m.group(4) == "2" else "b", int(m.group(3), 16)))

    def settle(self):
        last = -1
        for _ in range(30):
            time.sleep(0.5)
            self.poll()
            if len(self.acc) == last:
                break
            last = len(self.acc)
        return len(self.acc)


def fmt(a):
    rw, port, w, v = a
    return f"{rw}{'16' if w == 'w' else '8'} {port:04X} {v:0{4 if w == 'w' else 2}X}"


REF = [0x0108, 0x1000, 0x210b, 0x3004, 0x410b, 0x5004, 0x61e4, 0x713c,
       0x8280, 0x9300, 0xa000, 0xb0a5, 0xc000, 0xd000, 0xe0ff, 0xf0ff,
       0x0000, 0x1000, 0x2014, 0x3001, 0x4000, 0x5041, 0x617a, 0x7047, 0x8000]
SNAP = list(REF); SNAP[2] = 0x2800   # the fake build's register file at load
LIST = [(1, 2, 0x000), (1, 3, 0x04c), (1, 4, 0x000), (1, 5, 0x04c), (1, 6, 0x008), (1, 7, 0x008),
        (1, 8, 0x280), (1, 9, 0x600), (1, 0xa, 0x05a), (1, 0xb, 0x05a), (1, 0xe, 0x0ff), (1, 1, 0x008),
        (2, 3, 0x05a), (2, 4, 0x0a5), (2, 5, 0x000), (2, 6, 0x010), (2, 7, 0x010)]


def expected():
    e = [("R", 0x23c0, 0x1002)] * 2
    def vall():
        for k, sel in enumerate(SEL):
            e.extend([("W", 0x23c0, sel), ("R", 0x23c0, sel), ("R", 0x23c2, SNAP[k])])
    vall()
    vall()
    for blk, idx, test in LIST:
        sel = 0x1000 | idx << 8 | blk
        tw = idx << 12 | test
        ref = SNAP[(0 if blk == 1 else 16) + idx]
        ss, sr = (0x1001, SNAP[0]) if blk == 1 else (0x1002, SNAP[16])
        e += [("W", 0x23c0, sel), ("R", 0x23c0, sel), ("W", 0x23c2, tw), ("R", 0x23c0, sel),
              ("R", 0x23c2, tw), ("W", 0x23c2, ref), ("R", 0x23c2, ref),
              ("W", 0x23c0, ss), ("R", 0x23c0, ss), ("R", 0x23c2, sr)]
    vall()
    e += [("W", 0x23c0, 0x1002), ("R", 0x23c0, 0x1002)]
    return e


def vga_data_writes(acc):
    vd, ff = [], False
    for x in acc:
        if x[0] == "R" and x[1] in (0x3da, 0x3ba):
            ff = False
        if x[0] == "W" and x[1] in VGA:
            if x[1] == 0x3c0:
                if ff:
                    vd.append(x)
                ff = not ff
            elif x[1] not in VGA_INDEX:
                vd.append(x)
    return vd


def main():
    tc32 = os.environ.get("TC32_DIR", os.path.join(os.path.dirname(TOP), "tinycore-32-ram"))
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default=os.path.join(TOP, "out/bzImage"))
    ap.add_argument("--core", default=os.path.join(tc32, "out/core.xz"))
    ap.add_argument("--kdir", default=os.environ.get("KDIR", os.path.join(TOP, "build/kernel-obj")))
    ap.add_argument("--logdir", default=os.path.join(TOP, "out/test-logs"))
    ap.add_argument("--mem", type=int, default=32)
    a = ap.parse_args()
    os.makedirs(a.logdir, exist_ok=True)
    real = build_mod(a.kdir, os.path.join(TOP, "out/qemus2/real"), False)
    fake = build_mod(a.kdir, os.path.join(TOP, "out/qemus2/fake"), True)
    ents, dirs = [], set()

    def add_file(p, data, mode=0o100755):
        parts = os.path.dirname(p).strip("/").split("/")
        for i in range(1, len(parts) + 1):
            d = "/".join(parts[:i])
            if d and d not in dirs:
                dirs.add(d)
                ents.append((d, 0o040755, b""))
        ents.append((p.strip("/"), mode, data))

    add_file("wd24-init", qwd24.INIT.encode())
    add_file("wd24/wd24adv.ko", open(real, "rb").read(), 0o100644)
    add_file("wd24/wd24adv-qemufake.ko", open(fake, "rb").read(), 0o100644)
    bb = open(os.path.join(TOP, "out/modutils/busybox"), "rb").read()
    for t in ("insmod", "rmmod"):
        add_file(f"usr/local/bin/{t}", bb)
    initrd = os.path.join(a.logdir, "qemu-wd24adv-s2-initrd")
    with open(initrd, "wb") as f:
        core = open(a.core, "rb").read()
        f.write(core + b"\0" * ((4 - len(core) % 4) % 4) + newc(ents))
    tracef = os.path.join(a.logdir, "qemu-wd24adv-s2-trace.log")
    if os.path.exists(tracef):
        os.unlink(tracef)
    cmd = ["qemu-system-i386", "-accel", "tcg", "-M", "isapc", "-cpu", "486", "-m", str(a.mem),
           "-vga", "std", "-kernel", a.kernel, "-initrd", initrd,
           "-append", "console=ttyS0 loglevel=3 rdinit=/wd24-init",
           "-display", "none", "-serial", "stdio", "-no-reboot",
           "-trace", "vga_std_write_io", "-trace", "vga_std_read_io",
           "-trace", "memory_region_ops_write", "-trace", "memory_region_ops_read", "-D", tracef]
    results, seqlog = [], []
    tr = Trace(tracef)

    def check(desc, ok, detail=""):
        results.append((desc, bool(ok)))
        print(f"  [{'PASS' if ok else 'FAIL'}] {desc}" + (f"  ({detail})" if detail else ""), flush=True)
        return ok

    def step(g, c, timeout=120):
        t0 = tr.settle()
        out, _ = g.run(c, timeout)
        tr.settle()
        return out, tr.acc[t0:]

    def loaded(g):
        return "wd24adv" in g.run("cat /proc/modules")[0]

    print("== wd24adv S2 QEMU test: -M isapc -cpu 486 -vga std")
    g = Guest(cmd, os.path.join(a.logdir, "qemu-wd24adv-s2-serial.log"))
    try:
        g.expect("WD24-INIT-UP", 300)
        time.sleep(2)
        g.send("stty -echo; PS1='# '\r")
        time.sleep(1)
        g.run("true", 30)
        g.run("dmesg -n 1; dmesg -c >/dev/null")

        out, acc = step(g, "insmod /wd24/wd24adv.ko s2=1; echo RC=$?; dmesg -c")
        check("s2=1 (dry-run) loads, zero port accesses", "RC=0" in out and len(acc) == 0, f"{len(acc)}")
        pl, _ = g.run("cat /proc/wd24adv/s2")
        check("s2 dry-run: plan lists 17 registers, 110 + 34 writes",
              "CUR_POS_Y" in pl and "23C0h 110, 23C2h 34" in pl and "S2 plan" in pl)
        out, acc = step(g, "rmmod wd24adv; echo RC=$?")
        check("s2 dry-run: rmmod clean, zero accesses", "RC=0" in out and len(acc) == 0)

        t0 = tr.settle()
        for args, want in [("dryrun=0 s2=1 s1=1", "separate stage"), ("dryrun=0 s2=1 live=1", "separate stage"),
                           ("dryrun=0 s2=1 mode=640x480-8", "separate stage"), ("s2=1 s1=1", "separate stage")]:
            out, _ = g.run(f"insmod /wd24/wd24adv.ko {args}; echo RC=$?; dmesg -c")
            check(f"refused: {args}", "RC=0" not in out and want in out and not loaded(g),
                  next((l for l in out.splitlines() if "wd24adv:" in l), "").strip())
        tr.settle()
        check("S2 refusals: zero port accesses", len(tr.acc) == t0, f"{len(tr.acc) - t0}")

        out, acc = step(g, "insmod /wd24/wd24adv.ko dryrun=0 s2=1; echo RC=$?; dmesg -c")
        res, _ = g.run("cat /proc/wd24adv/s2")
        seqlog.append("# real wd24adv.ko: insmod dryrun=0 s2=1\n" + out + "\n" + res)
        seqlog += [fmt(x) for x in acc]
        ext = [x for x in acc if x[1] in EXT]
        check("real .ko s2: insmod 0, refused by the entry check", "RC=0" in out and "entry check: REFUSED" in res)
        check("real .ko s2: ZERO 23Cxh accesses (no 23C2h write)", len(ext) == 0, f"{len(ext)}")
        vd = vga_data_writes(acc)
        check("real .ko s2: ZERO VGA data writes", len(vd) == 0, ", ".join(fmt(x) for x in vd) or "none")
        out, acc = step(g, "rmmod wd24adv; echo RC=$?")
        check("real .ko s2: rmmod clean, zero accesses", "RC=0" in out and len(acc) == 0)

        out, acc = step(g, "insmod /wd24/wd24adv-qemufake.ko dryrun=0 s2=1; echo RC=$?; dmesg -c")
        res, _ = g.run("cat /proc/wd24adv/s2")
        seqlog.append("\n# TEST-ONLY wd24adv-qemufake.ko: insmod dryrun=0 s2=1\n" + out + "\n" + res)
        seqlog += [fmt(x) for x in acc]
        ext = [x for x in acc if x[1] in EXT]
        got = [(x[0], x[1], x[3]) for x in ext if not (x[0] == "R" and x[1] in (0x3c0, 0x3c1))]
        check("fake .ko s2: insmod 0, result OK", "RC=0" in out and "S2 result: OK" in res,
              next((l for l in res.splitlines() if "S2 result" in l), "").strip())
        check("fake .ko s2: all 23Cxh accesses 16-bit", all(x[2] == "w" for x in ext))
        # the trace shows QEMU's own read values (no 23C0h device): writes are
        # compared with values, reads by port; the module's access log gives
        # the modelled read values
        ex = expected()
        check("fake .ko s2: exact 23Cxh trace (write values, read ports, order)",
              [(w, p, v if w == "W" else 0) for w, p, v in got] == [(w, p, v if w == "W" else 0) for w, p, v in ex],
              f"{len(got)} vs {len(ex)}")
        lg = [(m.group(1), int(m.group(2), 16), int(m.group(3), 16))
              for m in re.finditer(r"^\s+([WR])16 (23c[0-7]) ([0-9a-f]{4})", res.replace("\r", ""), re.M)]
        check("fake .ko s2: module access log == expected (all values)", lg == ex, f"{len(lg)} vs {len(ex)}")
        snapl = next((l for l in res.splitlines() if l.startswith("snapshot (25):")), "")
        check("fake .ko s2: /proc logs the snapshot (SRC_LO 2800, not the S1 reference)",
              snapl.split(":", 1)[-1].split() == [f"{v:04x}" for v in SNAP], snapl.strip())
        check("fake .ko s2: 34 23C2h writes, 110 23C0h writes",
              sum(1 for x in ext if x[0] == "W" and x[1] == 0x23c2) == 34 and
              sum(1 for x in ext if x[0] == "W" and x[1] == 0x23c0) == 110)
        check("fake .ko s2: nothing on 23C4h-23C7h", not any(x[1] >= 0x23c4 for x in ext))
        vd = vga_data_writes(acc)
        check("fake .ko s2: ZERO VGA data writes", len(vd) == 0, ", ".join(fmt(x) for x in vd) or "none")
        out, acc = step(g, "rmmod wd24adv_qemufake; echo RC=$?")
        check("fake .ko: rmmod clean, zero accesses", "RC=0" in out and len(acc) == 0)
    except (TimeoutError, EOFError) as e:
        check("guest responded", False, str(e))
    finally:
        g.close()
        with open(os.path.join(a.logdir, "qemu-wd24adv-s2-sequence.txt"), "w") as f:
            f.write("\n".join(s.replace("\r", "") for s in seqlog) + "\n")
    bad = [d for d, ok in results if not ok]
    print(f"== wd24adv S2 QEMU test: {'PASS' if not bad else 'FAIL'} ({len(results) - len(bad)}/{len(results)})")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
