# wd24adv desktop acceleration: design

Status: **design, approved by the owner for commit (2026-11-02)**. Stage S0
(host model only) is the first stage to be implemented; no live code exists
for anything in this document.
It does not change wd24adv's proven state (5767c74) and does not touch
wd90c24fb. Every stage in §7 needs Brandon's explicit approval before it
runs on the 755C.

Target: a fast, ordinary X desktop (TinyX Xfbdev + jwm + rxvt) on the IBM
ThinkPad 755C. The chip is a WD90C24A with 1 MB VRAM, a 64 KiB banked window
at A0000h and no linear aperture. The kernel is 6.18.57-tc755x on a 486 with
36 MB RAM. We take the *architecture* of a good native driver like Windows
9x VGAC24.DRV as the model. Matching its numbers is not a goal.

Provenance rule: this document only describes what the Windows driver
**does**, at the level of "the BitBlt path uses the engine with a host-data
source". It holds no IBM code, no IBM register sequences and no IBM-derived
tables. The detailed evidence stays outside every git repo, in
`755c-wd90c24-research/`. The register meanings that wd24adv will use come
from the public WD90C31/C24 data books, XFree86, and live proofs on this
machine. As before, wd24adv shares no source, header, ioctl number or build
file with wd90c24fb. wd90c24fb is mentioned here only to say what has
already been seen to work on this hardware.

---

## 1. Hardware safety (comes before everything else)

The machine is irreplaceable, so every rule below binds every stage.

### 1.1 Never touched, at any stage

- No panel power: PR57 b2 and PR4 b5 (tri-state).
- No routing or panel control: PR18, PR19, PR1A, PR36, CR34 shadow CRTC,
  CR35 and CR3A.
- No PM ports 24h–27h, no 0D00h/0D01h and no CMOS.
- No clock registers: PR68 and PR69 stay behind `allow_clock=0`.
- No aperture: PR1 b5:4 stays at its BIOS value.
- No new display modes. Acceleration runs only in modes already proven live
  (640x480-8 first). It never changes CRTC timing, clocks or panel
  centring.
- No engine-reset experiments. If the engine misbehaves, the response is to
  stop using it (§1.5), never to poke undocumented reset bits.

The wd24adv register model already cannot express most of these: there is
no register file for PM ports, CMOS or 0D00h. The new register files below
leave that unchanged.

### 1.2 New registers enter only through the guard

wd24adv's `enum wd24_rf` has no way to reach 23C0h–23C5h today. The design
adds two register files and one port. Nothing else is added.

| New rf | Ports | Contents | Policy |
|---|---|---|---|
| `WD24_XB` (BITBLT) | 23C0h select = block 1, 23C2h data | Indices 0–Bh and Eh only (CNTRL1, CNTRL2, SRC, DST, DIM, PITCH, ROP, FG, BG, MASK). | Per-index value masks. CNTRL1 may only use a closed set of mode words. |
| `WD24_XC` (cursor) | 23C0h select = block 2, 23C2h data | Indices 0, 1, 2, 5, 6, 7 only | The pattern address must point at the reserved cursor slot (§4). |
| host data | 23C4h, 16-bit | Only inside an engine op that the kernel itself started with a host source | Exact word count (§1.4) |

- Any other index, any other block number, the "invalid block" encoding,
  or a value outside the masks is **refused by `wd24_guard()`**. A refusal
  logs `WD24_EV_REFUSED` and the operation does not start.
- Every sequence saves the 23C0h index-control value first and restores it
  at the end. The Windows driver does the same around its cursor code
  (§2), which is how it avoids corrupting a blit in flight.
- One spinlock, the existing `hw_lock`, covers every 23C0h/23C2h/23C4h
  sequence and every PR0A/PR0B bank write. That makes it impossible for the
  cursor and the blitter to interleave index selects.

### 1.3 Proof before use: model, then read-only, then readback

No new register is used live until it has passed, in order:

1. **The model** (box only). A host-side engine model in `tests/` (fill,
   copy forward/backward, mono expansion, ROP table, cursor registers).
   Every command the kernel can generate is checked against it, and the
   validator is fuzzed so that nothing outside the masks ever reaches
   `wd24_write()`.
2. **Read-only proof on the 755C.** Read blocks 1 and 2 (see stage S1). The
   only writes are 23C0h block selects, restored to the saved value. Proves
   that the registers can be read, which indices read back meaningfully,
   that busy reads idle, and that 23C0h restores exactly.
