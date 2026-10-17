// SPDX-License-Identifier: GPL-2.0
/*
 * wd90c24fb: native framebuffer for the WD90C24/WD90C24A(2) in the IBM
 * ThinkPad 755C. See docs/WD90C24.md for the staging plan.
 *
 * STAGE 1 (this file): detection only. The probe follows XFree86's
 * PVGA1Probe (Paradise lock test, then CR2B/SR07/SR10/SR14 and the
 * CR36/CR37 signature with PR1B locked), reads the memory size from PR1
 * and the panel state from PR18/PR19, and leaves every register as found.
 * It never touches timing, clock or memory-mapping registers, so it is
 * safe on any VGA. The module always returns -ENODEV after logging, so
 * nothing stays loaded.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/io.h>
#include "wd90c24_hw.h"

static bool probe_only = true;
module_param(probe_only, bool, 0444);
MODULE_PARM_DESC(probe_only, "Only identify the chip and log its state (stage 1: always on)");

static bool bios_sig(unsigned long base)
{
	void __iomem *p = ioremap(base + 0x7d, 4);
	bool ok = false;

	if (p) {
		ok = readb(p) == 'V' && readb(p + 1) == 'G' &&
		     readb(p + 2) == 'A' && readb(p + 3) == '=';
		iounmap(p);
	}
	return ok;
}

static int __init wd90c24fb_init(void)
{
	struct wd_hw hw = { 0 };
	enum wd_chip chip;

	/* vgacon owns 0x3c0-0x3df ("vga+"); like vga16fb we share it. */
	chip = wd_probe(&hw);

	pr_info("Paradise BIOS signature: C000:007D %s, E000:007D %s\n",
		bios_sig(0xc0000) ? "yes" : "no", bios_sig(0xe0000) ? "yes" : "no");
	if (chip == WD_NONE) {
		pr_info("no Paradise/WD chip found\n");
		return -ENODEV;
	}
	pr_info("found %s (CR36/37 \"%c%c\"), %u KiB video memory%s\n",
		wd_chip_name(chip), hw.sig[0] ? hw.sig[0] : '-',
		hw.sig[1] ? hw.sig[1] : '-', hw.vram_kb,
		hw.use_1meg ? " (C24 dual-scan rule)" : "");
	if (chip == WD_C24 || chip == WD_C2X)
		pr_info("panel: PR18=0x%02x PR19=0x%02x: LCD %s, CRT %s%s\n",
			hw.pr18, hw.pr19, hw.pr19 & PR19_LCD ? "on" : "off",
			hw.pr19 & PR19_CRT ? "on" : "off",
			hw.pr18 & 0x0b ? "" : ", dual scan");
	if (chip == WD_C24)
		pr_info("extended register blocks (BITBLT, cursor) %s: %s\n",
			hw.ext_blocks ? "present" : "absent",
			hw.ext_blocks ? "WD90C24A/A2" : "plain WD90C24");
	return -ENODEV;
}
module_init(wd90c24fb_init);

MODULE_DESCRIPTION("WD90C24/WD90C24A(2) framebuffer for the ThinkPad 755C (stage 1: detect)");
MODULE_LICENSE("GPL");
