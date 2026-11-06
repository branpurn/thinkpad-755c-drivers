/* SPDX-License-Identifier: GPL-2.0 */
/* i486 layout of the cmap ioctl argument. Run under qemu -cpu 486. */
#include <stddef.h>
#include <stdio.h>
#include <linux/fb.h>

int main(void)
{
	printf("WD24X-CMAP-ABI size %u red %u transp %u get 0x%lx put 0x%lx vs 0x%lx\n",
	       (unsigned)sizeof(struct fb_cmap),
	       (unsigned)offsetof(struct fb_cmap, red),
	       (unsigned)offsetof(struct fb_cmap, transp),
	       (unsigned long)FBIOGETCMAP,
	       (unsigned long)FBIOPUTCMAP,
	       (unsigned long)FBIOPUT_VSCREENINFO);
	return 0;
}
