/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Indexed Doom backend. Render stays 320x200 palette indices.
 * Every frame is also composed into a private 640x480 buffer.
 * /dev/fb0 is opened only when WD24X_DOOM_FB=mmap, ioctl, or aperture.
 * mmap requires 640x480x8. ioctl with WD24X_DOOM_PRESENT=13h asks for
 * 320x200x8 and falls back to letterbox if that is refused on a 640x480
 * frame. aperture with present 13h maps the 64 KiB chain-4 window at
 * 0xA0000 and draws the 64000-byte frame there. aperturecopy maps the
 * same window and copies one finished frame into it as one movl per
 * dword (not a string move). Neither
 * map writes a VGA register. WD24X_DOOM_PALETTE=put then sends PLAYPAL
 * through FBIOPUTCMAP.
 * Any other palette value does not. WD24X_DOOM_INPUT=tty owns the tty
 * termios and puts the saved struct back. A 13h session sets
 * KD_GRAPHICS on the active VT so vgacon does not write CR0C/CR0D,
 * and puts KD_TEXT back on the way out if this process was the one
 * that changed it. The default path opens neither device.
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
#include <time.h>

#include <linux/fb.h>
#include <linux/kd.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_video.h"
#include "wd24x-doom-keys.h"
#include "wd24x-doom-pick.h"
#include "wd24x-doom-present.h"
#include "wd24x-doom-view.h"
#include "wd24x-doom-time.h"
#include "wd24x-doom-framestats.h"
#include "wd24x-tty-own.h"
#include "../../modules/wd24adv/wd24adv_ioctl.h"

static unsigned frames;
static unsigned index_sum;
static unsigned present_sum;
static int reported;
static int frame_cap;
static int present_mode;
static uint8_t present[WD24X_DOOM_FRAME];
static uint8_t *fb;
static int fb_fd = -1;
static int fb_ioctl;
static int fb_aperture;
static int fb_apcopy;
static int fb_fake;
static uint8_t *fake_win;
static int use_13h;
static int fb_start_640;
static int fb_put_13h;
static struct fb_var_screeninfo fb_var_saved;
static struct fb_var_screeninfo fb_var_orig;
static unsigned int fb_x, fb_y, fb_bpp, fb_line, fb_smem;
static unsigned present_switches;
static struct wd24x_tty_own tty_own;
static int kd_fd = -1;
static int kd_restore;
static int do_flip, do_vsync, flip_said;
extern int detailLevel;
extern int screenblocks;
static struct wd24x_keys keys;
static int have_up;
static unsigned char pending_up;
static volatile sig_atomic_t got_sig;
static int bars_sent;
static int sum_frames;
static struct wd24x_doom_time doom_time;
static int fs_on;
static int fs_dumped;
static unsigned fs_presents;
static int fs_have_prev;
static int fs_have_tic;
static int fs_prev_tic;
static uint64_t fs_prev;
static struct wd24x_doom_fs fs;
extern int gametic;
static unsigned hud_show, hud_cr0c, hud_pr0a;
static void arm_signals(void);

static int time_of(void)
{
	const char *e = getenv("WD24X_DOOM_TIME");
	long n;

	if (!e || !*e || e[0] == '0')
		return 0;
	n = strtol(e, NULL, 10);
	if (n <= 0 || n > 10000)
		return 30;
	return (int)n;
}

static uint64_t mono_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int fs_enabled(void)
{
	const char *e = getenv("WD24X_DOOM_FRAMESTATS");

	return e && e[0] == '1' && !e[1];
}

