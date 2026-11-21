/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Linux 6.18.57 include/linux/fb.h: lock_fb_info() returns void.
 * The older helper returned int. This file is the call the module uses,
 * compiled against that void signature, plus a scan of the module source.
 */
#include <stdio.h>
#include <string.h>

struct fb_info {
	const void *fbops;
};

static inline void lock_fb_info(struct fb_info *info)
{
	(void)info;
}

static inline void unlock_fb_info(struct fb_info *info)
{
	(void)info;
}

static int cmap_lock_pattern(struct fb_info *info)
{
	if (!info)
		return -1;
	lock_fb_info(info);
	if (!info->fbops) {
		unlock_fb_info(info);
		return -1;
	}
	unlock_fb_info(info);
	return 0;
}

static int banned(const char *buf)
{
	static const char *const bad[] = {
		"!lock_fb_info",
		"= lock_fb_info",
		"if (lock_fb_info",
		NULL
	};
	unsigned i;

	for (i = 0; bad[i]; i++)
		if (strstr(buf, bad[i]))
			return 1;
	return 0;
}

int main(void)
{
	FILE *f;
	char buf[256 * 1024];
	size_t n;
	struct fb_info info;
	int fails = 0;

	info.fbops = &info;
	if (cmap_lock_pattern(&info) != 0 || cmap_lock_pattern(NULL) != -1) {
		printf("FAIL call pattern\n");
		fails++;
	}
	f = fopen("../wd24adv_main.c", "r");
	if (!f) {
		printf("FAIL open main\n");
		return 1;
	}
	n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	if (banned(buf)) {
		printf("FAIL lock_fb_info return used\n");
		fails++;
	}
	if (!strstr(buf, ".fb_ioctl")) {
		printf("FAIL fb_ioctl missing\n");
		fails++;
	}
	if (!strstr(buf, "WD24S6D_IOC_PRESENT")) {
		printf("FAIL present ioctl missing\n");
		fails++;
	}
	printf("test_fb_api_61857: %d failed\n", fails);
	return fails ? 1 : 0;
}
