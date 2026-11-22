/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 320x200 index buffer -> 640x480x8 frame.
 *
 * The S6d mapping is WD24S6D_FRAME 0x4b000 bytes, line 640, and the
 * fault handler maps one 64 KiB window. A store that crosses a window
 * boundary is the livelock case, so every copy here stops at the
 * boundary. Bytes at and past 0x4b000 are the guard, not the frame.
 *
 * Letterbox is the timedemo path. Each source pixel is a 2x2 block.
 * The picture is 640x400 with 40 rows of index 0 above and below.
 * A source row is expanded once with aligned 32-bit stores (two
 * pixels per store) and emitted twice. The caller may write the bars
 * once per session; this function still writes them. Framebuffer
 * pixels are square, so the picture is 8:5. A CRT showed 320x200 as
 * 4:3 by using tall pixels. This choice does not do that.
 *
 * Fill: 2x horizontally, and vertically 2 or 3 rows in the repeating
 * pattern 2,3,2,2,3 (12 destination rows for each 5 source rows,
 * 40*12 = 480). 320x200 then covers 640x480, which is the integer
 * form of that 4:3 stretch. The pattern is exact, not a filter.
 */
#ifndef WD24X_DOOM_PRESENT_H
#define WD24X_DOOM_PRESENT_H

#include <stdint.h>
#include <string.h>

#define WD24X_DOOM_SRC_W	320u
#define WD24X_DOOM_SRC_H	200u
#define WD24X_DOOM_DST_W	640u
#define WD24X_DOOM_DST_H	480u
#define WD24X_DOOM_FRAME	0x4b000u
#define WD24X_DOOM_WIN		0x10000u
#define WD24X_DOOM_LETTER_Y	40u	/* (480 - 400) / 2 */

enum {
	WD24X_DOOM_LETTER = 0,
	WD24X_DOOM_FILL = 1,
	WD24X_DOOM_DIRECT = 2
};

struct wd24x_bank_stat {
	unsigned spans;
	unsigned switches;
	unsigned max_consec;
	unsigned crossed;
	unsigned past_frame;
	unsigned back;
	unsigned last_win;
	unsigned have_win;
	unsigned consec;
};

static inline int wd24x_doom_present_mode(const char *s)
{
	if (!s || !s[0] || strcmp(s, "letterbox") == 0)
		return WD24X_DOOM_LETTER;
	if (strcmp(s, "fill") == 0)
		return WD24X_DOOM_FILL;
	if (strcmp(s, "13h") == 0)
		return WD24X_DOOM_DIRECT;
	return -1;
}

static inline const char *wd24x_doom_present_name(int mode)
{
	if (mode == WD24X_DOOM_FILL)
		return "fill";
	if (mode == WD24X_DOOM_DIRECT)
		return "13h";
	return "letterbox";
}

/* The 64000-byte index hash feeds only the sum line. */
static inline int wd24x_doom_index_hashed(int sum_on)
{
	return sum_on ? 1 : 0;
}

/* Live mmap is the S6d contract: 640x480x8, line 640, frame bytes present. */
static inline int wd24x_doom_fb_geometry_ok(unsigned x, unsigned y,
					    unsigned bpp, unsigned line,
					    unsigned smem)
{
	return x == WD24X_DOOM_DST_W && y == WD24X_DOOM_DST_H && bpp == 8u &&
	       line == WD24X_DOOM_DST_W && smem >= WD24X_DOOM_FRAME;
}

/* Chain-4 320x200. smem is the 64000-byte frame, not the 640x480 map. */
static inline int wd24x_doom_fb_geometry_13h(unsigned int x, unsigned int y,
					     unsigned int bpp, unsigned int line,
					     unsigned int smem)
{
	return x == WD24X_DOOM_SRC_W && y == WD24X_DOOM_SRC_H && bpp == 8u &&
	       line == WD24X_DOOM_SRC_W &&
	       smem >= WD24X_DOOM_SRC_W * WD24X_DOOM_SRC_H &&
	       smem < WD24X_DOOM_FRAME;
}

/*
 * DIRECT when the fb is 320x200x8 and the direct ioctl succeeded.
 * LETTER when the fb started as 640x480 and 13h was refused, so the
 * letterbox ioctl is still the right present. -1 means stop: do not
 * letterbox a session that is not the 640x480 frame.
 */
static inline int wd24x_doom_13h_pick(int start_640, int after_13h,
				      int direct_ok)
{
	if (after_13h && direct_ok)
		return WD24X_DOOM_DIRECT;
	if (start_640)
		return WD24X_DOOM_LETTER;
	return -1;
}

/* Top 6 bits, stored the way the colormap candidate shifts them back. */
static inline uint16_t wd24x_playpal_chan(unsigned c)
{
	return (uint16_t)((c & 255u) >> 2) << 10;
}

static inline int wd24x_doom_palette_put(const char *v)
{
	return v && strcmp(v, "put") == 0;
}

/* Repeats per source row inside each group of five: 2,3,2,2,3. */
static inline unsigned wd24x_doom_fill_rep(unsigned i)
{
	return (i == 1u || i == 4u) ? 3u : 2u;
}

static inline unsigned wd24x_doom_fill_src_y(unsigned dy)
{
	unsigned group = dy / 12u;
	unsigned within = dy % 12u;
	unsigned acc = 0;
	unsigned i;

	for (i = 0; i < 5u; i++) {
		unsigned rep = wd24x_doom_fill_rep(i);

		if (within < acc + rep)
			return group * 5u + i;
		acc += rep;
	}
	return group * 5u + 4u;
}

