// SPDX-License-Identifier: GPL-2.0
/*
 * tc32-vgafb-dev: create the "vga-framebuffer" platform device that
 * vga16fb binds to, without CONFIG_SYSFB in the core kernel.
 *
 * Since Linux 6.x, vga16fb is a platform driver; the device is normally
 * created at boot by drivers/firmware/sysfb.c (CONFIG_SYSFB, a bool that
 * would have to be built into the floppy kernel). This module does the same
 * thing from a .tcz: it copies the boot screen_info and registers the device
 * only when the firmware left the display in a VGA/EGA colour mode.
 *
 * In 6.18 vga16fb's check_mode_supported() also insists that the BIOS mode
 * at boot was already a 16-colour graphics mode (0x0D/0x0E/0x10/0x12),
 * which never happens on a text-mode boot (the x86 vga= option only selects
 * text modes, and the 755C video BIOS has no VBE). vga16fb programs every
 * CRTC/sequencer register itself in set_par, so the boot mode only matters
 * for that check: we hand it mode 0x12 (640x480x16) unless mode= says
 * otherwise. mode=0 passes the real boot mode through unchanged.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/screen_info.h>

static struct platform_device *pdev;

static int mode = 0x12;
module_param(mode, int, 0444);
MODULE_PARM_DESC(mode, "BIOS mode to report to vga16fb (0x12=640x480, 0x10=640x350; 0=keep boot mode)");

static int __init tc32_vgafb_dev_init(void)
{
	struct screen_info si = screen_info;
	unsigned int type = screen_info_video_type(&si);
	const char *name;

	switch (type) {
	case VIDEO_TYPE_VGAC:
		name = "vga-framebuffer";
		break;
	case VIDEO_TYPE_EGAC:
		name = "ega-framebuffer";
		break;
	default:
		pr_info("tc32-vgafb-dev: boot video type %u is not VGA/EGA colour, nothing to do\n", type);
		return -ENODEV;
	}
	if (mode) {
		pr_info("tc32-vgafb-dev: boot mode 0x%02x, presenting mode 0x%02x to vga16fb\n",
			si.orig_video_mode, mode);
		si.orig_video_mode = mode;
	}
	pdev = platform_device_register_data(NULL, name, 0, &si, sizeof(si));
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	pr_info("tc32-vgafb-dev: registered %s (%ux%u text)\n", name,
		si.orig_video_cols, si.orig_video_lines);
	return 0;
}

static void __exit tc32_vgafb_dev_exit(void)
{
	platform_device_unregister(pdev);
}

module_init(tc32_vgafb_dev_init);
module_exit(tc32_vgafb_dev_exit);
MODULE_DESCRIPTION("Register the vga-framebuffer platform device for vga16fb (no SYSFB)");
MODULE_LICENSE("GPL");
