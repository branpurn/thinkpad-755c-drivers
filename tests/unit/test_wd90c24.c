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

/* mode-set registers as the chip ends up holding them */
static void t_mode_set(void)
{
	struct wd_ext_state bios, ext;
	struct wd_std std;
	struct wd_hw hw;
	u8 pr11;

	mock_reset(M_C24A);
	M.seq[0x11] = 0x4d; M.seq[0x10] = 0x35; M.crtc[0x2e] = 0x50; M.crtc[0x2a] = 0x05;
	wd_probe(&hw);
	pr11 = M.crtc[0x2a];
	wd_save_ext(&hw, &bios);
	wd_mode_640x480x8(&hw, &bios, &std, &ext);
	wd_set_mode(&hw, &std, &ext);
	CHECK(M.misc == 0xe3, "MISC %02x", M.misc);
	CHECK(M.seq[4] == 0x0e && M.seq[1] == 0x01, "SEQ4 %02x SEQ1 %02x (screen must be on)", M.seq[4], M.seq[1]);
	CHECK(M.gfx[5] == 0x40 && M.gfx[6] == 0x05, "GR5 %02x GR6 %02x", M.gfx[5], M.gfx[6]);
	CHECK(M.crtc[0x13] == 0x50 && M.crtc[0x14] == 0x40 && M.crtc[0x17] == 0xe3, "CR13/14/17 %02x %02x %02x",
	      M.crtc[0x13], M.crtc[0x14], M.crtc[0x17]);
	CHECK(M.crtc[0x00] == 0x5f && M.crtc[0x06] == 0x0b && M.crtc[0x12] == 0xdf, "timing not written (CR11 protect?)");
	CHECK(M.attr[0x10] == 0x41 && (M.attr_i & 0x20), "ATC mode %02x, video %s", M.attr[0x10], (M.attr_i & 0x20) ? "on" : "off");
	CHECK(M.gfx[0x0e] == 0x01 && M.gfx[0x0d] == 0x00, "PR4 %02x PR3 %02x", M.gfx[0x0e], M.gfx[0x0d]);
	CHECK((M.gfx[0x0b] & 0xc8) == 0xc0, "PR1 %02x: size bits kept, single bank", M.gfx[0x0b]);
	CHECK((M.seq[0x31] & 0x1f) == 0x0d, "PR68 %02x: clock 0 select, MCLK kept", M.seq[0x31]);
	CHECK(M.seq[0x11] == 0x5d, "PR31 %02x", M.seq[0x11]);
	CHECK((M.seq[0x10] & 0xcf) == 0xc1 && (M.seq[0x10] & 0x30) == 0x30, "PR30A %02x", M.seq[0x10]);
	CHECK((M.crtc[0x2e] & 0x50) == 0x00, "PR15 %02x", M.crtc[0x2e]);
	CHECK(M.crtc[0x2a] == pr11, "LCD on: PR11 CRTC locks changed %02x -> %02x", pr11, M.crtc[0x2a]);
	CHECK(M.gfx[0x09] == 0, "bank not reset to 0");
}

static void t_banks(void)
{
	struct wd_ext_state bios, ext;
	struct wd_std std;
	struct wd_hw hw;

	mock_reset(M_C24A);
	wd_probe(&hw);
	wd_save_ext(&hw, &bios);
	wd_mode_640x480x8(&hw, &bios, &std, &ext);
	wd_set_mode(&hw, &std, &ext);
	CHECK(wd_selftest_banks(&hw, hw.vram_kb) == 0, "bank self-test fails on a C24A");
	CHECK(M.vram[0] == 0x5a && M.vram[0x8000] != M.vram[0x10000] && M.vram[0x40000] != 0, "markers not at 0/32K/64K/256K");

	/* QEMU-style plain VGA treated as a C24 (fake=1): must fail */
	mock_reset(M_VGA);
	hw.chip = WD_C24; hw.crtc = 0x3d4; hw.bank = -1; hw.vram_kb = 1024; hw.pr19 = 0; hw.ext_blocks = false;
	wd_save_ext(&hw, &bios);
	wd_mode_640x480x8(&hw, &bios, &std, &ext);
	wd_set_mode(&hw, &std, &ext);
	CHECK(wd_selftest_banks(&hw, 1024) != 0, "bank self-test passes on plain VGA");

	/* dual-bank mapping (PR1 bit 3) would break the linear window */
	mock_reset(M_C24A);
	wd_probe(&hw);
	wd_save_ext(&hw, &bios);
	wd_mode_640x480x8(&hw, &bios, &std, &ext);
	ext.pr1 |= 0x08;
	wd_set_mode(&hw, &std, &ext);
	CHECK(wd_selftest_banks(&hw, hw.vram_kb) != 0, "self-test misses a non-linear window");
}

