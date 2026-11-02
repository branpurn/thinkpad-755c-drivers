#!/usr/bin/env python3
"""QEMU system test of the SDL2 wdfb backend against the real wd90c24fb
module (fake=3: a RAM model of video memory, so `verify` shows whether every
changed pixel was sent with a DAMAGE op).

qemu-system-i386 -M isapc -cpu 486 boots the tc755 kernel with tc32's
core.xz plus a small overlay (modules, sdltest.i486, vtstate), runs a shell
on the serial port and checks: console keyboard (K_MEDIUMRAW), evdev
keyboard (grabbed), 1x / 2x / fullscreen, per-rect DAMAGE, the console
coming back in KD_TEXT/K_XLATE after exit and after a fatal signal, refusal
while another process holds /dev/fb0, and the 10 Hz fallback (xaccel=0).

  sdl/tests/qemu-wdfb.py [--kernel out/bzImage] [--modules out/modules]
                         [--core ../tinycore-32-ram/out/core.xz]
                         [--sdltest out/sdl/sdltest.i486] [--vtstate out/sdl/vtstate]
stdlib only. Serial log: out/test-logs/qemu-wdfb-serial.log
"""
import argparse, os, re, select, socket, subprocess, sys, time

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MODS = ["kernel/lib/fonts/font.ko", "kernel/drivers/video/fbdev/core/fb.ko",
        "kernel/drivers/video/fbdev/core/fb_sys_fops.ko", "kernel/drivers/video/fbdev/core/sysimgblt.ko",
        "kernel/drivers/video/vgastate.ko", "kernel/drivers/video/fbdev/core/sysfillrect.ko",
        "kernel/drivers/video/fbdev/core/syscopyarea.ko", "extra/wd90c24fb.ko",
        "kernel/drivers/input/evdev.ko"]

INIT = """#!/bin/sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev 2>/dev/null
/bin/busybox mkdir -p /tmp
export PATH=/usr/local/bin:/bin:/sbin:/usr/bin PS1= ENV=
echo WDFB-INIT-UP
exec sh -i </dev/ttyS0 >/dev/ttyS0 2>&1
"""


def newc(entries):
    """entries: (path, mode, data) -> uncompressed newc cpio bytes"""
    out = bytearray()
    ino = 1000
    for path, mode, data in entries + [("TRAILER!!!", 0, b"")]:
        name = path.encode() + b"\0"
        hdr = "070701" + "".join("%08X" % v for v in (
            ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name), 0))
        out += hdr.encode() + name
        out += b"\0" * ((4 - len(out) % 4) % 4)
        out += data
        out += b"\0" * ((4 - len(out) % 4) % 4)
        ino += 1
    return bytes(out)


class Guest:
    def __init__(self, cmd, logpath):
        self.log = open(logpath, "wb")
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, bufsize=0)
        self.buf = b""

    def _pump(self, timeout):
        r, _, _ = select.select([self.p.stdout], [], [], timeout)
        if r:
            d = os.read(self.p.stdout.fileno(), 65536)
            if not d:
                raise EOFError("qemu exited")
            self.buf += d
            self.log.write(d)
            self.log.flush()

    def expect(self, pattern, timeout):
        rx = re.compile(pattern.encode())
        end = time.time() + timeout
        while True:
            m = rx.search(self.buf)
            if m:
                out = self.buf[:m.end()]
                self.buf = self.buf[m.end():]
                return out.decode("latin-1"), m
            left = end - time.time()
            if left <= 0:
                raise TimeoutError(f"timeout waiting for {pattern!r}")
            self._pump(min(left, 0.5))

    def send(self, s):
        for ch in s.encode():
            self.p.stdin.write(bytes([ch]))
            self.p.stdin.flush()
            time.sleep(0.003)

    def run(self, cmd, timeout=120):
        tag = "__WDFB_%d__" % int(time.time() * 1000 % 1e9)
        self.send(f"{cmd}; echo {tag}=$?\r")
        out, m = self.expect(tag + r"=(\d+)\r?\n", timeout)
        body = out.rsplit(tag, 1)[0].replace("\r", "")
        return body.strip("\n"), int(m.group(1))

    def close(self):
        try:
            self.p.kill()
        except Exception:
            pass
        self.p.wait()
        self.log.close()


