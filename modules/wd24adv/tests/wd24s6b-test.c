// SPDX-License-Identifier: GPL-2.0
/*
 * wd24s6b-test: user-side test of /dev/wd24s6b (wd24adv s6b=1).
 *   wd24s6b-test run   : windows, cross-bank memmove through the mmap,
 *                        kernel COPY (cross-bank, overlapping both ways,
 *                        frame scroll), every byte restored; prints
 *                        S6B-RESULT OK|FAIL
 *   wd24s6b-test hold  : open + mmap + SETWIN, print S6B-HOLD, sleep (kill -9 test)
 * Static i486 build: i486-linux-musl-gcc -static -O2 -march=i486
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "../wd24adv_ioctl.h"

#define FRAME	0x4b000u
#define OFF0	0x50000u	/* off-screen test region */
#define OFFN	0x30000u	/* .. 7FFFFh (PR0A/PR0B reach) */
#define HALF	0x8000u

static int fd;
static volatile uint8_t *win;
static unsigned bad, steps;

static int setwin(unsigned w0, unsigned w1)
{
	struct wd24s6b_win w = { w0, w1 };

	return ioctl(fd, WD24S6B_IOC_SETWIN, &w) ? -errno : 0;
}

static int kcopy(unsigned dst, unsigned src, unsigned len)
{
	struct wd24s6b_copy c = { dst, src, len };

	return ioctl(fd, WD24S6B_IOC_COPY, &c) ? -errno : 0;
}

/* w0 half at page p, w1 parked far away */
static int at(unsigned p)
{
	return setwin(p, p >= 0x10 ? 0x08 : 0x70);
}

static int vio(unsigned off, uint8_t *buf, unsigned n, int wr)
{
	while (n) {
		unsigned o = off & 0xfff, k = HALF - o;
		int err = at(off >> 12);

		if (err)
			return err;
		if (k > n)
			k = n;
		for (unsigned i = 0; i < k; i++) {
			if (wr)
				win[o + i] = buf[i];
			else
				buf[i] = win[o + i];
		}
		off += k;
		buf += k;
		n -= k;
	}
	return 0;
}

static void chk(const char *what, int ok, const char *fmt, unsigned v)
{
	steps++;
	if (!ok)
		bad++;
	printf("S6B %s %s ", ok ? "ok  " : "FAIL", what);
	printf(fmt, v);
	printf("\n");
	fflush(stdout);
}

static unsigned diff(const uint8_t *a, const uint8_t *b, unsigned n)
{
	unsigned d = 0;

	for (unsigned i = 0; i < n; i++)
		d += a[i] != b[i];
	return d;
}