3. **Write-then-readback with the start bit clear** (stage S2). Every
   register the design uses is written and read back, then restored. ACT is
   never set at this stage.
4. **First engine use off screen**, on tiny rectangles. The result is
   checked against the model by reading VRAM back through the window.

Readback on every write stays on (`wd24_verify()`) for every register write
in every live-test stage. Two things can't be read back the normal way:

- CNTRL1 reads return busy status, not the written value. Its check is
  "busy within N µs, then idle within the timeout".
- The 23C4h data port is write-only. Its check is the exact word count,
  then idle, then a VRAM spot check against the model.

Whether to thin out readback once the default is on is a separate decision
for Brandon (§8). It is not part of this design.

### 1.4 What could stress the chip or the bus, and how each is contained

| Hazard | Mechanism | Containment |
|---|---|---|
| Blitter never goes idle (engine hang) | Bad parameters, a host-source op starved of data | The kernel computes the host word count from DIM and supplies exactly that many words, or never starts. Busy-wait has a bound derived from the op size (cap well under 0.2 s), and a timeout latches accel off until module reload (§1.5). |
| Bus lockup (IOCHRDY held, CPU frozen) | Host-port writes while the engine isn't accepting, CPU VRAM access during a blit (unknown on this chip) | Prevention only, because software can't recover from a frozen bus. Never write 23C4h unless the kernel started a host-source op. Wait for idle before any CPU VRAM access: the fault handler and every readback call `wait_idle` first. Use sizes ramped from 8x8 upward. Only use op types wd90c24fb has already run on this box first (fill, copy). Do mono-host ops in their own later stage. |
| Writes outside VRAM, or over the visible frame | Bad SRC/DST/DIM/PITCH | The kernel validator rejects any rectangle not wholly inside a known surface (frame or an offscreen allocation) within 1 MB. Early stages run in the scratch area only. |
| Overlapping copy corrupting the source | Wrong direction | The kernel, never userspace, picks forward or backward from the rectangle geometry. Checked in the model. |
| Stray hardware cursor over text mode | Enable bit left set | Every exit path clears cursor enable: revert, VT leave, close, module unload and the trial timer. `restore_all` checks it with a readback. |
| Panel stress | Timing, clock, routing or power changes | None of these are writable (§1.1). Engine and cursor work changes only VRAM contents and the overlay, which the panel sees as ordinary pixels. |
| Long CPU stalls with interrupts off | Spin-waiting under a spinlock | The busy-wait bound is small. Large ops are split into chunks of at most 64 KiB, with the lock dropped between chunks. |

### 1.5 Auto-revert and safe fallback at every stage

- **Live tests run under the proven trial mechanism.** With `trial=15`
  (default), the mode reverts to text after 15 s unless it is confirmed.
  The engine/cursor test runs inside that window and is followed by the
  proven 896/336 revert.
- **Accel latch.** Any timeout, an unexpected busy value or a readback
  mismatch sets `accel_dead`. From then on:
  - no more 23C0h–23C4h traffic except clearing cursor enable, and
  - X keeps running on the CPU path (§3.3), which needs no engine.

  The latch shows in `/proc/wd24adv/live` and in the event log, and clears
  only on module reload.
- **Process crash.** When the last close of the device happens (X crashed
  or was killed), the kernel waits for idle (bounded), clears the cursor,
  zaps mappings and restores text through the proven `restore_all`.
- **Kernel or bus hang.** There is no watchdog on a 755C, so the recovery is
  a power cycle, which the BIOS POST restores. That is exactly why §1.4
  relies on prevention, small steps and serial logging (`console=ttyS0` is
  already on the cmdline). Each disruptive step is logged with
  `/tmp/plog.sh` *before* it runs.

---

## 2. Evidence: what the Windows 9x driver accelerates

Source: disassembly of VGAC24.DRV (research tree, outside the repos). The
licence was accepted and the disassembly approved. The table counts how
often each port appears as an immediate in segment 1, then classifies
behaviour by export range. Note that research note §2's "23C0–23C5: none"
for VGAC24.DRV is **wrong**. The driver uses these ports heavily: 23C2h ×72,
23C0h ×50, 23C4h ×4 (plus `rep outsw` host streams).

