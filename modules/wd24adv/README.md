# wd24adv: experimental advanced WD90C24 driver (dry-run default, gated live backend)

`wd24adv` is a **separate, new** driver for the WD90C24/WD90C24A(2) in the
ThinkPad 755C. Its aim is mode switching (including the 13h fast path for
Doom), direct mapping and, later, page flipping on the **internal panel only**.

- The stable driver `modules/wd90c24fb` is **not modified**; the two drivers
  share no source file (the host cross-check compiles `wd90c24_hw.c`
  read-only).
- Only one of them may be loaded: `wd24adv` refuses to load (`-EBUSY`) while
  `wd90c24fb` is loaded.
- **Dry-run by default.** The default backend is a recorder that runs each
  plan against a model of the 755C boot state and logs the writes it *would*
  do. No port is touched.
- **Live only with `dryrun=0 live=1`** (both; either alone is refused). The
  live backend (`wd24adv_port.c`) executes the *same* plan code on the real
  ports; see "Live backend" below. On the 755C it has run load, unload and
  `640x480-8` with trial auto-revert (PROVEN live, 2026-11-02); each live
  step needs the owner's explicit approval and follows
  [`docs/WD24ADV-LIVE-TEST.md`](../../docs/WD24ADV-LIVE-TEST.md).
- **There is no panel/CRT routing at all.** The scaffold's routing and panel
  power sequences were deleted. `allow_routing` and `allow_crt` are refused
  (`-EPERM`) whatever their value.

## Files

| file | what |
|---|---|
| `wd24adv_core.h/.c` | Write whitelist, the unlock/save/restore/relock idioms (each mirrors a cited `wd90c24_hw.c` block), the load/mode/text plans, the dry-run backend, the boot-state model loader and the self-test shared by host and kernel. Plain C; builds in the kernel and on the host. |
| `wd24adv_modes.c` | Standard VGA register sets, the mode table with status and evidence, the validator and the boot-state model. |
| `wd24adv_port.c` | Live backend: executes plans with inb/outb + the A0000 window through `struct wd24_io`; reads every write back; SR06 latch index-width test; DAC and planar memory access. |
| `wd24adv_main.c` | Module: parameters and gates, single-driver check, `/proc/wd24adv/`, live setup (ports, ioremap, console lock), trial timer. |
| `wd24adv_ioctl.h` | Placeholder user API: `'W'` 0x30 (modeinfo, not implemented); 0x31 stays reserved and is never assigned. |
| `tests/test_core.c` | Host test: the shared self-test plus host-only checks (`make wd24adv-test`). |
| `tests/xcheck.c` | Host cross-check: drives the real `wd90c24fb/wd90c24_hw.c` against a port model and compares its write trace with wd24adv's for each mode. |
| `tests/vgamodel.h/.c` | Port-level VGA model (WD90C24 and QEMU std VGA profiles): registers with WD lock rules, AR flip-flop, DAC, 4 planes with write/read mode 0 and chain-4, latches; flags forbidden registers; fault injection (stuck register, read-only plane). |
| `tests/test_vga.c` | Live backend against the model: round trip, trial revert/confirm, zero-write refusals, readback mismatch handling. |
| `tests/qemu-wd24adv.py` | QEMU system test against `-vga std` with VGA port tracing. |

Built by `make oot` / `make wd24adv`. There is still **no `ext/*.ext`**, so no
`.tcz` is produced. Uses only symbols the trimmed tc755 kernel exports
(`vmalloc`, not `vzalloc`).

## Parameters (read-only after load)

| param | default | meaning |
|---|---|---|
| `dryrun` | 1 | Record only. 0 needs `live=1` (else `-EOPNOTSUPP`). |
| `live` | 0 | With `dryrun=0`: drive the hardware. With `dryrun=1`: `-EINVAL`. |
| `trial` | 15 | Live: seconds before a mode set reverts to text unless confirmed. 5..120; 0 (off) and anything outside are refused (`-EINVAL`). |
| `mode` | "" | Live: a PROVEN mode (`640x480-8`, `640x400-8`, `320x200-8`) to set right after load (the trial applies). Refused in dry-run and for UNPROVEN/unknown names. |
| `pattern` | 1 | Live: draw 16 colour bars over the whole frame (PR0A banks 0-4) after a packed-8 mode set. |
| `allow_routing` | 0 | Removed. Any value other than 0 is refused. |
| `allow_crt` | 0 | Removed. Any value other than 0 is refused. |
| `allow_clock` | 0 | Permits the PR68 b4:3 / PR69 clock block. No table mode needs it: with the boot clock it is a no-op and is skipped; a differing clock is refused while 0. |
| `also_conflict` | "" | Test aid: also refuse to load while the named module is loaded. |

