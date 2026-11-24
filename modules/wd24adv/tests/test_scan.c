/* SPDX-License-Identifier: GPL-2.0 */
/* Mode 13h live-scan line, and the picture that makes a start offset obvious. */
#include <stdio.h>
#include <string.h>
#include "vgamodel.h"
#include "../../../tools/wd24-scan/wd24x-scanpat.h"

static int fails, passes;
#define T(cond, what) do { if (cond) passes++; else { fails++; \
	printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

int main(void)
{
	struct vgamodel m;
	struct wd24_ctx c;
	struct wd24_io io;
	static u8 planes[4 * WD24_PLANE];
	struct wd24_scan sc;
	const struct wd24_mode *m13 = wd24_mode_find("320x200-8");
	uint8_t pic[WD24_SCAN_FRAME];
	char line[384];
	unsigned int n;
	unsigned int text_rows = WD24_SCAN_BAR_ROW - WD24_SCAN_PHOTO_ROW;
	unsigned int origin = text_rows * WD24_SCAN_TEXT_ROW;
	unsigned int crtc = origin / 2;
	int err;

	T(WD24_SCAN_PHOTO_BYTES == 36160u, "photo start is 36160");
	T(text_rows == 113u, "113 text rows");
	T(origin == 18080u, "text origin 18080");
	T(crtc == 9040u && (crtc << 2) == WD24_SCAN_PHOTO_BYTES,
	  "word start << 2 is the doubleword byte");
	T(WD24_SCAN_PHOTO_BYTES % WD24_SCAN_W == 0, "whole graphics rows");
	T((168u * 320u - 55u * 320u) == 36160u, "bar row 168 lands on display row 55");
	T(36000u % 320u == 160u, "36000 is half a row, not a text step");

	wd24_scanpat_fill(pic);
	T(pic[0] == 0 && pic[3] == 0, "row 0 bit 0 is four black pixels");
	T(pic[40] == WD24_SCAN_RED && pic[79] == WD24_SCAN_RED, "red block");
	T(pic[15 * 320 + 40] == WD24_SCAN_RED, "red block is 16 rows");
	T(pic[16 * 320 + 40] != WD24_SCAN_RED, "red block stops at row 16");
	T(pic[240] == WD24_SCAN_GRN && pic[279] == WD24_SCAN_GRN, "green block");
	T(pic[184 * 320 + 40] == WD24_SCAN_BLU &&
	  pic[199 * 320 + 79] == WD24_SCAN_BLU, "blue block");
	T(pic[184 * 320 + 240] == WD24_SCAN_YEL &&
	  pic[199 * 320 + 279] == WD24_SCAN_YEL, "yellow block");
	T(pic[10 * 320 + 32] == 15, "row 10 is white");
	T(pic[11 * 320 + 32] == 2, "row 11 band");
	T(pic[55 * 320 + 32] == 6, "row 55 band");
	T(pic[168 * 320 + 0] == 0 && pic[168 * 320 + 12] == 15 &&
	  pic[168 * 320 + 20] == 15 && pic[168 * 320 + 28] == 15,
	  "row 168 bits are four pixels wide");
	T(WD24_SCAN_RED > 14 && WD24_SCAN_GRN > 14 &&
	  WD24_SCAN_BLU > 14 && WD24_SCAN_YEL > 14,
	  "marker indexes are not the 1..14 bands");
	T(pic[WD24_SCAN_PHOTO_BYTES] == 15, "byte 36160 is row 113 bit 0");
	T(pic[WD24_SCAN_PHOTO_BYTES] != 4, "a 113-row start is not the red block");

	vm_init(&m, VM_WD24, 3);
	vm_io(&io, &m);
	wd24_ctx_init(&c, &wd24_port_backend);
	c.io = &io;
	c.planes = planes;
	c.be->begin(&c);
	err = wd24_plan_load(&c);
	if (!err)
		err = wd24_plan_mode(&c, m13);
	wd24_scan_read(&c, &sc);
	c.be->end(&c);
	if (err) {
		unsigned int i;

		printf("plan err %d\n", err);
		for (i = 0; i < c.nev; i++)
			if (c.ev[i].note)
				printf("  %s\n", c.ev[i].note);
	}
	T(err == 0, "13h plan");
	T(sc.cr0c == 0 && sc.cr0d == 0 && sc.cr08 == 0 && sc.ar13 == 0,
	  "start and pan read back 0");
	T(sc.cr14 == 0x40 && sc.cr17 == 0xa3 && sc.cr13 == 0x28 && sc.cr09 == 0x41,
	  "13h clocking");
	T(sc.sr04 == 0x0e && sc.gr05 == 0x40 && sc.gr06 == 0x05, "chain-4 map");
	n = wd24_scan_format(line, sizeof(line), "mode", &sc);
	T(n > 0 && strstr(line, "scan mode ") && strstr(line, "cr0c=00") &&
	  strstr(line, "cr0d=00") && strstr(line, "dw_bytes=0") &&
	  line[n - 1] == '\n', "scan line");
	T(wd24_scan_format(line, 4, "mode", &sc) < 4, "short buffer still ends");

	printf("test_scan: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
