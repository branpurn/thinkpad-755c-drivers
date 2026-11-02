#!/usr/bin/env python3
"""vbiostrace: run a VGA BIOS in a real-mode emulator (unicorn) and log every
I/O port access it makes for a list of INT 10h calls (mode sets, panel
services). Nothing here touches real hardware: the "VGA" is a register file
in Python that echoes indexed registers back, so the trace is exactly what
the BIOS *writes* for each mode, decoded into WD90C2x register names.

  vbiostrace.py --rom vrom.bin [--calls 0003,0012,0013,005c] [-o trace.txt]
  vbiostrace.py --sysbios sysbios.bin --lowmem lowmem.bin   (video BIOS that
      lives inside the system ROM: INT 10h entered via the dumped IVT)

Inputs are the files /proc/vromdump/* produces (stage0/README.md) or any
VGA option ROM (tested with SeaVGABIOS vgabios-isavga.bin). Output: a text
trace plus --json with every access, for diffing modes and BIOS versions.
"""
import argparse, hashlib, json, struct, sys
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR, UC_HOOK_INSN, UC_HOOK_MEM_WRITE
from unicorn.x86_const import *

# ---- WD90C2x register names (WD90C24A/A2 data sheet; see wd24adv README) ----
GR_NAMES = {0x09: "PR0A", 0x0A: "PR0B", 0x0B: "PR1", 0x0C: "PR2", 0x0D: "PR3", 0x0E: "PR4", 0x0F: "PR5"}
CR_NAMES = {0x29: "PR10", 0x2A: "PR11", 0x2B: "PR12", 0x2C: "PR13", 0x2D: "PR14", 0x2E: "PR15",
            0x2F: "PR16", 0x30: "PR17", 0x31: "PR18", 0x32: "PR19", 0x33: "PR1A", 0x34: "PR1B",
            0x3B: "PR36"}
SR_NAMES = {0x06: "PR20", 0x07: "PR21", 0x08: "PR22", 0x09: "PR23", 0x10: "PR30A", 0x11: "PR31",
            0x12: "PR32", 0x13: "PR33A", 0x14: "PR34A", 0x19: "PR57", 0x26: "PR58A", 0x27: "PR64",
            0x31: "PR68", 0x32: "PR69", 0x35: "PR68/69 unlock"}
PORT_NAMES = {0x3C2: "MISC", 0x3C3: "VGAEN", 0x46E8: "VGAEN46E8", 0x102: "POS102", 0x3C6: "DACMASK",
              0x3DA: "FCR/ST1", 0x3BA: "FCR/ST1(mono)", 0x23C0: "EXT-IDX", 0x23C2: "EXT-DATA",
              0x23C4: "BLT-DATA", 0xA875: "LB-UNLOCK", 0x2DF0: "LB0", 0x2DF1: "LB1", 0x2DF2: "LB2",
              0x402: "DEBUG", 0xE9: "DEBUG", 0x61: "SYSCTL", 0x80: "POST", 0x70: "CMOS-IDX", 0x71: "CMOS-DATA"}
VGA_ONLY = set(range(0x3B0, 0x3E0))
DEBUG_PORTS = {0x402, 0xE9}

LOW, ROMSEG = 0x7C00, 0xC000
STUBSEG, STUBOFF = 0xF000, 0xFE00          # IVT stubs (handled in Python)


def regname(kind, idx):
    n = {"SR": SR_NAMES, "GR": GR_NAMES, "CR": CR_NAMES}.get(kind, {}).get(idx)
    return f"{kind}{idx:02X}" + (f"/{n}" if n else "")


