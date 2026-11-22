/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include <string.h>
#include "wd24x-doom-keys.h"
#include "wd24x-doom-present.h"
#include "wd24x-doom-time.h"

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

static unsigned fnv(const uint8_t *p, unsigned n)
{
	unsigned s = 2166136261u;
	unsigned i;

	for (i = 0; i < n; i++) {
		s ^= p[i];
		s *= 16777619u;
	}
	return s;
}

int main(void)
{
	uint8_t src[WD24X_DOOM_SRC_W * WD24X_DOOM_SRC_H];
	uint8_t dst[WD24X_DOOM_FRAME + 16];
	struct wd24x_bank_stat st;
	struct wd24x_keys keys;
	unsigned char dk;
	unsigned i, c, differ;

	T(wd24x_doom_present_mode(0) == WD24X_DOOM_LETTER, "default letter");
	T(wd24x_doom_present_mode("letterbox") == WD24X_DOOM_LETTER, "name letter");
	T(wd24x_doom_present_mode("fill") == WD24X_DOOM_FILL, "name fill");
	T(wd24x_doom_present_mode("stretch") < 0, "unknown present");
	T(wd24x_doom_fb_geometry_ok(640, 480, 8, 640, 0x4b000), "geom");
	T(!wd24x_doom_fb_geometry_ok(640, 480, 32, 640, 0x4b000), "bpp");
	T(!wd24x_doom_fb_geometry_ok(640, 480, 8, 1024, 0x4b000), "line");
	T(!wd24x_doom_fb_geometry_ok(640, 400, 8, 640, 0x4b000), "yshort");
	T(!wd24x_doom_fb_geometry_ok(640, 480, 8, 640, 0x4afff), "smem");

	for (c = 0; c < 256; c++) {
		uint16_t v = wd24x_playpal_chan(c);

		if ((v >> 10) != (c >> 2) || (v & 0x03ff) != 0 || v > 0xfc00)
			break;
	}
	T(c == 256, "playpal chan");
	differ = 0;
	for (c = 0; c < 256; c++) {
		unsigned fbcon = (c * 257u) >> 10;

		if (fbcon != (c >> 2))
			differ++;
	}
	T(differ == 0, "6-bit matches fbcon shift");

	memset(src, 0, sizeof src);
	src[0] = 15;
	src[WD24X_DOOM_SRC_W + 3] = 9;
	memset(dst, 0xa5, sizeof dst);
	wd24x_doom_present(WD24X_DOOM_LETTER, src, dst, &st);
	T(dst[39 * 640] == 0, "letter top border");
	T(dst[40 * 640] == 15 && dst[40 * 640 + 1] == 15, "letter 2x x");
	T(dst[41 * 640] == 15, "letter 2x y");
	T(dst[42 * 640] == 0, "letter next src row");
	T(dst[42 * 640 + 6] == 9 && dst[42 * 640 + 7] == 9, "letter src x3");
	T(dst[439 * 640] == 0, "letter last picture row is src 199");
	T(dst[440 * 640] == 0, "letter bottom border");
	T(st.crossed == 0 && st.past_frame == 0 && st.back == 0, "letter banks");
	T(st.switches == 4 && st.max_consec == 1, "letter forward switches");
	T(dst[WD24X_DOOM_FRAME] == 0xa5, "letter guard");
	{
		uint8_t exp[WD24X_DOOM_DST_W];
		uint8_t pair[WD24X_DOOM_SRC_W];

		memset(pair, 0, sizeof pair);
		pair[0] = 1;
		pair[1] = 2;
		wd24x_doom_expand2(exp, pair);
		T(exp[0] == 1 && exp[1] == 1 && exp[2] == 2 && exp[3] == 2,
		  "dword expand");
	}
	memset(dst, 0x5a, sizeof dst);
	wd24x_doom_letter_bars(dst, NULL);
	T(dst[0] == 0 && dst[39 * 640] == 0 && dst[440 * 640] == 0, "bars");
	T(dst[40 * 640] == 0x5a, "bars leave picture");
	wd24x_doom_letter_picture(src, dst, NULL);
	T(dst[0] == 0 && dst[40 * 640] == 15 && dst[41 * 640 + 1] == 15,
	  "picture");
	src[0] = 4;
	wd24x_doom_letter_picture(src, dst, NULL);
	T(dst[0] == 0 && dst[440 * 640] == 0 && dst[40 * 640] == 4,
	  "second picture keeps bars");
	src[0] = 15;

	memset(dst, 0xa5, sizeof dst);
	wd24x_doom_present(WD24X_DOOM_FILL, src, dst, &st);
	T(dst[0] == 15 && dst[1] == 15, "fill first");
	T(dst[640] == 15, "fill second row still src 0");
	T(dst[2 * 640] == 0, "fill src row 1 starts at dest 2");
	T(dst[2 * 640 + 6] == 9, "fill src x3");
	T(wd24x_doom_fill_src_y(0) == 0 && wd24x_doom_fill_src_y(1) == 0, "rep 0");
	T(wd24x_doom_fill_src_y(2) == 1 && wd24x_doom_fill_src_y(4) == 1, "rep 1");
	T(wd24x_doom_fill_src_y(479) == 199, "last src row");
	T(st.crossed == 0 && st.past_frame == 0 && st.back == 0, "fill banks");
	T(st.switches == 4 && st.max_consec == 1, "fill switches");
	T(dst[WD24X_DOOM_FRAME] == 0xa5, "fill guard");
	for (i = 0; i < 5; i++) {
		unsigned n = 0;
		unsigned k;

		for (k = 0; k < 12; k++)
			n += wd24x_doom_fill_src_y(k) == i;
		if (n != wd24x_doom_fill_rep(i))
			break;
	}
	T(i == 5, "twelve rows per five");

	memset(&keys, 0, sizeof keys);
	T(!wd24x_keys_byte(&keys, 27, &dk), "esc start");
	T(!wd24x_keys_byte(&keys, '[', &dk), "esc bracket");
	T(wd24x_keys_byte(&keys, 'A', &dk) && dk == WD24X_KEY_UP, "up");
	memset(&keys, 0, sizeof keys);
	T(wd24x_keys_byte(&keys, 'w', &dk) && dk == 'W', "letter");
	T(wd24x_keys_byte(&keys, '\r', &dk) && dk == WD24X_KEY_ENTER, "enter");
	T(fnv(src, sizeof src) != 0, "src nonzero");
	T(wd24x_doom_index_hashed(0) == 0, "sum off skips the index hash");
	T(wd24x_doom_index_hashed(1) == 1, "sum on keeps the index hash");
	T(wd24x_doom_present_mode("13h") == WD24X_DOOM_DIRECT, "13h mode");
	T(wd24x_doom_present_mode("letterbox") == WD24X_DOOM_LETTER, "letter mode");
	T(strcmp(wd24x_doom_present_name(WD24X_DOOM_DIRECT), "13h") == 0, "13h name");
	T(wd24x_doom_fb_geometry_13h(320, 200, 8, 320, 64000) == 1, "13h geom");
	T(wd24x_doom_fb_geometry_13h(640, 480, 8, 640, 0x4b000) == 0, "640 is not 13h");
	T(wd24x_doom_13h_pick(0, 1, 1) == WD24X_DOOM_DIRECT, "already 13h");
	T(wd24x_doom_13h_pick(0, 1, 0) == -1, "13h ioctl fail stops");
	T(wd24x_doom_13h_pick(1, 0, 0) == WD24X_DOOM_LETTER, "put refused");
	T(wd24x_doom_13h_pick(1, 1, 0) == WD24X_DOOM_LETTER, "ioctl refused on 640");
	T(wd24x_doom_13h_pick(1, 1, 1) == WD24X_DOOM_DIRECT, "put then direct");
	T(wd24x_doom_13h_pick(0, 0, 0) == -1, "neither geometry");

	{
		struct wd24x_doom_time tm;
		int i;

		wd24x_doom_time_init(&tm, 2);
		T(wd24x_doom_time_frame(&tm, 1000, 1000, 5000) == 0, "first");
		T(tm.present_ns == 4000 && tm.render_ns == 0, "present only");
		T(wd24x_doom_time_frame(&tm, 8000, 9000, 10000) == 1, "every 2");
		T(tm.render_ns == 3000 && tm.palette_ns == 1000 &&
		  tm.present_ns == 5000, "split");
		T(wd24x_doom_time_ms(2500000ull) == 2, "ms");
		wd24x_doom_time_init(&tm, 0);
		for (i = 0; i < 5; i++)
			T(wd24x_doom_time_frame(&tm, 1, 1, 1) == 0, "off");
	}

	printf("test_doom_present: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
