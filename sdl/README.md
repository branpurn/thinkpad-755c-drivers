# sdl/: SDL2 "wdfb" video backend for wd90c24fb

A small SDL2 video driver for the ThinkPad 755C frame buffer
(`modules/wd90c24fb`). It runs SDL2 apps on the console without X: 8bpp
indexed surfaces, the palette through `FBIOPUTCMAP`, and one
`WDFB_OP_DAMAGE` ioctl per rect passed to `SDL_UpdateWindowSurfaceRects`.

```
sdl/
  wdfb/        backend source (copied into SDL's src/video/wdfb at build time)
  patches/     0001 registers the driver in SDL2 (configure, SDL_video.c, ...)
               0002 drops the AVX xgetbv probe on i486 builds (no xgetbv in the binary)
  build.sh     fetch the pinned SDL2 tarball (sha256 checked), patch, build static
  check-i486.sh  objdump scan: no cmov/MMX/SSE/rdtsc/... in a binary or archive
  sdltest/     sdltest.c: palette bars, bouncing box, per-rect updates, fps on stdout
  tests/       qemu-wdfb.py (qemu-system-i386 -cpu 486, wd90c24fb fake=3), vtstate.c
```

## Build

```sh
make sdl          # or: sdl/build.sh
```

The pinned version is SDL2 **2.32.10** (`SDL2-2.32.10.tar.gz`, sha256
`5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165`). The
build uses the tc32 musl i486 toolchain with `-march=i486 -mtune=i486 -Os`,
SDL's assembly, MMX, 3DNow and SSE are turned off, and so are X11, Wayland,
KMS/DRM, OpenGL, Vulkan, udev, D-Bus, HIDAPI and `dlopen`. Audio is OSS
only. Outputs:

| file | what |
|---|---|
| `out/sdl/prefix/` | `libSDL2.a`, headers, `sdl2-config`, `sdl2.pc` (static) |
| `out/sdl/sdltest.i486` | static, stripped test app |
| `out/sdl/vtstate` | test helper |

`check-i486.sh` runs on `sdltest.i486` and `libSDL2.a` at the end of every
build, and the build fails if it finds an instruction the 486 lacks.
Extra configure flags: `SDL_EXTRA_CONFIGURE="..." sdl/build.sh`.

Build an app against it:

```sh
i486-linux-musl-gcc -static -march=i486 -Os $(out/sdl/prefix/bin/sdl2-config --cflags) \
    app.c $(out/sdl/prefix/bin/sdl2-config --static-libs) -o app
```

## Use

```sh
tce-load -i fb-wd90c24.tcz                 # wd90c24fb (xaccel=1 is the default)
tce-load -i trackpoint.tcz                 # optional: evdev, so the keyboard can be grabbed
SDL_VIDEODRIVER=wdfb ./sdltest -t 10       # 320x200 centred, Ctrl+Alt+Backspace quits
SDL_VIDEODRIVER=wdfb SDL_WDFB_SCALE=2 ./sdltest
SDL_VIDEODRIVER=wdfb ./sdltest -f -w 640 -h 480
```

The backend also claims the display without `SDL_VIDEODRIVER` when
`/dev/fb0` reports `WD90C24`. Windows are either the full 640x480 frame or
centred inside it (320x200, 320x240, 640x400), with an optional integer 2x
scale. At 1x the window surface *is* the mmap'd frame buffer, so there is no
copy. At 2x each updated rect is doubled into the frame buffer.

| variable | meaning |
|---|---|
| `SDL_WDFB_DEVICE` | frame buffer (default `$SDL_FBDEV` or `/dev/fb0`). A regular file is a fake 640x480 fb, for tests. |
| `SDL_WDFB_SCALE` | `1` (default), `2`, or `auto` (2 when it fits) |
| `SDL_WDFB_CENTER` | `0` puts the window at the top left (default: centred) |
| `SDL_WDFB_BORDER` | palette index for the area outside the window (0) |
| `SDL_WDFB_INPUT` | `auto` (evdev, else console), `evdev`, `console`, `none` |
| `SDL_WDFB_KBD` | evdev device to use instead of scanning `/dev/input/event*` |
| `SDL_WDFB_TTY` | console to use (default `/dev/tty`, then `/dev/tty0`) |
| `SDL_WDFB_NOGRAPHICS` | `1` leaves the VT in `KD_TEXT` (fbcon may draw over the app) |
| `SDL_WDFB_FORCE` | `1` runs even if another process (such as Xfbdev) has the fb open |
| `SDL_WDFB_NOOPS` | `1` skips the damage ioctl and relies on the driver's 10 Hz refresh |
| `SDL_WDFB_DEBUG` | `1` prints stats each second on stderr, `2` also prints each op |