static void t_flush(void)
{
	static u8 shadow[WD_FB_SIZE];
	struct wd_ext_state bios, ext;
	struct wd_std std;
	struct wd_hw hw;
	unsigned int i, bad = 0, seed = 1;

	mock_reset(M_C24A);
	wd_probe(&hw);
	wd_save_ext(&hw, &bios);
	wd_mode_640x480x8(&hw, &bios, &std, &ext);
	wd_set_mode(&hw, &std, &ext);
	for (i = 0; i < WD_FB_SIZE; i++)
		shadow[i] = (u8)(i * 7 + (i >> 9));
	wd_flush_rect(&hw, shadow, 0, 0, WD_XRES, WD_YRES);
	for (i = 0; i < WD_FB_SIZE; i++)
		bad += M.vram[i] != shadow[i];
	CHECK(bad == 0 && M.win_oob == 0, "full flush: %u bytes wrong, %u out-of-window", bad, M.win_oob);

	/* random rectangles, including rows that straddle 64 KiB (y = 102) */
	for (i = 0; i < 2000; i++) {
		unsigned int x, y, w, h, k;

		seed = seed * 1103515245 + 12345; x = (seed >> 8) % WD_XRES;
		seed = seed * 1103515245 + 12345; y = (seed >> 8) % WD_YRES;
		seed = seed * 1103515245 + 12345; w = 1 + (seed >> 8) % 200;
		seed = seed * 1103515245 + 12345; h = 1 + (seed >> 8) % 40;
		if (i == 0) { x = 300; y = 102; w = 340; h = 1; }
		for (k = 0; k < WD_FB_SIZE; k += 4099)
			shadow[k] ^= 0xff;
		memset(shadow + y * WD_PITCH + x, (u8)i, (w > WD_XRES - x ? WD_XRES - x : w));
		wd_flush_rect(&hw, shadow, x, y, w, h);
		for (k = 0; k < WD_FB_SIZE; k += 4099)
			shadow[k] ^= 0xff;	/* undo: only the rect was flushed */
		wd_flush_rect(&hw, shadow, 0, 0, WD_XRES, WD_YRES);
	}
	bad = 0;
	for (i = 0; i < WD_FB_SIZE; i++)
		bad += M.vram[i] != shadow[i];
	CHECK(bad == 0 && M.win_oob == 0, "rect flushes: %u bytes wrong, %u out-of-window", bad, M.win_oob);
	CHECK(M.vram[WD_FB_SIZE] == 0, "flush wrote past the frame");
}

