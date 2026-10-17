#!/usr/bin/env python3
"""tc755 QEMU test: boot the module-capable tc32 floppy, load every .tcz
from an IDE disk and check that each driver binds where QEMU has the device.

QEMU (-M isapc -cpu 486, TCG) provides: ISA VGA (vga16fb target), i8042 PS/2
mouse (psmouse), cs4231a ISA codec (stand-in for the 755C's CS4248: driven
by snd-ad1848 exactly as on the ThinkPad, and by snd-cs4231). It has NO
WD90C24 and NO i82365/Ricoh PCMCIA bridge; those extensions are checked to
load their core modules and fail cleanly on the missing hardware.
stdlib only. Logs + JSON + screenshot: out/test-logs/
"""
import argparse, json, os, re, select, socket, struct, subprocess, sys, time, zlib

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


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
            self.log.write(d); self.log.flush()

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

    def send(self, s, slow=False):
        for ch in s.encode():
            self.p.stdin.write(bytes([ch])); self.p.stdin.flush()
            time.sleep(0.02 if slow else 0.005)

    def run(self, cmd, timeout=120):
        tag = "__TC755_%d__" % int(time.time() * 1000 % 1e9)
        self.send(f"{cmd}; echo {tag}=$?\r")
        out, m = self.expect(tag + r"=(\d+)\r?\n", timeout)
        body = out.rsplit(tag, 1)[0]
        if body.startswith(cmd):
            body = body.split("\n", 1)[1] if "\n" in body else ""
        if body.startswith("# "):
            body = body[2:]
        return body.replace("\r", ""), int(m.group(1))

    def close(self):
        try:
            self.p.kill()
        except Exception:
            pass
        self.p.wait()
        self.log.close()


def parse_modules(text):
    return {m.group(1): int(m.group(2)) for m in re.finditer(r"^(\S+) (\d+) \d+ ", text, re.M)}


