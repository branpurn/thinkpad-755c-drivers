/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Indexed Doom backend. Render stays 320x200 palette indices.
 * Every frame is also composed into a private 640x480 buffer.
 * /dev/fb0 is opened only when WD24X_DOOM_FB=mmap, and only when the
 * fixed screen is 640x480x8 with line length 640 and room for 0x4b000
 * bytes. WD24X_DOOM_PALETTE=put then sends PLAYPAL through FBIOPUTCMAP.
 * Any other palette value does not. WD24X_DOOM_INPUT=tty owns the tty
 * termios and puts the saved struct back. KD mode is never changed.
 * The default path opens neither device.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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
#include "doomkeys.h"
#include "i_video.h"
#include "wd24x-doom-keys.h"
#include "wd24x-doom-pick.h"
#include "wd24x-doom-present.h"
#include "wd24x-tty-own.h"

static unsigned frames;
static unsigned index_sum;
static unsigned present_sum;
static int reported;
static int frame_cap;
static int present_mode;
static uint8_t present[WD24X_DOOM_FRAME];
static uint8_t *fb;
static int fb_fd = -1;
static struct wd24x_tty_own tty_own;
static struct wd24x_keys keys;
static int have_up;
static unsigned char pending_up;
static volatile sig_atomic_t got_sig;

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

static unsigned fnv(const uint8_t *p, unsigned n)
{
	unsigned s = 2166136261u;
	unsigned i;

	for (i = 0; i < n; i++) {
		s ^= p[i];
		s *= 16777619u;
	}
	return s;
}

static void on_sig(int sig)
{
	got_sig = sig;
}

static void restore_and_signal(void)
{
	int sig = got_sig;

	if (!sig)
		return;
	wd24x_tty_own_end(&tty_own);
	signal(sig, SIG_DFL);
	raise(sig);
}

static void report(void)
{
	if (reported)
		return;
	reported = 1;
	wd24x_tty_own_end(&tty_own);
	printf("WD24X-DOOM frames %u index 0x%08x present 0x%08x\n",
	       frames, index_sum, present_sum);
	printf("WD24X-DOOM playpal0 %u %u %u\n",
	       (unsigned)colors[0].r, (unsigned)colors[0].g,
	       (unsigned)colors[0].b);
	fflush(stdout);
}

static void arm_signals(void)
{
	struct sigaction sa;

	sa.sa_handler = on_sig;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
}

static int open_fb(void)
{
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;

	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) != 0 ||
	    ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) != 0 ||
	    !wd24x_doom_fb_geometry_ok(var.xres, var.yres, var.bits_per_pixel,
				       fix.line_length, fix.smem_len)) {
		printf("WD24X-DOOM fb refused\n");
		if (fb_fd >= 0)
			close(fb_fd);
		fb_fd = -1;
		return -1;
	}
	fb = mmap(NULL, WD24X_DOOM_FRAME, PROT_READ | PROT_WRITE, MAP_SHARED,
		  fb_fd, 0);
	if (fb == MAP_FAILED) {
		printf("WD24X-DOOM fb refused\n");
		close(fb_fd);
		fb_fd = -1;
		fb = NULL;
		return -1;
	}
	printf("WD24X-DOOM fb mmap 640 480 index\n");
	return 0;
}

static void put_playpal(void)
{
	uint16_t red[256], green[256], blue[256];
	struct fb_cmap cmap;
	unsigned i;

	if (!palette_changed || fb_fd < 0)
		return;
	for (i = 0; i < 256; i++) {
		red[i] = wd24x_playpal_chan(colors[i].r);
		green[i] = wd24x_playpal_chan(colors[i].g);
		blue[i] = wd24x_playpal_chan(colors[i].b);
	}
	memset(&cmap, 0, sizeof cmap);
	cmap.start = 0;
	cmap.len = 256;
	cmap.red = red;
	cmap.green = green;
	cmap.blue = blue;
	if (ioctl(fb_fd, FBIOPUTCMAP, &cmap) != 0) {
		printf("WD24X-DOOM palette put failed\n");
		exit(1);
	}
	palette_changed = false;
	printf("WD24X-DOOM palette put 256\n");
}

static int read_tty_key(unsigned char *doomkey)
{
	struct pollfd p;
	unsigned char b;
	int pr;

	if (!tty_own.owned)
		return 0;
	p.fd = tty_own.fd;
	p.events = POLLIN;
	pr = poll(&p, 1, 0);
	if (pr < 0 || pr == 0)
		return 0;
	if (read(tty_own.fd, &b, 1) != 1)
		return 0;
	return wd24x_keys_byte(&keys, b, doomkey);
}

void DG_Init(void)
{
	const char *present_s = getenv("WD24X_DOOM_PRESENT");
	const char *dev;

	frame_cap = cap_of();
	present_mode = wd24x_doom_present_mode(present_s);
	atexit(report);
	printf("WD24X-DOOM render 320 200 index\n");
	if (present_mode < 0) {
		printf("WD24X-DOOM present refused\n");
		exit(2);
	}
	printf("WD24X-DOOM present %s\n",
	       present_mode == WD24X_DOOM_FILL ? "fill" : "letterbox");
	if (wd24x_doom_palette_put(getenv("WD24X_DOOM_PALETTE")) &&
	    !wd24x_doom_fb_opt_in(getenv("WD24X_DOOM_FB"))) {
		printf("WD24X-DOOM palette put needs mmap\n");
		exit(2);
	}
	if (!wd24x_doom_fb_opt_in(getenv("WD24X_DOOM_FB")))
		printf("WD24X-DOOM display file\n");
	else if (open_fb() != 0)
		exit(1);
	if (!wd24x_doom_input_opt_in(getenv("WD24X_DOOM_INPUT"))) {
		printf("WD24X-DOOM input none\n");
		return;
	}
	dev = getenv("WD24X_TTY_DEV");
	if (!dev || !dev[0])
		dev = "/dev/tty0";
	tty_own.fd = open(dev, O_RDONLY);
	if (tty_own.fd < 0 || wd24x_tty_own_begin(&tty_own, tty_own.fd) != 0) {
		printf("WD24X-DOOM input refused\n");
		exit(1);
	}
	arm_signals();
	printf("WD24X-DOOM input tty\n");
}

void DG_DrawFrame(void)
{
	const uint8_t *idx;

	restore_and_signal();
	if (!DG_ScreenBuffer)
		return;
	idx = (const uint8_t *)DG_ScreenBuffer;
	frames++;
	index_sum = fnv(idx, WD24X_DOOM_SRC_W * WD24X_DOOM_SRC_H);
	wd24x_doom_present(present_mode, idx, present, NULL);
	present_sum = fnv(present, WD24X_DOOM_FRAME);
	if (fb)
		wd24x_doom_emit(fb, 0, present, WD24X_DOOM_FRAME, NULL);
	if (wd24x_doom_palette_put(getenv("WD24X_DOOM_PALETTE")))
		put_playpal();
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
	unsigned char doomkey;

	restore_and_signal();
	if (!pressed || !key)
		return 0;
	if (have_up) {
		*pressed = 0;
		*key = pending_up;
		have_up = 0;
		return 1;
	}
	if (!read_tty_key(&doomkey))
		return 0;
	*pressed = 1;
	*key = doomkey;
	pending_up = doomkey;
	have_up = 1;
	return 1;
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
