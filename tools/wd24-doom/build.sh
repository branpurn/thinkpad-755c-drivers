#!/bin/sh
# Build the i486 doomgeneric backend. The engine is fetched, not committed.
# WD24X_DOOM_SRC defaults to the pin checked below.
set -eu
PIN=e257a09527062bfb986b6e5e0780839b6cbd5b71
SRC=${WD24X_DOOM_SRC:-/tmp/doom-src/doomgeneric/doomgeneric}
ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
CC=${WD24X_DOOM_CC:-/tmp/src/i486-linux-musl-cross/bin/i486-linux-musl-gcc}
STRIP=${WD24X_DOOM_STRIP:-/tmp/src/i486-linux-musl-cross/bin/i486-linux-musl-strip}
OUT=${WD24X_DOOM_OUT:-/tmp/wd24x-doom}
got=$(git -C "$(dirname "$SRC")" rev-parse HEAD)
if [ "$got" != "$PIN" ]; then
	echo "doomgeneric pin $PIN got $got"
	exit 1
fi
CFLAGS="-static -march=i486 -mtune=i486 -Os -std=gnu11 -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -Wno-missing-field-initializers -Wno-format -Wno-unused-but-set-variable -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -I$SRC -I$ROOT"
objs=
for c in dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input i_video doomgeneric; do
	o=/tmp/wd24-doom-obj/$c.o
	mkdir -p /tmp/wd24-doom-obj
	$CC $CFLAGS -c "$SRC/$c.c" -o "$o"
	objs="$objs $o"
done
$CC $CFLAGS -Werror -c "$ROOT/doomgeneric_wd24.c" -o /tmp/wd24-doom-obj/doomgeneric_wd24.o
$CC $CFLAGS $objs /tmp/wd24-doom-obj/doomgeneric_wd24.o -o "$OUT" -lm
$STRIP -o "$OUT.s" "$OUT"
echo "built $OUT.s"
sha256sum "$OUT.s"
stat -c '%s' "$OUT.s"
