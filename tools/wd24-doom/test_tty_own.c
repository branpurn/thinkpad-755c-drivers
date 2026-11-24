/* SPDX-License-Identifier: GPL-2.0 */
/* PTY proof that begin/end restores the termios we saved. Not a 755C tty. */
#include "wd24x-tty-own.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int fails, passes;
#define T(c, w) do { if (c) passes++; else { fails++; printf("FAIL %s\n", w); } } while (0)

static int same_termios(const struct termios *a, const struct termios *b)
{
	return a->c_iflag == b->c_iflag && a->c_oflag == b->c_oflag &&
	       a->c_cflag == b->c_cflag && a->c_lflag == b->c_lflag &&
	       memcmp(a->c_cc, b->c_cc, sizeof a->c_cc) == 0;
}

static volatile sig_atomic_t child_sig;

static void child_on_sig(int sig)
{
	child_sig = sig;
}

int main(void)
{
	int master, slave, status;
	pid_t pid;
	struct termios marked, after, mid;
	struct wd24x_tty_own own;

	T(openpty(&master, &slave, NULL, NULL, NULL) == 0, "openpty");
	T(tcgetattr(slave, &marked) == 0, "getattr");
	marked.c_iflag ^= IXON;
	marked.c_cc[VEOF] = 7;
	marked.c_lflag |= ICANON | ECHO;
	T(tcsetattr(slave, TCSANOW, &marked) == 0, "mark");

	pid = fork();
	T(pid >= 0, "fork");
	if (pid == 0) {
		if (wd24x_tty_own_begin(&own, slave) != 0)
			_exit(2);
		if (tcgetattr(slave, &mid) != 0)
			_exit(3);
		if (mid.c_lflag & ICANON)
			_exit(4);
		if (mid.c_lflag & ECHO)
			_exit(5);
		if (mid.c_cc[VMIN] != 0 || mid.c_cc[VTIME] != 0)
			_exit(6);
		if (mid.c_cc[VEOF] != 7)
			_exit(7);
		if ((mid.c_iflag & IXON) != (marked.c_iflag & IXON))
			_exit(8);
		wd24x_tty_own_end(&own);
		wd24x_tty_own_end(&own);
		_exit(0);
	}
	T(waitpid(pid, &status, 0) == pid, "wait");
	T(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child restore");
	T(tcgetattr(slave, &after) == 0, "after");
	T(same_termios(&after, &marked), "termios match");

	pid = fork();
	if (pid == 0) {
		struct sigaction sa;
		struct pollfd p;

		if (wd24x_tty_own_begin(&own, slave) != 0)
			_exit(2);
		sa.sa_handler = child_on_sig;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sigaction(SIGTERM, &sa, NULL);
		p.fd = slave;
		p.events = POLLIN;
		for (;;) {
			if (poll(&p, 1, 5000) < 0 && errno != EINTR)
				_exit(3);
			if (child_sig) {
				wd24x_tty_own_end(&own);
				_exit(0);
			}
		}
	}
	usleep(100000);
	kill(pid, SIGTERM);
	T(waitpid(pid, &status, 0) == pid, "wait signal");
	T(WIFEXITED(status) && WEXITSTATUS(status) == 0, "signal child");
	T(tcgetattr(slave, &after) == 0, "after signal");
	T(same_termios(&after, &marked), "restored after signal");

	close(master);
	close(slave);
	printf("test_tty_own: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
