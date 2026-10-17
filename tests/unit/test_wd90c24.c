// SPDX-License-Identifier: GPL-2.0
/* Unit tests for modules/wd90c24fb/wd90c24_hw.c against tests/unit/wdmock.c */
#include <stdio.h>
#include <string.h>
#include "../../modules/wd90c24fb/wd90c24_hw.h"

static int fails, passes;
#define CHECK(cond, ...) do { if (cond) passes++; else { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void t_probe_kinds(void)
{
	static const struct { enum mock_kind k; enum wd_chip want; const char *name; } cases[] = {
		{ M_VGA, WD_NONE, "plain VGA (QEMU std)" },
		{ M_PVGA1, WD_PVGA1, "PVGA1A" },
		{ M_C00, WD_C00, "WD90C00" },
		{ M_C1X, WD_C1X, "WD90C1x" },
		{ M_C20, WD_C2X, "WD90C20" },
		{ M_C24, WD_C24, "WD90C24" },
		{ M_C24A, WD_C24, "WD90C24A" },
		{ M_C30, WD_C30, "WD90C30" },
		{ M_C31, WD_C31, "WD90C31" },
	};
	struct mock_snap before, after;
	char why[64];
	unsigned int i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct wd_hw hw;
		enum wd_chip c;

		mock_reset(cases[i].k);
		mock_snap(&before);
		c = wd_probe(&hw);
		mock_snap(&after);
		CHECK(c == cases[i].want, "%s: probe says %s", cases[i].name, wd_chip_name(c));
		CHECK(!mock_snap_diff(&before, &after, why, sizeof(why)), "%s: probe left %s", cases[i].name, why);
		CHECK(M.timing_writes == 0, "%s: probe wrote %u timing/clock registers", cases[i].name, M.timing_writes);
		CHECK(hw.ext_blocks == (cases[i].k == M_C24A || cases[i].k == M_C31),
		      "%s: ext_blocks=%d", cases[i].name, hw.ext_blocks);
		if (cases[i].k != M_C24A && cases[i].k != M_C24 && cases[i].k != M_C31)
			CHECK(M.ext_port_writes == 0, "%s: touched 0x23c0 on a chip without it", cases[i].name);
	}
}

static void t_memory(void)
{
	struct wd_hw hw;

	mock_reset(M_C24A); M.gfx[0x0b] = 0xc5;
	wd_probe(&hw);
	CHECK(hw.vram_kb == 1024 && !hw.use_1meg, "1M: got %u", hw.vram_kb);
	CHECK(hw.pr19 == 0x30 && hw.pr18 == 0x0c, "panel regs read through PR1B: %02x %02x", hw.pr18, hw.pr19);

	mock_reset(M_C24A); M.gfx[0x0b] = 0x85;
	wd_probe(&hw);
	CHECK(hw.vram_kb == 512 && !hw.use_1meg, "512K, LCD on: got %u", hw.vram_kb);

	mock_reset(M_C24A); M.gfx[0x0b] = 0x85; M.fp[0x31] = 0x04; M.fp[0x32] = 0x20;
	wd_probe(&hw);
	CHECK(hw.vram_kb == 1024 && hw.use_1meg, "512K dual scan + CRT only: got %u", hw.vram_kb);

	mock_reset(M_C30); M.gfx[0x0b] = 0x45;
	wd_probe(&hw);
	CHECK(hw.vram_kb == 256, "256K: got %u", hw.vram_kb);
}

static void scribble(void)
{
	/* what a mode set does to the extended registers */
	wd_wrinx(WD_GFX, PR0A, 0x12);
	wd_wrinx(WD_GFX, PR0B, 0x34);
	wd_modinx(WD_GFX, PR1, 0x08, 0x08);
	wd_wrinx(WD_GFX, PR3, 0x00);
	wd_wrinx(WD_GFX, PR4, 0x01);
	wd_wrinx(0x3d4, PR13, 0x00);
	wd_wrinx(0x3d4, PR14, 0x00);
	wd_wrinx(0x3d4, PR15, 0x00);
	wd_wrinx(0x3d4, PR16, 0x00);
	wd_wrinx(WD_SEQ, PR31, 0x5c);
	wd_wrinx(WD_SEQ, PR30A, 0xc1);
	wd_wrinx(WD_SEQ, PR72, 0x50);
	wd_wrinx(WD_SEQ, PR68, (wd_rdinx(WD_SEQ, PR68) & 0xe7) | 0x08);
	wd_wrinx(WD_SEQ, PR72, 0x00);
}

static void t_save_restore(void)
{
	static const enum mock_kind kinds[] = { M_C24, M_C24A, M_C30, M_C31 };
	struct mock_snap before, after;
	struct wd_ext_state s;
	struct wd_hw hw;
	char why[64];
	unsigned int i;

	for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		mock_reset(kinds[i]);
		M.gfx[0x09] = 0x00; M.gfx[0x0e] = 0x00; M.crtc[0x2e] = 0x10;
		M.seq[0x10] = 0xc5; M.seq[0x11] = 0x4d; M.crtc[0x2a] = 0x07;
		wd_probe(&hw);
		mock_snap(&before);
		wd_save_ext(&hw, &s);
		scribble();
		wd_restore_ext(&hw, &s);
		wd_relock(&hw, &s);
		mock_snap(&after);
		CHECK(!mock_snap_diff(&before, &after, why, sizeof(why)), "kind %d: save/restore left %s", kinds[i], why);
		CHECK((M.seq[0x31] & 0x07) == 0x05, "kind %d: PR68 MCLK bits changed", kinds[i]);
	}
}

int main(void)
{
	t_probe_kinds();
	t_memory();
	t_save_restore();
	printf("wd90c24 unit tests: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
