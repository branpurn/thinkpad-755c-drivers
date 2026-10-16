// SPDX-License-Identifier: GPL-2.0
/*
 * wd90c24: Western Digital (Paradise) WD90C24/WD90C24A/WD90C24A2 support
 * for the IBM ThinkPad 755C.  STAGE 0: detect-only.
 *
 * This module does not drive the display yet. It identifies the chip and
 * logs the state the native framebuffer driver will need (docs/ROADMAP.md):
 *   - Paradise extended-register lock PR5 (GR 0x0F, unlock value 0x05)
 *   - PR10 (CR 0x29, unlock value 0x85) and PR20 (SR 0x06, unlock 0x48)
 *   - CR 0x31..0x37: chip ID "WD90Cxx" on desktop parts; on the flat-panel
 *     WD90C2x parts this range holds panel registers (PR18 = CR 0x31 is the
 *     read-only flat panel status), so it is dumped raw
 *   - the Paradise "VGA=" BIOS signature at offset 0x7D of the video BIOS,
 *     which the 755C maps at 0xE0000 instead of 0xC0000
 * Every lock register it touches is restored before returning.
 * Register meanings: XFree86 3.3.6 "wd" driver docs, VGADOC, and 86Box's
 * vid_paradise.c (WD90C11/C20/C30/C31 emulation) -- see docs/HARDWARE.md.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <video/vga.h>

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "Stay loaded even if no Paradise/WD chip is found");

static u8 rd(u16 idx_port, u8 idx)
{
	outb(idx, idx_port);
	return inb(idx_port + 1);
}

static void wr(u16 idx_port, u8 idx, u8 val)
{
	outb(idx, idx_port);
	outb(val, idx_port + 1);
}

static bool bios_sig(unsigned long base)
{
	void __iomem *p = ioremap(base + 0x7d, 4);
	bool ok = false;

	if (p) {
		ok = readb(p) == 'V' && readb(p + 1) == 'G' && readb(p + 2) == 'A' && readb(p + 3) == '=';
		iounmap(p);
	}
	return ok;
}

static int __init wd90c24_init(void)
{
	u16 crtc = (inb(VGA_MIS_R) & 1) ? 0x3d4 : 0x3b4;
	u8 gr0f, cr29, sr06, lk, ul, id[7];
	char asc[8];
	bool paradise;
	int i;

	gr0f = rd(VGA_GFX_I, 0x0f);
	wr(VGA_GFX_I, 0x0f, 0x00);              /* lock PR0-PR4 */
	lk = rd(VGA_GFX_I, 0x0f) & 0x17;
	wr(VGA_GFX_I, 0x0f, 0x05);              /* unlock */
	ul = rd(VGA_GFX_I, 0x0f) & 0x17;
	paradise = (ul == 0x05) && (lk != 0x05);

	if (paradise) {
		cr29 = rd(crtc, 0x29);
		sr06 = rd(VGA_SEQ_I, 0x06);
		wr(crtc, 0x29, 0x85);
		wr(VGA_SEQ_I, 0x06, 0x48);
		for (i = 0; i < 7; i++) {
			id[i] = rd(crtc, 0x31 + i);
			asc[i] = (id[i] >= 0x20 && id[i] < 0x7f) ? id[i] : '.';
		}
		asc[7] = 0;
		pr_info("wd90c24: Paradise/WD extended registers present (PR5 lock 0x%02x/unlock 0x%02x)\n", lk, ul);
		pr_info("wd90c24: CR31..37 = %7ph \"%s\"%s\n", id, asc,
			memcmp(asc, "WD90C", 5) ? " (no WD90Cxx ID: flat-panel WD90C2x layout?)" : "");
		pr_info("wd90c24: PR1 (GR0B)=0x%02x PR2 (GR0C)=0x%02x PR4 (GR0E)=0x%02x CR29 was 0x%02x SR06 was 0x%02x\n",
			rd(VGA_GFX_I, 0x0b), rd(VGA_GFX_I, 0x0c), rd(VGA_GFX_I, 0x0e), cr29, sr06);
		wr(VGA_SEQ_I, 0x06, sr06);
		wr(crtc, 0x29, cr29);
	} else {
		pr_info("wd90c24: no Paradise/WD chip (GR0F lock 0x%02x unlock 0x%02x)\n", lk, ul);
	}
	wr(VGA_GFX_I, 0x0f, gr0f);

	pr_info("wd90c24: Paradise BIOS signature: C000:007D %s, E000:007D %s\n",
		bios_sig(0xc0000) ? "yes" : "no", bios_sig(0xe0000) ? "yes" : "no");

	if (!paradise && !force)
		return -ENODEV;
	return 0;
}

static void __exit wd90c24_exit(void)
{
}

module_init(wd90c24_init);
module_exit(wd90c24_exit);
MODULE_DESCRIPTION("WD90C24/WD90C24A(2) ThinkPad 755C video: detect-only stage 0");
MODULE_LICENSE("GPL");