static void fs_dump(void)
{
	uint16_t sorted[WD24X_FS_CAP];
	unsigned n, i;

	if (!fs_on || fs_dumped)
		return;
	fs_dumped = 1;
	n = fs.n;
	if (n > WD24X_FS_CAP)
		n = WD24X_FS_CAP;
	if (n)
		memcpy(sorted, fs.ms, n * sizeof(sorted[0]));
	wd24x_fs_sort(sorted, n);
	fprintf(stderr,
		"WD24X-DOOM framestats frames %u intervals %u min %u med %u p95 %u p99 %u max %u over1 %u over2 %u render_ms %llu copy_ms %llu flip_ms %llu\n",
		fs_presents, fs.frames,
		wd24x_fs_pct(sorted, n, 0),
		wd24x_fs_pct(sorted, n, 50),
		wd24x_fs_pct(sorted, n, 95),
		wd24x_fs_pct(sorted, n, 99),
		wd24x_fs_pct(sorted, n, 100),
		fs.over1, fs.over2,
		(unsigned long long)(fs.render_ns / 1000000ull),
		(unsigned long long)(fs.copy_ns / 1000000ull),
		(unsigned long long)(fs.flip_ns / 1000000ull));
	fprintf(stderr, "WD24X-DOOM framestats tics");
	for (i = 0; i < WD24X_FS_TICS; i++)
		fprintf(stderr, " %u:%u", i, fs.tics[i]);
	fprintf(stderr, "\nWD24X-DOOM framestats hist");
	for (i = 0; i < WD24X_FS_BUCKETS; i++) {
		if (i + 1u < WD24X_FS_BUCKETS)
			fprintf(stderr, " <%u:%u", wd24x_fs_edge[i], fs.hist[i]);
		else
			fprintf(stderr, " >=%u:%u", wd24x_fs_edge[i - 1u],
				fs.hist[i]);
	}
	fprintf(stderr, "\n");
	{
		unsigned skip = 0, wait = 0, to = 0;
		const char *path = getenv("WD24X_WD24_LIVE");
		FILE *fp;
		char buf[256];

		if (!path || !path[0])
			path = "/proc/wd24adv/live";
		fp = fopen(path, "r");
		if (fp) {
			while (fgets(buf, sizeof(buf), fp)) {
				unsigned s, w, t;

				if (sscanf(buf, WD24_FLIP_COUNT_SCAN, &s, &w, &t) == 3) {
					skip = s;
					wait = w;
					to = t;
				}
			}
			fclose(fp);
		}
		fprintf(stderr,
			"WD24X-DOOM framestats flip skip %u wait %u latch_to %u\n",
			skip, wait, to);
	}
	fflush(stderr);
}

/* One running line so a SIGINT before exit still has counts. */
static void fs_tick(void)
{
	if (!fs_on)
		return;
	if (fs_presents != 1 && (fs_presents % 35u) != 0)
		return;
	fprintf(stderr,
		"WD24X-DOOM framestats tick frames %u intervals %u over1 %u over2 %u\n",
		fs_presents, fs.frames, fs.over1, fs.over2);
	fflush(stderr);
}

/* Rows 168..199 are the 32-pixel status bar when screenblocks is 10.
 * The hash is the engine buffer before the copy. The copy writes all
 * 64000 of that buffer into the CPU window. db5d913.. the flip change
 * did not touch that copy, a dirty rectangle, or a VRAM clear.
 * run is the longest identical-byte run. A HUD is long runs. Noise
 * in the source is a short run. Noise only on the glass, with a long
 * run here, is the scan reading a different bank than this buffer.
 */
static void hud_src_line(unsigned frame)
{
	const uint8_t *p;
	unsigned i, nz = 0, run = 1, best = 1;
	unsigned hash = 2166136261u;

	if (!fs_on || !DG_ScreenBuffer)
		return;
	if (frame != 1 && (frame % 35u) != 0)
		return;
	p = (const uint8_t *)DG_ScreenBuffer + 168u * 320u;
	for (i = 0; i < 32u * 320u; i++) {
		hash ^= p[i];
		hash *= 16777619u;
		if (p[i])
			nz++;
		if (i && p[i] == p[i - 1]) {
			run++;
			if (run > best)
				best = run;
		} else {
			run = 1;
		}
	}
	fprintf(stderr,
		"WD24X-DOOM hud src frame %u rows 168 199 hash 0x%08x nz %u run %u show %u cr0c %02x pr0a %02x\n",
		frame, hash, nz, best, hud_show, hud_cr0c, hud_pr0a);
	fflush(stderr);
}

