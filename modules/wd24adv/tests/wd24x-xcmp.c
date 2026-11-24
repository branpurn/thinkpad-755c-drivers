/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Reads X's default colormap with QueryColors and prints the >> 10
 * channels. Does not open /dev/fb0, does not StoreColors, does not
 * PutImage. A second fb open while X is mapped is refused, and the
 * close would drop X's pages, so the DAC compare is the driver's
 * published cache against this list.
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "wd24x-xproto.h"

static void on_alarm(int sig)
{
	(void)sig;
	fprintf(stderr, "wd24x-xcmp: alarm\n");
	_exit(2);
}

static int rdn(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;

	while (n) {
		ssize_t r = read(fd, p, n);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			return -1;
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

static int wrn(int fd, const void *buf, size_t n)
{
	const uint8_t *p = buf;

	while (n) {
		ssize_t r = write(fd, p, n);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static int load_cookie(const char *dpy, uint8_t *cookie, int *clen)
{
	const char *path = getenv("XAUTHORITY");
	char homeauth[256];
	uint8_t *buf;
	size_t n, i;
	FILE *f;
	const char *home;

	*clen = 0;
	if (!path || !path[0]) {
		home = getenv("HOME");
		if (!home)
			return 0;
		snprintf(homeauth, sizeof homeauth, "%s/.Xauthority", home);
		path = homeauth;
	}
	f = fopen(path, "rb");
	if (!f)
		return 0;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return 0;
	}
	n = (size_t)ftell(f);
	rewind(f);
	if (n == 0 || n > 65536) {
		fclose(f);
		return 0;
	}
	buf = malloc(n);
	if (!buf || fread(buf, 1, n, f) != n) {
		free(buf);
		fclose(f);
		return 0;
	}
	fclose(f);
	i = 0;
	while (i + 4 <= n) {
		uint16_t alen, nlen, namelen, dlen;
		const uint8_t *num, *name, *data;

		i += 2;
		alen = be16(buf + i);
		i += 2;
		if (i + alen + 2 > n)
			break;
		i += alen;
		nlen = be16(buf + i);
		i += 2;
		if (i + nlen + 2 > n)
			break;
		num = buf + i;
		i += nlen;
		namelen = be16(buf + i);
		i += 2;
		if (i + namelen + 2 > n)
			break;
		name = buf + i;
		i += namelen;
		dlen = be16(buf + i);
		i += 2;
		if (i + dlen > n)
			break;
		data = buf + i;
		i += dlen;
		if (nlen == strlen(dpy) && memcmp(num, dpy, nlen) == 0 &&
		    namelen == 18 && memcmp(name, "MIT-MAGIC-COOKIE-1", 18) == 0 &&
		    dlen > 0 && dlen <= 256) {
			memcpy(cookie, data, dlen);
			*clen = (int)dlen;
			free(buf);
			return 0;
		}
	}
	free(buf);
	return 0;
}

static int connect_dpy(const char *dpy)
{
	struct sockaddr_un un;
	int fd;
	char path[sizeof un.sun_path];

	snprintf(path, sizeof path, "/tmp/.X11-unix/X%s", dpy);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&un, 0, sizeof un);
	un.sun_family = AF_UNIX;
	memcpy(un.sun_path, path, strlen(path) + 1);
	if (connect(fd, (struct sockaddr *)&un, sizeof un) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int handshake(int fd, const char *dpy, uint32_t *cmap)
{
	uint8_t cookie[256];
	int clen = 0;
	uint8_t hdr[12];
	uint8_t pref[8];
	uint8_t *body = NULL;
	uint32_t blen, root;
	uint16_t width, height;
	uint8_t depth;
	const char *aname = "MIT-MAGIC-COOKIE-1";
	uint16_t anlen = 18;

	load_cookie(dpy, cookie, &clen);
	memset(hdr, 0, sizeof hdr);
	hdr[0] = 'l';
	wd24x_xp_put16(hdr + 2, 11);
	if (clen) {
		wd24x_xp_put16(hdr + 6, anlen);
		wd24x_xp_put16(hdr + 8, (uint16_t)clen);
	}
	if (wrn(fd, hdr, sizeof hdr) < 0)
		return -1;
	if (clen) {
		uint8_t pad[3] = {0, 0, 0};
		int np = (4 - (anlen & 3)) & 3;
		int dp = (4 - (clen & 3)) & 3;

		if (wrn(fd, aname, anlen) < 0 || (np && wrn(fd, pad, (size_t)np) < 0))
			return -1;
		if (wrn(fd, cookie, (size_t)clen) < 0 ||
		    (dp && wrn(fd, pad, (size_t)dp) < 0))
			return -1;
	}
	if (rdn(fd, pref, 8) < 0)
		return -1;
	if (pref[0] != 1) {
		fprintf(stderr, "wd24x-xcmp: setup refused %u\n", pref[0]);
		return -1;
	}
	blen = wd24x_xp_get16(pref + 6) * 4u;
	if (blen < 40 || blen > 1048576)
		return -1;
	body = malloc(blen);
	if (!body || rdn(fd, body, blen) < 0) {
		free(body);
		return -1;
	}
	if (wd24x_xsetup_screen(body, blen, &root, cmap, &width, &height,
				&depth) < 0) {
		free(body);
		return -1;
	}
	free(body);
	if (width != 640 || height != 480 || depth != 8)
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	const char *spec = (argc > 1) ? argv[1] : ":1";
	const char *dpy = spec;
	uint8_t req[12 + 256 * 4];
	uint8_t hdr[32];
	uint8_t *data = NULL;
	unsigned r[256], g[256], b[256], low = 0;
	uint32_t cmap = 0, nbytes;
	int fd, n, i;

	if (dpy[0] == ':')
		dpy++;
	if (!dpy[0] || strlen(dpy) > 8) {
		fprintf(stderr, "wd24x-xcmp: bad display\n");
		return 1;
	}
	signal(SIGALRM, on_alarm);
	alarm(15);
	fd = connect_dpy(dpy);
	if (fd < 0) {
		printf("WD24X-XCMAP bad connect\n");
		return 1;
	}
	if (handshake(fd, dpy, &cmap) < 0) {
		printf("WD24X-XCMAP bad setup\n");
		close(fd);
		return 1;
	}
	n = wd24x_xquery_colors(req, sizeof req, cmap, 256);
	if (n < 0 || wrn(fd, req, (size_t)n) < 0 || rdn(fd, hdr, sizeof hdr) < 0) {
		printf("WD24X-XCMAP bad query\n");
		close(fd);
		return 1;
	}
	if (hdr[0] == 0) {
		fprintf(stderr, "wd24x-xcmp: QueryColors error %u\n", hdr[1]);
		printf("WD24X-XCMAP bad query\n");
		close(fd);
		return 1;
	}
	nbytes = wd24x_xp_get32(hdr + 4) * 4u;
	if (nbytes > 65536) {
		printf("WD24X-XCMAP bad query\n");
		close(fd);
		return 1;
	}
	data = malloc(nbytes ? nbytes : 1);
	if (!data || (nbytes && rdn(fd, data, nbytes) < 0)) {
		free(data);
		printf("WD24X-XCMAP bad query\n");
		close(fd);
		return 1;
	}
	close(fd);
	{
		uint8_t *full = malloc(32u + nbytes);

		if (!full) {
			free(data);
			return 1;
		}
		memcpy(full, hdr, 32);
		if (nbytes)
			memcpy(full + 32, data, nbytes);
		if (wd24x_xcolors_parse(full, 32u + nbytes, 256, r, g, b, &low) < 0) {
			free(full);
			free(data);
			printf("WD24X-XCMAP bad query\n");
			return 1;
		}
		free(full);
	}
	free(data);
	alarm(0);
	printf("WD24X-XCMAP entries 256 shift10 1\n");
	for (i = 0; i < 256; i++)
		printf("%u %u %u %u\n", i, r[i], g[i], b[i]);
	if (low)
		fprintf(stderr, "wd24x-xcmp: lowbits %u\n", low);
	return 0;
}
