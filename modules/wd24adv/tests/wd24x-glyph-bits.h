/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Five 8x8 cells. The top-left 5x7 is a digit. The rest of the cell is
 * paper. Ink is colormap index 15. Paper is index 0.
 *
 * These are pixel values, not DAC colours. This header does not know
 * whether 0 and 15 differ on a panel.
 */
#ifndef WD24X_GLYPH_BITS_H
#define WD24X_GLYPH_BITS_H

#include <stdint.h>

#define WD24X_GLYPH_N		5
#define WD24X_GLYPH_W		8
#define WD24X_GLYPH_H		8
#define WD24X_GLYPH_INK		15
#define WD24X_GLYPH_PAPER	0
#define WD24X_GLYPH_PITCH	640

/* Untouched 8x8. Not drawn. GetImage only. */
#define WD24X_GLYPH_CTRL_X	300
#define WD24X_GLYPH_CTRL_Y	40

static const uint16_t wd24x_glyph_xy[WD24X_GLYPH_N][2] = {
	{16, 16},
	{16, 120},
	{16, 220},
	{16, 320},
	{16, 420},
};

/*
 * Five columns, bit 4 = left. Seven rows. Digits 1..5.
 * 1 is a stem, not a solid block. 2..5 keep that stem from being the
 * only shape.
 */
static const uint8_t wd24x_glyph_rows[WD24X_GLYPH_N][7] = {
	{0x0c, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f},
	{0x1f, 0x01, 0x01, 0x1f, 0x10, 0x10, 0x1f},
	{0x1f, 0x01, 0x01, 0x1f, 0x01, 0x01, 0x1f},
	{0x11, 0x11, 0x11, 0x1f, 0x01, 0x01, 0x01},
	{0x1f, 0x10, 0x10, 0x1f, 0x01, 0x01, 0x1f},
};

static inline int wd24x_glyph_ink(int n, int row, int col)
{
	uint8_t bits;

	if (n < 0 || n >= WD24X_GLYPH_N || row < 0 || col < 0)
		return 0;
	if (row >= 7 || col >= 5)
		return 0;
	bits = wd24x_glyph_rows[n][row];
	return (bits & (uint8_t)(0x10u >> col)) != 0;
}

static inline uint8_t wd24x_glyph_pix(int n, int row, int col)
{
	return wd24x_glyph_ink(n, row, col) ? WD24X_GLYPH_INK
					    : WD24X_GLYPH_PAPER;
}

#endif