/* save -> mode set -> restore returns every register (std via a snapshot) */
static void t_roundtrip(void)
{
	struct wd_ext_state bios, ext;
	struct mock_snap before, after;
	struct wd_std std, text;
	struct wd_hw hw;
	char why[64];
	int i;

	mock_reset(M_C24A);
	M.seq[0x10] = 0xc5; M.seq[0x11] = 0x4d; M.crtc[0x2a] = 0x07;
	M.seq[1] = 0x00; M.seq[2] = 0x03; M.seq[4] = 0x02; M.gfx[5] = 0x10; M.gfx[6] = 0x0e;
	M.crtc[0x09] = 0x4f; M.crtc[0x13] = 0x28; M.crtc[0x17] = 0xa3; M.attr[0x10] = 0x0c;
	wd_probe(&hw);
	mock_snap(&before);
	/* what vgastate saves */
	text.misc = M.misc;
	for (i = 0; i < 5; i++) text.seq[i] = M.seq[i];
	for (i = 0; i < 25; i++) text.crtc[i] = M.crtc[i];
	for (i = 0; i < 9; i++) text.gfx[i] = M.gfx[i];
	for (i = 0; i < 21; i++) text.attr[i] = M.attr[i];
	wd_save_ext(&hw, &bios);
	wd_mode_640x480x8(&hw, &bios, &std, &ext);
	wd_set_mode(&hw, &std, &ext);
	wd_selftest_banks(&hw, hw.vram_kb);
	wd_restore_ext(&hw, &bios);
	wd_write_std(&hw, &text);
	wd_screen_on(&hw, true);
	wd_relock(&hw, &bios);
	mock_snap(&after);
	CHECK(!mock_snap_diff(&before, &after, why, sizeof(why)), "mode set + restore left %s", why);
	CHECK(M.attr[0x10] == 0x0c, "ATC mode not restored");
}

/*
 * Driver stage 5 (APM suspend) in register terms: live -> powerdown blank ->
 * text for the BIOS (restore) -> re-save -> resume mode set -> rmmod
 * restore. Every pass must end at the BIOS snapshot, the re-saved state
 * must equal the first one, and MCLK (PR68 2:0, PR69) is never changed.
 */
static void t_suspend_cycle(void)
{
	struct wd_ext_state bios, bios2, ext;
	struct mock_snap before, after;
	struct wd_std std, text;
	struct wd_hw hw;
	char why[64];
	int pass, i;
	u8 mclk;

	mock_reset(M_C24A);
	wd_probe(&hw);
	mclk = M.seq[0x31] & 0x07;
	mock_snap(&before);
	text.misc = M.misc;	/* what vgastate saves */
	for (i = 0; i < 5; i++) text.seq[i] = M.seq[i];
	for (i = 0; i < 25; i++) text.crtc[i] = M.crtc[i];
	for (i = 0; i < 9; i++) text.gfx[i] = M.gfx[i];
	for (i = 0; i < 21; i++) text.attr[i] = M.attr[i];
	wd_save_ext(&hw, &bios);
	wd_relock(&hw, &bios);
	for (pass = 0; pass < 2; pass++) {
		wd_mode_640x480x8(&hw, &bios, &std, &ext);
		wd_set_mode(&hw, &std, &ext);
		CHECK((M.seq[0x31] & 0x07) == mclk, "pass %d: mode set changed MCLK", pass);
		wd_screen_on(&hw, false);
		wd_panel_off(&hw);
		wd_restore_ext(&hw, &bios);	/* wd_restore_text() order */
		wd_write_std(&hw, &text);
		wd_relock(&hw, &bios);
		wd_screen_on(&hw, true);
		wd_restore_panel(&hw, &bios);
		mock_snap(&after);
		CHECK(!mock_snap_diff(&before, &after, why, sizeof(why)), "pass %d: restore left %s", pass, why);
		wd_save_ext(&hw, &bios2);	/* the re-save on suspend */
		wd_relock(&hw, &bios2);
		CHECK(!memcmp(&bios, &bios2, sizeof(bios)), "pass %d: re-saved state differs", pass);
	}
	CHECK(M.seq[0x32] == before.seq[0x32], "PR69 changed");
}

static void live_c24a(struct wd_hw *hw, enum mock_kind k)
{
	struct wd_ext_state bios, ext;
	struct wd_std std;

	mock_reset(k);
	wd_probe(hw);
	wd_save_ext(hw, &bios);
	wd_mode_640x480x8(hw, &bios, &std, &ext);
	wd_set_mode(hw, &std, &ext);
}

