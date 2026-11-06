/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Memory-buffer backend for doomgeneric.
 * Render size is DOOMGENERIC_RESX x DOOMGENERIC_RESY, set to 320x200
 * by the build. That is not the 640x480 scanout of the S6d driver.
 * /dev/fb0 is opened only when WD24X_DOOM_FB=mmap.
 * WD24X_DOOM_PALETTE=1 refuses before any framebuffer call. This file
 * does not call FBIOPUTCMAP or KDSETMODE.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <linux/fb.h>

#include "doomgeneric.h"
#include "wd24x-doom-pick.h"

static unsigned frames;
static unsigned sum;
static int reported;
static int frame_cap;
static uint8_t *fb;
static unsigned fb_w, fb_h, fb_bpp;
static size_t fb_len;

static int cap_of(void)
{
	const char *e = getenv("WD24X_DOOM_FRAMES");
	long n;

	if (!e || !*e)
		return 0;
	n = strtol(e, NULL, 10);
	if (n < 0 || n > 100000)
		return 0;
	return (int)n;
}

static void report(void)
{
	if (reported)
		return;
	reported = 1;
	printf("WD24X-DOOM frames %u sum 0x%08x\n", frames, sum);
	fflush(stdout);
}

static void checksum(void)
{
	const uint32_t *p = (const uint32_t *)DG_ScreenBuffer;
	unsigned n = DOOMGENERIC_RESX * DOOMGENERIC_RESY;
	unsigned i, s = 2166136261u;

	for (i = 0; i < n; i++) {
		s ^= p[i];
		s *= 16777619u;
	}
	sum = s;
}

static void blit_luma(void)
{
	const uint32_t *p = (const uint32_t *)DG_ScreenBuffer;
	unsigned x, y, w, h;

	if (!fb || fb_bpp != 8)
		return;
	w = DOOMGENERIC_RESX;
	h = DOOMGENERIC_RESY;
	if (w > fb_w)
		w = fb_w;
	if (h > fb_h)
		h = fb_h;
	for (y = 0; y < h; y++) {
		uint8_t *row = fb + (size_t)y * fb_w;
		for (x = 0; x < w; x++)
			row[x] = (uint8_t)wd24x_doom_luma(p[y * DOOMGENERIC_RESX + x]);
	}
}

void DG_Init(void)
{
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	int fd;

	frame_cap = cap_of();
	atexit(report);
	printf("WD24X-DOOM render %d %d bpp 32\n", DOOMGENERIC_RESX, DOOMGENERIC_RESY);
	printf("WD24X-DOOM scanout separate\n");
	if (wd24x_doom_palette_refused(getenv("WD24X_DOOM_PALETTE"))) {
		printf("WD24X-DOOM palette refused\n");
		exit(2);
	}
	if (!wd24x_doom_fb_opt_in(getenv("WD24X_DOOM_FB"))) {
		printf("WD24X-DOOM display file\n");
		return;
	}
	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0 ||
	    ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 ||
	    fix.smem_len == 0) {
		printf("WD24X-DOOM fb refused\n");
		if (fd >= 0)
			close(fd);
		exit(1);
	}
	fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		printf("WD24X-DOOM fb refused\n");
		close(fd);
		exit(1);
	}
	fb_w = var.xres;
	fb_h = var.yres;
	fb_bpp = var.bits_per_pixel;
	fb_len = fix.smem_len;
	printf("WD24X-DOOM fb luminance not playpal %u %u bpp %u\n",
	       fb_w, fb_h, fb_bpp);
	(void)fb_len;
}

void DG_DrawFrame(void)
{
	if (!DG_ScreenBuffer)
		return;
	frames++;
	checksum();
	blit_luma();
	if (frame_cap && (int)frames >= frame_cap) {
		report();
		exit(0);
	}
}

void DG_SleepMs(uint32_t ms)
{
	(void)ms;
}

uint32_t DG_GetTicksMs(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint32_t)(tv.tv_sec * 1000u + tv.tv_usec / 1000u);
}

int DG_GetKey(int *pressed, unsigned char *key)
{
	(void)pressed;
	(void)key;
	return 0;
}

void DG_SetWindowTitle(const char *title)
{
	(void)title;
}

int main(int argc, char **argv)
{
	doomgeneric_Create(argc, argv);
	for (;;)
		doomgeneric_Tick();
	return 0;
}