class FakeVGA:
    """Indexed VGA register file + WD90C24 quirks (ID readback), no rendering."""

    def __init__(self, chip, memk):
        self.chip = chip
        self.sr, self.gr, self.cr, self.ar = [0] * 256, [0] * 256, [0] * 256, [0] * 32
        self.idx = {"SR": 0, "GR": 0, "CR": 0}
        self.ar_idx, self.ar_ff = 0, 0
        self.misc, self.st1 = 0x67, 0
        self.dac_w, self.dac_r, self.dac_c, self.dac = 0, 0, 0, [0] * 768
        self.ext = {}
        self.ext_idx = 0
        if chip == "wd90c24":
            self.gr[0x0B] = 0xC0 if memk >= 1024 else 0x80     # PR1 b7:6 memory size

    def crt_port(self, p):
        return (p & 0x3F0) == (0x3D0 if self.misc & 1 else 0x3B0)

    def rd_index(self, kind):
        i, regs = self.idx[kind], getattr(self, kind.lower())
        v = regs[i]
        if self.chip == "wd90c24":
            if kind == "GR" and i == 0x0F:
                v = (v & 0x17) | 0x80
            if kind == "CR" and i in (0x36, 0x37) and self.cr[0x34] == 0:
                v = ord("2") if i == 0x36 else ord("4")
        return v

    def inb(self, p):
        if p == 0x3C0: return self.ar_idx
        if p == 0x3C1: return self.ar[self.ar_idx & 31]
        if p == 0x3C2: return 0x10                         # ST0: switch sense
        if p == 0x3C4: return self.idx["SR"]
        if p == 0x3C5: return self.rd_index("SR")
        if p == 0x3CE: return self.idx["GR"]
        if p == 0x3CF: return self.rd_index("GR")
        if p == 0x3CC: return self.misc
        if p == 0x3C6: return self.ext.get(0x3C6, 0xFF)
        if p == 0x3C7: return 0
        if p == 0x3C8: return self.dac_w
        if p == 0x3C9:
            v = self.dac[(self.dac_r * 3 + self.dac_c) % 768]
            self.dac_c += 1
            if self.dac_c == 3: self.dac_c, self.dac_r = 0, (self.dac_r + 1) & 255
            return v
        if p in (0x3D4, 0x3B4): return self.idx["CR"]
        if p in (0x3D5, 0x3B5): return self.rd_index("CR")
        if p in (0x3DA, 0x3BA):
            self.ar_ff = 0
            self.st1 += 1                                     # b0 display enable, b3 vretrace toggle
            return (self.st1 & 1) | (0x08 if self.st1 & 0x10 else 0)
        if p == 0x23C2: return self.ext.get(("blk", self.ext.get(0x23C0, 0)), 0)
        return self.ext.get(p, 0xFF)

    def outb(self, p, v):
        """Returns a decoded name for the trace."""
        if p == 0x3C0:
            if self.ar_ff == 0:
                self.ar_idx, self.ar_ff = v, 1
                return None
            self.ar[self.ar_idx & 31] = v
            self.ar_ff = 0
            return f"AR{self.ar_idx & 31:02X}" + ("" if self.ar_idx & 0x20 else " (PAS=0)")
        if p == 0x3C4: self.idx["SR"] = v; return None
        if p == 0x3CE: self.idx["GR"] = v; return None
        if p in (0x3D4, 0x3B4): self.idx["CR"] = v; return None
        if p == 0x3C5: self.sr[self.idx["SR"]] = v; return regname("SR", self.idx["SR"])
        if p == 0x3CF: self.gr[self.idx["GR"]] = v; return regname("GR", self.idx["GR"])
        if p in (0x3D5, 0x3B5): self.cr[self.idx["CR"]] = v; return regname("CR", self.idx["CR"])
        if p == 0x3C2: self.misc = v; return "MISC"
        if p == 0x3C7: self.dac_r, self.dac_c = v, 0; return "DAC-RIDX"
        if p == 0x3C8: self.dac_w, self.dac_c = v, 0; return "DAC-WIDX"
        if p == 0x3C9:
            self.dac[(self.dac_w * 3 + self.dac_c) % 768] = v
            self.dac_c += 1
            if self.dac_c == 3: self.dac_c, self.dac_w = 0, (self.dac_w + 1) & 255
            return "DAC"
        if p == 0x23C2:
            self.ext[("blk", self.ext.get(0x23C0, 0))] = v
        self.ext[p] = v
        return PORT_NAMES.get(p, f"port{p:04X}")


class Trace:
    def __init__(self, raw):
        self.raw, self.events, self.lines = raw, [], []
        self.dac_run = None
        self.dbg = None

    def debug(self, at, b):
        if self.dbg is None:
            self.flush_dac()
            self.dbg = [at, ""]
        if b == 0x0A:
            self.flush_dbg()
        else:
            self.dbg[1] += chr(b) if 32 <= b < 127 else f"\\x{b:02x}"

    def flush_dbg(self):
        if self.dbg is not None:
            self.lines.append(f"    {self.dbg[0]}  -- BIOS debug port: {self.dbg[1]!r}")
            self.dbg = None

    def flush_dac(self):
        if self.dac_run:
            start, n, cs = self.dac_run
            self.lines.append(f"    {cs}  DAC data x{n} (palette load from index {start:02X}h)")
            self.dac_run = None

    def add(self, ev, text, dac=None):
        self.flush_dbg()
        self.events.append(ev)
        if dac is not None and not self.raw:
            if self.dac_run: self.dac_run[1] += 1
            else: self.dac_run = [dac, 1, ev["at"]]
            return
        self.flush_dac()
        self.lines.append(text)