/* reference: what a rectangle copy must produce (memmove semantics) */
static void ref_copy(u8 *fb, u32 sx, u32 sy, u32 dx, u32 dy, u32 w, u32 h)
{
	static u8 tmp[WD_FB_SIZE];
	u32 r;

	for (r = 0; r < h; r++)
		memcpy(tmp + r * w, fb + (sy + r) * WD_PITCH + sx, w);
	for (r = 0; r < h; r++)
		memcpy(fb + (dy + r) * WD_PITCH + dx, tmp + r * w, w);
}

static void t_blt(void)
{
	static u8 ref[WD_FB_SIZE];
	struct wd_hw hw;
	unsigned int i, bad, seed = 7;
	u32 r;

	live_c24a(&hw, M_C24A);
	M.blt_busy_polls = 5;
	for (i = 0; i < WD_FB_SIZE; i++)
		M.vram[i] = ref[i] = (u8)(i * 13 + (i >> 11));

	CHECK(wd_blt_fill(&hw, 10, 100, 50, 20, 0x07, false) == 0, "fill timed out");
	for (r = 0; r < 20; r++)
		memset(ref + (100 + r) * WD_PITCH + 10, 0x07, 50);
	CHECK(wd_blt_fill(&hw, 600, 470, 40, 10, 0xff, true) == 0, "xor fill timed out");
	for (r = 0; r < 10; r++) {
		u32 k;

		for (k = 0; k < 40; k++)
			ref[(470 + r) * WD_PITCH + 600 + k] ^= 0xff;
	}
	/* overlapping copies in all four directions, plus fbcon's scroll */
	{
		static const u32 c[][6] = {
			{ 0, 16, 0, 0, 640, 464 },	/* scroll up one text line */
			{ 0, 0, 0, 16, 640, 464 },	/* scroll down */
			{ 100, 50, 108, 50, 200, 100 },	/* right, same rows */
			{ 108, 50, 100, 50, 200, 100 },	/* left */
			{ 30, 200, 40, 210, 300, 100 },
			{ 40, 210, 30, 200, 300, 100 },
		};
		for (i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
			CHECK(wd_blt_copy(&hw, c[i][0], c[i][1], c[i][2], c[i][3], c[i][4], c[i][5]) == 0,
			      "copy %u timed out", i);
			ref_copy(ref, c[i][0], c[i][1], c[i][2], c[i][3], c[i][4], c[i][5]);
		}
	}
	for (i = 0; i < 300; i++) {
		u32 w, h, sx, sy, dx, dy;

		seed = seed * 1103515245 + 12345; w = 1 + (seed >> 8) % 320;
		seed = seed * 1103515245 + 12345; h = 1 + (seed >> 8) % 120;
		seed = seed * 1103515245 + 12345; sx = (seed >> 8) % (WD_XRES - w + 1);
		seed = seed * 1103515245 + 12345; sy = (seed >> 8) % (WD_YRES - h + 1);
		seed = seed * 1103515245 + 12345; dx = (seed >> 8) % (WD_XRES - w + 1);
		seed = seed * 1103515245 + 12345; dy = (seed >> 8) % (WD_YRES - h + 1);
		wd_blt_copy(&hw, sx, sy, dx, dy, w, h);
		ref_copy(ref, sx, sy, dx, dy, w, h);
	}
	CHECK(wd_blt_wait(&hw) == 0, "engine busy at the end");
	bad = 0;
	for (i = 0; i < WD_FB_SIZE; i++)
		bad += M.vram[i] != ref[i];
	CHECK(bad == 0, "blits: %u bytes differ from the reference", bad);
	CHECK(M.blt_started_while_busy == 0, "%u blits started while the engine was busy", M.blt_started_while_busy);
}

static void t_blt_selftest(void)
{
	struct wd_hw hw;

	live_c24a(&hw, M_C24A);
	M.blt_busy_polls = 50;
	CHECK(wd_blt_selftest(&hw) == 0, "BITBLT self-test fails on a C24A");
	CHECK(M.gfx[0x09] == 0, "self-test left bank %02x", M.gfx[0x09]);

	live_c24a(&hw, M_C24A);
	M.blt_stuck = true;
	CHECK(wd_blt_selftest(&hw) != 0, "self-test passes with a stuck engine");

	live_c24a(&hw, M_C24);	/* plain C24: no 23C0 blocks */
	CHECK(!hw.ext_blocks && wd_blt_selftest(&hw) != 0, "self-test passes without a BITBLT engine");
}