## /proc/wd24adv/

- **`plan`**: the boot-state model (each value tagged `L` live read, `P` POST
  trace, `S` standard VGA 03h), then the ordered writes of the load phase,
  every mode (`== name [STATUS] writes=N ==`) and the text restore. Each write
  shows `old -> new` (`--` when the old value is unknown) and a tag naming
  the `wd90c24fb` code it mirrors. UNPROVEN modes show `writes=0` and
  `-95` (`-EOPNOTSUPP`).
- **`modes`**: the table, status, flags and evidence, plus the validator result.
- **`selftest`**: the shared self-test.
- **`live`** (live only): the last hardware operation: result, port data and
  index write counts, readback mismatches, trial state, then every event
  (`W` writes with readback, `R` reads, `CHECK FAILED`, `MISMATCH`, `M` bulk
  memory/DAC transfers, notes).
- **`control`** (live only, root, write-only):
  - `mode NAME`: set a PROVEN mode. Needs text mode, no pending trial and no
    earlier readback mismatch (`-EBUSY` / `-EIO`). Starts the trial timer.
  - `confirm`: keep the current mode (stops the auto-revert).
  - `text`: restore the saved text state now.

## Live backend

**Gates, in order (load):** `allow_routing`/`allow_crt` refused; wd90c24fb
loaded refused; `dryrun`/`live` combination; `trial` range; vga16fb loaded
refused; `mode` must be PROVEN; the shared self-test must pass;
`request_region(3C0h, 32)`; `ioremap(A0000h, 64K)`. Only then is a port read.

**One plan, two backends.** `wd24_plan_load/mode/text` build and issue the
writes; the backend only executes them (`wd24_backend`: write/read, DAC,
memory, begin/end). Dry-run, host model and kernel run the identical code.

**Entry check (read-only, before every plan):** `wd24_entry_check()`.
MISC b0 = 1; MISC unchanged since the last plan (mode set); CR34 reads A0h
twice (PR18/PR19 readable without any write); PR18 = C7h, PR19 = 54h (the
live-verified panel route); GR0F/CR29 = 00/00 (BIOS), 00/85 (live, after wd90c24fb unloads) or 05/85 (wd90c24fb)
at load, or as left by the last plan; SR06 reads 0Fh and the 3C4
index-width test says locked; AR10 b0 = 0 (text) at load. Any difference
aborts with `-ENODEV` and **zero data-register writes** (the only writes
are index registers, put back to their entry values). Before a restore the
check is advisory except MISC/CR34/AR (the restore must be able to run).

**Readback.** Every write is read back and compared under a per-register
mask (`rb_mask()`: e.g. MISC EFh, SR00-04, GR00-08 and AR bits that are
defined on standard VGA, GR0F 07h, PR0A/PR0B 7Fh, else FFh). SR06 is a latch
that reads back only its low nibble, so it is verified by that nibble plus
the 3C4 index-width test (index 11h reads 11h when unlocked, 01h when
locked). A mismatch stops the plan, runs the full restore, and the
operation returns `-EIO`; `mode` is then refused until rmmod. A mismatch
*during* the restore is logged and the remaining restore steps still run.

**Saved state (at load, before anything visible changes).** Following
`wd90c24fb_main.c` 1444-1448 (`wd_save_ext`, `save_vga`, `wd_relock`) and
Linux `drivers/video/vgastate.c` (`save_vga`: cmap, mode, text):
the Paradise extended registers (`wd_save_ext`, hw.c 197-234), the DAC
(256 x 3, PEL mask), the standard VGA registers (MISC, SR00-04, CR00-18,
GR00-08, AR00-14) and **all 4 planes x 64 KiB** via planar read mode 0
(SR02/GR04 plane select, SR04 06h, GR05 00h, GR06 05h, SR01 screen off
under a synchronous reset around the copy, exactly the `save_vga_text()`
register sequence; the same sequence runs on the 755C today when
wd90c24fb loads, because it calls `save_vga()` with `VGA_SAVE_FONTS`). vgastate saves only the font/text
planes; wd24adv saves the whole 256 KiB.