static void time_line(void)
{
	printf("WD24X-DOOM time frames %u render_ms %llu present_ms %llu palette_ms %llu\n",
	       doom_time.frames,
	       (unsigned long long)wd24x_doom_time_ms(doom_time.render_ns),
	       (unsigned long long)wd24x_doom_time_ms(doom_time.present_ns),
	       (unsigned long long)wd24x_doom_time_ms(doom_time.palette_ns));
}

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

/* vgacon_scroll writes CR0C/CR0D while the VT stays KD_TEXT. Graphics
 * mode makes that return before the write. Linux 6.18.57 vt_kdsetmode.
 */
static void kd_hold(void)
{
	const char *dev = getenv("WD24X_TTY_DEV");
	int mode = KD_TEXT;

	if (kd_fd >= 0)
		return;
	if (!dev || !dev[0])
		dev = "/dev/tty0";
	kd_fd = open(dev, O_RDWR);
	if (kd_fd < 0) {
		printf("WD24X-DOOM kd refused\n");
		return;
	}
	if (ioctl(kd_fd, KDGETMODE, &mode) != 0 ||
	    (mode != KD_GRAPHICS && ioctl(kd_fd, KDSETMODE, KD_GRAPHICS) != 0)) {
		printf("WD24X-DOOM kd refused\n");
		close(kd_fd);
		kd_fd = -1;
		return;
	}
	kd_restore = mode != KD_GRAPHICS;
	printf("WD24X-DOOM kd graphics\n");
}

static void restore_and_signal(void)
{
	int sig = got_sig;

	if (!sig)
		return;
	fs_dump();
	kd_release();
	wd24x_tty_own_end(&tty_own);
	signal(sig, SIG_DFL);
	raise(sig);
}

static int want_fake(void)
{
	const char *e = getenv("WD24X_DOOM_FAKE");

	return e && e[0] == '1';
}

static void dump_window(void)
{
	const char *path = getenv("WD24X_DOOM_DUMP");
	FILE *fp;

	if (!path || !path[0] || !fake_win)
		return;
	fp = fopen(path, "wb");
	if (!fp)
		return;
	if (fwrite(fake_win, 1, WD24X_DOOM_WIN, fp) != WD24X_DOOM_WIN)
		printf("WD24X-DOOM dump short\n");
	fclose(fp);
}

static void report(void)
{
	if (reported)
		return;
	reported = 1;
	kd_release();
	wd24x_tty_own_end(&tty_own);
	dump_window();
	if (fb_fake) {
		fb = NULL;
		if (fb_aperture)
			DG_ScreenBuffer = NULL;
	}
	if (fb && (fb_aperture || fb_apcopy)) {
		munmap(fb, WD24X_DOOM_WIN);
		if (fb_aperture)
			DG_ScreenBuffer = NULL;
		fb = NULL;
	}
	if (fb_fd >= 0) {
		close(fb_fd);
		fb_fd = -1;
	}
	printf("WD24X-DOOM frames %u index 0x%08x present 0x%08x\n",
	       frames, index_sum, present_sum);
	if (doom_time.every)
		time_line();
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

static int read_fb_info(void)
{
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;

	if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) != 0 ||
	    ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) != 0)
		return -1;
	fb_x = var.xres;
	fb_y = var.yres;
	fb_bpp = var.bits_per_pixel;
	fb_line = fix.line_length;
	fb_smem = fix.smem_len;
	fb_var_saved = var;
	return 0;
}

static int fb_is_640(void)
{
	return wd24x_doom_fb_geometry_ok(fb_x, fb_y, fb_bpp, fb_line, fb_smem);
}

static int fb_is_13h(void)
{
	return wd24x_doom_fb_geometry_13h(fb_x, fb_y, fb_bpp, fb_line, fb_smem);
}

static void restore_fb_var(const struct fb_var_screeninfo *saved)
{
	struct fb_var_screeninfo back;

	if (!saved || fb_fd < 0)
		return;
	back = *saved;
	if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &back) == 0)
		read_fb_info();
}