class Emu:
    def __init__(self, a):
        self.a = a
        self.vga = FakeVGA(a.chip, a.vram)
        self.mu = Uc(UC_ARCH_X86, UC_MODE_16)
        self.mu.mem_map(0, 0x110000)                         # 1 MiB + HMA wrap
        self.vram_writes = self.rom_writes = 0
        self.ints = []
        self.unknown_ports = {}
        self.trace = None
        self.mu.hook_add(UC_HOOK_INSN, self.h_in, None, 1, 0, UC_X86_INS_IN)
        self.mu.hook_add(UC_HOOK_INSN, self.h_out, None, 1, 0, UC_X86_INS_OUT)
        self.mu.hook_add(UC_HOOK_INTR, self.h_intr)
        self.mu.hook_add(UC_HOOK_MEM_WRITE, self.h_vram, None, 0xA0000, 0xBFFFF)
        self.mu.hook_add(UC_HOOK_MEM_WRITE, self.h_romw, None, 0xC0000, 0xFFFFF)
        self.stub_ints = set(range(256))
        self.setup_memory()

    # -- memory / IVT / BDA -------------------------------------------------
    def setup_memory(self):
        a, mu = self.a, self.mu
        for n in range(256):                                   # stub IVT: F000:FE00+n -> IRET
            mu.mem_write(n * 4, struct.pack("<HH", STUBOFF + n, STUBSEG))
        mu.mem_write(STUBSEG * 16 + STUBOFF, b"\xCF" * 256)
        bda = bytearray(256)
        struct.pack_into("<H", bda, 0x10, 0x0020 | (2 << 4))    # equipment: 80x25 colour
        struct.pack_into("<H", bda, 0x13, 640)                 # base memory KB
        bda[0x49] = 3; struct.pack_into("<H", bda, 0x4A, 80); struct.pack_into("<H", bda, 0x63, 0x3D4)
        bda[0x84] = 24; bda[0x85] = 16; bda[0x87] = 0x60; bda[0x88] = 0x09; bda[0x89] = 0x51
        mu.mem_write(0x400, bytes(bda))
        if a.sysbios:
            sb = open(a.sysbios, "rb").read()
            mu.mem_write(0x100000 - len(sb), sb)
        if a.lowmem:
            lm = open(a.lowmem, "rb").read()[:0x500]
            mu.mem_write(0, lm)
            keep = {int(x, 16) for x in a.real_ints.split(",") if x}
            self.stub_ints = set(range(256)) - keep
            for n in self.stub_ints:                           # stubs for everything else
                mu.mem_write(n * 4, struct.pack("<HH", STUBOFF + n, STUBSEG))
        if a.rom:
            rom = open(a.rom, "rb").read()
            self.rom = rom
            mu.mem_write(ROMSEG * 16, rom)
        mu.mem_write(STUBSEG * 16 + 0xFFF0, b"\xF4")          # reset vector: halt

    def vec(self, n):
        off, seg = struct.unpack("<HH", self.mu.mem_read(n * 4, 4))
        return seg, off

    # -- hooks ----------------------------------------------------------------
    def at(self):
        cs, ip = self.mu.reg_read(UC_X86_REG_CS), self.mu.reg_read(UC_X86_REG_IP)
        return f"{cs:04X}:{ip:04X}"

    def h_in(self, uc, port, size, ud):
        if size == 1:
            v = self.vga.inb(port)
        elif size == 2:
            v = self.vga.inb(port) | self.vga.inb(port + 1) << 8
        else:
            v = sum(self.vga.inb(port + i) << (8 * i) for i in range(4))
        if port not in VGA_ONLY and port not in PORT_NAMES:
            self.unknown_ports[port] = self.unknown_ports.get(port, 0) + 1
        if self.trace is not None and (self.a.reads or port not in (0x3DA, 0x3BA, 0x61)):
            if self.a.reads:
                self.trace.add({"at": self.at(), "op": "in", "port": port, "size": size, "val": v},
                               f"    {self.at()}  in  {port:04X} -> {v:0{size * 2}X}")
        if port == 0x61:
            self.p61 = getattr(self, "p61", 0) ^ 0x10                # refresh toggle for delays
            return self.p61
        return v

    def h_out(self, uc, port, size, val, ud):
        at = self.at()
        bs = [(val >> (8 * i)) & 0xFF for i in range(size)]
        for i, b in enumerate(bs):
            p = port + i
            name = self.vga.outb(p, b)
            if p not in VGA_ONLY and p not in PORT_NAMES:
                self.unknown_ports[p] = self.unknown_ports.get(p, 0) + 1
            if self.trace is None:
                continue
            ev = {"at": at, "op": "out", "port": p, "val": b, "reg": name}
            if p in DEBUG_PORTS and not self.a.raw:                # BIOS debug console (SeaBIOS 0x402)
                self.trace.events.append(ev)
                self.trace.debug(at, b)
                continue
            if name is None and not self.a.raw:
                self.trace.events.append(ev)
                continue
            if name == "DAC":
                self.trace.add(ev, "", dac=self.vga.dac_w)
                continue
            label = name or f"port{p:04X}"
            self.trace.add(ev, f"    {at}  {label:<14} := {b:02X}" + (f"   (out {port:04X},{val:0{size*2}X})" if self.a.raw else ""))

    def h_vram(self, uc, access, addr, size, value, ud):
        self.vram_writes += size

    def h_romw(self, uc, access, addr, size, value, ud):
        if not (STUBSEG * 16 + STUBOFF <= addr < STUBSEG * 16 + STUBOFF + 256):
            self.rom_writes += size

    def h_intr(self, uc, n, ud):
        mu = self.mu
        seg, off = self.vec(n)
        ax = mu.reg_read(UC_X86_REG_AX)
        if n in (0x06, 0x0D) and (seg, off) == (STUBSEG, STUBOFF + n):
            self.ints.append(f"CPU exception {n:02X}h at {self.at()}")
            mu.emu_stop(); return
        if (seg, off) == (STUBSEG, STUBOFF + n):              # Python-side BIOS service
            self.ints.append(f"INT {n:02X}h AX={ax:04X} BX={mu.reg_read(UC_X86_REG_BX):04X} "
                             f"CX={mu.reg_read(UC_X86_REG_CX):04X} DX={mu.reg_read(UC_X86_REG_DX):04X} from {self.at()} (stubbed)")
            if self.trace is not None:
                self.trace.flush_dac()
                self.trace.lines.append(f"    {self.at()}  -- {self.ints[-1]}")
            fl = mu.reg_read(UC_X86_REG_EFLAGS)
            if n in (0x15, 0x1A):                                  # unsupported: CF=1, AH=86h
                mu.reg_write(UC_X86_REG_AX, (ax & 0xFF) | 0x8600)
                fl |= 1
            mu.reg_write(UC_X86_REG_EFLAGS, fl)
            return
        # real-mode dispatch through the IVT (unicorn does not do it itself)
        sp = (mu.reg_read(UC_X86_REG_SP) - 6) & 0xFFFF
        ss = mu.reg_read(UC_X86_REG_SS)
        mu.mem_write(ss * 16 + sp, struct.pack("<HHH", mu.reg_read(UC_X86_REG_IP), mu.reg_read(UC_X86_REG_CS),
                                                mu.reg_read(UC_X86_REG_EFLAGS) & 0xFFFF))
        mu.reg_write(UC_X86_REG_SP, sp)
        mu.reg_write(UC_X86_REG_EFLAGS, mu.reg_read(UC_X86_REG_EFLAGS) & ~0x300)
        mu.reg_write(UC_X86_REG_CS, seg)
        mu.reg_write(UC_X86_REG_IP, off)

    # -- running --------------------------------------------------------------
    def run_stub(self, code, regs, label):
        mu = self.mu
        mu.mem_write(LOW, code + b"\xF4")
        end = LOW + len(code)
        for r, v in regs.items():
            mu.reg_write(r, v)
        mu.reg_write(UC_X86_REG_CS, 0); mu.reg_write(UC_X86_REG_DS, 0); mu.reg_write(UC_X86_REG_ES, 0)
        mu.reg_write(UC_X86_REG_SS, 0); mu.reg_write(UC_X86_REG_SP, 0x7000)
        self.trace, self.vram_writes, self.rom_writes, self.ints = Trace(self.a.raw), 0, 0, []
        err = None
        try:
            mu.emu_start(LOW, end, count=self.a.max_insns)
        except UcError as e:
            err = f"emulator error: {e} at {self.at()}"
        cs, ip = mu.reg_read(UC_X86_REG_CS), mu.reg_read(UC_X86_REG_IP)
        done = cs * 16 + ip == end
        if not done and not err:
            err = f"did not return within {self.a.max_insns} instructions (stopped at {self.at()})"
        if self.ints and self.ints[-1].startswith("CPU exception"):
            err = self.ints[-1]
        self.trace.flush_dac(); self.trace.flush_dbg()
        return {"label": label, "ok": done and not err, "error": err, "trace": self.trace,
                "ax": mu.reg_read(UC_X86_REG_AX), "bx": mu.reg_read(UC_X86_REG_BX),
                "cf": mu.reg_read(UC_X86_REG_EFLAGS) & 1, "vram_writes": self.vram_writes,
                "rom_writes": self.rom_writes, "ints": list(self.ints),
                "bda_mode": self.mu.mem_read(0x449, 1)[0]}

    def post(self):
        return self.run_stub(bytes([0x9A, 0x03, 0x00]) + struct.pack("<H", ROMSEG), {UC_X86_REG_AX: 0}, "POST (far call C000:0003)")

    def int10(self, ax, bx=0, cx=0, dx=0, es=0, di=0):
        if self.vec(0x10) == (STUBSEG, STUBOFF + 0x10):
            return {"label": f"INT 10h AX={ax:04X} BX={bx:04X}", "ok": False, "trace": Trace(self.a.raw),
                    "error": "INT 10h is not hooked (the ROM's POST declined this hardware, or wrong --lowmem)",
                    "ax": ax, "bx": bx, "cf": 0, "vram_writes": 0, "rom_writes": 0, "ints": [],
                    "bda_mode": self.mu.mem_read(0x449, 1)[0]}
        r = self.run_stub(b"\xCD\x10", {UC_X86_REG_AX: ax, UC_X86_REG_BX: bx, UC_X86_REG_CX: cx,
                                       UC_X86_REG_DX: dx, UC_X86_REG_DI: di}, f"INT 10h AX={ax:04X} BX={bx:04X}")
        return r

    def snapshot(self):
        v = self.vga
        return {"SR": list(v.sr), "GR": list(v.gr), "CR": list(v.cr), "AR": list(v.ar), "MISC": v.misc,
                "EXT": {f"{k:04X}": x for k, x in v.ext.items() if isinstance(k, int)}}


