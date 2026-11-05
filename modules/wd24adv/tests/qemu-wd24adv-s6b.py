#!/usr/bin/env python3
"""QEMU system test of wd24adv S6b (s6b=1: /dev/wd24s6b dual-window mmap + PR31 b7 copy).

  * s6b=1 (dry-run): a model session (cross-bank scroll + memmove), ZERO port accesses;
  * refusals (s6b with s1-s6, dryrun=0 without live=1 mode=640x480-8): zero accesses;
  * the real wd24adv.ko, dryrun=0 live=1 mode=640x480-8 s6b=1: the live entry
    check refuses (QEMU is not a WD90C24): ZERO 23Cxh accesses, ZERO VGA data
    writes, device never armed (open -> ENODEV);
  * the TEST-ONLY fake build: wd24s6b-test run through the real mmap/ioctl
    paths (cross-bank memmove through the mapping, kernel COPY cross-bank and
    overlapping, frame scroll, restore) = OK; exclusive open; kill -9 of a
    holder ends the session (registers restored, re-open OK); rmmod refused
    while held, clean after; ZERO port accesses throughout.
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
    objs = "wd24adv_main.o wd24adv_core.o wd24adv_modes.o wd24adv_port.o wd24adv_s1.o wd24adv_s2.o wd24adv_s3.o wd24adv_s6.o wd24adv_s6b.o"
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
    ap.add_argument("--cc", default=os.environ.get("I486CC", "i486-linux-musl-gcc"))
    ap.add_argument("--mem", type=int, default=32)
    a = ap.parse_args()
    os.makedirs(a.logdir, exist_ok=True)
    real = build_mod(a.kdir, os.path.join(TOP, "out/qemus6b/real"), False)
    fake = build_mod(a.kdir, os.path.join(TOP, "out/qemus6b/fake"), True)
    tool = os.path.join(TOP, "out/qemus6b/wd24s6b-test")
    subprocess.run([a.cc, "-static", "-O2", "-march=i486", "-Wall", "-Werror", "-o", tool,
                    os.path.join(os.path.dirname(os.path.abspath(__file__)), "wd24s6b-test.c")], check=True)
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
    add_file("wd24/wd24s6b-test", open(tool, "rb").read())
    bb = open(os.path.join(TOP, "out/modutils/busybox"), "rb").read()
    for t in ("insmod", "rmmod"):
        add_file(f"usr/local/bin/{t}", bb)
    initrd = os.path.join(a.logdir, "qemu-wd24adv-s6b-initrd")
    with open(initrd, "wb") as f:
        core = open(a.core, "rb").read()
        f.write(core + b"\0" * ((4 - len(core) % 4) % 4) + newc(ents))
    tracef = os.path.join(a.logdir, "qemu-wd24adv-s6b-trace.log")
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
    NODE = ("[ -c /dev/wd24s6b ] || mknod /dev/wd24s6b c "
            "$(sed -n 's/.*major \\([0-9]*\\).*/\\1/p' /proc/wd24adv/s6b | head -1) 0")

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

    print("== wd24adv S6b QEMU test: -M isapc -cpu 486 -vga std")
    g = Guest(cmd, os.path.join(a.logdir, "qemu-wd24adv-s6b-serial.log"))
    try:
        g.expect("WD24-INIT-UP", 300)
        time.sleep(2)
        g.send("stty -echo; PS1='# '\r")
        time.sleep(1)
        g.run("true", 30)
        g.run("dmesg -n 1; dmesg -c >/dev/null")

        out, acc = step(g, "insmod /wd24/wd24adv.ko s6b=1; echo RC=$?; dmesg -c")
        pl, _ = g.run("cat /proc/wd24adv/s6b")
        seqlog.append("# s6b=1 dry-run (model session)\n" + out + "\n" + pl)
        check("s6b=1 dry-run loads, zero port accesses", "RC=0" in out and len(acc) == 0, f"{len(acc)}")
        check("s6b dry-run: model session OK (begin, SETWIN, cross-bank scroll, memmove, end)",
              "S6b (model, no port access): OK" in out and "model: COPY frame scroll up 16 lines (cross-bank) -> 0" in pl
              and "model: COPY overlapping memmove forward-dst -> 0" in pl and "model: begin -> 0" in pl
              and "restore-bad 0" in pl)
        out, acc = step(g, "ls /dev/wd24s6b; rmmod wd24adv; echo RC=$?")
        check("s6b dry-run: no device, rmmod clean, zero accesses",
              "RC=0" in out and "No such file" in out and len(acc) == 0)

        t0 = tr.settle()
        for args, want in [("dryrun=0 s6b=1", "s6b=1 with dryrun=0 needs live=1"),
                           ("dryrun=0 s6b=1 live=1", "s6b=1 with dryrun=0 needs live=1"),
                           ("dryrun=0 s6b=1 live=1 mode=640x400-8", "s6b=1 with dryrun=0 needs live=1"),
                           ("s6b=1 s1=1", "separate stage"), ("s6b=1 s2=1", "separate stage"),
                           ("s6b=1 s3=1", "separate stage"), ("s6b=1 s5=1", "separate stage"),
                           ("s6b=1 s6=1", "separate stage")]:
            out, _ = g.run(f"insmod /wd24/wd24adv.ko {args}; echo RC=$?; dmesg -c")
            check(f"refused: {args}", "RC=0" not in out and want in out and not loaded(g),
                  next((l for l in out.splitlines() if "wd24adv:" in l), "").strip())
        tr.settle()
        check("S6b refusals: zero port accesses", len(tr.acc) == t0, f"{len(tr.acc) - t0}")

        out, acc = step(g, "insmod /wd24/wd24adv.ko dryrun=0 live=1 mode=640x480-8 s6b=1; echo RC=$?; dmesg -c")
        seqlog.append("\n# real wd24adv.ko: live s6b=1\n" + out)
        ext = [x for x in acc if x[1] in EXT]
        check("real .ko live s6b=1: ZERO 23Cxh accesses", len(ext) == 0, f"{len(ext)}")
        vd = vga_data_writes(acc)
        check("real .ko live s6b=1: ZERO VGA data writes", len(vd) == 0, ", ".join(fmt(x) for x in vd) or "none")
        if loaded(g):
            out, acc = step(g, NODE + "; /wd24/wd24s6b-test run; echo RC=$?")
            check("real .ko: device never armed (open refused), zero accesses",
                  "S6B-RESULT FAIL open" in out and len(acc) == 0, out.strip().splitlines()[-2:] and "")
            g.run("rmmod wd24adv")

        out, acc = step(g, "insmod /wd24/wd24adv-qemufake.ko dryrun=0 s6b=1; echo RC=$?; dmesg -c; " + NODE, 300)
        check("fake s6b=1: insmod 0, device registered", "RC=0" in out and "/dev/wd24s6b major" in out)
        out, acc = step(g, "/wd24/wd24s6b-test run; echo RC=$?", 600)
        seqlog.append("\n# TEST-ONLY fake: wd24s6b-test run\n" + out)
        check("fake: wd24s6b-test run: S6B-RESULT OK", "S6B-RESULT OK" in out and "RC=0" in out,
              next((l for l in out.splitlines() if "S6B-RESULT" in l), "").strip())
        check("fake: mmap cross-bank memmove + COPY cross-bank/overlap + scroll all ok",
              "FAIL" not in out and "S6B ok   mmap cross-bank memmove" in out and
              out.count("S6B ok   kernel COPY") == 4 and "S6B ok   COPY frame scroll" in out and
              "S6B ok   frame restored" in out and "S6B ok   off-screen restored" in out)
        check("fake: run: ZERO port accesses", len(acc) == 0, f"{len(acc)}")

        out, _ = g.run("/wd24/wd24s6b-test hold & sleep 3; /wd24/wd24s6b-test run; echo RC=$?; "
                       "rmmod wd24adv_qemufake; echo RMMOD=$?", 120)
        check("fake: second open refused while held (exclusive)", "S6B-HOLD setwin 0" in out and
              "S6B-RESULT FAIL open: " in out and "busy" in out)
        check("fake: rmmod refused while the session is held", "RMMOD=0" not in out and loaded(g))
        out, acc = step(g, "kill -9 %1 2>/dev/null || kill -9 $(pidof wd24s6b-test); sleep 2; cat /proc/wd24adv/s6b | tail -12")
        seqlog.append("\n# TEST-ONLY fake: kill -9 holder\n" + out)
        check("fake: kill -9: session ended by release, restored",
              "release (close / munmap / exit) -> 0" in out and "restore-bad 0" in out)
        out, acc = step(g, "/wd24/wd24s6b-test run; echo RC=$?", 600)
        check("fake: re-open after kill -9: S6B-RESULT OK (entry state was restored)", "S6B-RESULT OK" in out)
        out, acc = step(g, "rmmod wd24adv_qemufake; echo RC=$?; ls /dev/wd24s6b")
        check("fake: rmmod clean after", "RC=0" in out and not loaded(g))
        check("fake: kill/rmmod: ZERO port accesses", len(acc) == 0, f"{len(acc)}")
    except (TimeoutError, EOFError) as e:
        check("guest responded", False, str(e))
    finally:
        g.close()
        with open(os.path.join(a.logdir, "qemu-wd24adv-s6b-sequence.txt"), "w") as f:
            f.write("\n".join(s.replace("\r", "") for s in seqlog) + "\n")
    bad = [d for d, ok in results if not ok]
    print(f"== wd24adv S6b QEMU test: {'PASS' if not bad else 'FAIL'} ({len(results) - len(bad)}/{len(results)})")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
