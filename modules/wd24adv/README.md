# wd24adv: experimental advanced WD90C24 driver (scaffold)

`wd24adv` is a **separate, new** framebuffer driver for the WD90C24/WD90C24A(2)
in the ThinkPad 755C. It is meant to add mode switching, direct mapping, page
flipping and panel/CRT routing. Things to know:

- The stable driver `modules/wd90c24fb` is **not modified**. Its source,
  behaviour, ioctls and build are unchanged.
- The two drivers share no file.
- Only one of them may be loaded. `wd24adv` refuses to load (`-EBUSY`) when
  `wd90c24fb` is loaded.

**Status: scaffold, dry-run only.** The only backend records register writes
and delays; it never sends them to hardware. Loading with `dryrun=0` is
refused (`-EOPNOTSUPP`). The real backend arrives at Stage 2, once the Stage 0
BIOS trace (see `../../stage0/`) has supplied the WD extended register values.
Until then, every mode in the table is flagged `[pending Stage 0]`.

## Files

| file | what |
|---|---|
| `wd24adv_core.h/.c` | Register guard (the safety interlocks), panel power sequence, routing sequence and mode programming plan. Plain C: it builds both in the kernel and on the host. |
| `wd24adv_modes.c` | Mode table skeleton and its validator. |
| `wd24adv_main.c` | Module: parameters, single-driver check, dry-run plan, `/proc/wd24adv/`. |
| `wd24adv_ioctl.h` | Its own user API header: placeholder; reserves `'W'` 0x30/0x31; it is independent of `wd90c24fb_ioctl.h`. |
| `tests/` | Host unit test of the core: `make wd24adv-test`. Currently 52 checks. |

The module is built by `make oot` (or `make wd24adv`). There is deliberately
**no `ext/*.ext`** yet, so no `.tcz` is produced and nothing loads it by
default. It is compile-only against the tc755 kernel (6.18.57-tc755, i386) and
uses only symbols the trimmed kernel already exports, so neither the kernel
nor `config/ksyms-keep.txt` changes.

## Parameters

All parameters are read-only after load, and every permission defaults to off.

| param | default | meaning |
|---|---|---|
| `dryrun` | 1 | Record only. 0 is refused in this build. |
| `allow_routing` | 0 | Allow panel power sequencing and changes to PR18, PR19 b7:4, PR1A, PR36 and the shadow CRTC. |
| `allow_crt` | 0 | Allow CRT-only routing and modes (panel powered down). |
| `allow_clock` | 0 | Allow PR68/PR69 clock writes (needs SR35=50h). |
| `mode` | `320x200-8` | The mode to plan; see `/proc/wd24adv/modes`. |
| `route` | `panel` | `panel`, `crt` or `both`. |
| `trial` | 10 | Reserved: auto-revert to text after N seconds (real backend). |
| `also_conflict` | "" | Test aid: also refuse to load while the named module is loaded. |

## /proc/wd24adv/

- **`plan`**: every register write, delay, refusal and note that switching to
  `mode=` on `route=` would produce. It starts from the boot state: panel
  powered, PR19 = panel.
- **`modes`**: the table plus the validator result.
- **`selftest`**: 11 in-kernel checks that the interlocks refuse by default.

## Safety interlocks (all in `wd24_guard()`)

Every register write goes through `wd24_write()` → `wd24_guard()`:

1. **Routing-class registers need both `allow_routing=1` and a panel powered
   down by `wd24_panel_off()`.** These are PR18 (CR31), PR1A (CR33), PR36
   (CR3B), PR19 (CR32) b7:4, and the panel shadow CRTC (CR34 = xxxxx110b, and
   any CR00-07/10-12/15/16 write while it is open).
   - Without the parameter the guard returns `-EPERM`.
   - With the parameter but the panel still powered, it returns `-EBUSY`.
   - This follows the datasheet rule: never change panel timing or routing
     while the panel is powered.
2. **PNLOFF (PR57 = SR19 b2) and the panel tri-state (PR4 = GR0E b5) change
   only inside `wd24_panel_off()` / `wd24_panel_on()`.** As a result, the
   power order and its delays cannot be bypassed.
3. **PR68/PR69 (SR31/SR32) require `allow_clock=1`.**
4. **The aperture/memory-map bits PR1 (GR0B) b5:4 are always refused**
   (Stage 4). The local-bus ports A875h and 2DF0h-2DF2h cannot even be
   expressed in this API.
5. **Mode register lists may not contain any of the registers above, or the
   unlock registers.** `wd24_mode_check()` rejects such a mode. Routing is
   always a separate, guarded step.