def regdump(s, prev=None):
    out = []
    for kind, n in (("SR", 0x40), ("GR", 0x10), ("CR", 0x40), ("AR", 0x15)):
        vals = s[kind][:n]
        cells = []
        for i, x in enumerate(vals):
            ch = prev is not None and prev[kind][i] != x
            cells.append(f"{x:02X}{'*' if ch else ' '}")
        for row in range(0, n, 16):
            out.append(f"    {kind}{row:02X}: " + " ".join(cells[row:row + 16]))
    out.append(f"    MISC: {s['MISC']:02X}   other ports: " + (", ".join(f"{k}={x:02X}" for k, x in sorted(s['EXT'].items())) or "none"))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", help="VGA option ROM image (loaded at C0000, POST run at C000:0003)")
    ap.add_argument("--sysbios", help="system BIOS image (loaded to end at 0xFFFFF), e.g. sysbios.bin")
    ap.add_argument("--lowmem", help="IVT+BDA snapshot (lowmem.bin): INT 10h is entered through its vector")
    ap.add_argument("--real-ints", default="10,6d,42,1f,43",
                    help="with --lowmem: vectors kept from the snapshot (hex list); all others are stubbed")
    ap.add_argument("--no-post", action="store_true", help="skip the option ROM POST call")
    ap.add_argument("--calls", default="0003,0012,0013,005c,005d,005e,005f,007f:0200,1a00",
                    help="comma list of INT 10h AX[:BX] values (hex) to call in order; default: modes 03/12/13, Paradise 5C-5F, Paradise 007F BH=02 status, 1A00 display combination")
    ap.add_argument("--vbe", action="store_true", help="also probe VBE 4F00 / 4F01 (mode 101h) / 4F02")
    ap.add_argument("--chip", default="wd90c24", choices=["wd90c24", "generic"],
                    help="register-file quirks: WD90C24 ID readback (CR36/37 '24', GR0F, GR0B mem size)")
    ap.add_argument("--vram", type=int, default=1024, help="KiB of video memory to report (GR0B)")
    ap.add_argument("--max-insns", type=int, default=20_000_000)
    ap.add_argument("--reads", action="store_true", help="also log port reads")
    ap.add_argument("--raw", action="store_true", help="log index-port writes and every DAC byte too")
    ap.add_argument("-o", "--output", help="text trace (default stdout)")
    ap.add_argument("--json", help="write all events + register snapshots as JSON")
    a = ap.parse_args()
    if not a.rom and not a.lowmem:
        ap.error("need --rom or --lowmem")
    emu = Emu(a)
    out, js = [], {"inputs": {}, "calls": []}
    for k in ("rom", "sysbios", "lowmem"):
        f = getattr(a, k)
        if f:
            d = open(f, "rb").read()
            js["inputs"][k] = {"file": f, "bytes": len(d), "sha256": hashlib.sha256(d).hexdigest()}
            out.append(f"# {k}: {f} ({len(d)} bytes, sha256 {hashlib.sha256(d).hexdigest()})")
    if a.rom:
        r = emu.rom
        ok = r[:2] == b"\x55\xAA"
        out.append(f"# option ROM header: {'55AA' if ok else 'NO 55AA'}, size byte {r[2]} = {r[2] * 512} bytes, "
                   f"checksum {'OK' if sum(r[:r[2] * 512]) & 0xFF == 0 else 'BAD'}")
        for tag in (b"IBM", b"WD", b"Western Digital", b"PARADISE", b"VGA"):
            i = r.find(tag)
            if i >= 0:
                s = r[i:i + 48].split(b"\0")[0]
                out.append(f"# string at +{i:04X}: {s.decode('latin-1', 'replace')!r}")
    out.append(f"# chip model: {a.chip}, {a.vram} KiB")
    calls = []
    if a.rom and not a.no_post:
        calls.append(("post", None))
    for c in a.calls.split(","):
        if not c: continue
        ax, _, bx = c.partition(":")
        calls.append(("int10", (int(ax, 16), int(bx or "0", 16))))
    if a.vbe:
        calls += [("int10", (0x4F00, 0)), ("int10", (0x4F01, 0)), ("int10", (0x4F02, 0x0101))]
    prev = emu.snapshot()
    summary = []
    for kind, arg in calls:
        if kind == "post":
            res = emu.post()
        else:
            ax, bx = arg
            # VBE info blocks go to 0000:0600
            res = emu.int10(ax, bx=bx, cx=0x0101 if ax == 0x4F01 else 0, di=0x600)
        snap = emu.snapshot()
        t = res["trace"]
        writes = [e for e in t.events if e["op"] == "out"]
        named = [e for e in writes if e.get("reg")]
        prs = sorted({e["reg"] for e in named if "/PR" in (e["reg"] or "") or (e["reg"] or "").startswith(("EXT", "LB", "VGAEN", "POS"))})
        out.append("")
        out.append(f"=== {res['label']}: {'OK' if res['ok'] else 'FAILED: ' + str(res['error'])}")
        out.append(f"    returned AX={res['ax']:04X} BX={res['bx']:04X} CF={res['cf']}  BDA mode={res['bda_mode']:02X}h  "
                   f"port writes={len(writes)}  VRAM bytes written={res['vram_writes']}  ROM-area writes={res['rom_writes']}")
        if prs:
            out.append("    WD extended registers touched: " + ", ".join(prs))
        out.extend(t.lines)
        out.append("    -- register file after this call (* = changed by it):")
        out.extend(regdump(snap, prev))
        js["calls"].append({"label": res["label"], "ok": res["ok"], "error": res["error"], "ax": res["ax"],
                            "bx": res["bx"], "cf": res["cf"], "bda_mode": res["bda_mode"],
                            "vram_writes": res["vram_writes"], "ints": res["ints"], "events": t.events,
                            "after": snap})
        summary.append(f"{res['label']:<34} {'OK ' if res['ok'] else 'FAIL'} writes={len(writes):<5} PR regs={len(prs)}")
        prev = snap
    if emu.unknown_ports:
        out.append("")
        out.append("# non-VGA ports accessed: " + ", ".join(f"{p:04X}x{n}" for p, n in sorted(emu.unknown_ports.items())))
    out.append("")
    out.append("# summary")
    out.extend("#   " + s for s in summary)
    text = "\n".join(out) + "\n"
    if a.output:
        open(a.output, "w").write(text)
    else:
        sys.stdout.write(text)
    if a.json:
        json.dump(js, open(a.json, "w"), indent=1)
    print("\n".join(summary), file=sys.stderr)
    sys.exit(0 if all(c["ok"] for c in js["calls"]) else 1)


if __name__ == "__main__":
    main()
