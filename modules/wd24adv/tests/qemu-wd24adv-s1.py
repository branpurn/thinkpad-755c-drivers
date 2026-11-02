#!/usr/bin/env python3
"""QEMU system test of the wd24adv S1 probe (s1=1): exact port sequence.

Same guest as qemu-wd24adv.py (-M isapc -cpu 486 -vga std, tc755 kernel,
busybox insmod/rmmod), plus QEMU's memory_region_ops_read/write events:
23C0h-23C7h are unassigned on isapc, so their accesses (any width) land
in the root 'io' region and are traced there, in order with the VGA
vga_std_read_io / vga_std_write_io events.  Checks:

  * s1=1 (dry-run): loads, /proc/wd24adv/s1 lists the plan, ZERO port
    accesses on 3B0-3DF and 23C0-23C7; rmmod clean, zero accesses;
  * refusals: s1 with live=1 / mode= / s1_echo without s1: zero accesses;
  * the real wd24adv.ko, s1=1 dryrun=0: the entry check refuses (QEMU is not
    a WD90C24); only VGA index writes (put back), no VGA data write, ZERO
    23Cxh accesses; the module stays loaded holding the result (busybox
    insmod would otherwise run init twice); rmmod: zero accesses;
  * a TEST-ONLY build (wd24adv-qemufake.ko, -DWD24S1_QEMU_FAKE, built here
    under out/, never installed): real inw/outw, modelled 23C0h/23C2h
    values, entry check result ignored.  The trace must show exactly the
    51 23C0h writes in order (pass 1, pass 2, restore), only 23C0h/23C2h
    reads, nothing on 23C4h-23C7h, no VGA data write.

  modules/wd24adv/tests/qemu-wd24adv-s1.py [--kernel out/bzImage]
      [--modules out/modules] [--core ../tinycore-32-ram/out/core.xz]
Logs: out/test-logs/qemu-wd24adv-s1-{serial,trace}.log and
out/test-logs/qemu-wd24adv-s1-sequence.txt (the S1 port accesses).
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


def build_fake(kdir, dst):
    """copy the module sources to dst and build wd24adv-qemufake.ko there"""
    src = os.path.join(TOP, "modules/wd24adv")
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)
    for f in os.listdir(src):
        if f.endswith((".c", ".h")) and not f.startswith("wd24adv_engine"):
            shutil.copy(os.path.join(src, f), dst)
    with open(os.path.join(dst, "Kbuild"), "w") as f:
        f.write("# QEMU TEST BUILD ONLY: modelled 23C0h values, never install\n"
                "obj-m += wd24adv-qemufake.o\n"
                "wd24adv-qemufake-y := wd24adv_main.o wd24adv_core.o wd24adv_modes.o wd24adv_port.o wd24adv_s1.o\n"
                "ccflags-y += -DWD24S1_QEMU_FAKE\n")
    subprocess.run(["make", "-s", "-C", kdir, "ARCH=i386", f"M={dst}", "modules"], check=True)
    return os.path.join(dst, "wd24adv-qemufake.ko")


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


def main():
    tc32 = os.environ.get("TC32_DIR", os.path.join(os.path.dirname(TOP), "tinycore-32-ram"))
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default=os.path.join(TOP, "out/bzImage"))
    ap.add_argument("--modules", default=os.path.join(TOP, "out/modules"))
    ap.add_argument("--core", default=os.path.join(tc32, "out/core.xz"))
    ap.add_argument("--kdir", default=os.environ.get("KDIR", os.path.join(TOP, "build/kernel-obj")))
    ap.add_argument("--logdir", default=os.path.join(TOP, "out/test-logs"))
    ap.add_argument("--mem", type=int, default=32)
    a = ap.parse_args()
    os.makedirs(a.logdir, exist_ok=True)

    kver = os.listdir(os.path.join(a.modules, "lib/modules"))[0]
    md = os.path.join(a.modules, "lib/modules", kver)
    fake = build_fake(a.kdir, os.path.join(TOP, "out/qemufake/wd24adv"))
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
    add_file("wd24/wd24adv.ko", open(os.path.join(md, "extra/wd24adv.ko"), "rb").read(), 0o100644)
    add_file("wd24/wd24adv-qemufake.ko", open(fake, "rb").read(), 0o100644)
    bb = open(os.path.join(TOP, "out/modutils/busybox"), "rb").read()
    for t in ("insmod", "rmmod"):
        add_file(f"usr/local/bin/{t}", bb)
    initrd = os.path.join(a.logdir, "qemu-wd24adv-s1-initrd")
    with open(initrd, "wb") as f:
        core = open(a.core, "rb").read()
        f.write(core + b"\0" * ((4 - len(core) % 4) % 4) + newc(ents))

    tracef = os.path.join(a.logdir, "qemu-wd24adv-s1-trace.log")
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

    print(f"== wd24adv S1 QEMU test: -M isapc -cpu 486 -vga std, kernel {kver}")
    g = Guest(cmd, os.path.join(a.logdir, "qemu-wd24adv-s1-serial.log"))
    try:
        g.expect("WD24-INIT-UP", 300)
        time.sleep(2)
        g.send("stty -echo; PS1='# '\r")
        time.sleep(1)
        g.run("true", 30)
        g.run("dmesg -n 1; dmesg -c >/dev/null")

        # 1: s1=1 dry-run
        out, acc = step(g, "insmod /wd24/wd24adv.ko s1=1; echo RC=$?; dmesg -c")
        check("s1=1 (dry-run) loads", "RC=0" in out, out.strip().splitlines()[-1] if out.strip() else "")
        check("s1=1 dry-run: zero port accesses (VGA, 23Cxh)", len(acc) == 0, f"{len(acc)}")
        pl, _ = g.run("cat /proc/wd24adv/s1")
        check("s1=1 dry-run: plan lists both passes and 51 writes",
              pl.count("1001 1101 1201") == 2 and "23C0h 51" in pl and "1802\n" in pl.replace("\r", ""))
        out, acc = step(g, "rmmod wd24adv; echo RC=$?")
        check("s1 dry-run: rmmod clean, zero accesses", "RC=0" in out and len(acc) == 0)

        # 2: refusals
        t0 = tr.settle()
        for args, want in [("s1=1 live=1", "needs dryrun=0"), ("dryrun=0 s1=1 live=1", "separate read-only"),
                           ("s1=1 mode=640x480-8", "separate read-only"),
                           ("dryrun=0 s1=1 mode=640x480-8", "separate read-only"),
                           ("s1_echo=0", "needs s1=1"), ("dryrun=0 s1_echo=0", "needs s1=1")]:
            out, _ = g.run(f"insmod /wd24/wd24adv.ko {args}; echo RC=$?; dmesg -c")
            check(f"refused: {args}", "RC=0" not in out and want in out and not loaded(g),
                  next((l for l in out.splitlines() if "wd24adv:" in l), "").strip())
        tr.settle()
        check("S1 refusals: zero port accesses", len(tr.acc) == t0, f"{len(tr.acc) - t0}")

        # 3: the real module on std VGA
        out, acc = step(g, "insmod /wd24/wd24adv.ko dryrun=0 s1=1; echo RC=$?; dmesg -c")
        res, _ = g.run("cat /proc/wd24adv/s1")
        seqlog.append("# real wd24adv.ko: insmod dryrun=0 s1=1 (QEMU -vga std)\n" + out + "\n" + res)
        seqlog += [fmt(x) for x in acc]
        ext = [x for x in acc if x[1] in EXT]
        vw = [x for x in acc if x[0] == "W" and x[1] in VGA]
        ff, data_w, idx_w = False, [], []
        for x in acc:
            if x[0] == "R" and x[1] in (0x3da, 0x3ba):
                ff = False
            if x[0] != "W" or x[1] not in VGA:
                continue
            if x[1] == 0x3c0:
                (data_w if ff else idx_w).append(x)
                ff = not ff
            elif x[1] in VGA_INDEX:
                idx_w.append(x)
            else:
                data_w.append(x)
        print("      dmesg:", " | ".join(l.strip() for l in out.splitlines() if "wd24adv" in l))
        print("      VGA writes:", ", ".join(fmt(x) for x in vw))
        check("real .ko s1: insmod returns 0 (result held, init runs once)", "RC=0" in out)
        check("real .ko s1: refused by the entry check", "REFUSED" in res and "entry check: REFUSED" in res,
              next((l for l in res.splitlines() if "CHECK FAILED" in l), "").strip())
        check("real .ko s1: ZERO 23Cxh accesses", len(ext) == 0, f"{len(ext)}")
        check("real .ko s1: ZERO VGA data writes", len(data_w) == 0, ", ".join(fmt(x) for x in data_w) or "none")
        restored = True
        for port in VGA_INDEX:
            ws = [x[3] for x in idx_w if x[1] == port]
            rs = [x[3] for x in acc if x[0] == "R" and x[1] == port]
            if ws and (not rs or ws[-1] != rs[0]):
                restored = False
        check("real .ko s1: index registers put back", restored, f"{len(idx_w)} index writes")
        out, acc = step(g, "rmmod wd24adv; echo RC=$?")
        check("real .ko s1: rmmod clean, zero accesses", "RC=0" in out and len(acc) == 0, f"{len(acc)}")

        # 4: the fake build: exact 23C0h sequence
        out, acc = step(g, "insmod /wd24/wd24adv-qemufake.ko dryrun=0 s1=1; echo RC=$?; dmesg -c")
        res, _ = g.run("cat /proc/wd24adv/s1")
        seqlog.append("\n# TEST-ONLY wd24adv-qemufake.ko: insmod dryrun=0 s1=1\n" + out + "\n" + res)
        seqlog += [fmt(x) for x in acc]
        ext = [x for x in acc if x[1] in EXT]
        ew = [x for x in ext if x[0] == "W"]
        er = [x for x in ext if x[0] == "R"]
        expect = SEL + SEL + [0x1001]
        check("fake .ko s1: insmod 0, result OK", "RC=0" in out and "S1 result: OK" in res,
              next((l for l in res.splitlines() if "S1 result" in l), "").strip())
        check("fake .ko s1: exactly 51 23Cxh writes", len(ew) == 51, f"{len(ew)}")
        check("fake .ko s1: all writes 16-bit to 23C0h, in order: pass 1, pass 2, restore 1001 (the modelled S)",
              [(x[1], x[2], x[3]) for x in ew] == [(0x23c0, "w", v) for v in expect])
        check("fake .ko s1: reads only 23C0h/23C2h, 16-bit", all(x[1] in (0x23c0, 0x23c2) and x[2] == "w" for x in er),
              f"{len(er)} reads")
        check("fake .ko s1: nothing on 23C4h-23C7h", not any(x[1] >= 0x23c4 for x in ext))
        # interleave: R R (S), then per select W R(23C0) R(23C2), then W R
        order = [(x[0], x[1]) for x in ext]
        want = [("R", 0x23c0)] * 2 + [("W", 0x23c0), ("R", 0x23c0), ("R", 0x23c2)] * 50 + [("W", 0x23c0), ("R", 0x23c0)]
        check("fake .ko s1: 23Cxh access order exact (2 + 150 + 2)", order == want, f"{len(order)} accesses")
        vd = []
        ff = False
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
        check("fake .ko s1: ZERO VGA data writes", len(vd) == 0, ", ".join(fmt(x) for x in vd) or "none")
        check("fake .ko: QEMU FAKE banner in the result", "QEMU FAKE BUILD" in res)
        out, acc = step(g, "rmmod wd24adv_qemufake; echo RC=$?")
        check("fake .ko: rmmod clean, zero accesses", "RC=0" in out and len(acc) == 0, f"{len(acc)}")
    except (TimeoutError, EOFError) as e:
        check("guest responded", False, str(e))
    finally:
        g.close()
        with open(os.path.join(a.logdir, "qemu-wd24adv-s1-sequence.txt"), "w") as f:
            f.write("\n".join(s.replace("\r", "") for s in seqlog) + "\n")
    bad = [d for d, ok in results if not ok]
    print(f"== wd24adv S1 QEMU test: {'PASS' if not bad else 'FAIL'} ({len(results) - len(bad)}/{len(results)})")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