static int open_fb_fd(void)
{
	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0 || read_fb_info() != 0 || !fb_is_640()) {
		printf("WD24X-DOOM fb refused\n");
		if (fb_fd >= 0)
			close(fb_fd);
		fb_fd = -1;
		return -1;
	}
	return 0;
}

/* Opens /dev/fb0 and records the geometry. Does not require 640x480. */
static int open_fb_any(void)
{
	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0 || read_fb_info() != 0) {
		printf("WD24X-DOOM fb refused\n");
		if (fb_fd >= 0)
			close(fb_fd);
		fb_fd = -1;
		return -1;
	}
	return 0;
}

static void fall_back_letterbox(void)
{
	printf("WD24X-DOOM present 13h refused\n");
	use_13h = 0;
	present_mode = WD24X_DOOM_LETTER;
	printf("WD24X-DOOM present letterbox\n");
	printf("WD24X-DOOM blit letterbox dword\n");
}

/* Ask for 320x200x8 when the fb is not already that size. A match
 * copies the published var back and writes no register. */
static int settle_13h(void)
{
	int after;

	fb_start_640 = fb_is_640();
	after = fb_is_13h();
	fb_var_orig = fb_var_saved;
	if (present_mode == WD24X_DOOM_DIRECT && !after) {
		struct fb_var_screeninfo req;

		memset(&req, 0, sizeof(req));
		req.xres = WD24X_DOOM_SRC_W;
		req.yres = WD24X_DOOM_SRC_H;
		req.xres_virtual = WD24X_DOOM_SRC_W;
		req.yres_virtual = WD24X_DOOM_SRC_H;
		req.bits_per_pixel = 8;
		if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &req) == 0) {
			fb_put_13h = 1;
			if (read_fb_info() != 0)
				after = 0;
			else
				after = fb_is_13h();
		}
	}
	return after;
}

/* Map the 64 KiB chain-4 window. The map writes no VGA register.
 * own_screen: the renderer stores into the window.
 * Otherwise the malloc stays the frame and DrawFrame copies it.
 * Letterbox is not a fallback. */
static int open_fake(int own_screen)
{
	fake_win = calloc(1, WD24X_DOOM_WIN);
	if (!fake_win)
		return -1;
	fb = fake_win;
	fb_fake = 1;
	use_13h = 1;
	if (own_screen) {
		free(DG_ScreenBuffer);
		DG_ScreenBuffer = fake_win;
		printf("WD24X-DOOM fb mmap 320 200 aperture\n");
	} else if (fb_apcopy) {
		printf("WD24X-DOOM fb mmap 320 200 aperturecopy\n");
	} else {
		printf("WD24X-DOOM fb ioctl 320 200\n");
	}
	return 0;
}

static int open_aperture(int own_screen)
{
	void *map;
	int after;

	if (want_fake())
		return open_fake(own_screen);

	if (open_fb_any() != 0)
		return -1;
	after = settle_13h();
	if (!after) {
		printf("WD24X-DOOM fb mmap refused\n");
		close(fb_fd);
		fb_fd = -1;
		return -1;
	}
	map = mmap(NULL, WD24X_DOOM_WIN, PROT_READ | PROT_WRITE, MAP_SHARED,
		   fb_fd, 0);
	if (map == MAP_FAILED) {
		printf("WD24X-DOOM fb mmap refused\n");
		if (fb_put_13h)
			restore_fb_var(&fb_var_orig);
		close(fb_fd);
		fb_fd = -1;
		return -1;
	}
	fb = map;
	use_13h = 1;
	if (own_screen) {
		free(DG_ScreenBuffer);
		DG_ScreenBuffer = map;
		printf("WD24X-DOOM fb mmap 320 200 aperture\n");
	} else {
		printf("WD24X-DOOM fb mmap 320 200 aperturecopy\n");
	}
	return 0;
}