def main():
    tc32 = os.environ.get("TC32_DIR", os.path.join(os.path.dirname(TOP), "tinycore-32-ram"))
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default=os.path.join(TOP, "out/bzImage"))
    ap.add_argument("--modules", default=os.path.join(TOP, "out/modules"))
    ap.add_argument("--core", default=os.path.join(tc32, "out/core.xz"))
    ap.add_argument("--sdltest", default=os.path.join(TOP, "out/sdl/sdltest.i486"))
    ap.add_argument("--vtstate", default=os.path.join(TOP, "out/sdl/vtstate"))
    ap.add_argument("--logdir", default=os.path.join(TOP, "out/test-logs"))
    ap.add_argument("--mem", type=int, default=32)
    a = ap.parse_args()
    os.makedirs(a.logdir, exist_ok=True)

    kdirs = [d for d in os.listdir(os.path.join(a.modules, "lib/modules"))]
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
    for m in MODS:
        add_file(f"wdfb/{os.path.basename(m)}", open(os.path.join(md, m), "rb").read(), 0o100644)
    mb = os.path.join(TOP, "out/modutils/busybox")
    if not os.path.exists(mb):
        mb = os.path.join(os.path.dirname(a.kernel), "modutils/busybox")
    add_file("usr/local/bin/insmod", open(mb, "rb").read())
    add_file("usr/local/bin/sdltest", open(a.sdltest, "rb").read())
    add_file("usr/local/bin/vtstate", open(a.vtstate, "rb").read())
    initrd = os.path.join(a.logdir, "qemu-wdfb-initrd")
    with open(initrd, "wb") as f:
        core = open(a.core, "rb").read()
        f.write(core + b"\0" * ((4 - len(core) % 4) % 4) + newc(ents))

    mon = os.path.join(a.logdir, "qemu-wdfb-monitor.sock")
    if os.path.exists(mon):
        os.unlink(mon)
    cmd = ["qemu-system-i386", "-accel", "tcg", "-M", "isapc", "-cpu", "486", "-m", str(a.mem),
           "-kernel", a.kernel, "-initrd", initrd,
           "-append", "console=ttyS0 loglevel=3 rdinit=/wdfb-init",
           "-display", "none", "-serial", "stdio", "-monitor", f"unix:{mon},server,nowait",
           "-no-reboot"]
    results = []

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
        time.sleep(0.5)
        s.close()

    print(f"== wdfb QEMU test: -M isapc -cpu 486 -m {a.mem}, kernel {kver}, wd90c24fb fake=3")
    g = Guest(cmd, os.path.join(a.logdir, "qemu-wdfb-serial.log"))
    try:
        g.expect("WDFB-INIT-UP", 300)
        time.sleep(2)
        g.send("stty -echo; PS1='# '\r")
        time.sleep(1)
        g.run("true", 30)
        out, rc = g.run("for m in font fb fb_sys_fops sysimgblt vgastate sysfillrect syscopyarea; do "
                        "insmod /wdfb/$m.ko || echo FAIL $m; done; insmod /wdfb/wd90c24fb.ko fake=3; "
                        "cat /proc/fb; ls -l /dev/fb0", 180)
        check("wd90c24fb fake=3 loads, /dev/fb0", rc == 0 and "WD90C24" in out and "FAIL" not in out,
              out.splitlines()[0] if out else "")
        vt0, _ = g.run("vtstate /dev/tty1")
        check("tty1 before: KD_TEXT, K_XLATE or K_UNICODE", "kd=text" in vt0, vt0)

        def sdl_run(name, env, args, key=True, hold=True, extra_checks=()):
            log = f"/tmp/{name}.log"
            g.run(f"{env} sdltest {args}{' -p 60' if hold else ''} >{log} 2>&1 & echo $! >/tmp/pid", 30)
            end = time.time() + 300
            while time.time() < end:
                o, _ = g.run(f"cat {log}")
                if ("holding" in o if hold else "done" in o) or "sdltest: done" in o or "rror" in o:
                    break
                time.sleep(2)
            ver, _ = g.run("cat /sys/module/wd90c24fb/parameters/verify")
            st, _ = g.run("cat /sys/module/wd90c24fb/parameters/stats")
            if key:
                sendkey("q")
            g.run("wait", 120)
            o, _ = g.run(f"cat {log}")
            vt, _ = g.run("vtstate /dev/tty1")
            return o, ver, st, vt

        def summary(o):
            m = re.search(r"sdltest: (\d+) frames.*?([\d.]+) fps", o)
            return f"{m.group(1)} frames, {m.group(2)} fps" if m else "no summary"

        # 1: console keyboard (no evdev module yet), 1x centred 320x200, /dev/tty0 fallback
        o, ver, st, vt = sdl_run("con", "SDL_WDFB_DEBUG=1", "--frames 400")
        check("1x 320x200: wdfb driver, INDEX8 surface", "video driver wdfb" in o and "INDEX8" in o and "scale 1" in o)
        check("1x: verify 0 differing bytes while holding", re.search(r"(^|\s)0 differing", ver) is not None, ver.strip())
        check("1x: driver in client mode, per-rect damage", "client=1" in st,
              " ".join(x for x in st.split() if x.split("=")[0] in ("rects", "ioctls", "bytes", "full", "client")))
        check("console keyboard (K_MEDIUMRAW) delivers a key", "keyboard: console" in o and "key Q" in o)
        check("console restored after exit", "kd=text" in vt and vt.split("kb=")[-1] == vt0.split("kb=")[-1], vt)
        print("     ", summary(o))

        # 2: 2x scale
        o, ver, st, vt = sdl_run("x2", "SDL_WDFB_SCALE=2 SDL_WDFB_DEBUG=1 SDL_WDFB_TTY=/dev/tty1", "-w 320 -h 200 --frames 200")
        check("2x 320x200 -> 640x400: scale 2", "scale 2" in o and "surface 320x200" in o)
        check("2x: verify 0 differing bytes", re.search(r"(^|\s)0 differing", ver) is not None, ver.strip())
        check("2x: console restored", "kd=text" in vt, vt)
        print("     ", summary(o))

        # 3: fullscreen 640x480 with evdev keyboard
        out, rc = g.run("insmod /wdfb/evdev.ko; ls /dev/input/")
        o, ver, st, vt = sdl_run("ev", "SDL_WDFB_DEBUG=1", "-f -w 640 -h 480 --frames 200 -s 48")
        check("fullscreen 640x480 surface", "surface 640x480" in o)
        check("fullscreen: verify 0 differing bytes", re.search(r"(^|\s)0 differing", ver) is not None, ver.strip())
        check("evdev keyboard found, grabbed, delivers a key", "keyboard /dev/input/event" in o and "key Q" in o)
        check("evdev: console restored", "kd=text" in vt and vt.split("kb=")[-1] == vt0.split("kb=")[-1], vt)
        print("     ", summary(o))

        # 4: one DAMAGE op per rect
        o, _ = g.run("SDL_WDFB_DEBUG=2 sdltest --frames 5 2>&1 | grep -c 'DAMAGE'")
        n = int(re.findall(r"\d+", o)[-1]) if re.findall(r"\d+", o) else -1
        check("per-rect DAMAGE: 5 frames x 2 rects + initial", n >= 11, f"{n} DAMAGE ops")

        # 5: coexistence
        o, rc = g.run("sleep 600 </dev/fb0 & sp=$!; sleep 1; sdltest --frames 10 2>&1; r=$?; "
                      "SDL_WDFB_FORCE=1 sdltest --frames 10 >/dev/null 2>&1; f=$?; kill $sp; echo R=$r F=$f")
        check("refuses while another process holds /dev/fb0", "R=1" in o and "sleep" in o, o.splitlines()[0] if o else "")
        check("SDL_WDFB_FORCE=1 overrides", "F=0" in o)

        # 6: fatal signal restores the console
        g.run("SDL_WDFB_TTY=/dev/tty1 sdltest -t 600 >/tmp/sig.log 2>&1 & sp=$!; sleep 8; vtstate /dev/tty1; kill -SEGV $sp; wait $sp; echo rc=$?", 120)
        vt, _ = g.run("vtstate /dev/tty1")
        check("SIGSEGV: console restored", "kd=text" in vt and vt.split("kb=")[-1] == vt0.split("kb=")[-1], vt)

        # 7: xaccel=0 -> no ioctls, driver's 10 Hz flush
        o, rc = g.run("echo 0 >/sys/module/wd90c24fb/parameters/xaccel; SDL_WDFB_DEBUG=1 sdltest --frames 50 2>&1; "
                      "echo 1 >/sys/module/wd90c24fb/parameters/xaccel")
        check("xaccel=0: runs without the ioctls (caps 0)", "caps 0x0" in o and "sdltest: done" in o)
    except (TimeoutError, EOFError) as e:
        check("guest responded", False, str(e))
    finally:
        g.close()
    bad = [d for d, ok in results if not ok]
    print(f"== wdfb QEMU test: {'PASS' if not bad else 'FAIL'} ({len(results) - len(bad)}/{len(results)})")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
