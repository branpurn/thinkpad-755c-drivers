/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Read-only VT snapshot. No KDSETMODE, no VT_ACTIVATE, no palette write.
 * KD mode, foreground VT, and vtcon bind are different facts.
 * A keystroke is none of these.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/kd.h>
#include <linux/vt.h>

int main(void)
{
	int fd, tty_ok = 0, mode = -1;
	struct vt_stat st;
	char name[128];
	char bind[32];
	int nname = -1, nbind = -1;
	int fn, fb;

	memset(&st, 0, sizeof st);
	fd = open("/dev/tty0", O_RDONLY);
	if (fd >= 0) {
		tty_ok = 1;
		if (ioctl(fd, KDGETMODE, &mode) != 0)
			mode = -1;
		if (ioctl(fd, VT_GETSTATE, &st) != 0)
			memset(&st, 0, sizeof st);
		close(fd);
	}
	fn = open("/sys/class/vtconsole/vtcon0/name", O_RDONLY);
	if (fn >= 0) {
		nname = (int)read(fn, name, sizeof name - 1);
		if (nname < 0)
			nname = 0;
		name[nname] = 0;
		close(fn);
		{
			char *s = name;
			char *w = name;
			for (; *s; s++) {
				if (*s == '\n' || *s == '\r')
					continue;
				*w++ = *s;
			}
			*w = 0;
		}
	} else {
		name[0] = 0;
	}
	fb = open("/sys/class/vtconsole/vtcon0/bind", O_RDONLY);
	if (fb >= 0) {
		nbind = (int)read(fb, bind, sizeof bind - 1);
		if (nbind < 0)
			nbind = 0;
		bind[nbind] = 0;
		close(fb);
		{
			char *s = bind;
			char *w = bind;
			for (; *s; s++) {
				if (*s == '\n' || *s == '\r')
					continue;
				*w++ = *s;
			}
			*w = 0;
		}
	} else {
		bind[0] = 0;
	}
	if (!tty_ok && nname < 0) {
		printf("WD24X-TTY-READ bad open\n");
		return 1;
	}
	printf("WD24X-TTY-READ kd %d fg %d name %s bind %s\n",
	       mode, st.v_active, name, bind);
	return 0;
}
