/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Phase 2. Opens /dev/fb0 and FBIOGETCMAP only.
 * No FBIOPUTCMAP, no FBIOPUT_VSCREENINFO, no mmap, no X.
 * Open with s6d_cmap=1 publishes: 3C6=FF, 3C7=00, 768 reads of 3C9.
 * That read does not write colour entries. GET returns the cache.
 * Close does not restore the DAC. Trial expiry does.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include "wd24x-cmap-check.h"
#include "wd24x-cmap-os.h"

#if defined(__i386__)
_Static_assert(sizeof(struct fb_cmap) == 24, "i386 fb_cmap");
_Static_assert(offsetof(struct fb_cmap, red) == 8, "i386 red");
_Static_assert(offsetof(struct fb_cmap, transp) == 20, "i386 transp");
#endif

static void prefill(uint16_t *r, uint16_t *g, uint16_t *b)
{
	int i;

	for (i = 0; i < WD24X_CMAP_N; i++)
		r[i] = g[i] = b[i] = 0x0001;
}

int wd24x_cmap_get_run(const struct wd24x_os *os)
{
	struct fb_cmap cmap;
	uint16_t r[WD24X_CMAP_N], g[WD24X_CMAP_N], b[WD24X_CMAP_N];
	int fd, rc;

	if (!os || !os->open || !os->ioctl || !os->close || !os->sleep)
		return 1;
	fd = os->open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		printf("WD24X-CMAP-GET bad open\n");
		return 1;
	}
	prefill(r, g, b);
	memset(&cmap, 0, sizeof cmap);
	cmap.start = 0;
	cmap.len = WD24X_CMAP_N;
	cmap.red = r;
	cmap.green = g;
	cmap.blue = b;
	rc = os->ioctl(fd, FBIOGETCMAP, &cmap);
	if (rc != 0 || !wd24x_cmap_all_shift10(r, g, b)) {
		printf("WD24X-CMAP-GET bad get\n");
		os->close(fd);
		return 1;
	}
	printf("WD24X-CMAP-GET entries 256 shift10 1\n");
	fflush(stdout);
	os->sleep(8);
	os->close(fd);
	return 0;
}

#ifndef WD24X_CMAP_NO_MAIN
static void on_alarm(int sig)
{
	(void)sig;
	fprintf(stderr, "wd24x-cmap-get: alarm\n");
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
	if (wd24x_cmap_get_run(&os) != 0)
		return 1;
	alarm(0);
	return 0;
}
#endif
