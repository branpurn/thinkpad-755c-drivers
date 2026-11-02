#!/bin/sh
# doomgeneric for wd90c24fb (ThinkPad 755C): static i486 musl, no SDL.
# Doom draws 8-bit paletted straight into the frame buffer's mmap'd shadow.
# See doom/README.md.
#   out/doom/doom-wdfb   the game (stripped)
#   out/sdl/vtstate      test helper for doom/tests/qemu-doom.py
#   out/doom/fbdump      test helper: frame buffer + palette as a PPM
. "$(dirname "$0")/../scripts/common.sh"

DG_COMMIT=e257a09527062bfb986b6e5e0780839b6cbd5b71
DG_URL=https://github.com/ozkl/doomgeneric/archive/$DG_COMMIT.tar.gz
DG_SHA256=26f8f60daa6c6e4d3c01e5eac9fb700d592dede803d13ac848154ff356b2a2e7

CC="${MUSL_TC_PREFIX}gcc"
command -v "$CC" >/dev/null || die "musl toolchain missing: run 'make tools' (or 'make') in $TC32_DIR"
# i486: no cmov / MMX / SSE / x87 fcomi. -O2: the renderer is the hot path.
CFLAGS_I486="-march=i486 -mtune=i486 -O2 -fno-pie -ffunction-sections -fdata-sections"
# CMAP256: 8-bit pixels; DG_DIRECT + SCREENPITCH: draw into the 640-wide shadow
DOOM_DEFS="-DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE -DCMAP256 -DDG_DIRECT -DSCREENPITCH=640"
DOOMDIR="$TOP/doom"
SRC="$BUILD/doomgeneric-$DG_COMMIT"
OBJ="$BUILD/doom-obj"

tarball=$(fetch "$DG_URL" "$DG_SHA256")
rm -rf "$SRC" "$OBJ"
tar -xzf "$tarball" -C "$BUILD"
log "patching doomgeneric ${DG_COMMIT%"${DG_COMMIT#???????}"}"
for p in "$DOOMDIR"/patches/*.patch; do
    patch -d "$SRC" -p1 --no-backup-if-mismatch -s < "$p" || die "patch failed: $p"
done
cp "$DOOMDIR/doomgeneric_wdfb.c" "$TOP/modules/wd90c24fb/wd90c24fb_ioctl.h" "$SRC/doomgeneric/"

# upstream Makefile's SRC_DOOM, with our platform instead of doomgeneric_xlib
OBJS="dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net
f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_sound i_system
i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc m_random p_ceilng
p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup
p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky
r_things sha1 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff w_checksum w_file
w_main w_wad z_zone w_file_stdc i_input i_video doomgeneric doomgeneric_wdfb"

log "building doom-wdfb"
mkdir -p "$OBJ" "$OUT/doom" "$OUT/sdl"
# -w: upstream's 1990s C warns a lot; our file is built with warnings below
for o in $OBJS; do
    echo "$o"
done | xargs -P "$JOBS" -I{} sh -c \
    "\"$CC\" $CFLAGS_I486 $DOOM_DEFS -w -c \"$SRC/doomgeneric/{}.c\" -o \"$OBJ/{}.o\"" \
    || die "doomgeneric build failed"
"$CC" $CFLAGS_I486 $DOOM_DEFS -Wall -Wextra -Wno-unused-parameter -Werror -fsyntax-only \
    -I"$SRC/doomgeneric" "$SRC/doomgeneric/doomgeneric_wdfb.c" || die "doomgeneric_wdfb.c has warnings"
objs=""
for o in $OBJS; do objs="$objs $OBJ/$o.o"; done
# shellcheck disable=SC2086
"$CC" -static -no-pie $CFLAGS_I486 -Wl,--gc-sections -o "$OUT/doom/doom-wdfb" $objs -lm \
    || die "doom-wdfb link failed"

log "building test helpers"
"$CC" -static -no-pie $CFLAGS_I486 -Wall -Wextra -Werror -Wl,--gc-sections \
    -o "$OUT/sdl/vtstate" "$TOP/sdl/tests/vtstate.c" || die "vtstate build failed"
"$CC" -static -no-pie $CFLAGS_I486 -Wall -Wextra -Werror -Wl,--gc-sections \
    -o "$OUT/doom/fbdump" "$DOOMDIR/tests/fbdump.c" || die "fbdump build failed"
"${MUSL_TC_PREFIX}strip" -s "$OUT/doom/doom-wdfb" "$OUT/sdl/vtstate" "$OUT/doom/fbdump"
"$TOP/sdl/check-i486.sh" "$OUT/doom/doom-wdfb" "$OUT/doom/fbdump"
log "doom-wdfb: $(stat -c %s "$OUT/doom/doom-wdfb") bytes, sha256 $(sha256sum "$OUT/doom/doom-wdfb" | cut -d' ' -f1)"
