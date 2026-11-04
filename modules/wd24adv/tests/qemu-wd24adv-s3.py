#!/usr/bin/env python3
"""QEMU system test of wd24adv S3 (s3=1): refusals, dry-run, exact write list.

  * s3=1 (dry-run): /proc/wd24adv/s3 lists the plan, ZERO port accesses;
  * refusals (s3 with s1/s2, dryrun=0 without live=1 mode=640x480-8): zero accesses;
  * the real wd24adv.ko, dryrun=0 live=1 mode=640x480-8 s3=1: the live entry
    check refuses (QEMU is not a WD90C24): ZERO 23Cxh accesses;
  * the TEST-ONLY fake build (registers preset to the S2 run 2 snapshot,
    engine + video memory modelled in RAM): result OK, the 23C2h writes are
    exactly the closed list (3 ops + restore, 46 words), 23C0h ends 1002h,
    nothing on 23C4h, no VGA data write.
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
    objs = "wd24adv_main.o wd24adv_core.o wd24adv_modes.o wd24adv_port.o wd24adv_s1.o wd24adv_s2.o wd24adv_s3.o"
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


WLIST = [int(w, 16) for w in """
2000 3060 4000 5060 8280 6008 7008 e0ff 9300 1000 a03c 0908
2200 3060 4200 5060 8280 6020 7020 e0ff 9300 1000 a0c3 0908
2200 3060 4100 5060 8280 6020 7020 e0ff 9300 1000 0900
1000 210b 3004 410b 5004 61e4 713c 8280 9300 a000 e0ff""".split()]


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
    real = build_mod(a.kdir, os.path.join(TOP, "out/qemus3/real"), False)
    fake = build_mod(a.kdir, os.path.join(TOP, "out/qemus3/fake"), True)
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
    initrd = os.path.join(a.logdir, "qemu-wd24adv-s3-initrd")
    with open(initrd, "wb") as f:
        core = open(a.core, "rb").read()
        f.write(core + b"\0" * ((4 - len(core) % 4) % 4) + newc(ents))
    tracef = os.path.join(a.logdir, "qemu-wd24adv-s3-trace.log")
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

    print("== wd24adv S3 QEMU test: -M isapc -cpu 486 -vga std")
    g = Guest(cmd, os.path.join(a.logdir, "qemu-wd24adv-s3-serial.log"))
    try:
        g.expect("WD24-INIT-UP", 300)
        time.sleep(2)
        g.send("stty -echo; PS1='# '\r")
        time.sleep(1)
        g.run("true", 30)
        g.run("dmesg -n 1; dmesg -c >/dev/null")

        out, acc = step(g, "insmod /wd24/wd24adv.ko s3=1; echo RC=$?; dmesg -c")
        check("s3=1 (dry-run) loads, zero port accesses", "RC=0" in out and len(acc) == 0, f"{len(acc)}")
        pl, _ = g.run("cat /proc/wd24adv/s3")
        check("s3 dry-run: plan lists the 3 ops", "S3 plan" in pl and "op 3" in pl and "0900" in pl)
        out, acc = step(g, "rmmod wd24adv; echo RC=$?")
        check("s3 dry-run: rmmod clean, zero accesses", "RC=0" in out and len(acc) == 0)

        t0 = tr.settle()
        for args, want in [("dryrun=0 s3=1", "needs live=1"), ("dryrun=0 s3=1 live=1", "needs live=1"),
                           ("dryrun=0 s3=1 live=1 mode=640x480-4", "needs live=1"),
                           ("s3=1 s1=1", "separate stage"), ("s3=1 s2=1", "separate stage")]:
            out, _ = g.run(f"insmod /wd24/wd24adv.ko {args}; echo RC=$?; dmesg -c")
            check(f"refused: {args}", "RC=0" not in out and want in out and not loaded(g),
                  next((l for l in out.splitlines() if "wd24adv:" in l), "").strip())
        tr.settle()
        check("S3 refusals: zero port accesses", len(tr.acc) == t0, f"{len(tr.acc) - t0}")

        out, acc = step(g, "insmod /wd24/wd24adv.ko dryrun=0 live=1 mode=640x480-8 s3=1; echo RC=$?; dmesg -c")
        seqlog.append("# real wd24adv.ko: live s3\n" + out)
        seqlog += [fmt(x) for x in acc]
        ext = [x for x in acc if x[1] in EXT]
        check("real .ko live s3: ZERO 23Cxh accesses", len(ext) == 0, f"{len(ext)}")
        vd = vga_data_writes(acc)
        check("real .ko live s3: ZERO VGA data writes", len(vd) == 0, ", ".join(fmt(x) for x in vd) or "none")
        if loaded(g):
            g.run("rmmod wd24adv")

        t0 = tr.settle()
        for args in ("dryrun=0 s3=1 live=1 mode=640x480-8", "dryrun=0 s3=1 mode=640x480-8"):
            out, _ = g.run(f"insmod /wd24/wd24adv-qemufake.ko {args}; echo RC=$?; dmesg -c")
            check(f"fake refused: {args}", "RC=0" not in out and not loaded(g))
        tr.settle()
        check("fake refusals: zero port accesses", len(tr.acc) == t0, f"{len(tr.acc) - t0}")

        out, acc = step(g, "insmod /wd24/wd24adv-qemufake.ko dryrun=0 s3=1; echo RC=$?; dmesg -c")
        res, _ = g.run("cat /proc/wd24adv/s3")
        seqlog.append("\n# TEST-ONLY wd24adv-qemufake.ko: insmod dryrun=0 s3=1\n" + out + "\n" + res)
        seqlog += [fmt(x) for x in acc]
        ext = [x for x in acc if x[1] in EXT]
        check("fake .ko s3: insmod 0, result OK", "RC=0" in out and "S3 result: OK" in res,
              next((l for l in res.splitlines() if "S3 result" in l), "").strip())
        check("fake .ko s3: 3 ops VERIFIED", res.count("VERIFIED") == 3)
        check("fake .ko s3: all 23Cxh accesses 16-bit", all(x[2] == "w" for x in ext))
        w2 = [x[3] for x in ext if x[0] == "W" and x[1] == 0x23c2]
        check("fake .ko s3: 23C2h writes == the closed list (46)", w2 == WLIST, f"{len(w2)}: " + " ".join(f"{v:04x}" for v in w2))
        w0 = [x[3] for x in ext if x[0] == "W" and x[1] == 0x23c0]
        check("fake .ko s3: 23C0h writes only selects, last 1002h",
              w0 and w0[-1] == 0x1002 and all(v & 0xf0ff in (0x1001, 0x1002) for v in w0), f"{len(w0)}")
        cur, b2w = 0, 0
        for x in ext:
            if x[0] == "W" and x[1] == 0x23c0:
                cur = x[3]
            elif x[0] == "W" and x[1] == 0x23c2 and cur & 0xff != 1:
                b2w += 1
        check("fake .ko s3: no 23C2h write unless block 1 is selected", b2w == 0, f"{b2w}")
        check("fake .ko s3: nothing on 23C4h-23C7h", not any(x[1] >= 0x23c4 for x in ext))
        vd = vga_data_writes(acc)
        check("fake .ko s3: ZERO VGA data writes", len(vd) == 0, ", ".join(fmt(x) for x in vd) or "none")
        out, acc = step(g, "rmmod wd24adv_qemufake; echo RC=$?")
        check("fake .ko: rmmod clean, zero accesses", "RC=0" in out and len(acc) == 0)

        # timeout, then the (fake) BITBLT goes idle: normal path, rmmod OK
        out, acc = step(g, "insmod /wd24/wd24adv-qemufake.ko dryrun=0 s3=1 fake_hang=2; echo RC=$?; dmesg -c")
        res, _ = g.run("cat /proc/wd24adv/s3")
        seqlog.append("\n# fake_hang=2\n" + out + "\n" + res)
        check("fake_hang=2: TIMEOUT, then seen idle (unlocked)", "RC=0" in out and "S3 result: TIMEOUT" in res
              and "IDLE seen" in res and "LOCKED" not in out, next((l for l in res.splitlines() if "idle" in l), ""))
        ext = [x for x in acc if x[1] in EXT]
        st = max(i for i, x in enumerate(ext) if x[0] == "W" and x[1] == 0x23c2)
        check("fake_hang=2: no 23Cxh write after the start that timed out", not any(x[0] == "W" for x in ext[st + 1:]))
        check("fake_hang=2: ZERO VGA data writes", len(vga_data_writes(acc)) == 0)
        out, _ = g.run("rmmod wd24adv_qemufake; echo RC=$?")
        check("fake_hang=2: rmmod OK (not pinned)", "RC=0" in out and not loaded(g))

        # timeout, BITBLT never finishes: LOCKED, sticky, pinned (last: stays loaded)
        out, acc = step(g, "insmod /wd24/wd24adv-qemufake.ko dryrun=0 s3=1 fake_hang=1; echo RC=$?; dmesg -c")
        res, _ = g.run("cat /proc/wd24adv/s3")
        seqlog.append("\n# fake_hang=1\n" + out + "\n" + res)
        check("fake_hang=1: TIMEOUT, STILL BUSY, LOCKED, power cycle required", "RC=0" in out and
              "S3 result: TIMEOUT" in res and "STILL BUSY" in res and "LOCKED" in out and "power cycle required" in out)
        ext = [x for x in acc if x[1] in EXT]
        st = max(i for i, x in enumerate(ext) if x[0] == "W" and x[1] == 0x23c2)
        check("fake_hang=1: no 23Cxh write after the start", not any(x[0] == "W" for x in ext[st + 1:]))
        check("fake_hang=1: nothing on 23C4h-23C7h", not any(x[1] >= 0x23c4 for x in ext))
        check("fake_hang=1: ZERO VGA data writes", len(vga_data_writes(acc)) == 0)
        # the fake build has no live=1 control file; control/mode gating is host + review only
        out, _ = g.run("ls /proc/wd24adv/")
        check("fake_hang=1: s3 report present, no control file (fake build)", "s3" in out and "control" not in out)
        out, acc = step(g, "rmmod wd24adv_qemufake; echo RC=$?; dmesg -c")
        check("fake_hang=1: rmmod refused (pinned), still loaded, zero accesses",
              "RC=0" not in out and loaded(g) and len(acc) == 0)
        out, acc = step(g, "insmod /wd24/wd24adv-qemufake.ko; echo RC=$?")
        check("fake_hang=1: reload refused (still loaded), zero accesses", "RC=0" not in out and len(acc) == 0)
    except (TimeoutError, EOFError) as e:
        check("guest responded", False, str(e))
    finally:
        g.close()
        with open(os.path.join(a.logdir, "qemu-wd24adv-s3-sequence.txt"), "w") as f:
            f.write("\n".join(s.replace("\r", "") for s in seqlog) + "\n")
    bad = [d for d, ok in results if not ok]
    print(f"== wd24adv S3 QEMU test: {'PASS' if not bad else 'FAIL'} ({len(results) - len(bad)}/{len(results)})")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
