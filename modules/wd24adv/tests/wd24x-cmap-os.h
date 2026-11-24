/* SPDX-License-Identifier: GPL-2.0 */
#ifndef WD24X_CMAP_OS_H
#define WD24X_CMAP_OS_H

struct wd24x_os {
	int (*open)(const char *path, int flags);
	int (*close)(int fd);
	int (*ioctl)(int fd, unsigned long req, void *arg);
	unsigned (*sleep)(unsigned sec);
};

int wd24x_cmap_get_run(const struct wd24x_os *os);
int wd24x_cmap_set_run(const struct wd24x_os *os);

#endif