static void t_panel(void)
{
	struct mock_snap before, after;
	struct wd_ext_state bios, ext;
	struct wd_std std, text;
	struct wd_hw hw;
	char why[64];
	int i;

	mock_reset(M_C24A);
	wd_probe(&hw);
	CHECK(wd_panel_off(&hw), "panel_off: panel was on");
	CHECK(!(M.fp[0x32] & 0x10) && (M.gfx[0x0e] & 0x20) && !(M.seq[0x19] & 0x04),
	      "off: PR19 %02x PR4 %02x PR57 %02x", M.fp[0x32], M.gfx[0x0e], M.seq[0x19]);
	CHECK(!strcmp(M.panel_log, "74P"), "off order %s, want PR57 (while PR19 bit 4 still gives it PNLOFF), PR4, PR19", M.panel_log);
	CHECK(!wd_panel_off(&hw), "panel_off twice");
	M.panel_log_n = 0; memset(M.panel_log, 0, sizeof(M.panel_log)); M.sleeps_ms = 0;
	wd_set_outputs(&hw, true, true);
	CHECK(!strcmp(M.panel_log, "74P"), "on order %s, want PR57, PR4, PR19", M.panel_log);
	CHECK(M.sleeps_ms >= 200, "on: only %u ms between steps", M.sleeps_ms);
	CHECK((M.fp[0x32] & 0x30) == 0x30 && !(M.gfx[0x0e] & 0x20) && (M.seq[0x19] & 0x04), "on: not back on");

	wd_set_outputs(&hw, false, true);
	CHECK((M.fp[0x32] & 0x30) == 0x20, "CRT only: PR19 %02x", M.fp[0x32]);
	wd_set_outputs(&hw, true, false);
	CHECK((M.fp[0x32] & 0x30) == 0x10, "LCD only: PR19 %02x", M.fp[0x32]);

	/* blank to powerdown while live, then restore: everything as the BIOS had it */
	mock_reset(M_C24A);
	M.gfx[0x0e] = 0x00;
	wd_probe(&hw);
	mock_snap(&before);
	text.misc = M.misc;
	for (i = 0; i < 5; i++) text.seq[i] = M.seq[i];
	for (i = 0; i < 25; i++) text.crtc[i] = M.crtc[i];
	for (i = 0; i < 9; i++) text.gfx[i] = M.gfx[i];
	for (i = 0; i < 21; i++) text.attr[i] = M.attr[i];
	wd_save_ext(&hw, &bios);
	wd_mode_640x480x8(&hw, &bios, &std, &ext);
	wd_set_mode(&hw, &std, &ext);
	wd_set_outputs(&hw, false, true);	/* output=crt */
	wd_panel_off(&hw);
	M.sleeps_ms = 0;
	wd_restore_ext(&hw, &bios);
	CHECK(M.gfx[0x0e] & 0x20, "restore_ext drove the panel pins before panel power");
	wd_write_std(&hw, &text);
	wd_screen_on(&hw, true);
	wd_relock(&hw, &bios);
	wd_restore_panel(&hw, &bios);
	mock_snap(&after);
	CHECK(!mock_snap_diff(&before, &after, why, sizeof(why)), "panel off + restore left %s", why);
	CHECK(M.sleeps_ms >= 200, "restore powered the panel up without the 100 ms steps");
}