| Operation | Evidence | Confidence |
|---|---|---|
| Screen-to-screen BitBlt | The BITBLT export path selects block 1, waits for idle through a shared wait routine (bounded poll), then programs SRC/DST/DIM/ROP and starts the engine | **High** |
| Raster ops | The ROP register is written with either a constant (SRC) or a value taken from the GDI ROP. Some paths chain several passes with different ROPs (XOR, AND etc.), which is how a 16-entry binary-ROP engine emulates ternary ROPs with a pattern | **High** (binary ROPs), **medium** (multi-pass ROP3) |
| Solid fills | Fixed-colour source with FG colour set. The opaque-rectangle part of ExtTextOut uses it | **High** |
| Mono-to-colour expansion from host data | The BitBlt helpers start a "packed, monochrome, host source" op, then stream the bitmap through 23C4h with 16-bit string output | **High** that the mechanism exists. **Medium** that it is used for patterns and mono bitmaps |
| Glyph drawing | No 23C4h use in the ExtTextOut range. The engine is used only for the opaque background, so glyphs appear to be CPU-drawn into banked VRAM | **Medium** |
| DIB / StretchBlt uploads | 23C2h/23C4h use and host streams in the StretchBlt/DIB range | **Medium** |
| Hardware cursor | SetCursor/MoveCursor/CheckCursor program block 2: pattern address, origin (hot spot), X/Y and enable. MoveCursor runs at interrupt time and saves/restores the index register | **High** |
| Lines / polygons (OUTPUT) | No 23Cxh access anywhere in the OUTPUT export range, so CPU-drawn | **Medium-high** |
| FastBorder | No 23Cxh access, so CPU | **Medium** |
| Offscreen caching (fonts/bitmaps) | Several blit paths compute VRAM addresses as y·1024 + x, which suggests a 1024-byte-pitch 2D layout. That leaves room right of and below a 640-wide screen for saved bits and cached bitmaps. Not confirmed: no proven offscreen source address was seen | **Low**. Open question Q3 |
| Mode set | ENABLE sets the mode through VESA INT 10h/4F02h (the VESA.EXE TSR), then initialises the blocks | **High** (not relevant: wd24adv sets proven modes itself) |

The architectural lesson matters more than any figure:

- **The Windows driver has no shadow framebuffer.** It draws into VRAM:
  - the engine handles copies, fills, the opaque parts of text, mono
    expansion and the cursor;
  - the CPU, banking its way through the window, handles everything else.
- **It never copies a whole frame**, so the cost of an operation is
  proportional to the pixels that operation changes.
- **The cursor is a hardware overlay.** Moving it never redraws pixels.

---

## 3. Target architecture

### 3.1 Choice of X server

| Option | Availability (Tiny Core 32-bit) | Verdict |
|---|---|---|
| TinyX Xfbdev (kdrive) | `Xfbdev.tcz` 1.3 in 16.x/x86. We build git `feab72ca` with patches 0001–0005 (gfx/versions.env) | **Use.** It's small, already runs on the 755C, and its single-threaded structure fits a synchronous engine. |
| Xorg (`xorg-server.tcz` 21.1.16) + `xf86-video-fbdev` 0.5.0 | Packaged in 16.x/x86 | Reject. Far too heavy for 36 MB. fbdev DDX is shadow-only with no accel. EXA would need a new DDX and assumes CPU-linear offscreen memory, which we don't have. |
| Xvesa | Packaged | Reject. It uses BIOS calls, has no guard, and only does a shadow or banked dumb framebuffer. |
| DRM/KMS driver | Kernel side | Reject. There is no generic 2D accel API. The dumb buffer + fbdev emulation path *is* a shadow copy. GEM/KMS cost RAM and code, and the 2D ioctls would be private anyway. |

TinyX's `kdrive/src` has no KAA (kaa.c/koffscreen.c are absent), so the
accel layer is new code in the X tree. It is a small screen/GC wrapper in
the style of XAA/KAA, in its own file (`kdrive/fbdev/wd24accel.c`) behind
its own patch series. It touches neither wd90c24fb's patches (0004/0005)
nor its ioctls. It doesn't use 0003's `fbBltHook` either: wd24adv wraps GC
ops instead, so it depends on nothing that exists for wd90c24fb. It turns
itself on only when the fb's `id` is `wd24adv` **and** the device reports
the accel caps.

### 3.2 Split between kernel and userspace

