/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 320x200 test picture for the mode 13h scanout.
 *
 * Columns 0..31 are the row number, four pixels per bit, low bit on
 * the left, colour 15 or 0. A one-pixel bit column reads as a ragged
 * edge in a photo. Every tenth row is colour 15 from column 32 across.
 * Other rows use band colours 1..14.
 *
 * The four corner blocks are 40 by 16 and use indexes that are not
 * those band colours. A one-row block in colour 4 or 2 sits on the
 * text palette's dark red and dark green and does not show in a photo;
 * the bottom blocks were eight rows of blue and yellow, which did.
 *   rows 0..15,    columns 40..79    0xE0 red
 *   rows 0..15,    columns 240..279  0xE1 green
 *   rows 184..199, columns 40..79    0xE2 blue
 *   rows 184..199, columns 240..279  0xE3 yellow
 * The program writes those four DAC entries full-scale, plus 0 and 15.
 *
 * Doom's status bar is buffer rows 168..199. Seen near display row 55,
 * the scan starts 113 rows down: 113 * 320 = 36160.
 */
#ifndef WD24X_SCANPAT_H
#define WD24X_SCANPAT_H

#include <stdint.h>

#define WD24_SCAN_W 320u
#define WD24_SCAN_H 200u
#define WD24_SCAN_FRAME (WD24_SCAN_W * WD24_SCAN_H)
#define WD24_SCAN_BAR_ROW 168u
#define WD24_SCAN_PHOTO_ROW 55u
#define WD24_SCAN_TEXT_ROW 160u
#define WD24_SCAN_PHOTO_BYTES ((WD24_SCAN_BAR_ROW - WD24_SCAN_PHOTO_ROW) * WD24_SCAN_W)
#define WD24_SCAN_RED 0xe0u
#define WD24_SCAN_GRN 0xe1u
#define WD24_SCAN_BLU 0xe2u
#define WD24_SCAN_YEL 0xe3u

static inline uint8_t wd24_scanpat_at(unsigned int x, unsigned int y)
{
	if (x >= WD24_SCAN_W || y >= WD24_SCAN_H)
		return 0;
	if (y < 16 && x >= 40 && x < 80)
		return WD24_SCAN_RED;
	if (y < 16 && x >= 240 && x < 280)
		return WD24_SCAN_GRN;
	if (y >= 184 && x >= 40 && x < 80)
		return WD24_SCAN_BLU;
	if (y >= 184 && x >= 240 && x < 280)
		return WD24_SCAN_YEL;
	if (x < 32)
		return (y & (1u << (x / 4u))) ? 15 : 0;
	if ((y % 10u) == 0)
		return 15;
	return (uint8_t)((y / 10u) % 14u + 1u);
}

static inline void wd24_scanpat_fill(uint8_t *dst)
{
	unsigned int x, y;

	for (y = 0; y < WD24_SCAN_H; y++)
		for (x = 0; x < WD24_SCAN_W; x++)
			dst[y * WD24_SCAN_W + x] = wd24_scanpat_at(x, y);
}

#endif
