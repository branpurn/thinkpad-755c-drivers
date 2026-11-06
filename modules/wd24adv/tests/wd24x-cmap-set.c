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
#include "wd24x-cmap-os.h"

static void prefill(uint16_t *r, uint16_t *g, uint16_t *b)
{
	int i;

	for (i = 0; i < WD24X_CMAP_N; i++)
		r[i] = g[i] = b[i] = 0x0001;
}

static int get_all(const struct wd24x_os *os, int fd,
		   uint16_t *r, uint16_t *g, uint16_t *b)
{
	struct fb_cmap cmap;

	prefill(r, g, b);
	memset(&cmap, 0, sizeof cmap);
	cmap.start = 0;
	cmap.len = WD24X_CMAP_N;
	cmap.red = r;
	cmap.green = g;
	cmap.blue = b;
	return os->ioctl(fd, FBIOGETCMAP, &cmap);
}

int wd24x_cmap_set_run(const struct wd24x_os *os)
{
	uint16_t r0[WD24X_CMAP_N], g0[WD24X_CMAP_N], b0[WD24X_CMAP_N];
	uint16_t r1[WD24X_CMAP_N], g1[WD24X_CMAP_N], b1[WD24X_CMAP_N];
	uint16_t one_r = WD24X_CMAP_SET_VALUE;
	uint16_t one_g = WD24X_CMAP_SET_VALUE;
	uint16_t one_b = WD24X_CMAP_SET_VALUE;
	struct fb_cmap put;
	int fd;

	if (!os || !os->open || !os->ioctl || !os->close || !os->sleep)
		return 1;
	fd = os->open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		printf("WD24X-CMAP-SET bad open\n");
		return 1;
	}
	if (get_all(os, fd, r0, g0, b0) != 0 ||
	    !wd24x_cmap_all_shift10(r0, g0, b0)) {
		printf("WD24X-CMAP-SET bad get\n");
		os->close(fd);
		return 1;
	}
	memset(&put, 0, sizeof put);
	put.start = WD24X_CMAP_SET_INDEX;
	put.len = 1;
	put.red = &one_r;
	put.green = &one_g;
	put.blue = &one_b;
	if (os->ioctl(fd, FBIOPUTCMAP, &put) != 0) {
		printf("WD24X-CMAP-SET bad put\n");
		os->close(fd);
		return 1;
	}
	if (get_all(os, fd, r1, g1, b1) != 0 ||
	    !wd24x_cmap_set_cache_ok(r0, g0, b0, r1, g1, b1)) {
		printf("WD24X-CMAP-SET bad cache\n");
		os->close(fd);
		return 1;
	}
	printf("WD24X-CMAP-SET index 7 put 1 cache 1\n");
	fflush(stdout);
	os->sleep(8);
	os->close(fd);
	return 0;
}

#ifndef WD24X_CMAP_NO_MAIN
static void on_alarm(int sig)
{
	(void)sig;
	fprintf(stderr, "wd24x-cmap-set: alarm\n");
	_exit(2);
}

static int os_open(const char *path, int flags)
{
	return open(path, flags);
}

static int os_close(int fd)
{
	return close(fd);
}

static int os_ioctl(int fd, unsigned long req, void *arg)
{
	return ioctl(fd, req, arg);
}

static unsigned os_sleep(unsigned sec)
{
	return sleep(sec);
}

int main(void)
{
	static const struct wd24x_os os = {
		os_open, os_close, os_ioctl, os_sleep
	};

	signal(SIGALRM, on_alarm);
	alarm(15);
	if (wd24x_cmap_set_run(&os) != 0)
		return 1;
	alarm(0);
	return 0;
}
#endif
