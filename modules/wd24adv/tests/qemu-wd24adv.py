#!/usr/bin/env python3
"""QEMU system test of wd24adv.ko against QEMU's standard VGA (-vga std).

qemu-system-i386 -M isapc -cpu 486 boots the tc755 kernel with tc32's
core.xz plus an overlay (wd24adv.ko, wd90c24fb.ko and its fb modules), runs
a shell on the serial port and records every VGA port access with QEMU's
vga_std_read_io / vga_std_write_io trace events.  It checks:

  * dry-run load (the default): loads, /proc/wd24adv/{plan,modes,selftest},
    selftest ok, ZERO VGA port accesses of any kind, rmmod clean;
  * parameter gates: live=1 alone, dryrun=0 alone, trial=0, trial=600,
    allow_routing=1, allow_crt=1, mode=<unproven>, mode= in dry-run: refused;
  * dryrun=0 live=1 on QEMU std VGA: refused by the read-only entry check
    (not a WD90C24: CR34 does not read A0, PR18/PR19 do not read C7/54),
    with ZERO writes to any VGA data register (3C2, 3C5, 3CF, 3D5, 3B5,
    attribute data on 3C0, 3C6, 3C7/3C8/3C9) in the trace: only index
    registers are written and are put back to their entry values;
  * coexistence: refused while wd90c24fb (fake=3) is loaded; then
    wd90c24fb text=1 + rmmod work (the live-test hand-over, QEMU only).

  modules/wd24adv/tests/qemu-wd24adv.py [--kernel out/bzImage]
      [--modules out/modules] [--core ../tinycore-32-ram/out/core.xz]
stdlib only.  Logs: out/test-logs/qemu-wd24adv-{serial,trace}.log and
out/test-logs/qemu-wd24adv-live-trace.txt (the VGA accesses of the live
insmod, annotated).
"""
import argparse, os, re, sys, time

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(TOP, "sdl/tests"))
import importlib.util
_spec = importlib.util.spec_from_file_location("qwdfb", os.path.join(TOP, "sdl/tests/qemu-wdfb.py"))
qwdfb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(qwdfb)
newc, Guest = qwdfb.newc, qwdfb.Guest

FBMODS = ["kernel/lib/fonts/font.ko", "kernel/drivers/video/fbdev/core/fb.ko",
          "kernel/drivers/video/fbdev/core/fb_sys_fops.ko", "kernel/drivers/video/fbdev/core/sysimgblt.ko",
          "kernel/drivers/video/vgastate.ko", "kernel/drivers/video/fbdev/core/sysfillrect.ko",
          "kernel/drivers/video/fbdev/core/syscopyarea.ko", "extra/wd90c24fb.ko"]

INIT = """#!/bin/sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev 2>/dev/null
/bin/busybox mkdir -p /tmp
export PATH=/usr/local/bin:/bin:/sbin:/usr/bin PS1= ENV=
echo WD24-INIT-UP
exec sh -i </dev/ttyS0 >/dev/ttyS0 2>&1
"""

RX = re.compile(r"vga_std_(read|write)_io addr 0x([0-9a-f]+), val 0x([0-9a-f]+)")
INDEX_PORTS = {0x3c4: "SR idx", 0x3ce: "GR idx", 0x3d4: "CR idx", 0x3b4: "CR idx (mono)"}
DATA_PORTS = {0x3c2: "MISC", 0x3c5: "SR data", 0x3cf: "GR data", 0x3d5: "CR data", 0x3b5: "CR data (mono)",
              0x3c6: "PEL mask", 0x3c7: "DAC read idx", 0x3c8: "DAC write idx", 0x3c9: "DAC data",
              0x3da: "feature ctl", 0x3ba: "feature ctl (mono)"}


def trace_lines(path):
    with open(path, "r", errors="replace") as f:
        return [l for l in f if "vga_std_" in l]


def analyse(lines):
    """-> (annotated text, accesses, data_writes, index_writes); 3C0 via the AR flip-flop"""
    ff, out, data_w, idx_w, n = False, [], [], [], 0
    for l in lines:
        m = RX.search(l)
        if not m:
            continue
        n += 1
        rw, port, val = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
        if rw == "read":
            if port in (0x3da, 0x3ba):
                ff = False
            out.append(f"R {port:03X} -> {val:02X}")
            continue
        if port == 0x3c0:
            kind = "AR data" if ff else "AR idx"
            ff = not ff
        elif port in INDEX_PORTS:
            kind = INDEX_PORTS[port]
        else:
            kind = DATA_PORTS.get(port, "other")
        (data_w if kind not in ("AR idx",) + tuple(INDEX_PORTS.values()) else idx_w).append((port, val, kind))
        out.append(f"W {port:03X} <- {val:02X}  {kind}")
    return "\n".join(out), n, data_w, idx_w