def ppm_to_png(src, dst):
    with open(src, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():]
    raw = b"".join(b"\0" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(dst, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    return w, h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mem", type=int, default=32)
    ap.add_argument("--img", default=os.path.join(TOP, "out/tc755.img"))
    ap.add_argument("--disk", default=os.path.join(TOP, "out/tcz-disk.img"))
    ap.add_argument("--logdir", default=os.path.join(TOP, "out/test-logs"))
    ap.add_argument("--boot-timeout", type=int, default=300)
    a = ap.parse_args()
    os.makedirs(a.logdir, exist_ok=True)
    name = f"m{a.mem}"
    mon = os.path.join(a.logdir, f"monitor-{name}.sock")
    if os.path.exists(mon):
        os.unlink(mon)
    cmd = ["qemu-system-i386", "-accel", "tcg", "-M", "isapc", "-cpu", "486", "-m", str(a.mem),
           "-drive", f"if=floppy,index=0,format=raw,file={a.img},readonly=on",
           "-drive", f"if=ide,index=0,format=raw,file={a.disk},snapshot=on",
           "-boot", "a", "-display", "none", "-serial", "stdio",
           "-monitor", f"unix:{mon},server,nowait", "-no-reboot",
           "-audiodev", "none,id=snd0", "-device", "cs4231a,audiodev=snd0"]
    results, metrics = [], {"mem": a.mem, "qemu": " ".join(cmd[1:])}

    def check(desc, ok, detail=""):
        results.append((desc, bool(ok), detail))
        print(f"  [{'PASS' if ok else 'FAIL'}] {desc}" + (f"  ({detail})" if detail else ""), flush=True)
        return ok

    def monitor(c):
        s = socket.socket(socket.AF_UNIX); s.connect(mon); s.settimeout(5)
        time.sleep(0.3); s.recv(4096)
        s.sendall((c + "\n").encode()); time.sleep(1.5)
        try:
            s.recv(4096)
        except Exception:
            pass
        s.close()

    print(f"== tc755 module test: -m {a.mem} -M isapc -cpu 486 (TCG) + cs4231a + IDE tcz disk")
    g = Guest(cmd, os.path.join(a.logdir, f"serial-{name}.log"))
    try:
        g.expect(r"boot: ", 60)
        # QEMU's cs4231a sits at 0x534/IRQ9/DMA3, not the 755C's 0x4E30/IRQ10/DMA0:
        # override snd-cs4248.tcz's defaults the same way a user would.
        g.send("tc32 floppy=0,thinkpad tce=sda1 snd_ad1848.port=0x534 snd_ad1848.irq=9 snd_ad1848.dma1=3 snd_ad1848.thinkpad=0\r", slow=True)
        out, _ = g.expect(r"tc32: ready\.", a.boot_timeout)
        check("module-capable kernel boots to rcS", True)
        g.expect(r"root@box:[^\n#]*# ", 60)
        g.run("stty -echo 2>/dev/null; export PS1='# '", 20)
        g.expect(r"# ", 10)
        g.run("dmesg -n 1")   # keep printk off the serial shell; read it with dmesg
        metrics["uname"] = g.run("uname -r")[0].strip()
        check("tc755 kernel has its own vermagic (uname -r 6.18.57-tc755)", metrics["uname"].endswith("-tc755"), metrics["uname"])
        o, rc = g.run("cat /proc/modules | wc -l; cat /proc/sys/kernel/modprobe 2>/dev/null; ls /sys/module | wc -l")
        check("kernel has module support (/proc/modules present, empty)", rc == 0 and o.split()[0] == "0", " ".join(o.split()))

        def free_kb():
            o, _ = g.run("awk '/MemFree/{f=$2} /^Cached/{c=$2} END{print f, c}' /proc/meminfo")
            f, c = o.split()[:2]
            return int(f), int(c)
        base = free_kb(); metrics["memfree_cached_kb_baseline"] = base
        T = "/mnt/sda1/tce/optional"
        mem = {}

        def tce(ext, expect_ok=True):
            before = free_kb()
            o, rc = g.run(f"tce-load -i {T}/{ext}.tcz 2>&1", 300)
            o2, _ = g.run(f"cat /proc/modules")
            after = free_kb()
            mods = parse_modules(o2)
            mem[ext] = {"memfree_delta_kb": before[0] - after[0], "rc": rc, "output": o.strip()}
            return o, rc, mods

        o, rc, _ = tce("modutils")
        o2, rc2 = g.run("lsmod && readlink /usr/local/sbin/insmod")
        check("modutils.tcz: insmod/lsmod available", rc == 0 and rc2 == 0 and "Module" in o2 and "usr/local/sbin/insmod" in o2, " ".join(o2.split()))

        o, rc, mods = tce("wd90c24-probe")
        d, _ = g.run("dmesg | grep wd90c24 | tail -2")
        check("wd90c24-probe.tcz: out-of-tree module builds+loads, reports no WD chip in QEMU",
              "wd90c24:" in d and ("no Paradise" in d or "Paradise/WD extended" in d), " | ".join(l.split("] ")[-1] for l in d.strip().splitlines()))
        metrics["wd90c24_probe"] = d.strip()

        o, rc, mods = tce("fb-vga16")
        o2, _ = g.run("cat /proc/fb; ls /dev/fb0; dmesg | grep -iE 'vga16fb|frame buffer|tc32-vgafb|Console: switching' | tail -5")
        check("fb-vga16.tcz: vga16fb bound via tc32-vgafb-dev, /dev/fb0 + fbcon",
              rc == 0 and "VGA16" in o2 and "/dev/fb0" in o2, " | ".join(l.strip() for l in o2.strip().splitlines()[:4]))
        o3, rc3 = g.run("cat /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel")
        check("framebuffer is 640x480x4", "640,480" in o3 and o3.split()[-1] == "4", " ".join(o3.split()))
        g.run("clear > /dev/tty1; printf 'tc755: vga16fb + fbcon on tty1 (640x480, 80x30)\\n' > /dev/tty1; sleep 2")
        try:
            ppm = os.path.join(a.logdir, f"screen-vga16fb-{name}.ppm")
            monitor(f"screendump {ppm}")
            w, h = ppm_to_png(ppm, ppm[:-4] + ".png"); os.unlink(ppm)
            check("screendump of fbcon", (w, h) == (640, 480), f"{w}x{h} -> {os.path.basename(ppm[:-4])}.png")
        except Exception as e:
            check("screendump of fbcon", False, str(e))

        o, rc, mods = tce("trackpoint")
        o2, _ = g.run("ls /dev/input/; grep -A1 -i 'mouse' /proc/bus/input/devices | grep N: ")
        check("trackpoint.tcz: psmouse bound to i8042 AUX, /dev/input/mice", rc == 0 and "mice" in o2,
              " ".join(o2.split()))

        def play(label):
            irq0, _ = g.run("awk '$1==\"9:\"{print $2}' /proc/interrupts")
            o, rc = g.run("dd if=/dev/urandom of=/dev/dsp bs=1024 count=24 2>&1 | tail -1", 120)
            irq1, _ = g.run("awk '$1==\"9:\"{print $2}' /proc/interrupts")
            n0, n1 = int(irq0.strip() or 0), int(irq1.strip() or 0)
            check(f"{label}: 24 KiB to /dev/dsp (OSS emu), DMA3/IRQ9 interrupts", rc == 0 and n1 > n0,
                  f"{o.strip()}; IRQ9 {n0}->{n1}")

        o, rc, mods = tce("snd-cs4248")
        o2, _ = g.run("cat /proc/asound/cards; dmesg | grep -iE 'ad1848|wss|cs42' | tail -3")
        check("snd-cs4248.tcz: snd-ad1848 (the 755C driver) binds QEMU cs4231a", rc == 0 and re.search(r"^ ?0 \[", o2, re.M),
              " | ".join(l.strip() for l in o2.strip().splitlines()[:3]))
        play("snd-ad1848")
        o, rc = g.run("rmmod snd_pcm_oss snd_ad1848 && rmmod snd_wss_lib; lsmod | grep -c snd_ad1848")
        check("rmmod snd-ad1848 (MODULE_UNLOAD)", o.strip().endswith("0"), "")

        o, rc, mods = tce("snd-cs4231")
        o2, _ = g.run("cat /proc/asound/cards")
        check("snd-cs4231.tcz: snd-cs4231 binds QEMU cs4231a", rc == 0 and "CS4231" in o2.upper(), " ".join(o2.split()))
        play("snd-cs4231")

        o, rc, mods = tce("pcmcia")
        check("pcmcia.tcz: core modules load; i82365 finds no bridge in QEMU (expected)",
              "pcmcia_core" in mods and "pcmcia" in mods and "i82365" not in mods,
              " | ".join(l.strip() for l in o.strip().splitlines()[-5:]))

        o, _ = g.run("cat /proc/modules")
        metrics["modules"] = parse_modules(o)
        metrics["module_bytes_total"] = sum(metrics["modules"].values())
        end = free_kb(); metrics["memfree_cached_kb_end"] = end
        metrics["per_extension"] = mem
        print(f"  modules resident: {len(metrics['modules'])}, {metrics['module_bytes_total']} bytes; MemFree {base[0]} -> {end[0]} kB")

        g.send("poweroff\r")
        try:
            g.expect(r"(Power down|System halted)", 90)
            check("clean poweroff", True)
        except (TimeoutError, EOFError) as e:
            check("clean poweroff", False, str(e))
    except (TimeoutError, EOFError) as e:
        check("harness step", False, str(e))
        print(g.buf.decode("latin-1")[-800:])
    finally:
        g.close()
        if os.path.exists(mon):
            os.unlink(mon)
    ok = bool(results) and all(r[1] for r in results)
    metrics["pass"] = ok
    metrics["results"] = [{"check": d, "pass": p, "detail": x} for d, p, x in results]
    with open(os.path.join(a.logdir, f"result-{name}.json"), "w") as f:
        json.dump(metrics, f, indent=2)
    print(f"== -m {a.mem}: {'PASS' if ok else 'FAIL'} ({sum(r[1] for r in results)}/{len(results)} checks)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
