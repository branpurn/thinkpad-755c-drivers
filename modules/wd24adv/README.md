# wd24adv: experimental advanced WD90C24 driver (dry-run)

`wd24adv` is a **separate, new** driver for the WD90C24/WD90C24A(2) in the
ThinkPad 755C. Its aim is mode switching (including the 13h fast path for
Doom), direct mapping and, later, page flipping on the **internal panel only**.

- The stable driver `modules/wd90c24fb` is **not modified**; the two drivers
  share no source file (the host cross-check compiles `wd90c24_hw.c`
  read-only).
- Only one of them may be loaded: `wd24adv` refuses to load (`-EBUSY`) while
  `wd90c24fb` is loaded.
- **Dry-run only.** The only backend is a recorder that runs each plan
  against a model of the 755C boot state and logs the writes it *would* do.
  `dryrun=0` is refused (`-EOPNOTSUPP`).
- **There is no panel/CRT routing at all.** The scaffold's routing and panel
  power sequences were deleted. `allow_routing` and `allow_crt` are refused
  (`-EPERM`) whatever their value.

## Files

| file | what |
|---|---|
| `wd24adv_core.h/.c` | Write whitelist, the unlock/save/restore/relock idioms (each mirrors a cited `wd90c24_hw.c` block), the load/mode/text plans, the dry-run backend, the boot-state model loader and the self-test shared by host and kernel. Plain C; builds in the kernel and on the host. |
| `wd24adv_modes.c` | Standard VGA register sets, the mode table with status and evidence, the validator and the boot-state model. |
| `wd24adv_main.c` | Module: parameters, single-driver check, `/proc/wd24adv/`. |
| `wd24adv_ioctl.h` | Placeholder user API: `'W'` 0x30 (modeinfo, not implemented); 0x31 stays reserved and is never assigned. |
| `tests/test_core.c` | Host test: the shared self-test plus host-only checks (`make wd24adv-test`). |
| `tests/xcheck.c` | Host cross-check: drives the real `wd90c24fb/wd90c24_hw.c` against a port model and compares its write trace with wd24adv's for each mode. |

Built by `make oot` / `make wd24adv`. There is still **no `ext/*.ext`**, so no
`.tcz` is produced. Uses only symbols the trimmed tc755 kernel exports
(`vmalloc`, not `vzalloc`).

## Parameters (read-only after load)

| param | default | meaning |
|---|---|---|
| `dryrun` | 1 | Record only. 0 is refused. |
| `allow_routing` | 0 | Removed. Any value other than 0 is refused. |
| `allow_crt` | 0 | Removed. Any value other than 0 is refused. |
| `allow_clock` | 0 | Permits the PR68 b4:3 / PR69 clock block. No table mode needs it: with the boot clock it is a no-op and is skipped; a differing clock is refused while 0. |
| `trial` | 10 | Reserved for the future real backend (auto-revert); unused. |
| `also_conflict` | "" | Test aid: also refuse to load while the named module is loaded. |

## /proc/wd24adv/

- **`plan`**: the boot-state model (each value tagged `L` live read, `P` POST
  trace, `S` standard VGA 03h), then the ordered writes of the load phase,
  every mode (`== name [STATUS] writes=N ==`) and the text restore. Each write
  shows `old -> new` (`--` when the old value is unknown) and a tag naming
  the `wd90c24fb` code it mirrors. UNPROVEN modes show `writes=0` and
  `-95` (`-EOPNOTSUPP`).
- **`modes`**: the table, status, flags and evidence, plus the validator result.
- **`selftest`**: the shared self-test (62 checks).

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
| 640x480x8, banked | **PROVEN** | 88 | `wd90c24fb` `wd_mode_640x480x8` values | the `wd90c24fb` packed-8 ext set (PR0A=0, then SR06 restored) | The running `wd90c24fb` mode: live 3CC=E3, GR0F=05, GR0E=01, CR11=8C, CR29=85, SR11=5D, SR31=0D, SR06=0F, with the panel fine. `xcheck` matches `wd90c24fb` write for write. |
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
- The `vgastate` save/restore of fonts and planes is not modelled; it is a note in the plan.

## Tests

- `make wd24adv-test` builds and runs `tests/test_core` (78 checks) and
  `tests/xcheck` (18 checks). xcheck fails if `wd90c24fb` and `wd24adv`
  ever diverge; a one-bit PR16 mutation was seen to fail it.
- QEMU (ad hoc; wd24adv is not in the tcz harness), `-M isapc -cpu 486`, tc755 floppy:
  - `dryrun=0` gives `-EOPNOTSUPP`.
  - `allow_crt=1` and `allow_routing=1` give `-EPERM`.
  - With `wd90c24fb fake=2` loaded, the load gives `-EBUSY`.
  - `also_conflict=vromdump` with vromdump loaded gives `-EBUSY`.
  - A plain load passes the selftest (62/62); `/proc` plan/modes are complete and untruncated; rmmod is clean; no Oops/WARNING.

## Before a first live test (not done)

1. A real port backend. It does not exist in this build.
2. A `vgastate`-style font/plane save so the text restore is complete.
3. A decision on the 23C0h cursor block (`ext_blocks`).
4. An auto-revert (`trial`) and serial-console capture.
5. Brandon's explicit approval to load a non-dry-run build on the 755C.

The first live target is 640x480x8. It is identical to `wd90c24fb`. 640x400
and 13h come after that.

## Single-driver rule

`wd24adv` walks the module list from its own node, because the trimmed
kernel exports no module lookup. The reverse is not covered: the frozen
`wd90c24fb` cannot see `wd24adv`, so do not load `fb-wd90c24.tcz` while
`wd24adv` is loaded (harmless in this dry-run build).
