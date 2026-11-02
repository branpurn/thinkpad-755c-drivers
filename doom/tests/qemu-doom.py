#!/usr/bin/env python3
"""QEMU system test of doom-wdfb against the real wd90c24fb module (fake=3:
a RAM model of video memory, so `verify` shows whether the frame on screen
matches what Doom drew).

qemu-system-i386 -M isapc -cpu 486 -m 36 boots the tc755 kernel with tc32's
core.xz plus a small overlay (modules, doom-wdfb, fbdump, vtstate). The
IWADs go on a FAT disk image (-hda) mounted read-only, as on the 755C. Checks:
start-up to the title and demo (screenshots as PNG), 1x direct and 2x, the
console keyboard (K_MEDIUMRAW) and evdev driving the menu to quit, the
console coming back after quit, I_Error and a fatal signal, refusal while
another process holds /dev/fb0, then -timedemo demo1..3 per IWAD.

  doom/tests/qemu-doom.py --iwad DOOM.WAD [--iwad freedoom1.wad ...]
                          [--kernel out/bzImage] [--modules out/modules]
                          [--core ../tinycore-32-ram/out/core.xz]
                          [--shots DIR] [--no-timedemo]
No WAD ships with the repo: bring your own. Needs mtools and Pillow (PNG).
Serial log: out/test-logs/qemu-doom-serial.log. QEMU's 486 is not a 755C:
the timedemo figures show only that it works, not how fast.
"""
import argparse, os, re, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                                "sdl", "tests"))
qw = __import__("qemu-wdfb")   # Guest, newc, MODS
TOP = qw.TOP

INIT = qw.INIT.replace("WDFB-INIT-UP", "DOOM-INIT-UP")


