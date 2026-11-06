/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Bounded S6d mmap exercise for a later watched live run. Do not start it
 * from a boot script. It touches one bank at a time (separate volatile
 * accesses), checks the bytes, then munmaps and closes.
 *
 * SIGALRM calls _exit(2) only if the signal is delivered. Delivery waits
 * until the task is back in userspace or in an interruptible sleep.
 * mutex_lock, console_lock, and a fault stuck in the kernel are
 * uninterruptible: the alarm does not fire, and kill -9 does not break
 * that sleep either. The alarm is not a recovery path for a lock deadlock.
 * The default alarm is 20 s. --hold keeps the mapping after a good
 * read-back (alarm then 60 s). A normal return munmaps, then closes.
 *
 * It does not read /proc. After WD24S6D-MMAP, read /proc/wd24adv/s6d once.
 * Default mode: 10 faults and 10 window switches, thrash 0.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include "../wd24adv_s6d_fault.h"

static void on_alarm(int sig)
{
	(void)sig;
	_exit(2);
}

static void stamp_at(volatile u8 *fb, u32 base, u8 pix)
{
	u32 row, col;

	for (row = 0; row < WD24S6D_STAMP; row++) {
		for (col = 0; col < WD24S6D_STAMP; col++)
			fb[base + row * 640u + col] = pix;
	}
}

static int stamp_ok(volatile u8 *fb, u32 base, u8 pix)
{
	u32 row, col;

	for (row = 0; row < WD24S6D_STAMP; row++) {
		for (col = 0; col < WD24S6D_STAMP; col++) {
			if (fb[base + row * 640u + col] != pix)
				return 0;
		}
	}
	return 1;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/fb0";
	struct fb_fix_screeninfo fix;
	volatile u8 *fb;
	int fd, bad = 0, hold = 0;
	unsigned i;

	if (argc == 2 && strcmp(argv[1], "--hold") == 0)
		hold = 1;
	else if (argc == 2)
		dev = argv[1];
	else if (argc > 2) {
		fprintf(stderr, "usage: wd24s6d-mmap [/dev/fbN | --hold]\n");
		return 1;
	}
	signal(SIGALRM, on_alarm);
	alarm(20);

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "wd24s6d-mmap: open %s: %s\n", dev, strerror(errno));
		return 1;
	}
	memset(&fix, 0, sizeof(fix));
	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		fprintf(stderr, "wd24s6d-mmap: FBIOGET_FSCREENINFO: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}
	if (strncmp(fix.id, "wd24s6d", 7) != 0 || fix.line_length != 640) {
		fprintf(stderr, "wd24s6d-mmap: %s id '%.16s' line %u is not wd24s6d\n",
			dev, fix.id, fix.line_length);
		close(fd);
		return 1;
	}

	fb = mmap(NULL, WD24S6D_FRAME_BYTES, PROT_READ | PROT_WRITE,
		  MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		fprintf(stderr, "wd24s6d-mmap: mmap: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	for (i = 0; i < WD24S6D_TOUCH_N; i++) {
		u32 off = wd24s6d_touch_off(i);

		fb[off] = fb[off];		/* one faulting byte */
		if (i < WD24S6D_STAMP_N)
			stamp_at(fb, off, wd24s6d_stamp_pix(i));
		if (off == 0x4afffu) {
			fb[off] = 0x3c;
			if (fb[off] != 0x3c)
				bad++;
		}
	}
	for (i = 0; i < WD24S6D_STAMP_N; i++) {
		if (!stamp_ok(fb, wd24s6d_stamp_off(i), wd24s6d_stamp_pix(i)))
			bad++;
	}
	if (fb[0x4afffu] != 0x3c)
		bad++;

	if (bad) {
		printf("WD24S6D-MMAP bad %d\n", bad);
		munmap((void *)fb, WD24S6D_FRAME_BYTES);
		close(fd);
		return 1;
	}
	if (hold) {
		printf("WD24S6D-MMAP hold\n");
		fflush(stdout);
		alarm(60);
		for (;;)
			pause();
	}
	alarm(0);
	if (munmap((void *)fb, WD24S6D_FRAME_BYTES) < 0) {
		fprintf(stderr, "wd24s6d-mmap: munmap: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (close(fd) < 0) {
		fprintf(stderr, "wd24s6d-mmap: close: %s\n", strerror(errno));
		return 1;
	}
	printf("WD24S6D-MMAP ok stamps %u tail 1\n", WD24S6D_STAMP_N);
	return 0;
}
