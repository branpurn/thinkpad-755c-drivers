/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Phase 3. One FBIOPUTCMAP of index 7, then FBIOGETCMAP.
 * GET after a successful PUT is the kernel cache of the 16-bit request,
 * not a new DAC read. Rollback of colour RAM is the module's trial
 * expiry, not this process.
 * No FBIOPUT_VSCREENINFO, no mmap, no X.
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
	fprintf(stderr, "wd24x-cmap-set: alarm\n");
	_exit(2);
}

static int get_all(int fd, uint16_t *r, uint16_t *g, uint16_t *b)
{
	struct fb_cmap cmap;

	memset(&cmap, 0, sizeof cmap);
	cmap.start = 0;
	cmap.len = WD24X_CMAP_N;
	cmap.red = r;
	cmap.green = g;
	cmap.blue = b;
	return ioctl(fd, FBIOGETCMAP, &cmap);
}

int main(void)
{
	uint16_t r0[WD24X_CMAP_N], g0[WD24X_CMAP_N], b0[WD24X_CMAP_N];
	uint16_t r1[WD24X_CMAP_N], g1[WD24X_CMAP_N], b1[WD24X_CMAP_N];
	uint16_t one_r = WD24X_CMAP_SET_VALUE;
	uint16_t one_g = WD24X_CMAP_SET_VALUE;
	uint16_t one_b = WD24X_CMAP_SET_VALUE;
	struct fb_cmap put;
	int fd;

	signal(SIGALRM, on_alarm);
	alarm(15);
	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		printf("WD24X-CMAP-SET bad open\n");
		return 1;
	}
	if (get_all(fd, r0, g0, b0) != 0) {
		printf("WD24X-CMAP-SET bad get\n");
		close(fd);
		return 1;
	}
	memset(&put, 0, sizeof put);
	put.start = WD24X_CMAP_SET_INDEX;
	put.len = 1;
	put.red = &one_r;
	put.green = &one_g;
	put.blue = &one_b;
	if (ioctl(fd, FBIOPUTCMAP, &put) != 0) {
		printf("WD24X-CMAP-SET bad put\n");
		close(fd);
		return 1;
	}
	if (get_all(fd, r1, g1, b1) != 0 ||
	    !wd24x_cmap_set_cache_ok(r0, g0, b0, r1, g1, b1)) {
		printf("WD24X-CMAP-SET bad cache\n");
		close(fd);
		return 1;
	}
	printf("WD24X-CMAP-SET index 7 put 1 cache 1\n");
	fflush(stdout);
	sleep(8);
	close(fd);
	alarm(0);
	return 0;
}