**Restore (`wd24_restore_all`, unload, trial expiry, `text`, any failure
after writes).** Order of `wd_restore_text` (`wd90c24fb_main.c` 802-815) and
`restore_vga` (mode, text, cmap): extended registers, standard registers,
planes, DAC, relock, screen on. `wd_restore_panel` is omitted: no panel
register is ever changed.

**Trial.** Every successful mode set starts a delayed work (`trial` s).
Unless `confirm` arrives first it runs the restore. It cannot be disabled
(0 is refused). rmmod cancels it and always restores.

**Locking.** Every operation runs under a module mutex and
`console_lock()`, so vgacon does not run concurrently with a plan.

**Failure after writes at load.** The state is restored and the module
stays loaded but **inert** (`/proc/wd24adv/live` keeps the log; rmmod
touches nothing). Returning an error instead would make busybox `insmod`
retry with `init_module()` and run the load twice (seen in QEMU: a
refused live load appears twice in dmesg; harmless there, zero writes).

**Requirements for live:** VGA text console (vgacon) on the panel; **no
framebuffer console (`/proc/fb` empty), no X server**, wd90c24fb and
vga16fb not loaded.

**23C0h cursor block: omitted.** `wd90c24fb` saves/restores the 23C0h
hardware-cursor block only when `hw->ext_blocks` is set (`wd90c24_hw.c`
230 save, 276 restore). wd24adv never enables the block, so it neither
writes nor needs to save it.

## The write rule

Every write is either a standard VGA register or a register **and value
pattern that `wd90c24fb` already writes**. The whitelist in `wd24_guard()`
cites the `wd90c24_hw.c` line for every extended index. Everything else is
refused with `-EPERM` and logged as `REFUSED`, including:

- PM ports 24h-27h, 0D00h/0D01h, CMOS;
- CR35/CR3A;
- the shadow-timing unlock CR34=06h (only the `(L&0F)|A0`, then `L`, panel-read idiom is allowed);
- any write to PR18 (CR31) or PR19 (CR32), and every panel power control;
- the aperture (PR1B/PR1A linear window), always;
- the clock bits unless `allow_clock`.

The unlock idioms are the ones `wd90c24fb` uses:

- **CR34:** `(L&0F)|A0`, then `L`.
- **SR06:** `4F`, then the saved value.
- **SR35:** `50`, then `P`.

## Why low-resolution modes are centred, not stretched

The live read shows PR19 = 54h. That means LCD on and CRT off. Bits 3:2 = 01
select centring with no expansion. PR19 is a panel register that `wd90c24fb`
never writes, and wd24adv never writes it either. So modes with fewer than
480 lines are shown **centred** in a black border. Every mode set first
re-reads PR18/PR19/MISC and aborts with `-ENODEV` unless PR19=54h, PR18=C7h
and MISC b0=1. 320-wide modes use the 8-bit PEL clock (AR10 b6), as BIOS
mode 13h does.

## Mode table and evidence

Sources:

