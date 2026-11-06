/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Save the whole termios of a tty we opened, change only the flags
 * needed to read one byte at a time, and put the saved struct back.
 * KD mode is not touched. SIGKILL cannot run the restore.
 */
#ifndef WD24X_TTY_OWN_H
#define WD24X_TTY_OWN_H

#include <termios.h>

struct wd24x_tty_own {
	int fd;
	int owned;
	struct termios saved;
};

int wd24x_tty_own_begin(struct wd24x_tty_own *o, int fd);
void wd24x_tty_own_end(struct wd24x_tty_own *o);

#endif
