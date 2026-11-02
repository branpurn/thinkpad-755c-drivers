# doom-wdfb: Doom on the 755C's wd90c24fb, no SDL

[doomgeneric](https://github.com/ozkl/doomgeneric) (Chocolate Doom 1.x
based) with a platform file for the native `wd90c24fb` driver
(`fb-wd90c24.tcz`): one static i486 binary, no SDL, no X, no libraries.
Doom draws its 8-bit paletted frame **straight into the driver's frame
shadow** (the RAM that `mmap(/dev/fb0)` returns) and reports it with one
`WDFB_OP_DAMAGE` per frame; the palette goes through `FBIOPUTCMAP`.

**No WAD is included, and none may ever be committed** (`*.wad` is in
`.gitignore`). Bring your own: `DOOM.WAD` / `DOOM1.WAD` (shareware) /
`DOOM2.WAD`, or Freedoom's `freedoom1.wad`.

## Build

```sh
make doom          # = doom/build.sh -> out/doom/doom-wdfb (+ test helpers)
```

`doom/build.sh` downloads doomgeneric at a pinned commit
(`e257a09527062bfb986b6e5e0780839b6cbd5b71`, archive sha256
`26f8f60daa6c6e4d3c01e5eac9fb700d592dede803d13ac848154ff356b2a2e7`), applies
`patches/`, adds `doomgeneric_wdfb.c`, and builds with tc32's musl toolchain:
`-march=i486 -O2`, static, `--gc-sections`, stripped, then
`sdl/check-i486.sh` (no instruction newer than the 486). About 470 KiB.

## Run (on the 755C)

```sh
tce-load -i fb-wd90c24      # wd90c24fb; evdev from trackpoint.tcz is optional
doom-wdfb -iwad /mnt/hda1/doom/DOOM.WAD
```

Run it from a writable directory: `.default.cfg` and `.savegame/` go into the
current directory. Run it from a VT (the console), not from X; it refuses a
frame buffer another process (Xfbdev) has open.

| variable | meaning |
|---|---|
| `DOOM_WDFB_SCALE=2` | 640x400 (pixel-doubled) instead of 320x200 centred. Costs about 3x the flush time |
| `DOOM_WDFB_DEVICE` | frame buffer, default `/dev/fb0` |
| `DOOM_WDFB_INPUT` | `auto` (default: evdev, else console), `evdev`, `console`, `none` |
| `DOOM_WDFB_KBD` | one evdev device instead of scanning `/dev/input/event*` |
| `DOOM_WDFB_TTY` | console to switch to graphics mode (default the controlling tty, else `/dev/tty0`) |
| `DOOM_WDFB_FORCE=1` | use the frame buffer even if another process has it open |
| `DOOM_WDFB_DEBUG=1` | frames per second and flush time per frame on stderr every 2 s |

Keys (Doom defaults): arrows move/turn, **Ctrl** fire, **Space** use/open,
**Alt** strafe, **Shift** run, `1`-`7` weapons, **Esc** menu, **Tab** map,
F1-F12 as in Doom (F10 quit). **Ctrl+Alt+Backspace** exits at once. Keys come
from evdev when `evdev` is loaded (grabbed, so nothing reaches the shell),
otherwise from the console in `K_MEDIUMRAW`. On exit, `I_Error`, SIGINT/SIGTERM or a
fatal signal the screen is cleared, the old palette and console text mode
and keyboard mode come back.

Sound: none yet. The 755C's CS4248 has an OSS `/dev/dsp` (`snd-cs4248.tcz`);
an `i_sound` module writing 8-bit mono to it is the obvious next step.

## How it works

`patches/0001-draw-paletted-into-a-pitched-frame.patch` (doomgeneric):

- `SCREENPITCH`: compile-time row pitch of `I_VideoBuffer` (default
  `SCREENWIDTH`, so other doomgeneric ports are unchanged). This build uses
  640, the frame buffer's pitch; a constant keeps the column and span drawers'
  inner loops as tight as upstream's. Everything that writes the screen uses
  it (renderer row table, `V_*` drawing to the screen, automap, finale text,
  border erase, screenshots); the screen wipes work in a packed buffer and
  copy each step out.
- `DG_DIRECT`: the platform supplies `I_VideoBuffer` (`DG_VideoBuffer()`)
  and `I_FinishUpdate` only calls `DG_DrawFrame()`: no 32-bit
  `DG_ScreenBuffer`, no colour conversion (`CMAP256`).

`doomgeneric_wdfb.c`: at 1x `DG_VideoBuffer` returns the shadow at
(160,140), so a frame costs Doom's own drawing plus one ioctl that copies
320x200 to video memory (about 9 ms measured on the 755C). At 2x Doom draws
into a buffer that is pixel-doubled into the shadow (about 33 ms flush on the
755C). The border is cleared once to palette index 0; like the VGA overscan
border under DOS it takes the red/gold tint of the pain and pick-up palettes.
Frame buffer, console and keyboard handling are adapted from `sdl/wdfb`.

## Test (QEMU)

```sh
make test-doom IWAD=/path/to/DOOM.WAD [IWAD2=/path/to/freedoom1.wad]
```

`doom/tests/qemu-doom.py` boots `-M isapc -cpu 486 -m 36` with the tc755
kernel, the real `wd90c24fb` in `fake=3` (RAM model of video memory) and the
IWADs on a FAT disk image (`out/test-logs/qemu-doom-wads.img`, never
committed). It checks: start-up to the title and demo at 1x (direct) and 2x
with screenshots (`out/test-logs/doom-shots/*.png`, from the frame buffer and
palette via `fbdump`), the driver's `verify` (video memory = what Doom drew),
the console keyboard and evdev driving the menu to quit, the console coming
back after quit, `I_Error` and SIGSEGV, refusal while `/dev/fb0` is busy, then
`-timedemo demo1..3` at 1x and 2x for each IWAD.

QEMU's TCG 486 is far faster than a 75 MHz 486DX4 and its video memory
costs nothing, so its timedemo figures say only that the demos run to the
end. Real numbers need the 755C:

```sh
doom-wdfb -iwad DOOM.WAD -timedemo demo1    # prints "timed N gametics in M realtics"
```

fps = 35 x N / M.

## Notes

- `doomgeneric_Tick` sometimes draws a frame without a new game tic (upstream
  behaviour, `TryRunTics` returns once a tic has elapsed); `DOOM_WDFB_DEBUG`
  shows up to 70 fps in QEMU for that reason. On the 755C, where a frame takes
  longer than a tic, it should not happen often.
- `I_GetPaletteIndex` (only used for the mouse-sensitivity menu box colour)
  still uses doomgeneric's RGB565 table: harmless.
- The patch's `SCREENPITCH != SCREENWIDTH` paths are what this build uses;
  the default (`SCREENPITCH == SCREENWIDTH`) paths are upstream's code.