def main():
    tc32 = os.environ.get("TC32_DIR", os.path.join(os.path.dirname(TOP), "tinycore-32-ram"))
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default=os.path.join(TOP, "out/bzImage"))
    ap.add_argument("--modules", default=os.path.join(TOP, "out/modules"))
    ap.add_argument("--core", default=os.path.join(tc32, "out/core.xz"))
    ap.add_argument("--logdir", default=os.path.join(TOP, "out/test-logs"))
    ap.add_argument("--mem", type=int, default=32)
    a = ap.parse_args()
    os.makedirs(a.logdir, exist_ok=True)

    kdirs = os.listdir(os.path.join(a.modules, "lib/modules"))
    if len(kdirs) != 1:
        sys.exit(f"expected one kernel under {a.modules}/lib/modules, found {kdirs}")
    kver = kdirs[0]
    md = os.path.join(a.modules, "lib/modules", kver)
    ents, dirs = [], set()

    def add_file(p, data, mode=0o100755):
        parts = os.path.dirname(p).strip("/").split("/")
        for i in range(1, len(parts) + 1):
            d = "/".join(parts[:i])
            if d and d not in dirs:
                dirs.add(d)
                ents.append((d, 0o040755, b""))
        ents.append((p.strip("/"), mode, data))

    add_file("wd24-init", INIT.encode())
    for m in FBMODS + ["extra/wd24adv.ko"]:
        add_file(f"wd24/{os.path.basename(m)}", open(os.path.join(md, m), "rb").read(), 0o100644)
    bb = open(os.path.join(TOP, "out/modutils/busybox"), "rb").read()
    for t in ("insmod", "rmmod"):
        add_file(f"usr/local/bin/{t}", bb)
    initrd = os.path.join(a.logdir, "qemu-wd24adv-initrd")
    with open(initrd, "wb") as f:
        core = open(a.core, "rb").read()
        f.write(core + b"\0" * ((4 - len(core) % 4) % 4) + newc(ents))

    tracef = os.path.join(a.logdir, "qemu-wd24adv-trace.log")
    if os.path.exists(tracef):
        os.unlink(tracef)
    cmd = ["qemu-system-i386", "-accel", "tcg", "-M", "isapc", "-cpu", "486", "-m", str(a.mem),
           "-vga", "std", "-kernel", a.kernel, "-initrd", initrd,
           "-append", "console=ttyS0 loglevel=3 rdinit=/wd24-init",
           "-display", "none", "-serial", "stdio", "-no-reboot",
           "-trace", "vga_std_write_io", "-trace", "vga_std_read_io", "-D", tracef]
    results = []

    def check(desc, ok, detail=""):
        results.append((desc, bool(ok)))
        print(f"  [{'PASS' if ok else 'FAIL'}] {desc}" + (f"  ({detail})" if detail else ""), flush=True)
        return ok

    def settle():
        """wait until the trace file stops growing; -> its VGA lines"""
        last = -1
        for _ in range(20):
            time.sleep(0.5)
            n = os.path.getsize(tracef) if os.path.exists(tracef) else 0
            if n == last:
                break
            last = n
        return trace_lines(tracef) if os.path.exists(tracef) else []

    print(f"== wd24adv QEMU test: -M isapc -cpu 486 -vga std, kernel {kver}")
    g = Guest(cmd, os.path.join(a.logdir, "qemu-wd24adv-serial.log"))
    try:
        g.expect("WD24-INIT-UP", 300)
        time.sleep(2)
        g.send("stty -echo; PS1='# '\r")
        time.sleep(1)
        g.run("true", 30)
        g.run("dmesg -n 1; dmesg -c >/dev/null")

        # 1: dry-run load (default): no VGA port access at all
        t0 = len(settle())
        out, rc = g.run("insmod /wd24/wd24adv.ko; echo RC=$?")
        tr = settle()[t0:]
        check("dry-run load (default) succeeds", "RC=0" in out, out)
        check("dry-run load: zero VGA port accesses (reads or writes)", len(tr) == 0, f"{len(tr)} accesses")
        sel, _ = g.run("cat /proc/wd24adv/selftest | tail -1")
        check("dry-run: /proc/wd24adv/selftest all pass", re.search(r"\b0 fail", sel) is not None, sel.strip())
        pl, _ = g.run("grep -c '' /proc/wd24adv/plan; grep -c PROVEN /proc/wd24adv/modes; ls /proc/wd24adv")
        check("dry-run: plan/modes present, no live/control entries", "live" not in pl and "control" not in pl,
              " ".join(pl.split()))
        t0 = len(settle())
        out, rc = g.run("rmmod wd24adv; echo RC=$?; dmesg -c")
        tr = settle()[t0:]
        check("dry-run: rmmod clean, zero VGA port accesses", "RC=0" in out and len(tr) == 0, f"{len(tr)} accesses")

        # 2: parameter gates (all must fail before any hardware access)
        t0 = len(settle())
        for args, want in [("live=1", "needs dryrun=0"), ("dryrun=0", "without live=1"),
                           ("dryrun=0 live=1 trial=0", "trial=0 refused"),
                           ("dryrun=0 live=1 trial=600", "trial=600 refused"),
                           ("allow_routing=1", "refused"), ("allow_crt=1", "refused"),
                           ("dryrun=0 live=1 mode=nosuchmode", "not a PROVEN"),
                           ("mode=640x480-8", "needs dryrun=0 live=1")]:
            out, rc = g.run(f"insmod /wd24/wd24adv.ko {args}; echo RC=$?; dmesg -c")
            check(f"refused: {args}", "RC=0" not in out and want in out and "wd24adv" not in g.run("cat /proc/modules")[0],
                  next((l for l in out.splitlines() if "wd24adv:" in l), "").strip())
        tr = settle()[t0:]
        check("parameter refusals: zero VGA port accesses", len(tr) == 0, f"{len(tr)} accesses")

        # 3: live on QEMU std VGA: refused by the entry check, zero data writes
        t0 = len(settle())
        out, rc = g.run("insmod /wd24/wd24adv.ko dryrun=0 live=1; echo RC=$?; dmesg -c")
        tr = settle()[t0:]
        text, n, dw, iw = analyse(tr)
        with open(os.path.join(a.logdir, "qemu-wd24adv-live-trace.txt"), "w") as f:
            f.write("# VGA port accesses of: insmod wd24adv.ko dryrun=0 live=1 (QEMU -vga std)\n")
            f.write(out.replace("\r", "") + "\n# trace:\n" + text + "\n")
        print("      dmesg:", " | ".join(l.strip() for l in out.splitlines() if "wd24adv" in l))
        print("      trace:", text.replace("\n", "; "))
        check("live=1 on std VGA: insmod fails", "RC=0" not in out)
        check("live=1: refused by the entry check", "REFUSED by the entry check" in out or "entry check" in out,
              next((l for l in out.splitlines() if "entry check" in l), "").strip())
        check("live=1: the entry check did read the chip (trace not empty)", n > 0, f"{n} accesses")
        check("live=1: ZERO VGA data-register writes in the trace", len(dw) == 0,
              ", ".join(f"{p:03X}<-{v:02X} {k}" for p, v, k in dw) or "none")
        restored = True
        for port in INDEX_PORTS:
            ws = [v for p, v, k in iw if p == port]
            rs = [int(m.group(3), 16) for m in (RX.search(l) for l in tr)
                  if m and m.group(1) == "read" and int(m.group(2), 16) == port]
            if ws and (not rs or ws[-1] != rs[0]):
                restored = False
        check("live=1: index registers written back to their entry values", restored,
              f"{len(iw)} index writes")
        check("live=1: module not loaded afterwards", "wd24adv" not in g.run("cat /proc/modules")[0])
        check("live=1: backend counted 0 data writes", re.search(r"port data writes 0\b", out) is not None,
              next((l for l in out.splitlines() if "port data writes" in l), "").strip())

        # 4: coexistence with wd90c24fb, then the text=1 + rmmod hand-over
        out, rc = g.run("for m in font fb fb_sys_fops sysimgblt vgastate sysfillrect syscopyarea; do "
                        "insmod /wd24/$m.ko || echo FAIL $m; done; insmod /wd24/wd90c24fb.ko fake=3; echo RC=$?", 180)
        if check("wd90c24fb fake=3 loads", "RC=0" in out and "FAIL" not in out):
            for args in ("", "dryrun=0 live=1"):
                out, rc = g.run(f"insmod /wd24/wd24adv.ko {args}; echo RC=$?; dmesg -c")
                check(f"refused while wd90c24fb is loaded: '{args or 'dry-run'}'",
                      "RC=0" not in out and "wd90c24fb is loaded" in out)
            out, rc = g.run("echo 1 >/sys/module/wd90c24fb/parameters/text; sleep 2; rmmod wd90c24fb; echo RC=$?; "
                            "grep -c wd90c24fb /proc/modules")
            check("wd90c24fb text=1 then rmmod (QEMU, fake=3 only)", "RC=0" in out and out.strip().endswith("0"),
                  " ".join(out.split()))
            out, rc = g.run("insmod /wd24/wd24adv.ko; echo RC=$?; rmmod wd24adv; echo RM=$?")
            check("after wd90c24fb rmmod: wd24adv dry-run loads and unloads", "RC=0" in out and "RM=0" in out)
    except (TimeoutError, EOFError) as e:
        check("guest responded", False, str(e))
    finally:
        g.close()
    bad = [d for d, ok in results if not ok]
    print(f"== wd24adv QEMU test: {'PASS' if not bad else 'FAIL'} ({len(results) - len(bad)}/{len(results)})")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