static int open_fb(void)
{
	if (open_fb_fd() != 0)
		return -1;
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

/* 1 is this switch, not the page. show is the page just published.
 * The first call is before any pixel, so show is 0 and the CPU bank
 * is the hidden page. A log line "flip 0" was that page.
 */
static int flip_once(void)
{
	struct wd24s6d_flip fr;

	memset(&fr, 0, sizeof(fr));
	fr.vsync = do_vsync ? 1u : 0u;
	if (ioctl(fb_fd, WD24S6D_IOC_FLIP, &fr) != 0)
		return -1;
	if (!flip_said) {
		printf("WD24X-DOOM flip 1 vsync %u show %u\n",
		       fr.vsync, fr.show);
		printf("WD24X-DOOM flip cr0c %02x cr0d %02x pr0a %02x\n",
		       fr.cr0c, fr.cr0d, fr.pr0a);
		printf("WD24X-DOOM flip latched 1\n");
		flip_said = 1;
	}
	hud_show = fr.show;
	hud_cr0c = fr.cr0c;
	hud_pr0a = fr.pr0a;
	return 0;
}

void DG_Init(void)
{
	if (!fs_on && fs_enabled()) {
		fs_on = 1;
		wd24x_fs_clear(&fs);
		atexit(fs_dump);
		/* Timedemo does not own the tty, so SIGINT would
		 * otherwise leave no histogram.
		 */
		arm_signals();
	}
	const char *present_s = getenv("WD24X_DOOM_PRESENT");
	const char *dev;

	frame_cap = cap_of();
	wd24x_doom_time_init(&doom_time, (unsigned)time_of());
	present_mode = wd24x_doom_present_mode(present_s);
	atexit(report);
	printf("WD24X-DOOM render 320 200 index\n");
	{
		struct wd24x_doom_view view;

		/* Applied before D_DoomMain reads these. No default.cfg
		 * in the run, so the engine keeps the values.
		 */
		if (wd24x_doom_view_parse(getenv("WD24X_DOOM_LOW"),
					 getenv("WD24X_DOOM_BLOCKS"), &view) != 0) {
			printf("WD24X-DOOM view refused\n");
			exit(2);
		}
		if (view.low)
			detailLevel = 1;
		if (view.blocks)
			screenblocks = view.blocks;
		printf("WD24X-DOOM view low %d blocks %d\n",
		       detailLevel, screenblocks);
	}
	if (present_mode < 0) {
		printf("WD24X-DOOM present refused\n");
		exit(2);
	}
	printf("WD24X-DOOM present %s\n", wd24x_doom_present_name(present_mode));
	if (present_mode == WD24X_DOOM_LETTER)
		printf("WD24X-DOOM blit letterbox dword\n");
	if (wd24x_doom_palette_put(getenv("WD24X_DOOM_PALETTE")) &&
	    !wd24x_doom_fb_opt_in(getenv("WD24X_DOOM_FB"))) {
		printf("WD24X-DOOM palette put needs mmap\n");
		exit(2);
	}
	fb_ioctl = wd24x_doom_fb_is_ioctl(getenv("WD24X_DOOM_FB"));
	fb_aperture = wd24x_doom_fb_is_aperture(getenv("WD24X_DOOM_FB"));
	fb_apcopy = wd24x_doom_fb_is_aperture_copy(getenv("WD24X_DOOM_FB"));
	if (present_mode == WD24X_DOOM_DIRECT && !fb_ioctl && !fb_aperture &&
	    !fb_apcopy)
		fall_back_letterbox();
	if (!wd24x_doom_fb_opt_in(getenv("WD24X_DOOM_FB")))
		printf("WD24X-DOOM display file\n");
	else if (fb_aperture || fb_apcopy) {
		if (present_mode != WD24X_DOOM_DIRECT ||
		    open_aperture(fb_aperture) != 0)
			exit(1);
	} else if (fb_ioctl) {
		int after;

		if (want_fake()) {
			if (open_fake(0) != 0)
				exit(1);
			goto fake_ready;
		}
		if (open_fb_any() != 0)
			exit(1);
		after = settle_13h();
		if (present_mode == WD24X_DOOM_DIRECT) {
			int pick = wd24x_doom_13h_pick(fb_start_640, after, after);

			if (pick == WD24X_DOOM_DIRECT) {
				use_13h = 1;
			} else if (pick == WD24X_DOOM_LETTER) {
				if (fb_put_13h)
					restore_fb_var(&fb_var_orig);
				fall_back_letterbox();
			} else {
				printf("WD24X-DOOM present 13h refused\n");
				exit(1);
			}
		} else if (!fb_start_640) {
			printf("WD24X-DOOM fb refused\n");
			exit(1);
		}
		printf("WD24X-DOOM fb ioctl 320 200\n");
	} else if (open_fb() != 0)
		exit(1);
fake_ready:
	{
		const char *sum_s = getenv("WD24X_DOOM_SUM");

		if (sum_s && sum_s[0] == '1')
			sum_frames = 1;
		else if (sum_s && sum_s[0] == '0')
			sum_frames = 0;
		else
			sum_frames = (fb || fb_ioctl) ? 0 : 1;
	}
	if (!fb_fake && (fb_aperture || fb_apcopy || use_13h))
		kd_hold();
	{
		const char *fl = getenv("WD24X_DOOM_FLIP");
		const char *vs = getenv("WD24X_DOOM_VSYNC");

		do_flip = fl && fl[0] == '1' && !fl[1];
		do_vsync = vs && vs[0] == '1' && !vs[1];
	}
	if (fb_aperture || fb_apcopy || use_13h)
		printf("WD24X-DOOM flip env %d\n", do_flip ? 1 : 0);
	/* The mmap starts on the page the CRTC is scanning. One flip
	 * before any pixel publishes that page and banks the CPU at the
	 * next one, so the first frame is not drawn on the beam.
	 */
	if (do_flip && !fb_fake && fb_fd >= 0 &&
	    (fb_aperture || fb_apcopy || use_13h) && flip_once() != 0) {
		printf("WD24X-DOOM flip refused\n");
		do_flip = 0;
	}
	printf("WD24X-DOOM sum %s\n", sum_frames ? "on" : "off");
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
	uint64_t t_enter = 0, t_pal = 0, t_left = 0;
	uint64_t fs_enter = 0, fs_flip = 0;

	restore_and_signal();
	if (fs_on)
		fs_enter = mono_ns();
	if (!DG_ScreenBuffer)
		return;
	idx = (const uint8_t *)DG_ScreenBuffer;
	frames++;
	if (wd24x_doom_index_hashed(sum_frames))
		index_sum = fnv(idx, WD24X_DOOM_SRC_W * WD24X_DOOM_SRC_H);
	if (doom_time.every)
		t_enter = mono_ns();
	if (!fb_fake && fb_fd >= 0 &&
	    wd24x_doom_palette_put(getenv("WD24X_DOOM_PALETTE")))
		put_playpal();
	if (doom_time.every)
		t_pal = mono_ns();
	if (fb_apcopy) {
		wd24x_doom_copy_dwords(fb, idx, WD24X_DOOM_SRC_W * WD24X_DOOM_SRC_H);
		if (frames == 1)
			printf("WD24X-DOOM blit aperturecopy 64000\n");
	} else if (fb_aperture) {
		if (frames == 1)
			printf("WD24X-DOOM blit aperture 64000\n");
	} else if (fb_ioctl && use_13h && fb_fake) {
		wd24x_doom_copy_dwords(fake_win, idx,
				       WD24X_DOOM_SRC_W * WD24X_DOOM_SRC_H);
		if (frames == 1)
			printf("WD24X-DOOM blit direct 64000\n");
	} else if (fb_ioctl && use_13h) {
		struct wd24s6d_present req;

		memset(&req, 0, sizeof(req));
		req.pixels = (uint64_t)(uintptr_t)idx;
		req.src_w = 320;
		req.src_h = 200;
		req.scale = 1;
		req.op = WD24ADV_BLIT_DIRECT;
		req.nbytes = WD24S6D_PRESENT_SRC;
		if (ioctl(fb_fd, WD24S6D_IOC_PRESENT, &req) != 0) {
			if (!fb_start_640) {
				printf("WD24X-DOOM present 13h refused\n");
				exit(1);
			}
			if (fb_put_13h)
				restore_fb_var(&fb_var_orig);
			fall_back_letterbox();
		} else {
			present_switches = req.switches;
			if (frames == 1) {
				printf("WD24X-DOOM blit direct 64000\n");
				printf("WD24X-DOOM present switches %u\n",
				       present_switches);
			}
		}
	} else if (fb_ioctl && !use_13h) {
		struct wd24s6d_present req;

		memset(&req, 0, sizeof req);
		req.pixels = (uint64_t)(uintptr_t)idx;
		req.src_w = 320;
		req.src_h = 200;
		req.dst_x = 0;
		req.dst_y = 0;
		req.scale = 2;
		req.op = WD24ADV_BLIT_LETTER2X;
		req.nbytes = WD24S6D_PRESENT_SRC;
		if (ioctl(fb_fd, WD24S6D_IOC_PRESENT, &req) != 0) {
			printf("WD24X-DOOM present refused\n");
			exit(1);
		}
		present_switches = req.switches;
		if (frames == 1)
			printf("WD24X-DOOM present switches %u\n",
			       present_switches);
	} else if (!fb_ioctl && fb && present_mode == WD24X_DOOM_LETTER) {
		if (!bars_sent) {
			wd24x_doom_present(present_mode, idx, fb, NULL);
			bars_sent = 1;
		} else {
			wd24x_doom_letter_picture(idx, fb, NULL);
		}
		if (sum_frames)
			wd24x_doom_present(present_mode, idx, present, NULL);
	} else if (!fb_ioctl) {
		wd24x_doom_present(present_mode, idx, present, NULL);
		if (fb)
			wd24x_doom_emit(fb, 0, present, WD24X_DOOM_FRAME, NULL);
	}
	if (fs_on)
		fs_flip = mono_ns();
	if (do_flip && !fb_fake && fb_fd >= 0 &&
	    (fb_aperture || fb_apcopy || use_13h) && flip_once() != 0) {
		printf("WD24X-DOOM flip refused\n");
		do_flip = 0;
	}
	if (fs_on) {
		uint64_t now = mono_ns();
		uint64_t render = 0, copy = 0, flip = 0;
		unsigned tics = 0;

		if (fs_have_prev && fs_enter >= fs_prev)
			render = fs_enter - fs_prev;
		if (fs_flip >= fs_enter)
			copy = fs_flip - fs_enter;
		if (now >= fs_flip)
			flip = now - fs_flip;
		fs_presents++;
		if (fs_have_prev && now >= fs_prev) {
			unsigned gap = (unsigned)((now - fs_prev) / 1000000ull);

			if (fs_have_tic) {
				int d = gametic - fs_prev_tic;

				if (d < 0)
					d = 0;
				tics = (unsigned)d;
			}
			wd24x_fs_add(&fs, gap, tics, render, copy, flip);
		} else {
			fs.render_ns += render;
			fs.copy_ns += copy;
			fs.flip_ns += flip;
		}
		fs_prev = now;
		fs_have_prev = 1;
		fs_prev_tic = gametic;
		fs_have_tic = 1;
		fs_tick();
		hud_src_line(frames);
	}
	if (sum_frames)
		present_sum = fnv(present, WD24X_DOOM_FRAME);
	if (doom_time.every) {
		t_left = mono_ns();
		if (!t_pal)
			t_pal = t_enter;
		if (wd24x_doom_time_frame(&doom_time, t_enter, t_pal, t_left))
			time_line();
	}
	if (frame_cap && (int)frames >= frame_cap) {
		report();
		exit(0);
	}
}

void DG_SleepMs(uint32_t ms)
{
	/* I_Sleep(1) is the tic-wait yield. Without high-res timers a
	 * 1 ms sleep waits for the next periodic tick. That insert is
	 * not used here.
	 */
	(void)ms;
}

uint32_t DG_GetTicksMs(void)
{
	struct timespec ts;

	/* Monotonic, so a wall-clock step does not move the tic. */
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		return wd24x_doom_ms_of((uint64_t)ts.tv_sec, (uint32_t)ts.tv_nsec);
	{
		struct timeval tv;

		gettimeofday(&tv, NULL);
		return (uint32_t)(tv.tv_sec * 1000u + tv.tv_usec / 1000u);
	}
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