```
 jwm / rxvt / clients
        | X protocol
 Xfbdev (TinyX kdrive)
   wd24accel.c  GC/Screen wrapper: decides accel vs fallback, packs commands,
                tracks offscreen allocations, mirrors cursor
   fb/          software rendering for everything else (CPU path, §3.3)
        | write(2)/ioctl on /dev/fb0: batched command buffer   | mmap: VRAM
 wd24adv.ko (fbdev)                                            | (fault-banked)
   cmd validator  -> wd24_guard() -> wd24_write()+verify -> engine
   bank fault handler (PR0A, whitelisted, wait_idle first)
   cursor, VT/close handover, trial/revert, accel latch, event log
        |
 WD90C24A
```

- **The kernel owns every port and validates every command.** Userspace
  gets no `iopl`/`ioperm`. That costs a syscall per batch (a few µs on a
  486) and keeps a buggy X from writing routing, PM or panel registers.
  This is the difference from old XFree86/kdrive drivers, which poked MMIO
  directly.
- **The command interface lives in wd24adv's own namespace,
  `wd24adv_ioctl.h`.** 'W' 0x30 stays MODEINFO and **0x31 is never
  assigned**. Proposed numbers:

  | Number | Name | Purpose |
  |---|---|---|
  | 0x32 | `WD24ADV_IOC_CAPS` | Returns: a caps bitmask (ACCEL_FILL, ACCEL_COPY, ACCEL_MONO, ACCEL_HOSTCOL, HWCURSOR, BANKED_MMAP), VRAM size, frame size and pitch, the offscreen range, the max batch size, and `accel_dead`. |
  | 0x33 | `WD24ADV_IOC_SUBMIT` | A pointer and length for a batch of fixed-size commands plus inline host data. Returns the index of the first refused command. A refusal aborts the rest of the batch. |
  | 0x34 | `WD24ADV_IOC_SYNC` | Waits for idle (bounded). |
  | 0x35 | `WD24ADV_IOC_CURSOR` | Takes a pattern (32x32, 2 bpp, packed by the kernel from image+mask), hot spot, position and show/hide. |
  | 0x36 | `WD24ADV_IOC_OFFSCREEN` | Optional. The kernel hands out the offscreen range once. Userspace allocates inside it, but the kernel still bounds-checks every rectangle. |

- **Commands are geometric, not register writes:**
  - `FILL(dst rect, colour, rop)`
  - `COPY(src rect origin, dst rect, rop)`
  - `MONO(dst rect, fg, bg|transparent, rop, bitmap)`
  - `HOSTCOL(dst rect, rop, pixels)`
  - `HLINE`/`VLINE` as 1-pixel `FILL`s

  Rectangles name a **surface**: 0 = the visible frame, 1..n = offscreen
  slots from the CAPS range. The kernel turns each command into register
  values itself: address = base + y·pitch + x, it picks the direction, and
  it computes the host word count. So userspace can never produce an
  arbitrary register value.
- **One pitch.** Our reading of the data book is that the engine has a
  single PITCH register shared by source and destination (high
  confidence). So offscreen surfaces are 2D at the screen pitch (640):
  rows 480..1637 of the 1 MB (1024 KiB / 640 B = 1638 rows). Mono host
  data is packed and needs no pitch.

### 3.3 Avoiding the shadow-copy cost: direct banked VRAM, not a shadow

wd90c24fb renders everything into a RAM shadow, then pushes damage, or the
whole frame at 10 Hz, through the window. Even with blits mirrored, every
pixel the CPU draws is written twice, plus bookkeeping. Text, which is most
of what rxvt does, is CPU-drawn and pays the full upload.

Design for wd24adv:

1. **The screen pixmap is VRAM.** `mmap` of /dev/fb0 exposes 1 MB of
   virtual range. A fault handler maps the window's pages for the bank
   containing the faulting address:
   - it calls `wait_idle`, writes PR0A (whitelisted, already proven in
     banked modes), uses `vmf_insert_pfn` for the window's 16 pages, and
     zaps the previously mapped bank's PTEs in every VMA;
   - it is single-bank at first (PR0A only). The PR0A/PR0B dual-window
     read/write split is a later option (Q6).
   - So the fb software renderer draws straight into VRAM. The cost of an
     op is its own pixels plus about one fault per 64 KiB bank crossed
     (at pitch 640 a bank is about 102 lines).
