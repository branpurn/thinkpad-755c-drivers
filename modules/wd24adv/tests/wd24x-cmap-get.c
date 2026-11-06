/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Phase 2. Opens /dev/fb0 and FBIOGETCMAP only.
 * No FBIOPUTCMAP, no FBIOPUT_VSCREENINFO, no mmap, no X.
 * Open with s6d_cmap=1 publishes: 3C6=FF, 3C7=00, 768 reads of 3C9.
 * That read does not write colour entries. GET returns the cache.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include "wd24x-cmap-check.h"

static void on_alarm(int sig)
{
	(void)sig;
	fprintf(stderr, "wd24x-cmap-get: alarm\n");
	_exit(2);
}

int main(void)
{
	struct fb_cmap cmap;
	uint16_t r[WD24X_CMAP_N], g[WD24X_CMAP_N], b[WD24X_CMAP_N];
	int fd, rc;

	signal(SIGALRM, on_alarm);
	alarm(15);
	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		printf("WD24X-CMAP-GET bad open\n");
		return 1;
	}
	memset(&cmap, 0, sizeof cmap);
	cmap.start = 0;
	cmap.len = WD24X_CMAP_N;
	cmap.red = r;
	cmap.green = g;
	cmap.blue = b;
	rc = ioctl(fd, FBIOGETCMAP, &cmap);
	if (rc != 0 || !wd24x_cmap_all_shift10(r, g, b)) {
		printf("WD24X-CMAP-GET bad get\n");
		close(fd);
		return 1;
	}
	printf("WD24X-CMAP-GET entries 256 shift10 1\n");
	fflush(stdout);
	sleep(8);
	close(fd);
	alarm(0);
	return 0;
}