### Console and keyboard

* The kernel has `CONFIG_VT=y` and `CONFIG_KEYBOARD_ATKBD=y`. `CONFIG_INPUT_EVDEV`
  is `=m` and ships only in `trackpoint.tcz`. With `evdev.ko` loaded, the
  backend picks the first `/dev/input/event*` with letter keys and
  `EVIOCGRAB`s it. Without evdev it puts the VT in `K_MEDIUMRAW` with raw
  termios and decodes the scancodes itself. The text input map is US ASCII.
* The VT goes to `KD_GRAPHICS`, so fbcon stops drawing. On exit, on
  `SDL_Quit`, or on SIGHUP/QUIT/ILL/ABRT/BUS/FPE/SEGV, the backend restores
  the keyboard mode, termios, the `KD_*` mode and the console palette, then
  blanks the frame. Signals are re-raised afterwards. The driver's last
  release forces a full refresh.
* Ctrl+Alt+Backspace sends `SDL_QUIT` the first time. A second press
  restores the console and exits at once, which helps with a hung app.
* **SIGKILL can't be caught.** After `kill -9` the VT is left in
  `KD_GRAPHICS`, and in `K_MEDIUMRAW` if the console keyboard was in use.
  An evdev grab is released by the kernel, so the keyboard still works
  in that case. Recover with `kbd_mode -u; printf '\033c'` from a serial or ssh
  shell, or run any wdfb app again and quit it cleanly.
* While the keyboard is grabbed, Alt+Fn VT switching is off.
* There is no mouse yet (the TrackPoint is not read).

### Xfbdev

Before mapping the fb, the backend checks `/proc/*/fd` for another process
that has the same device open, and it refuses to start if one does
(`SDL_Init` fails with "in use by pid N (Xfbdev)"). `SDL_WDFB_FORCE=1`
overrides this with a warning. Both clients would then draw to the same
memory.

### Rendering

`SDL_GetWindowSurface` plus `SDL_UpdateWindowSurface(Rects)` is the fast
path. The surface is `SDL_PIXELFORMAT_INDEX8`, so set colours with
`SDL_SetPaletteColors(surface->format->palette, ...)` (each change goes out
with `FBIOPUTCMAP`). `SDL_Renderer` works through the software renderer,
but it converts RGB to INDEX8 on every present, which is slow on a 486.

## Tests

```sh
sdl/tests/qemu-wdfb.py            # needs out/bzImage, out/modules, out/sdl/*, tc32 core.xz
```

The test boots `qemu-system-i386 -M isapc -cpu 486 -m 32` with the tc755
kernel and `wd90c24fb fake=3`, a RAM model of video memory that QEMU's
VGA lacks. It checks:

* 1x, 2x and 640x480 modes, with the model and shadow compared through
  the `verify` parameter while the app holds its last frame (0 differing bytes)
* client mode with per-rect damage, and one `DAMAGE` op per rect
  (`SDL_WDFB_DEBUG=2`)
* console and evdev keyboards (a key sent through the QEMU monitor), and
  the console restored after each run (`vtstate`)
* refusal while another process holds `/dev/fb0`, and the `SDL_WDFB_FORCE`
  override
* restore after SIGSEGV
* `xaccel=0` (no ioctls)

The user-mode smoke test needs no VT or keyboard:

```sh
truncate -s 307200 /tmp/fb.raw
SDL_VIDEODRIVER=wdfb SDL_WDFB_DEVICE=/tmp/fb.raw SDL_WDFB_INPUT=none \
    qemu-i386 -cpu 486 out/sdl/sdltest.i486 -t 3    # palette in /tmp/fb.raw.pal
```

## sdltest

```
sdltest [-w W] [-h H] [-f] [-t SECONDS | -n FRAMES] [-c] [-s BOX] [-p HOLD]
```

sdltest draws 256 palette bars in the top half, then bounces a box in the
bottom half, updating only the old and new box rects each frame. `-c`
cycles the palette. It prints `fps N` each second and a summary at the end.
`-p` holds the last frame for HOLD seconds, which is how the test reads
`verify`. Esc or Q quits.

## Licence

`sdl/wdfb/` and `sdl/sdltest/` are zlib-licensed like SDL, so the backend
can be linked into any SDL app. The patches modify SDL2 (zlib).