2. **Hot ops never take the CPU path:**
   - copies and scrolls → COPY;
   - window backgrounds, borders and clears → FILL;
   - core text (ImageText and PolyText with the fixed font rxvt and jwm
     use) → a FILL for the opaque box, then glyph runs packed in RAM and
     sent as one MONO op through 23C4h. At 1 bit/pixel the host stream is
     8x smaller than CPU writes at 8 bpp, and it needs no banking;
   - XPutImage at 8 bpp → HOSTCOL.

   wd90c24fb found colour expansion "1.0x": with a shadow, the CPU still
   draws the shadow, so the expansion saved nothing. Without a shadow the
   saving is real. It must be measured (S8), not assumed.
3. **Mixing the two paths: the KAA MarkSync/WaitSync discipline.**
   - The wrapper calls SYNC before any fb fallback touches the screen or
     an offscreen surface.
   - The kernel also enforces it: the fault handler waits for idle, and
     SUBMIT zaps the current bank mapping first. So the CPU never touches
     VRAM while the engine runs, even if userspace forgets.
   - This matters for safety because CPU access during a blit is unproven
     on this chip (Q5).
4. **Fallback when accel is dead or absent.** Everything still works
   through the fault-banked CPU path. There is no shadow and no timer
   copy, so `accel_dead` costs speed and nothing else.

Kept as a fallback: if the fault-banked mmap proves too slow or unsafe in
S6, the alternative is a damage-tracked RAM shadow uploaded with HOSTCOL
blits rather than banked CPU copies. That is still wd24adv's own code, and
still cheaper than whole-frame copies.

### 3.4 Batching, queueing and sync

- **Userspace:** commands accumulate in a buffer (about 4 KiB of commands
  plus up to 32 KiB host data). It is flushed:
  - when it is full,
  - before any fallback (SYNC),
  - in the X BlockHandler (end of each request batch), and
  - before cursor updates that need ordering.
- **Kernel:** SUBMIT validates the *whole* batch first, then executes the
  commands one by one. For each command it takes `hw_lock`, saves 23C0h,
  waits for idle, programs the registers with readback, starts the engine,
  then restores 23C0h and drops the lock. There is no deep queue, because
  the chip has none worth relying on.
- **Waiting:** the kernel polls busy with a size-derived bound.
  - Ops that would exceed one 64 KiB chunk of destination are split by the
    kernel.
  - Polling uses `udelay` steps.
  - IRQ-driven completion isn't used, since the 755C engine IRQ is
    unknown (Q7).
- **Overlap of CPU and engine:** after starting an op the kernel can return
  before it finishes ("fire and forget"), and the next command or a fault
  waits first. That was the Windows driver's pattern: wait-before-program,
  not wait-after. Live stages S3–S5 use wait-after-every-op. Moving to
  wait-before is its own gate.

### 3.5 Hardware cursor

- A 32x32, 2 bpp pattern (256 B) sits at a reserved doubleword-aligned
  offscreen slot.
- Position, hot spot and enable go through `WD24ADV_IOC_CURSOR`.
- kdrive moves the cursor from its input handler, which in our case is a
  syscall rather than an interrupt. `hw_lock` serialises the cursor against
  blits, and the 23C0h save/restore (the behaviour the Windows driver
  needs at interrupt time) is kept as defence in depth.
- Cursors larger than 32x32 fall back to a software cursor.
- Every exit path disables the cursor (§1.4).

---

## 4. Offscreen VRAM map (640x480-8, pitch 640)

| Range (byte offsets) | Rows | Use |
|---|---|---|
| 00000h–4AFFFh (300 KiB) | 0–479 | Visible frame |
| 4B000h–4BFFFh | ~480–486 | Guard gap (never allocated) |
| 4C000h–4CFFFh | ~486–492 | Engine self-test / scratch (S3–S5). Kept for runtime self-test on load |
| 4D000h–4D0FFh | | Cursor pattern slot (256 B) |
| 4E000h–FFFFFh (~712 KiB) | ~499–1637 | 2D offscreen pool at pitch 640 for pixmaps, saved-under bits and (if Q2 allows) a mono glyph cache |

- The allocator is simple first fit over row bands, with LRU eviction back
  to RAM. Evicted pixmaps become ordinary fb pixmaps.
- Pixmaps are migrated to offscreen only when they're used as a blit source
  twice or more (for example jwm's title-bar or button pixmaps, and the
  backgrounds of tiled windows).