int main(int argc, char **argv)
{
	int hold = argc > 1 && !strcmp(argv[1], "hold");
	struct wd24s6b_info in;
	uint8_t *sav_off, *sav_fr, *ref, *rd;
	unsigned i, s, d, n;
	int err, saved_off = 0, saved_fr = 0;

	if (argc < 2 || (!hold && strcmp(argv[1], "run"))) {
		fprintf(stderr, "usage: wd24s6b-test run|hold\n");
		return 2;
	}
	fd = open("/dev/wd24s6b", O_RDWR);
	if (fd < 0) {
		printf("S6B-RESULT FAIL open: %s\n", strerror(errno));
		return 1;
	}
	win = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (win == MAP_FAILED) {
		printf("S6B-RESULT FAIL mmap: %s\n", strerror(errno));
		return 1;
	}
	if (hold) {
		err = setwin(0x50, 0x70);
		printf("S6B-HOLD setwin %d pid %d\n", err, (int)getpid());
		fflush(stdout);
		for (;;)
			pause();
	}
	sav_off = malloc(OFFN);
	sav_fr = malloc(FRAME);
	ref = malloc(OFFN);
	rd = malloc(FRAME);
	if (!sav_off || !sav_fr || !ref || !rd)
		return 1;
	err = ioctl(fd, WD24S6B_IOC_INFO, &in) ? -errno : 0;
	chk("info", !err && in.state == 1 && in.w0 == 0 && in.w1 == 8, "state %u", in.state);

	/* refusals: no hardware write */
	chk("setwin overlap refused", setwin(0x50, 0x54) == -EINVAL, "%u", 0);
	chk("setwin w1<8 refused", setwin(0x10, 0x04) == -EINVAL, "%u", 0);
	chk("setwin >78h refused", setwin(0x79, 0x08) == -EINVAL, "%u", 0);
	chk("copy past 512K refused", kcopy(0x7f000, 0x50000, 0x2000) == -EINVAL, "%u", 0);

	/* save the off-screen region and the frame (restored at the end) */
	err = vio(OFF0, sav_off, OFFN, 0);
	saved_off = !err;
	chk("save off-screen 50000-7FFFF", !err, "err %d", err);
	err = vio(0, sav_fr, FRAME, 0);
	saved_fr = !err;
	chk("save frame", !err, "err %d", err);
	if (bad)
		goto restore;

	/* pattern, written through w0 and checked through w1 (split) */
	for (i = 0; i < OFFN; i++)
		ref[i] = (uint8_t)(i * 7 + (i >> 9) * 13 + 1);
	err = vio(OFF0, ref, OFFN, 1);
	chk("pattern write", !err, "err %d", err);
	err = setwin(0x10, 0x60);
	for (d = 0, i = 0; !err && i < HALF; i++)
		d += win[HALF + i] != ref[0x10000 + i];
	chk("split: A8000 half reads page 60h", !err && !d, "%u bad", d);

	/* cross-bank memmove through the mapping: src 52xxx (bank 5), dst 6Bxxx (bank 6) */
	s = 0x52345;
	d = 0x6b123;
	n = 0x5000;
	err = setwin(s >> 12, d >> 12);
	if (!err)
		memmove((void *)(win + HALF + (d & 0xfff)), (void *)(win + (s & 0xfff)), n);
	memmove(ref + d - OFF0, ref + s - OFF0, n);
	err = err ? err : vio(OFF0, rd, OFFN, 0);
	chk("mmap cross-bank memmove 52345->6B123 len 5000", !err && !diff(rd, ref, OFFN), "%u bad",
	    err ? 0 : diff(rd, ref, OFFN));

	/* kernel COPY: cross-bank, overlapping forward and backward */
	{
		static const unsigned c[][3] = {
			{ 0x6f000, 0x51000, 0x9000 },	/* banks 5 -> 6, disjoint */
			{ 0x5e100, 0x5c000, 0x8f00 },	/* overlap, dst > src, crosses 60000 */
			{ 0x58000, 0x5a777, 0x12000 },	/* overlap, dst < src, crosses 60000 */
			{ 0x7ff00, 0x50000, 0x100 },	/* up to the reach end */
		};
		for (i = 0; i < 4; i++) {
			err = kcopy(c[i][0], c[i][1], c[i][2]);
			memmove(ref + c[i][0] - OFF0, ref + c[i][1] - OFF0, c[i][2]);
			err = err ? err : vio(OFF0, rd, OFFN, 0);
			printf("S6B copy %05x <- %05x len %05x: err %d\n", c[i][0], c[i][1], c[i][2], err);
			chk("kernel COPY", !err && !diff(rd, ref, OFFN), "%u bad",
			    err ? 0 : diff(rd, ref, OFFN));
			if (err)
				goto restore;
		}
	}

	/* frame scroll up 16 lines (visible, then restored) */
	err = kcopy(0, 640 * 16, FRAME - 640 * 16);
	err = err ? err : vio(0, rd, FRAME, 0);
	chk("COPY frame scroll 16 lines", !err && !diff(rd, sav_fr + 640 * 16, FRAME - 640 * 16) &&
	    !diff(rd + FRAME - 640 * 16, sav_fr + FRAME - 640 * 16, 640 * 16), "%u bad",
	    err ? 0 : diff(rd, sav_fr + 640 * 16, FRAME - 640 * 16));

restore:
	if (saved_fr) {
		err = vio(0, sav_fr, FRAME, 1);
		err = err ? err : vio(0, rd, FRAME, 0);
		chk("frame restored", !err && !diff(rd, sav_fr, FRAME), "%u bad", err ? 0 : diff(rd, sav_fr, FRAME));
	}
	if (saved_off) {
		err = vio(OFF0, sav_off, OFFN, 1);
		err = err ? err : vio(OFF0, rd, OFFN, 0);
		chk("off-screen restored", !err && !diff(rd, sav_off, OFFN), "%u bad",
		    err ? 0 : diff(rd, sav_off, OFFN));
	}
	err = setwin(0, 8);
	chk("windows back to 0/8", !err, "err %d", err);
	err = ioctl(fd, WD24S6B_IOC_INFO, &in) ? -errno : 0;
	printf("S6B-RESULT %s %u/%u steps; state %u copies %u setwin %u writes %u\n",
	       bad ? "FAIL" : "OK", steps - bad, steps, in.state, in.ncopy, in.nset, in.nwr);
	munmap((void *)win, 0x10000);
	close(fd);
	return bad ? 1 : 0;
}