- `live-read-plan.md` §10-12 (live register reads on the 755C);
- `win98-driver.md` §8-9 (the vendor driver's mode list and register usage, described, not copied);
- `vbios-trace.md` (POST/INT 10h trace);
- `registers.md`;
- `REPORT.md`.

No vendor code or disassembly is reproduced here.

| mode | status | writes | standard VGA part | extended part | evidence |
|---|---|---|---|---|---|
| 640x480x8, banked | **PROVEN** | 88 | `wd90c24fb` `wd_mode_640x480x8` values | the `wd90c24fb` packed-8 ext set (PR0A=0, then SR06 restored) | The running `wd90c24fb` mode: live 3CC=E3, GR0F=05, GR0E=01, CR11=8C, CR29=85, SR11=5D, SR31=0D, SR06=0F, with the panel fine. `xcheck` matches `wd90c24fb` write for write. **Seen live with wd24adv** (ccfb5a9: full-frame bars, 93/263, revert 896/336, 0 mismatches). |
| 640x400x8, banked | **PROVEN** | 88 | 640x480 horizontals + standard VGA 400-line verticals (MISC 63, CR06 BF, CR07 1F, CR10 9C, CR11 8E, CR12 8F, CR15 96, CR16 B9) | same as 640x480 | Listed on the panel in the vendor driver's VESA list. Centred via the existing PR19=54h. Not yet seen live. |
| 320x200x8 (13h), direct A0000 | **PROVEN** | 91 | standard VGA mode 13h set (chain-4, AR10=41, CR09=41 doublescan, CR14=40, CR17=A3) | the BIOS text-mode ext block, as the `wd90c24fb` text restore writes it | The vbios trace shows mode 13h programs only standard VGA registers. The ext values are the BIOS ones `wd90c24fb` restores on exit. This is the Doom fast path. Not yet seen live. |
| 320x240x8 Mode X | UNPROVEN | 0 | – | – | Unchained 480-line verticals on the panel never observed; disabled. |
| 640x480x8, 640x800 virtual (flip) | UNPROVEN | 0 | – | – | Page flip through CR0C/CR0D + PR3 start bits never observed; disabled. |

Differences from `wd90c24fb`'s own sequence, all deliberate and checked by
`xcheck`:

- The clock block is skipped when it would rewrite the same value.
- SR06 is restored at the end of a packed-8 mode set.
- The 23C0h hardware-cursor block is omitted (`wd90c24fb` writes it only with `ext_blocks`).
- `wd_restore_panel` is omitted because it writes panel registers.
- The `vgastate` part (fonts, planes, DAC, standard registers) is wd24adv's
  own code (`save_std/planes/cmap`); xcheck checks it separately by replaying
  it into the port model for the text test.

## Tests

- `make wd24adv-test` builds and runs:
  - `tests/test_core` (92 checks: the shared self-test + host checks);
  - `tests/xcheck` (18 checks; fails if `wd90c24fb` and `wd24adv` ever
    diverge; a one-bit PR16 mutation was seen to fail it);
  - `tests/test_vga` (55 checks on the port model): load leaves the model
    byte-identical and saves the exact planes and DAC; each PROVEN mode +
    pattern, then trial expiry, gives an identical model; 640x480 matches
    the live probe values; confirm path; refusals with **0 data writes**
    (QEMU profile, PR19=5C, CR34 not A0, SR06 unlocked, graphics AR10,
    state changed since the last plan); stuck CR13 -> `-EIO` + identical
    after restore; stuck SR06 latch at load -> `-EIO`, identical; stuck
    GR06 during restore -> `-EIO`, other steps still run; read-only plane
    -> `-EIO`, the rest restored.
- `make wd24adv-qemu` (`tests/qemu-wd24adv.py`, 26 checks) on
  `-M isapc -cpu 486 -vga std` with `-trace vga_std_{read,write}_io`:
  dry-run load/rmmod with zero VGA port accesses; all parameter refusals
  with zero accesses; `dryrun=0 live=1` refused by the entry check (CR34
  reads 00) with **zero data-register writes** in the trace (3C0 is
  decoded with the AR flip-flop) and the index registers written back;
  refused while wd90c24fb is loaded; wd90c24fb `text=1` + rmmod (fake=3).
  QEMU has no WD90C24, so the live path past the entry check runs only
  against the host model.

## Live test

See [`docs/WD24ADV-LIVE-TEST.md`](../../docs/WD24ADV-LIVE-TEST.md). **Not
done.** It needs the owner's explicit approval. The first target is a
live load + rmmod with no mode, then 640x480-8 (the wd90c24fb mode).

## Single-driver rule

`wd24adv` walks the module list from its own node, because the trimmed
kernel exports no module lookup. The reverse is not covered: the frozen
`wd90c24fb` cannot see `wd24adv`, so do not load `fb-wd90c24.tcz` while
`wd24adv` is loaded. In live mode wd24adv also holds the 3C0h-3DFh region
(nested under vgacon's non-busy `vga+`).
