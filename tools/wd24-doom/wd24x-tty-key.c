/* SPDX-License-Identifier: GPL-2.0 */
/*
 * One byte from a tty, then the saved termios is put back.
 * Timeout is WD24X_TTY_KEY_SEC (default 15). No KDSETMODE.
 * The byte value is not printed.
 */
#include "wd24x-tty-own.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static struct wd24x_tty_own own;
static volatile sig_atomic_t got_sig;

static void on_sig(int sig)
{
	got_sig = sig;
}

static void finish(int rc)
{
	int sig = got_sig;

	wd24x_tty_own_end(&own);
	if (sig) {
		signal(sig, SIG_DFL);
		raise(sig);
	}
	exit(rc);
}

int main(void)
{
	const char *dev = getenv("WD24X_TTY_DEV");
	const char *sec_s = getenv("WD24X_TTY_KEY_SEC");
	int sec = 15;
	int fd;
	struct pollfd p;
	unsigned char b;
	struct sigaction sa;

	if (!dev || !dev[0])
		dev = "/dev/tty0";
	if (sec_s && sec_s[0]) {
		sec = atoi(sec_s);
		if (sec < 1)
			sec = 1;
		if (sec > 120)
			sec = 120;
	}
	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		printf("WD24X-TTY-KEY bad open\n");
		return 1;
	}
	if (wd24x_tty_own_begin(&own, fd) != 0) {
		printf("WD24X-TTY-KEY bad termios\n");
		close(fd);
		return 1;
	}
	sa.sa_handler = on_sig;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	p.fd = fd;
	p.events = POLLIN;
	for (;;) {
		int pr = poll(&p, 1, sec * 1000);
		if (got_sig)
			finish(1);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			printf("WD24X-TTY-KEY bad read\n");
			finish(1);
		}
		if (pr == 0) {
			printf("WD24X-TTY-KEY timeout\n");
			finish(1);
		}
		if (read(fd, &b, 1) == 1) {
			printf("WD24X-TTY-KEY byte 1\n");
			finish(0);
		}
	}
}
