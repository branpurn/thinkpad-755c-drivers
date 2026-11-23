/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Draw the row-band picture into the mode 13h window and leave it up
 * for 10 seconds. stdout is a log, not the VGA console: the run script
 * redirects it. KD_GRAPHICS stops vgacon from writing CR0C/CR0D while
 * the picture is up. If the VT was already in graphics, it is left there.
 *
 *   /tmp/wd24x-scanpat >/tmp/wd24x-scanpat.out
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/fb.h>
#include <linux/kd.h>

#include "wd24x-scanpat.h"

#define WIN 65536u

static int kd_fd = -1;
static int kd_restore;

static void kd_release(void)
{
	if (kd_fd < 0)
		return;
	if (kd_restore)
		ioctl(kd_fd, KDSETMODE, KD_TEXT);
	close(kd_fd);
	kd_fd = -1;
	kd_restore = 0;
}

static int kd_hold(void)
{
	const char *dev = getenv("WD24X_TTY_DEV");
	int mode = KD_TEXT;

	if (!dev || !dev[0])
		dev = "/dev/tty0";
	kd_fd = open(dev, O_RDWR);
	if (kd_fd < 0)
		return -1;
	if (ioctl(kd_fd, KDGETMODE, &mode) != 0 ||
	    (mode != KD_GRAPHICS && ioctl(kd_fd, KDSETMODE, KD_GRAPHICS) != 0)) {
		close(kd_fd);
		kd_fd = -1;
		return -1;
	}
	kd_restore = mode != KD_GRAPHICS;
	return 0;
}

int main(int argc, char **argv)
{
	const char *fbdev = argc > 1 ? argv[1] : "/dev/fb0";
	struct fb_var_screeninfo var;
	uint8_t *win;
	int fd, y;

	if (kd_hold() != 0) {
		fprintf(stderr, "WD24X-SCANPAT kd refused\n");
		return 1;
	}
	fd = open(fbdev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "WD24X-SCANPAT fb refused\n");
		kd_release();
		return 1;
	}
	memset(&var, 0, sizeof(var));
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 ||
	    var.xres != WD24_SCAN_W || var.yres != WD24_SCAN_H ||
	    var.bits_per_pixel != 8) {
		fprintf(stderr, "WD24X-SCANPAT geometry refused\n");
		close(fd);
		kd_release();
		return 1;
	}
	win = mmap(NULL, WIN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (win == MAP_FAILED) {
		fprintf(stderr, "WD24X-SCANPAT mmap refused\n");
		close(fd);
		kd_release();
		return 1;
	}
	{
		struct fb_cmap cmap;
		unsigned short red[256], green[256], blue[256];
		unsigned i;

		for (i = 0; i < 256; i++)
			red[i] = green[i] = blue[i] = 0;
		/* Top 6 bits, the same shift the colormap path uses. */
		red[WD24_SCAN_RED] = 63u << 10;
		green[WD24_SCAN_GRN] = 63u << 10;
		blue[WD24_SCAN_BLU] = 63u << 10;
		red[WD24_SCAN_YEL] = green[WD24_SCAN_YEL] = 63u << 10;
		red[15] = green[15] = blue[15] = 63u << 10;
		memset(&cmap, 0, sizeof(cmap));
		cmap.start = 0;
		cmap.len = 256;
		cmap.red = red;
		cmap.green = green;
		cmap.blue = blue;
		if (ioctl(fd, FBIOPUTCMAP, &cmap) != 0)
			fprintf(stderr, "WD24X-SCANPAT palette refused\n");
	}
	wd24_scanpat_fill(win);
	memset(win + WD24_SCAN_FRAME, 0, WIN - WD24_SCAN_FRAME);
	printf("WD24X-SCANPAT red rows 0-15 cols 40-79\n");
	printf("WD24X-SCANPAT green rows 0-15 cols 240-279\n");
	printf("WD24X-SCANPAT blue rows 184-199 cols 40-79\n");
	printf("WD24X-SCANPAT yellow rows 184-199 cols 240-279\n");
	printf("WD24X-SCANPAT white every 10 rows, bits in cols 0-31\n");
	for (y = 0; y < 20; y++)
		printf("WD24X-SCANPAT row %d colour %u\n", y * 10,
		       (unsigned)wd24_scanpat_at(8, (unsigned)y * 10));
	fflush(stdout);
	sleep(10);
	munmap(win, WIN);
	close(fd);
	kd_release();
	return 0;
}
