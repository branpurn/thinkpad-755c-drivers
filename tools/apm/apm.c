// SPDX-License-Identifier: GPL-2.0
/*
 * apm: APM BIOS status and suspend for tc32 on the ThinkPad 755C.
 *
 *   apm          battery / AC status from /proc/apm
 *   apm -m       same, remaining time in minutes only (for scripts)
 *   apm -s       suspend to RAM   (APM_IOC_SUSPEND on /dev/apm_bios)
 *   apm -S       standby          (APM_IOC_STANDBY)
 *   apm -v       APM driver and BIOS version
 *
 * The command set follows the classic apmd "apm" tool; this is a new,
 * minimal implementation against the kernel's documented /proc/apm format
 * (arch/x86/kernel/apm_32.c) and <linux/apm_bios.h> ioctls.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <linux/apm_bios.h>

#define PROC_APM "/proc/apm"
#define DEV_APM "/dev/apm_bios"

struct apm_st {
	char drv[16];
	int maj, min;
	unsigned flags, ac, bstat, bflag;
	int pct, time;
	char units[8];
};

static int read_apm(struct apm_st *s)
{
	FILE *f = fopen(PROC_APM, "r");
	int n;

	if (!f) {
		fprintf(stderr, "apm: %s: %s (no APM BIOS or CONFIG_APM off)\n",
			PROC_APM, strerror(errno));
		return -1;
	}
	n = fscanf(f, "%15s %d.%d %x %x %x %x %d%% %d %7s", s->drv, &s->maj,
		   &s->min, &s->flags, &s->ac, &s->bstat, &s->bflag, &s->pct,
		   &s->time, s->units);
	fclose(f);
	if (n != 10) {
		fprintf(stderr, "apm: cannot parse %s\n", PROC_APM);
		return -1;
	}
	return 0;
}

static const char *ac_str(unsigned ac)
{
	switch (ac) {
	case 0: return "off-line";
	case 1: return "on-line";
	case 2: return "on backup power";
	default: return "unknown";
	}
}

static const char *bat_str(unsigned b)
{
	switch (b) {
	case 0: return "high";
	case 1: return "low";
	case 2: return "critical";
	case 3: return "charging";
	case 4: return "not present";
	default: return "unknown";
	}
}

static int status(int minutes_only)
{
	struct apm_st s;
	int mins;

	if (read_apm(&s))
		return 1;
	mins = s.time < 0 ? -1 : (!strcmp(s.units, "sec") ? s.time / 60 : s.time);
	if (minutes_only) {
		printf("%d\n", mins);
		return 0;
	}
	printf("AC %s, battery %s", ac_str(s.ac), bat_str(s.bstat));
	if (s.pct >= 0)
		printf(", %d%%", s.pct);
	if (mins >= 0)
		printf(", %d:%02d left", mins / 60, mins % 60);
	putchar('\n');
	return 0;
}

static int version(void)
{
	struct apm_st s;

	if (read_apm(&s))
		return 1;
	printf("kernel APM driver %s, APM BIOS %d.%d (flags 0x%02x)\n", s.drv,
	       s.maj, s.min, s.flags);
	return 0;
}

static int sleep_req(unsigned long req, const char *what)
{
	int fd = open(DEV_APM, O_RDWR);

	/* no devtmpfs node (e.g. a static /dev): misc major 10, minor 134 */
	if (fd < 0 && errno == ENOENT &&
	    mknod(DEV_APM, S_IFCHR | 0600, makedev(10, 134)) == 0)
		fd = open(DEV_APM, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "apm: %s: %s\n", DEV_APM, strerror(errno));
		return 1;
	}
	sync();
	if (ioctl(fd, req) < 0) {
		fprintf(stderr, "apm: %s refused: %s\n", what, strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	printf("apm: resumed from %s\n", what);
	return 0;
}

static int usage(int rc)
{
	fputs("usage: apm [-m | -s | -S | -v]\n"
	      "  (none) AC/battery status   -m minutes left (-1 unknown)\n"
	      "  -s suspend to RAM   -S standby   -v versions\n",
	      rc ? stderr : stdout);
	return rc;
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return status(0);
	if (argc != 2 || argv[1][0] != '-' || !argv[1][1] || argv[1][2])
		return usage(2);
	switch (argv[1][1]) {
	case 'm': return status(1);
	case 's': return sleep_req(APM_IOC_SUSPEND, "suspend");
	case 'S': return sleep_req(APM_IOC_STANDBY, "standby");
	case 'v': return version();
	case 'h': return usage(0);
	default: return usage(2);
	}
}