- **Text-mode coexistence.** The VGA text font and text live in planes
  0–2 of the first 256 KiB, which overlaps the frame and the start of the
  pool. wd24adv already saves and restores all 4 x 64 KiB planes around
  every mode set and revert (`WD24_HAVE_PLANES`). The offscreen content is
  therefore *volatile* across VT switches: X must treat the pool as lost on
  every VT enter and re-upload on demand.
- The other modes (640x400-8 and 320x200-8) get maps of the same shape from
  their frame size. Only 640x480-8 is in scope for the first desktop.

---

## 5. Console handover and crash recovery

- **The console stays vgacon.** wd24adv doesn't bind fbcon. That keeps the
  console on the proven text restore, avoids fbcon's banked redraw cost,
  and matches wd24adv's current requirement ("vgacon, no fbcon").
- **Opening /dev/fb0 and setting a proven mode** (`FBIOPUT_VSCREENINFO`,
  with the WD24ADV_STATUS_PROVEN modes only) runs the proven mode path,
  including trial semantics during testing.
- **VT leave**, through kdrive's fbdev LeaveVT and the kernel's `fb_release`
  or KD_TEXT path:
  1. SYNC;
  2. cursor off (readback);
  3. zap mappings;
  4. `restore_all` back to text.

  **VT enter:** mode set again, and the offscreen pool is marked lost.
- **X exit or crash:** the last close does the same sequence (§1.5).
- **Coexistence with wd90c24fb:** only one of the two drivers may be loaded,
  and the existing `also_conflict` check stays. The boot default stays
  wd90c24fb until stage S11.

---

## 6. What wd24adv needs that it doesn't have yet

1. An fbdev registration (`register_framebuffer`) for the proven 8 bpp
   modes, with `fb_mmap` → the fault-banked VMA (§3.3). No fbcon binding.
2. Register files `WD24_XB` and `WD24_XC`, their guard entries and masks,
   and a model in `tests/` (§1.2, §1.3).
3. A command validator and executor (§3.2, §3.4), plus `accel_dead`.
4. Cursor support (§3.5).
5. New ioctls in `wd24adv_ioctl.h` only (0x32–0x36).
6. On the X side: `wd24accel.c` plus its own patch series, enabled by fb id
   and caps.

---

## 7. Staged plan (each stage small, revertible and gated)

General rules for all stages:

- Brandon approves each stage before it touches the 755C.
- Each stage is a separate commit on wd24adv (code commits only when a stage
  is approved for implementation).
- Each stage is preceded by box tests (model, guard fuzz, PII scan) and a
  `/tmp/plog.sh` entry for every disruptive step.
- Stop at the first unexpected value and report.
- wd90c24fb is restored afterwards exactly as at boot.

| Stage | What runs on the 755C | Proves | Containment / revert |
|---|---|---|---|
| **S0** model (box only) | Nothing | The engine model, the guard masks for XB/XC, validator fuzzing, the address math and direction choice | n/a |
| **S1** read-only | wd24adv (live, text mode, no fb driver). Saves 23C0h, reads block 1 idx 0–Fh and block 2 idx 0–7, restores 23C0h. Run twice, compare | Registers are readable, which ones are stable, the busy bit is idle, 23C0h restores exactly. **The only writes are the 23C0h selects** | Nothing visible changes. Any oddity: stop |
| **S2** write + readback, no start | 640x480-8 under trial. Write each XB register except CNTRL1-start, plus XC position/address with enable **off**. Read back, then restore saved values | Per-register readback masks for §1.3, and that writes without ACT have no visible effect | Trial auto-revert at 15 s. Mismatch → `accel_dead`, revert |
| **S3** first blit, off screen | 640x480-8 under trial. FILL 8x8 in scratch at 4C000h, then 32x32, then COPY scratch → scratch. Wait after each op, VRAM readback vs model | The engine starts and stops within bound, and writes exactly the model's bytes and nothing outside | Screen untouched (the frame stays the test pattern). Timeout → latch + revert |
| **S4** on-screen fill/copy | Same, but FILL bars onto the frame, then forward and backward overlapping COPYs. Checksum vs model, and Brandon looks | Visible correctness, overlap direction, all ROPs used by X (copy, xor, and, or, invert, clear, set) | Trial revert |
| **S5** host data | MONO expansion (opaque, then transparent), then HOSTCOL, small to 64x64, exact word counts | The host port neither stalls nor locks the bus, the bit order and packing are right, transparency works | Trial revert. Run only after S3/S4 are clean, because this is the riskiest op type |
| **S6** fbdev + fault-banked mmap, **no accel** | Unaccelerated Xfbdev on wd24adv, started by hand (not via the boot autostart) | The CPU path is correct, there are no fault storms, and VT switch, exit and `kill -9` of the X PID all restore text | Last close → `restore_all`. wd90c24fb restored after |
| **S7** X accel: FILL/COPY | `wd24accel` with fill/copy only, behind an env flag (`WD24ACCEL=1`) | Real desktop correctness (moving and scrolling windows, jwm), and that SYNC discipline holds | Unset the flag → S6 behaviour. `accel_dead` → CPU path |
| **S8** X accel: text and PutImage | MONO for core text, HOSTCOL for PutImage | rxvt and jwm text correct and faster than the S7 CPU text path (measured in our own terms) | Same |
| **S9** hardware cursor | `WD24ADV_IOC_CURSOR` from kdrive | The cursor's shape, hot spot and movement are right, and it is off in text mode after every exit path | Software cursor fallback |
| **S10** offscreen pool | Pixmap migration and (if Q2) a glyph cache | Pool correctness, VT-switch loss handling, eviction | Pool disabled → S9 behaviour |
| **S11** default | Owner-chosen boot option (e.g. cmdline `wd24adv`). wd90c24fb remains the default and the fallback | Boot path, fallback when wd24adv refuses to load | Remove the cmdline flag |

