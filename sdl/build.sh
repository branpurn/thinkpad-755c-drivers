#!/bin/sh
# SDL2 with the "wdfb" video backend (wd90c24fb, ThinkPad 755C), static i486
# musl, plus the sdltest smoke test. See sdl/README.md.
#   out/sdl/prefix/   static libSDL2.a + headers + sdl2-config for porting games
#   out/sdl/sdltest.i486  static test binary (stripped)
#   out/sdl/vtstate       test helper for sdl/tests/qemu-wdfb.py
# Env: SDL_EXTRA_CONFIGURE="..." appended to SDL's configure line.
. "$(dirname "$0")/../scripts/common.sh"

SDL2_VER=2.32.10
SDL2_URL=https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VER/SDL2-$SDL2_VER.tar.gz
SDL2_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165

CC="${MUSL_TC_PREFIX}gcc"
command -v "$CC" >/dev/null || die "musl toolchain missing: run 'make tools' (or 'make') in $TC32_DIR"
# i486: no cmov / MMX / SSE / x87 fcomi; -Os keeps it small
CFLAGS_I486="-march=i486 -mtune=i486 -Os -fno-pie -ffunction-sections -fdata-sections"
SDLDIR="$TOP/sdl"
SRC="$BUILD/SDL2-$SDL2_VER"
PREFIX="$OUT/sdl/prefix"

tarball=$(fetch "$SDL2_URL" "$SDL2_SHA256")
rm -rf "$SRC" "$PREFIX"
tar -xzf "$tarball" -C "$BUILD"
log "patching SDL2 $SDL2_VER"
for p in "$SDLDIR"/patches/*.patch; do
    patch -d "$SRC" -p1 --no-backup-if-mismatch -s < "$p" || die "patch failed: $p"
done
mkdir -p "$SRC/src/video/wdfb"
cp "$SDLDIR"/wdfb/*.c "$SDLDIR"/wdfb/*.h "$SRC/src/video/wdfb/"
cp "$TOP/modules/wd90c24fb/wd90c24fb_ioctl.h" "$SRC/src/video/wdfb/"

log "configuring SDL2 (static, wdfb only)"
mkdir -p "$SRC/obj"
cd "$SRC/obj"
# shellcheck disable=SC2086
CC="$CC" AR="${MUSL_TC_PREFIX}ar" RANLIB="${MUSL_TC_PREFIX}ranlib" STRIP="${MUSL_TC_PREFIX}strip" \
CFLAGS="$CFLAGS_I486" LDFLAGS="-static -no-pie" \
../configure --host=i486-linux-musl --build="$(../build-scripts/config.guess)" --prefix="$PREFIX" \
    --disable-shared --enable-static \
    --enable-video-wdfb --enable-video-dummy --enable-video-offscreen \
    --disable-video-x11 --disable-video-wayland --disable-video-kmsdrm --disable-video-rpi \
    --disable-video-vivante --disable-video-directfb --disable-video-opengl --disable-video-opengles \
    --disable-video-opengles1 --disable-video-opengles2 --disable-video-vulkan --disable-render-d3d \
    --disable-assembly --disable-mmx --disable-3dnow --disable-sse --disable-sse2 --disable-sse3 \
    --disable-altivec --disable-lsx \
    --enable-audio --enable-oss --disable-alsa --disable-pulseaudio --disable-pipewire --disable-jack \
    --disable-esd --disable-arts --disable-nas --disable-sndio --disable-fusionsound --disable-diskaudio \
    --disable-libsamplerate --disable-dbus --disable-ime --disable-ibus --disable-fcitx --disable-libudev \
    --disable-hidapi --disable-hidapi-libusb --disable-loadso --disable-rpath \
    --disable-dependency-tracking --enable-pthreads --enable-joystick --enable-haptic --disable-sensor \
    ${SDL_EXTRA_CONFIGURE:-} > "$BUILD/sdl2-configure.log" 2>&1 \
    || { tail -30 "$BUILD/sdl2-configure.log" >&2; die "SDL2 configure failed (log: build/sdl2-configure.log)"; }
grep -q '^#define SDL_VIDEO_DRIVER_WDFB 1' include/SDL_config.h || die "wdfb not enabled in SDL_config.h"
log "building SDL2"
make -j"$JOBS" > "$BUILD/sdl2-make.log" 2>&1 || { tail -40 "$BUILD/sdl2-make.log" >&2; die "SDL2 build failed (log: build/sdl2-make.log)"; }
make install > "$BUILD/sdl2-install.log" 2>&1 || die "SDL2 install failed"
log "libSDL2.a: $(stat -c %s "$PREFIX/lib/libSDL2.a") bytes"

log "building sdltest"
"$CC" -static -no-pie $CFLAGS_I486 -Wall -Wextra -Wl,--gc-sections \
    -I"$PREFIX/include/SDL2" -D_REENTRANT -o "$OUT/sdl/sdltest.i486" "$SDLDIR/sdltest/sdltest.c" \
    -L"$PREFIX/lib" -lSDL2 -lm -lpthread || die "sdltest build failed"
"$CC" -static -no-pie $CFLAGS_I486 -Wall -Wextra -Werror -Wl,--gc-sections \
    -o "$OUT/sdl/vtstate" "$SDLDIR/tests/vtstate.c" || die "vtstate build failed"
"${MUSL_TC_PREFIX}strip" -s "$OUT/sdl/sdltest.i486" "$OUT/sdl/vtstate"
"$SDLDIR/check-i486.sh" "$OUT/sdl/sdltest.i486" "$PREFIX/lib/libSDL2.a"
log "sdltest.i486: $(stat -c %s "$OUT/sdl/sdltest.i486") bytes, sha256 $(sha256sum "$OUT/sdl/sdltest.i486" | cut -d' ' -f1)"