static inline void wd24x_doom_emit(uint8_t *dst, unsigned off,
				   const uint8_t *bytes, unsigned n,
				   struct wd24x_bank_stat *st)
{
	while (n) {
		unsigned room, chunk, win;

		if (off >= WD24X_DOOM_FRAME) {
			if (st)
				st->past_frame++;
			return;
		}
		win = off / WD24X_DOOM_WIN;
		room = WD24X_DOOM_WIN - (off - win * WD24X_DOOM_WIN);
		chunk = n < room ? n : room;
		if (off + chunk > WD24X_DOOM_FRAME)
			chunk = WD24X_DOOM_FRAME - off;
		if (st) {
			if (st->have_win) {
				if (win != st->last_win) {
					if (win < st->last_win)
						st->back++;
					st->switches++;
					st->consec++;
					if (st->consec > st->max_consec)
						st->max_consec = st->consec;
				} else {
					st->consec = 0;
				}
			}
			st->have_win = 1;
			st->last_win = win;
			st->spans++;
			if (off / WD24X_DOOM_WIN != (off + chunk - 1) / WD24X_DOOM_WIN)
				st->crossed++;
		}
		if (dst)
			memcpy(dst + off, bytes, chunk);
		off += chunk;
		bytes += chunk;
		n -= chunk;
	}
}

/* Two source pixels become one aligned 32-bit store: a,a,b,b. */
static inline void wd24x_doom_expand2(uint8_t *dst, const uint8_t *src)
{
	unsigned x;

	for (x = 0; x < WD24X_DOOM_SRC_W; x += 2) {
		uint32_t a = src[x];
		uint32_t b = src[x + 1];
		uint32_t v = a | (a << 8) | (b << 16) | (b << 24);

		memcpy(dst + x * 2, &v, 4);
	}
}

/* Index 0 bars. 40 rows above the picture and 40 below. */
static inline void wd24x_doom_letter_bars(uint8_t *dst,
					  struct wd24x_bank_stat *st)
{
	uint8_t row[WD24X_DOOM_DST_W] __attribute__((aligned(4)));
	unsigned y;

	memset(row, 0, sizeof row);
	for (y = 0; y < WD24X_DOOM_LETTER_Y; y++)
		wd24x_doom_emit(dst, y * WD24X_DOOM_DST_W, row,
				WD24X_DOOM_DST_W, st);
	for (y = WD24X_DOOM_LETTER_Y + WD24X_DOOM_SRC_H * 2u;
	     y < WD24X_DOOM_DST_H; y++)
		wd24x_doom_emit(dst, y * WD24X_DOOM_DST_W, row,
				WD24X_DOOM_DST_W, st);
}

/* 200 source rows, each expanded once and stored as two dest rows. */
static inline void wd24x_doom_letter_picture(const uint8_t *src, uint8_t *dst,
					     struct wd24x_bank_stat *st)
{
	uint8_t row[WD24X_DOOM_DST_W] __attribute__((aligned(4)));
	unsigned sy;

	for (sy = 0; sy < WD24X_DOOM_SRC_H; sy++) {
		unsigned y = WD24X_DOOM_LETTER_Y + sy * 2u;

		wd24x_doom_expand2(row, src + sy * WD24X_DOOM_SRC_W);
		wd24x_doom_emit(dst, y * WD24X_DOOM_DST_W, row,
				WD24X_DOOM_DST_W, st);
		wd24x_doom_emit(dst, (y + 1u) * WD24X_DOOM_DST_W, row,
				WD24X_DOOM_DST_W, st);
	}
}

static inline void wd24x_doom_present(int mode, const uint8_t *src,
				      uint8_t *dst, struct wd24x_bank_stat *st)
{
	unsigned y;

	if (st)
		memset(st, 0, sizeof *st);
	if (mode != WD24X_DOOM_FILL) {
		/* Top, picture, bottom. Bottom before the picture would
		 * switch to the last window and back. */
		{
			uint8_t row[WD24X_DOOM_DST_W] __attribute__((aligned(4)));
			unsigned y;

			memset(row, 0, sizeof row);
			for (y = 0; y < WD24X_DOOM_LETTER_Y; y++)
				wd24x_doom_emit(dst, y * WD24X_DOOM_DST_W, row,
						WD24X_DOOM_DST_W, st);
			wd24x_doom_letter_picture(src, dst, st);
			for (y = WD24X_DOOM_LETTER_Y + WD24X_DOOM_SRC_H * 2u;
			     y < WD24X_DOOM_DST_H; y++)
				wd24x_doom_emit(dst, y * WD24X_DOOM_DST_W, row,
						WD24X_DOOM_DST_W, st);
		}
		return;
	}
	for (y = 0; y < WD24X_DOOM_DST_H; y++) {
		uint8_t row[WD24X_DOOM_DST_W] __attribute__((aligned(4)));
		unsigned sy = wd24x_doom_fill_src_y(y);

		wd24x_doom_expand2(row, src + sy * WD24X_DOOM_SRC_W);
		wd24x_doom_emit(dst, y * WD24X_DOOM_DST_W, row,
				WD24X_DOOM_DST_W, st);
	}
}

#endif
