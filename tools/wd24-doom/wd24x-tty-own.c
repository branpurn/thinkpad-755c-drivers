/* SPDX-License-Identifier: GPL-2.0 */
#include "wd24x-tty-own.h"

#include <unistd.h>

int wd24x_tty_own_begin(struct wd24x_tty_own *o, int fd)
{
	struct termios next;

	if (!o || fd < 0)
		return -1;
	if (tcgetattr(fd, &o->saved) != 0)
		return -1;
	next = o->saved;
	next.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
	next.c_cc[VMIN] = 0;
	next.c_cc[VTIME] = 0;
	if (tcsetattr(fd, TCSANOW, &next) != 0)
		return -1;
	o->fd = fd;
	o->owned = 1;
	return 0;
}

void wd24x_tty_own_end(struct wd24x_tty_own *o)
{
	if (!o || !o->owned)
		return;
	if (tcsetattr(o->fd, TCSANOW, &o->saved) != 0) {
		/* Still drop ownership so a later end does not loop. */
	}
	o->owned = 0;
}