6. **CRT-only modes are refused on the panel (`-EINVAL`), and without
   `allow_crt` (`-EPERM`).**
7. **The screen stays blanked (SR01 b5) from the first write of a mode switch
   to the last.**

### Panel power sequence

The power sequence is implemented as follows; the dry-run log shows it in
`plan`.

**OFF:**
1. Unlock (GR0F=05h, CR29=85h, SR06=(old&EFh)|48h, CR34=A0h).
2. Blank (SR01 b5=1).
3. PR19 b4=0 (stop panel data).
4. PR4 b5=1 (tri-state the panel interface).
5. PR57 b2=0 (PNLOFF: panel power down).
6. Wait 100 ms.

**ON:**
1. PR57 b2=1.
2. Wait 100 ms.
3. PR4 b5=0.
4. Wait 100 ms.
5. PR19 b4=1.
6. SR01 b5=0, CR17 b7=1.

**Routing change** (datasheet "changing display type"):
1. Panel OFF.
2. PR19 b5:4=00.
3. Program the PRs (Stage 0 values).
4. Save the base CRTC.
5. CR34=A6h, load the shadow CRTC, then CR34=A0h.
6. Reload the base CRTC.
7. Set the route bits.
8. Wait 100 ms, then panel ON (if the panel is a target); otherwise just
   unblank the CRT.

> **Open item before Stage 6:** the OFF order above follows the XFree86 `wd`
> driver. REPORT.md §4 lists the datasheet steps as PNLOFF first. The 755C
> BIOS's own Fn+F7 path will be traced in Stage 0, and the order will be
> confirmed against that trace before any real routing write is enabled.

## Mode table (REPORT.md §4)

| name | size | mapping | stage | notes |
|---|---|---|---|---|
| `640x480-8` | 640x480x8 | banked (307200 B) | 2 | Like wd90c24fb; BIOS mode 5Fh. |
| `640x400-8` | 640x400x8 | banked (256000 B) | 3 | PR19 b3:2 centre/expand; mode 5Eh. |
| `320x200-8` | 320x200x8 chained | direct, 64000 B | 2 | Mode 13h; standard VGA registers filled in; panel expand/centre from Stage 0. |
| `320x240-8` | 320x240x8 packed | banked (76800 B) | 3 | 640x480 CRTC, dot clock /2, double scan. |
| `320x240-8x` | Mode X, 3 pages | direct, 4 planes | 3 | Standard Mode X registers filled in; flip via CR0C/0D. |
| `800x600-8-crt` | 800x600x8 | banked | 6 | CRT only, panel off, VCLK ~40 MHz via PR69. |
| `640x480-8-v800` | 640x480 view of 800x600 | banked | 3 | Panning on the panel. |
| `text-03h` | 80x25 text | - | 2 | Restore path. |

`fb_check_var` will accept only table modes. `fb_set_par` will run:
1. blank
2. unlock
3. extended, then standard registers
4. PR19 b3:2
5. self-test
6. unblank

…with the `trial=` auto-revert.

## Staged plan (REPORT.md §7)

| stage | work | gate on the device |
|---|---|---|
| 0 | Read-only ROM dump (`vromdump`), then the clean-room emulator trace (`stage0/`) | ROM sum8 = 0; the mode 03h trace matches the saved text state |
| 1 | Read-only diagnostics (PR30A/31/33A/57/59/62/64/19/68/69) plus benchmarks | values logged |
| 2 | Real backend: 320x200 chained, direct mmap, switching to and from 640x480 and text | text restore OK ×20 |
| 3 | 320x240, 640x400, Mode X with pan/flip | tear-free flip |
| 4 | Linear aperture (PR1 b5:4, PR34A, 2DF2): opt-in | marker test |
| 5 | Fault-banked mmap (if 4 fails) | stress test without oopses |
| 6 | 800x600 CRT-only plus the panel-off sequence | external monitor; visible delays; auto-revert |
| 7 | MCLK/FIFO tuning parameter | VRAM pattern test |

## Single-driver rule

- **How the check works:** `wd24adv` walks the module list from its own node,
  because the trimmed kernel exports no module lookup. It skips the list head:
  every `struct module` sits in the vmalloc/module area on i386, and the head
  does not.
- **Tested in QEMU:** `also_conflict=vromdump` with `vromdump` loaded gives
  `-EBUSY`.
- **The reverse is not covered:** `wd90c24fb` cannot see `wd24adv`, because
  its source is frozen. So do not load `fb-wd90c24.tcz` while `wd24adv` is
  loaded. In this dry-run build that would be harmless anyway.