def fat_image(path, files):
    """FAT32 superfloppy (no partition table) holding files, via mtools"""
    size = sum(os.path.getsize(f) for f in files) + (16 << 20)
    sectors = (size // 512 // 1008 + 1) * 1008          # 16 heads x 63 sectors
    with open(path, "wb") as f:
        f.truncate(sectors * 512)
    subprocess.run(["mformat", "-i", path, "-F", "-h", "16", "-s", "63", "-t", str(sectors // 1008), "::"],
                   check=True)
    for f in files:
        subprocess.run(["mcopy", "-i", path, f, "::" + os.path.basename(f)], check=True)


def parse_dump(text):
    """fbdump output -> (w, h, palette bytes, pixel bytes) or None"""
    m = re.search(r"FBDUMP (\d+) (\d+)\nPAL ([0-9a-f]{1536})\n", text)
    if not m:
        return None
    w, h = int(m.group(1)), int(m.group(2))
    rows = dict((int(y), bytes.fromhex(hx)) for y, hx in re.findall(r"^R (\d+) ([0-9a-f]+)$", text, re.M))
    if len(rows) != h or any(len(r) != w for r in rows.values()):
        return None
    return w, h, bytes.fromhex(m.group(3)), b"".join(rows[y] for y in range(h))


def save_png(dump, path):
    w, h, pal, pix = dump
    try:
        from PIL import Image
    except ImportError:
        path = path[:-4] + ".ppm"
        with open(path, "wb") as f:
            f.write(b"P6 %d %d 255\n" % (w, h) + b"".join(pal[p * 3:p * 3 + 3] for p in pix))
        return path
    im = Image.frombytes("P", (w, h), pix)
    im.putpalette(pal)
    im.save(path)
    return path


def stats(dump, x0, y0, x1, y1):
    """distinct colours inside the box, and whether everything outside is
    index 0 (black in PLAYPAL 0; the pain and pick-up palettes tint it like
    the VGA overscan border under DOS)"""
    w, h, pal, pix = dump
    inside, outside_black = set(), True
    for y in range(h):
        row = pix[y * w:(y + 1) * w]
        if y0 <= y < y1:
            inside.update(row[x0:x1])
            out = row[:x0] + row[x1:]
        else:
            out = row
        if outside_black and out.strip(b"\0"):
            outside_black = False
    rgb = set(pal[p * 3:p * 3 + 3] for p in inside)
    return len(rgb), outside_black


def main():
    tc32 = os.environ.get("TC32_DIR", os.path.join(os.path.dirname(TOP), "tinycore-32-ram"))
    ap = argparse.ArgumentParser()
    ap.add_argument("--iwad", action="append", required=True, help="IWAD to test (repeatable); first one drives the UI tests")
    ap.add_argument("--kernel", default=os.path.join(TOP, "out/bzImage"))
    ap.add_argument("--modules", default=os.path.join(TOP, "out/modules"))
    ap.add_argument("--core", default=os.path.join(tc32, "out/core.xz"))
    ap.add_argument("--doom", default=os.path.join(TOP, "out/doom/doom-wdfb"))
    ap.add_argument("--fbdump", default=os.path.join(TOP, "out/doom/fbdump"))
    ap.add_argument("--vtstate", default=os.path.join(TOP, "out/sdl/vtstate"))
    ap.add_argument("--logdir", default=os.path.join(TOP, "out/test-logs"))
    ap.add_argument("--shots", default=os.path.join(TOP, "out/test-logs/doom-shots"))
    ap.add_argument("--mem", type=int, default=36)
    ap.add_argument("--no-timedemo", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.logdir, exist_ok=True)
    os.makedirs(a.shots, exist_ok=True)
    if not shutil.which("mformat"):
        sys.exit("needs mtools (mformat, mcopy)")

    kdirs = os.listdir(os.path.join(a.modules, "lib/modules"))
    if len(kdirs) != 1:
        sys.exit(f"expected one kernel under {a.modules}/lib/modules, found {kdirs}")
    kver = kdirs[0]
    md = os.path.join(a.modules, "lib/modules", kver)
    ents, dirs = [], set()

    def add_dir(p):
        parts = p.strip("/").split("/")
        for i in range(1, len(parts) + 1):
            d = "/".join(parts[:i])
            if d not in dirs:
                dirs.add(d)
                ents.append((d, 0o040755, b""))

    def add_file(p, data, mode=0o100755):
        add_dir(os.path.dirname(p))
        ents.append((p.strip("/"), mode, data))

    add_file("wdfb-init", INIT.encode())
    for m in qw.MODS:
        add_file(f"wdfb/{os.path.basename(m)}", open(os.path.join(md, m), "rb").read(), 0o100644)
    mb = os.path.join(TOP, "out/modutils/busybox")
    if not os.path.exists(mb):
        mb = os.path.join(os.path.dirname(a.kernel), "modutils/busybox")
    add_file("usr/local/bin/insmod", open(mb, "rb").read())
    for name, path in (("doom", a.doom), ("fbdump", a.fbdump), ("vtstate", a.vtstate)):
        add_file(f"usr/local/bin/{name}", open(path, "rb").read())
    initrd = os.path.join(a.logdir, "qemu-doom-initrd")
    with open(initrd, "wb") as f:
        core = open(a.core, "rb").read()
        f.write(core + b"\0" * ((4 - len(core) % 4) % 4) + qw.newc(ents))
    disk = os.path.join(a.logdir, "qemu-doom-wads.img")
    fat_image(disk, a.iwad)
    wads = [os.path.basename(w) for w in a.iwad]

    mon = os.path.join(a.logdir, "qemu-doom-monitor.sock")
    if os.path.exists(mon):
        os.unlink(mon)
    cmd = ["qemu-system-i386", "-accel", "tcg", "-M", "isapc", "-cpu", "486", "-m", str(a.mem),
           "-kernel", a.kernel, "-initrd", initrd,
           "-drive", f"file={disk},format=raw,if=ide,index=0,snapshot=on",
           "-append", "console=ttyS0 loglevel=3 rdinit=/wdfb-init",
           "-display", "none", "-serial", "stdio", "-monitor", f"unix:{mon},server,nowait",
           "-no-reboot"]
    results, timedemo = [], []

    def check(desc, ok, detail=""):
        results.append((desc, bool(ok)))
        print(f"  [{'PASS' if ok else 'FAIL'}] {desc}" + (f"  ({detail})" if detail else ""), flush=True)
        return ok

    def sendkey(k):
        s = socket.socket(socket.AF_UNIX)
        s.connect(mon)
        s.settimeout(5)
        time.sleep(0.2)
        try:
            s.recv(4096)
        except Exception:
            pass
        s.sendall(f"sendkey {k}\n".encode())
        time.sleep(0.4)
        s.close()

    print(f"== doom-wdfb QEMU test: -M isapc -cpu 486 -m {a.mem}, kernel {kver}, wd90c24fb fake=3, IWADs {', '.join(wads)}")
    g = qw.Guest(cmd, os.path.join(a.logdir, "qemu-doom-serial.log"))

    def shot(name, box):
        out, rc = g.run("fbdump", 300)
        d = parse_dump(out.replace("\r", "") + "\n")
        if not d:
            check(f"{name}: frame buffer dump", False, "unparsable")
            return None, 0, False
        path = save_png(d, os.path.join(a.shots, f"{name}.png"))
        ncol, black = stats(d, *box)
        return path, ncol, black

    def wait_log(log, pattern, timeout):
        end = time.time() + timeout
        while time.time() < end:
            o, _ = g.run(f"cat {log}")
            if re.search(pattern, o):
                return o
            time.sleep(2)
        return o

    def verify():
        """stop Doom between frames and compare the shadow with 'video memory'"""
        best = ""
        for _ in range(5):
            v, _ = g.run("kill -STOP $(cat /tmp/pid); sleep 1; cat /sys/module/wd90c24fb/parameters/verify; "
                         "kill -CONT $(cat /tmp/pid)")
            best = v.strip()
            if re.search(r"(^|\s)0 differing", best):
                break
            time.sleep(0.7)
        return best

    def vt_ok(vt, vt0):
        return "kd=text" in vt and vt.split("kb=")[-1] == vt0.split("kb=")[-1]

    iwad = f"/mnt/{wads[0]}"
    box1 = (160, 140, 480, 340)
    box2 = (0, 40, 640, 440)
    try:
        g.expect("DOOM-INIT-UP", 300)
        time.sleep(2)
        g.send("stty -echo; PS1='# '\r")
        time.sleep(1)
        g.run("true", 30)
        out, rc = g.run("for m in font fb fb_sys_fops sysimgblt vgastate sysfillrect syscopyarea; do "
                        "insmod /wdfb/$m.ko || echo FAIL $m; done; insmod /wdfb/wd90c24fb.ko fake=3; "
                        "cat /proc/fb; mkdir -p /mnt; mount -t vfat -o ro,utf8 /dev/sda /mnt && ls -l /mnt; "
                        "cd /tmp", 180)
        check("wd90c24fb fake=3 loads, IWAD disk mounted", "WD90C24" in out and "FAIL" not in out
              and all(w in out for w in wads), out.splitlines()[0] if out else "")
        vt0, _ = g.run("vtstate /dev/tty1")
        check("tty1 before: KD_TEXT", "kd=text" in vt0, vt0)

        # 1: 1x direct, console keyboard (evdev not loaded yet)
        g.run(f"DOOM_WDFB_DEBUG=1 DOOM_WDFB_TTY=/dev/tty1 doom -iwad {iwad} >/tmp/d1.log 2>&1 & echo $! >/tmp/pid", 30)
        o = wait_log("/tmp/d1.log", r"fps|rror", 600)
        check("1x: starts, direct render into the shadow", "(direct)" in o and "damage ioctl" in o,
              next((l for l in o.splitlines() if "doom-wdfb: /dev" in l), ""))
        check("1x: console keyboard K_MEDIUMRAW", "console K_MEDIUMRAW" in o)
        p, ncol, black = shot("doom-1x-title", box1)
        check("1x: title screen drawn, border index 0", p and ncol >= 32 and black, f"{p}, {ncol} colours")
        time.sleep(15)
        p, ncol, black = shot("doom-1x-demo", box1)
        check("1x: demo playing, border index 0", p and ncol >= 32 and black, f"{p}, {ncol} colours")
        ver = verify()
        check("1x: screen matches what Doom drew (verify)", re.search(r"(^|\s)0 differing", ver), ver)
        st, _ = g.run("cat /sys/module/wd90c24fb/parameters/stats")
        check("1x: driver in client mode (damage ops)", "client=1" in st,
              " ".join(x for x in st.split() if x.split("=")[0] in ("rects", "ioctls", "full", "client")))
        sendkey("esc")
        time.sleep(3)
        p, ncol, _ = shot("doom-1x-menu", box1)
        print(f"      menu: {p}")
        for k in ("up", "ret"):            # main menu: up wraps to Quit Game
            sendkey(k)
            time.sleep(1.5)
        sendkey("y")
        o, rc = g.run("wait $(cat /tmp/pid); echo rc=$?; cat /tmp/d1.log", 120)
        check("console keys: Esc, Up, Enter, Y quit Doom (rc 0)", "rc=0" in o, o.splitlines()[-1] if o else "")
        vt, _ = g.run("vtstate /dev/tty1")
        check("console restored after quit", vt_ok(vt, vt0), vt)

        # 2: 2x with evdev
        g.run("insmod /wdfb/evdev.ko; ls /dev/input/")
        g.run(f"DOOM_WDFB_DEBUG=1 DOOM_WDFB_SCALE=2 DOOM_WDFB_TTY=/dev/tty1 doom -iwad {iwad} >/tmp/d2.log 2>&1 & "
              "echo $! >/tmp/pid", 30)
        o = wait_log("/tmp/d2.log", r"fps|rror", 600)
        check("2x: 640x400, back buffer", "640x400 at 0,40" in o and "(direct)" not in o)
        check("2x: evdev keyboard", "keyboard: 1 evdev" in o)
        time.sleep(12)
        p, ncol, black = shot("doom-2x-demo", box2)
        check("2x: picture, border index 0", p and ncol >= 32 and black, f"{p}, {ncol} colours")
        ver = verify()
        check("2x: verify", re.search(r"(^|\s)0 differing", ver), ver)
        sendkey("f10")
        time.sleep(2)
        sendkey("y")
        o, rc = g.run("wait $(cat /tmp/pid); echo rc=$?", 120)
        check("evdev keys: F10, Y quit Doom (rc 0)", "rc=0" in o, o.splitlines()[-1] if o else "")
        vt, _ = g.run("vtstate /dev/tty1")
        check("console restored after quit (evdev)", vt_ok(vt, vt0), vt)

        # 3: fatal signal, I_Error, refusal
        g.run(f"DOOM_WDFB_DEBUG=1 DOOM_WDFB_TTY=/dev/tty1 doom -iwad {iwad} >/tmp/d3.log 2>&1 & "
              "echo $! >/tmp/pid", 30)
        wait_log("/tmp/d3.log", r"fps|rror", 600)
        o, _ = g.run("vtstate /dev/tty1; kill -SEGV $(cat /tmp/pid); wait $(cat /tmp/pid); echo rc=$?", 60)
        vt, _ = g.run("vtstate /dev/tty1")
        check("SIGSEGV: console restored", "kd=graphics" in o and vt_ok(vt, vt0), f"{o.splitlines()[0]} -> {vt}")
        o, rc = g.run(f"DOOM_WDFB_TTY=/dev/tty1 doom -iwad {iwad} -playdemo nosuchdemo >/tmp/d4.log 2>&1; echo rc=$?; "
                      "tail -3 /tmp/d4.log", 600)
        vt, _ = g.run("vtstate /dev/tty1")
        check("I_Error (no such demo): message, console restored", "rc=" in o and "rc=0" not in o and vt_ok(vt, vt0),
              " / ".join(l for l in o.splitlines() if l.strip())[-120:])
        o, rc = g.run(f"sleep 600 </dev/fb0 & sp=$!; sleep 1; doom -iwad {iwad} >/tmp/d5.log 2>&1; echo rc=$?; "
                      "grep 'in use' /tmp/d5.log; "
                      "kill $sp; wait $sp 2>/dev/null; true")
        check("refuses while another process holds /dev/fb0", "in use by pid" in o and "rc=1" in o, o.splitlines()[-1] if o else "")

        # 4: timedemos
        if not a.no_timedemo:
            for w in wads:
                for demo in ("demo1", "demo2", "demo3"):
                    for scale in ("1", "2"):
                        o, rc = g.run(f"DOOM_WDFB_SCALE={scale} DOOM_WDFB_TTY=/dev/tty1 doom -iwad /mnt/{w} "
                                      f"-timedemo {demo} >/tmp/td.log 2>&1; grep -i 'timed\\|rror' /tmp/td.log", 1800)
                        m = re.search(r"timed (\d+) gametics in (\d+) realtics", o)
                        if m:
                            fps = int(m.group(1)) * 35 / int(m.group(2))
                            timedemo.append((w, demo, scale, int(m.group(1)), int(m.group(2)), fps))
                            print(f"      timedemo {w} {demo} {scale}x: {m.group(1)} gametics in "
                                  f"{m.group(2)} realtics = {fps:.1f} fps", flush=True)
                        else:
                            check(f"timedemo {w} {demo} {scale}x", False, o.strip()[-120:])
            vt, _ = g.run("vtstate /dev/tty1")
            check("timedemos ran, console restored", timedemo and vt_ok(vt, vt0), f"{len(timedemo)} runs")
    except (TimeoutError, EOFError) as e:
        check("guest responded", False, str(e))
    finally:
        g.close()
    bad = [d for d, ok in results if not ok]
    print(f"== doom-wdfb QEMU test: {'PASS' if not bad else 'FAIL'} ({len(results) - len(bad)}/{len(results)})")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
