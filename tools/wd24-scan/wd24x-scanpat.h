/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 320x200 test picture for the mode 13h scanout.
 *
 * Columns 0..7 of each row are that row number, low bit in column 0,
 * colour 15 for a set bit and 0 for a clear bit. Every tenth row is
 * colour 15 from column 8 across. Other rows are a band colour in
 * 1..14. Four markers sit on top of that:
 *   row 0,    columns 8..23    colour 4  (red in the text palette)
 *   row 0,    columns 304..319 colour 2  (green)
 *   rows 192..199, columns 8..23    colour 1 (blue)
 *   rows 192..199, columns 304..319 colour 14 (yellow)
 * A start address of 0 puts the red block at the top, just right of
 * the bit strip, and the yellow block in the bottom-right corner.
 *
 * Doom's status bar is buffer rows 168..199. Seen near display row 55,
 * the scan starts 113 rows down: 113 * 320 = 36160. vgacon's origin
 * moves in 160-byte text rows and write_vga(12, origin/2) stores that
 * in CR0C/CR0D. Mode 13h doubleword (CR14 bit 6) multiplies by 4, so
 * one text row is 320 graphics bytes and 113 text rows are 36160,
 * which is display row 55 with no horizontal shift. 36000 is 112 rows
 * plus 160 columns and is not a whole text row.
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

static inline uint8_t wd24_scanpat_at(unsigned int x, unsigned int y)
{
	if (x >= WD24_SCAN_W || y >= WD24_SCAN_H)
		return 0;
	if (y == 0 && x >= 8 && x < 24)
		return 4;
	if (y == 0 && x >= 304)
		return 2;
	if (y >= 192 && x >= 8 && x < 24)
		return 1;
	if (y >= 192 && x >= 304)
		return 14;
	if (x < 8)
		return (y & (1u << x)) ? 15 : 0;
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