static void test_cursor(void)
{
	struct wd_ext_state bios, ext;
	struct wd_std std;
	struct wd_hw hw;
	u8 img[128] = {0}, msk[128] = {0}, pat[WD_CUR_BYTES];

	mock_reset(M_C24A);
	wd_probe(&hw);
	wd_save_ext(&hw, &bios);
	wd_mode_640x480x8(&hw, &bios, &std, &ext);
	wd_set_mode(&hw, &std, &ext);
	/* row 0: col 0 fg, col 1 bg, col 2+ transparent; row 31 col 31 fg */
	msk[0] = 0xc0; img[0] = 0x80;
	msk[127] = 0x01; img[127] = 0x01;
	wd_cursor_pattern(img, msk, pat);
	CHECK(pat[0] == 0x3f && pat[1] == 0x80, "pat[0] == 0x3f && pat[1] == 0x80");	/* AND, XOR cols 0-7 */
	CHECK(pat[2] == 0xff && pat[3] == 0x00, "pat[2] == 0xff && pat[3] == 0x00");	/* cols 8-15 transparent */
	CHECK(pat[254] == 0xfe && pat[255] == 0x01, "pat[254] == 0xfe && pat[255] == 0x01");

	wd_cursor_load(&hw, 0x50000, 15, 0, 3, 5, true);
	CHECK(M.curs[1] == 0x000 && M.curs[2] == 0x014, "M.curs[1] == 0x000 && M.curs[2] == 0x014");	/* 0x50000 >> 2 */
	CHECK(M.curs[3] == 15 && M.curs[4] == 0, "M.curs[3] == 15 && M.curs[4] == 0");
	CHECK(M.curs[5] == ((5 << 6) | 3), "M.curs[5] == ((5 << 6) | 3)");
	CHECK(M.cursor == 0x0820, "M.cursor == 0x0820");	/* enable, two colours + invert */
	wd_cursor_move(&hw, 639, 479);
	CHECK(M.curs[6] == 639 && M.curs[7] == 479, "M.curs[6] == 639 && M.curs[7] == 479");
	CHECK(wd_cursor_read(&hw, 6) == 639, "wd_cursor_read(&hw, 6) == 639");
	wd_cursor_enable(&hw, false);
	CHECK(M.cursor == 0x0020, "M.cursor == 0x0020");
	wd_vram_write(&hw, 0x50000 - 2, pat, 4);	/* across a bank */
	CHECK(M.vram[0x50000 - 2] == pat[0] && M.vram[0x50001] == pat[3], "M.vram[0x50000 - 2] == pat[0] && M.vram[");
}

static void t_cexp(void)
{
	struct wd_ext_state bios, ext;
	struct wd_std std;
	struct wd_hw hw;
	char why[96];
	int v, want, pass;

	CHECK(wd_cexp_words(37, 0) == 10 && wd_cexp_words(37, 1) == 10 &&
	      wd_cexp_words(13, 1) == 4 && wd_cexp_words(16, 8) == 1 &&
	      wd_cexp_words(17, 9) == 2, "word counts");
	for (want = 0; want < WD_CEXP_VARIANTS; want++) {
		mock_reset(M_C24A);
		wd_probe(&hw);
		wd_save_ext(&hw, &bios);
		wd_mode_640x480x8(&hw, &bios, &std, &ext);
		wd_set_mode(&hw, &std, &ext);
		M.cexp_var = want;
		pass = -1;
		for (v = WD_CEXP_VARIANTS - 1; v >= 0; v--) {
			int bad = wd_cexp_probe(&hw, v, why, sizeof(why));

			CHECK(bad >= 0, "variant %d hung on chip variant %d", v, want);
			if (!bad && pass < 0)
				pass = v;
		}
		/* a doubleword-padded chip also passes the word-padded probe
		 * when rows need an even number of words; never the reverse */
		CHECK(pass == want || pass == (want | 1), "chip %d: probe picked %d", want, pass);
		CHECK(!wd_cexp_probe(&hw, want, why, sizeof(why)), "chip %d own variant: %s", want, why);
		CHECK(M.hm_stray == 0, "chip %d: %u port writes with no expansion running", want, M.hm_stray);
	}
}

int main(void)
{
	t_probe_kinds();
	t_memory();
	t_save_restore();
	t_mode_set();
	t_banks();
	t_flush();
	t_roundtrip();
	t_blt();
	t_blt_selftest();
	t_panel();
	t_suspend_cycle();
	test_cursor();
	t_cexp();
	printf("wd90c24 unit tests: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