Moving from wait-after-every-op to wait-before-program (§3.4) is a separate
mini-gate between S8 and S9.

---

## 8. Open questions

- **Q1. Readback.** Which BITBLT and cursor registers read back as
  written? Answered in S1/S2. This decides how readback works for each
  register.
- **Q2. Mono source.** Can the engine take a mono source from **VRAM**
  (screen-to-screen colour expansion)? If not, a glyph cache in VRAM
  doesn't help, and text stays host-MONO.
- **Q3. Offscreen use by the Windows driver.** Does the Windows driver
  really use a 1024-byte pitch and offscreen caching? This is only
  interesting as design confirmation, not something we need.
- **Q4. Host-port width.** Is the port 16 or 32 bits, and how is the last
  partial word of a row padded? The Windows driver uses 16-bit packed;
  wd90c24fb's notes say 32. Settled in S5 before any wider use.
- **Q5. CPU access during a blit.** Is CPU access to the window safe while
  the engine is busy? The design avoids it regardless, so the question only
  matters for future overlap.
- **Q6. Dual window.** Should the PR0A/PR0B dual window (separate read and
  write banks) be used to halve fault traffic for copy-like fallbacks? It
  needs the PR1 split. Is that already within the whitelist without the
  aperture bits?
- **Q7. Completion IRQ.** Does the 755C wire up an engine-complete IRQ?
  It's assumed not.
- **Q8. Fault-banking cost.** What does fault-driven banking cost on this
  486 (fault time × crossings)? If it's too high, use the damage + HOSTCOL
  fallback in §3.3.
- **Q9. Font path.** Does jwm in our build draw through core fonts or Xft?
  Xft would need Render, which TinyX lacks, so it's probably core fonts.
  This decides how much of the UI hits MONO.
- **Q10. Readback after default.** Should readback-on-every-write stay on
  once accel is the default? That's for Brandon to decide.

## 9. Risks

- **Bus lockup on a host-data mistake.** It can't be recovered in software.
  It is mitigated by exact word counts, a dedicated stage (S5), tiny sizes
  first and serial logging.
- **Undocumented C24A deviations from the C31 data book** (bit meanings,
  masks). Mitigated by S1/S2 proofs and the model.
- **The fault-banked mmap interacting with other mappers** (another
  process mmapping fb0). The kernel limits it to one mapping owner, or
  zaps all VMAs.
- **Scope creep into mode or panel work.** That is excluded by §1.1 and by
  the guard, whatever any stage asks for.
- **RAM.** The X-side code and buffers must stay small (target: under
  100 KiB extra resident). Xorg stays rejected for this reason.
- **Provenance.** Reviews check that no IBM-derived sequences or constants
  enter the repos. Register knowledge in code cites the public data books,
  XFree86 and live proofs only.
