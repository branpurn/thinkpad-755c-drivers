#!/usr/bin/env python3
"""QEMU system test of wd24adv S5 (s5=1 MONO, s5=2 HOSTCOL): refusals, dry-run,
exact 23C2h and 23C4h write lists (restated here from the table and the word
format), ACT read before every host word, HOST DATA SHORT and TIMEOUT paths.

(S4 header kept below for the shared structure.)

  Same structure as qemu-wd24adv-s3.py; the S4 program (on-screen bank 1:
  7 ROP fills, 2 forward + 2 backward overlapping copies) and its closed
  23C2h write list are restated here from the geometry, independently of
  the C table.

  * s4=1 (dry-run): /proc/wd24adv/s4 lists the plan, ZERO port accesses;
  * refusals (s4 with s1/s2/s3, dryrun=0 without live=1 mode=640x480-8): zero accesses;
  * the real wd24adv.ko, dryrun=0 live=1 mode=640x480-8 s4=1: the live entry
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


def bit(pat, r, c):
    if pat == 0: return (r ^ c) & 1
    if pat == 1: return (r + c) % 3 == 0
    if pat == 2: return ((r * 5 + c * 3) >> 2) & 1
    return ((r * r + c * c) >> 4) & 1


def px(pat, r, c):
    if pat == 0: return 0x38 + ((r + c) & 7)
    if pat == 1: return 0x30 + ((r ^ c) & 0xf)
    if pat == 4: return 0x40 + r * 4 + c
    return (r * 4 + c) & 0xff


MONO = [(16, 112, 8, 8, 0, 0x3c, 0x01, 0), (40, 112, 13, 9, 0, 0x3f, 0, 1), (64, 112, 32, 16, 0, 0x30, 0x3e, 2),
        (112, 120, 64, 64, 0, 0x3c, 0, 3), (200, 112, 8, 8, 1, 0x3f, 0, 0), (224, 112, 13, 9, 1, 0x38, 0, 1),
        (248, 112, 32, 16, 1, 0x3c, 0, 2), (296, 120, 64, 64, 1, 0x3f, 0, 3)]
HCOL = [(560, 112, 4, 2, 4), (400, 112, 8, 8, 0), (424, 112, 32, 16, 1), (472, 120, 64, 64, 2)]
REST = [0x1000, 0x210b, 0x3004, 0x410b, 0x5004, 0x61e4, 0x713c, 0x8280, 0x9300, 0xa000, 0xb0a5, 0xe0ff]


def _wl(mono):
    """(23C2h list, 23C4h list), restated from the S5 table and the word format"""
    P = 640
    d2, d4 = [], []
    for op in (MONO if mono else HCOL):
        x, y, w, h = op[:4]
        d = y * P + x
        d2 += [0x2000, 0x3000, 0x4000 | d & 0xfff, 0x5000 | d >> 12, 0x8000 | P, 0x6000 | w, 0x7000 | h,
               0xe0ff, 0x9300]
        if mono:
            t, fg, bg, pat = op[4:]
            d2 += [0x1008 if t else 0x1000, 0xa000 | fg, 0xb000 | bg, 0x090e]
            for r in range(h):
                for k in range((w + 3) // 4):
                    d4.append(sum(8 >> q for q in range(4) if 4 * k + q < w and bit(pat, r, 4 * k + q)))
        else:
            pat = op[4]
            d2 += [0x1000, 0x0902]
            for r in range(h):
                for k in range((w + 3) // 4 * 2):
                    c = 2 * k
                    d4.append((px(pat, r, c) if c < w else 0) | ((px(pat, r, c + 1) << 8) if c + 1 < w else 0))
    return d2 + (REST if mono else [v for v in REST if v not in (0xa000, 0xb0a5)]), d4





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
    real = build_mod(a.kdir, os.path.join(TOP, "out/qemus5/real"), False)
    fake = build_mod(a.kdir, os.path.join(TOP, "out/qemus5/fake"), True)
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
    initrd = os.path.join(a.logdir, "qemu-wd24adv-s5-initrd")
    with open(initrd, "wb") as f:
        core = open(a.core, "rb").read()
        f.write(core + b"\0" * ((4 - len(core) % 4) % 4) + newc(ents))
    tracef = os.path.join(a.logdir, "qemu-wd24adv-s5-trace.log")
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

    print("== wd24adv S5 QEMU test: -M isapc -cpu 486 -vga std")
    g = Guest(cmd, os.path.join(a.logdir, "qemu-wd24adv-s5-serial.log"))
    try:
        g.expect("WD24-INIT-UP", 300)
        time.sleep(2)
        g.send("stty -echo; PS1='# '\r")
        time.sleep(1)
        g.run("true", 30)
        g.run("dmesg -n 1; dmesg -c >/dev/null")

        for v, name, nops in ((1, "S5 MONO", 8), (2, "S5 HOSTCOL", 4)):
            out, acc = step(g, f"insmod /wd24/wd24adv.ko s5={v}; echo RC=$?; dmesg -c")
            check(f"s5={v} (dry-run) loads, zero port accesses", "RC=0" in out and len(acc) == 0, f"{len(acc)}")
            pl, _ = g.run("cat /proc/wd24adv/s5")
            check(f"s5={v} dry-run: plan lists {nops} ops + word counts", f"{name} plan" in pl and f"op {nops}:" in pl
                  and "23C4h only owed words" in pl)
            out, acc = step(g, "rmmod wd24adv; echo RC=$?")
            check(f"s5={v} dry-run: rmmod clean, zero accesses", "RC=0" in out and len(acc) == 0)

        t0 = tr.settle()
        for args, want in [("dryrun=0 s5=1", "s5=1 with dryrun=0 needs live=1"), ("dryrun=0 s5=1 live=1", "needs live=1"),
                           ("dryrun=0 s5=2 live=1 mode=640x480-4", "s5=2 with dryrun=0 needs live=1"),
                           ("s5=3", "must be 1"), ("s5=-1", "must be 1"),
                           ("s5=1 s1=1", "separate stage"), ("s5=1 s2=1", "separate stage"),
                           ("s5=1 s3=1", "separate stage"), ("s5=1 s4=1", "separate stage")]:
            out, _ = g.run(f"insmod /wd24/wd24adv.ko {args}; echo RC=$?; dmesg -c")
            check(f"refused: {args}", "RC=0" not in out and want in out and not loaded(g),
                  next((l for l in out.splitlines() if "wd24adv:" in l), "").strip())
        tr.settle()
        check("S5 refusals: zero port accesses", len(tr.acc) == t0, f"{len(tr.acc) - t0}")

        out, acc = step(g, "insmod /wd24/wd24adv.ko dryrun=0 live=1 mode=640x480-8 s5=1; echo RC=$?; dmesg -c")
        seqlog.append("# real wd24adv.ko: live s5=1\n" + out)
        ext = [x for x in acc if x[1] in EXT]
        check("real .ko live s5=1: ZERO 23Cxh accesses", len(ext) == 0, f"{len(ext)}")
        vd = vga_data_writes(acc)
        check("real .ko live s5=1: ZERO VGA data writes", len(vd) == 0, ", ".join(fmt(x) for x in vd) or "none")
        if loaded(g):
            g.run("rmmod wd24adv")

        for v, name, nops in ((1, "S5 MONO", 8), (2, "S5 HOSTCOL", 4)):
            out, acc = step(g, f"insmod /wd24/wd24adv-qemufake.ko dryrun=0 s5={v}; echo RC=$?; dmesg -c", 300)
            res, _ = g.run("cat /proc/wd24adv/s5")
            seqlog.append(f"\n# TEST-ONLY wd24adv-qemufake.ko: insmod dryrun=0 s5={v}\n" + out + "\n" + res)
            seqlog += [fmt(x) for x in acc if x[1] != 0x23c4]
            ext = [x for x in acc if x[1] in EXT]
            w2l, w4l = _wl(v == 1)
            check(f"fake s5={v}: insmod 0, result OK", "RC=0" in out and f"{name} result: OK" in res,
                  next((l for l in res.splitlines() if "result" in l), "").strip())
            check(f"fake s5={v}: {nops} ops VERIFIED", res.count("VERIFIED") == nops)
            check(f"fake s5={v}: all 23Cxh accesses 16-bit", all(x[2] == "w" for x in ext))
            w2 = [x[3] for x in ext if x[0] == "W" and x[1] == 0x23c2]
            check(f"fake s5={v}: 23C2h writes == the closed list ({len(w2l)})", w2 == w2l, f"{len(w2)}")
            w4 = [x[3] for x in ext if x[0] == "W" and x[1] == 0x23c4]
            check(f"fake s5={v}: 23C4h writes == the restated words ({len(w4l)})", w4 == w4l, f"{len(w4)}")
            check(f"fake s5={v}: no 23C4h read, nothing on 23C6h/23C7h",
                  not any(x[1] == 0x23c4 and x[0] == "R" for x in ext) and not any(x[1] >= 0x23c6 for x in ext))
            cur, last_r, bad, start = 0, None, 0, False
            for x in ext:   # every 23C4h word: 23C0h on 1001h, preceded by a 23C2h read with ACT
                if x[0] == "W" and x[1] == 0x23c0:
                    cur = x[3]
                elif x[1] == 0x23c2 and x[0] == "R":
                    last_r = x[3]
                elif x[0] == "W" and x[1] == 0x23c4:
                    bad += not (cur & 0xf0ff == 0x1001 and last_r is not None and last_r & 0x0800)
                    last_r = None
            check(f"fake s5={v}: each 23C4h word right after an ACT=1 read of CNTRL1", bad == 0, f"{bad}")
            w0 = [x[3] for x in ext if x[0] == "W" and x[1] == 0x23c0]
            check(f"fake s5={v}: 23C0h writes only selects, last 1002h",
                  w0 and w0[-1] == 0x1002 and all(x & 0xf0ff in (0x1001, 0x1002) for x in w0), f"{len(w0)}")
            check(f"fake s5={v}: ZERO VGA data writes", len(vga_data_writes(acc)) == 0)
            out, acc = step(g, "rmmod wd24adv_qemufake; echo RC=$?")
            check(f"fake s5={v}: rmmod clean, zero accesses", "RC=0" in out and len(acc) == 0)

        # HOSTCOL probe: engine takes 1 px/word -> 4 alternative words, HCOL_ALT, restored, rmmod OK
        hrest = [v for v in REST if v not in (0xa000, 0xb0a5)]
        out, acc = step(g, "insmod /wd24/wd24adv-qemufake.ko dryrun=0 s5=2 fake_hang=5; echo RC=$?; dmesg -c")
        res, _ = g.run("cat /proc/wd24adv/s5")
        seqlog.append("\n# fake_hang=5\n" + out + "\n" + res)
        ext = [x for x in acc if x[1] in EXT]
        check("fake_hang=5: HOSTCOL 1 PX/WORD, restored", "RC=0" in out and "HOSTCOL 1 PX/WORD" in res
              and "(equal)" in res, next((l for l in res.splitlines() if "result" in l), "").strip())
        w4 = [x[3] for x in ext if x[0] == "W" and x[1] == 0x23c4]
        check("fake_hang=5: 4 words then 4 alternative words", w4 == [0x4140, 0x4342, 0x4544, 0x4746,
              0x4444, 0x4545, 0x4646, 0x4747], " ".join(f"{v:04x}" for v in w4))
        check("fake_hang=5: report shows 4 alternative words", "probe alternative words: 4" in res)
        w2 = [x[3] for x in ext if x[0] == "W" and x[1] == 0x23c2]
        check("fake_hang=5: restore list after the stop", w2[-len(hrest):] == hrest)
        check("fake_hang=5: ZERO VGA data writes", len(vga_data_writes(acc)) == 0)
        out, acc = step(g, "rmmod wd24adv_qemufake; echo RC=$?")
        check("fake_hang=5: rmmod OK", "RC=0" in out and not loaded(g))

        # HOSTCOL bytes swapped: VRAM BAD at the probe with the class named, restored
        out, acc = step(g, "insmod /wd24/wd24adv-qemufake.ko dryrun=0 s5=2 fake_swap=1; echo RC=$?; dmesg -c")
        res, _ = g.run("cat /proc/wd24adv/s5")
        seqlog.append("\n# fake_swap=1\n" + out + "\n" + res)
        ext = [x for x in acc if x[1] in EXT]
        check("fake_swap=1: VRAM BAD, class bytes swapped, restored", "RC=0" in out and "VRAM" in res and
              "bytes swapped" in res and "(equal)" in res,
              next((l for l in res.splitlines() if "result" in l), "").strip())
        check("fake_swap=1: only the probe's 4 words", len([x for x in ext if x[0] == "W" and x[1] == 0x23c4]) == 4)
        out, acc = step(g, "rmmod wd24adv_qemufake; echo RC=$?")
        check("fake_swap=1: rmmod OK", "RC=0" in out and not loaded(g))

        # engine idle before all words: HOST DATA SHORT, restored, rmmod OK
        out, acc = step(g, "insmod /wd24/wd24adv-qemufake.ko dryrun=0 s5=1 fake_hang=3; echo RC=$?; dmesg -c")
        res, _ = g.run("cat /proc/wd24adv/s5")
        seqlog.append("\n# fake_hang=3\n" + out + "\n" + res)
        ext = [x for x in acc if x[1] in EXT]
        w4 = [x for x in ext if x[0] == "W" and x[1] == 0x23c4]
        check("fake_hang=3: HOST DATA SHORT, restored", "RC=0" in out and "S5 MONO result: HOST DATA SHORT" in res
              and "(equal)" in res, next((l for l in res.splitlines() if "result" in l), ""))
        check("fake_hang=3: exactly half of op 1's words sent (8)", len(w4) == 8, f"{len(w4)}")
        w2 = [x[3] for x in ext if x[0] == "W" and x[1] == 0x23c2]
        check("fake_hang=3: restore list after the stop", w2[-len(REST):] == REST)
        out, _ = g.run("rmmod wd24adv_qemufake; echo RC=$?")
        check("fake_hang=3: rmmod OK", "RC=0" in out and not loaded(g))

        # engine never idle after the words: TIMEOUT, LOCKED, pinned (last: stays loaded)
        out, acc = step(g, "insmod /wd24/wd24adv-qemufake.ko dryrun=0 s5=1 fake_hang=4; echo RC=$?; dmesg -c")
        res, _ = g.run("cat /proc/wd24adv/s5")
        seqlog.append("\n# fake_hang=4\n" + out + "\n" + res)
        check("fake_hang=4: TIMEOUT, STILL BUSY, LOCKED, power cycle required", "RC=0" in out and
              "S5 MONO result: TIMEOUT" in res and "STILL BUSY" in res and "LOCKED" in out and "power cycle required" in out)
        ext = [x for x in acc if x[1] in EXT]
        st = max(i for i, x in enumerate(ext) if x[0] == "W" and x[1] == 0x23c4)
        check("fake_hang=4: 16 words, then no 23Cxh write of any kind", not any(x[0] == "W" for x in ext[st + 1:])
              and sum(1 for x in ext if x[0] == "W" and x[1] == 0x23c4) == 16)
        check("fake_hang=4: ZERO VGA data writes", len(vga_data_writes(acc)) == 0)
        out, acc = step(g, "rmmod wd24adv_qemufake; echo RC=$?; dmesg -c")
        check("fake_hang=4: rmmod refused (pinned), zero accesses", "RC=0" not in out and loaded(g) and len(acc) == 0)
    except (TimeoutError, EOFError) as e:
        check("guest responded", False, str(e))
    finally:
        g.close()
        with open(os.path.join(a.logdir, "qemu-wd24adv-s5-sequence.txt"), "w") as f:
            f.write("\n".join(s.replace("\r", "") for s in seqlog) + "\n")
    bad = [d for d, ok in results if not ok]
    print(f"== wd24adv S5 QEMU test: {'PASS' if not bad else 'FAIL'} ({len(results) - len(bad)}/{len(results)})")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
