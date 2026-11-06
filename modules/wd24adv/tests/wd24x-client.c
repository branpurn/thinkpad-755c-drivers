/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal X11 client for one display. Speaks the core protocol on the
 * local socket. No libX11. No fonts. No mouse. No StoreColors.
 *
 * Pixel values are colormap indices already loaded in the DAC. This
 * client does not change the palette.
 *
 * XGetImage reads the server pixmap. On this Xfbdev that pixmap is the
 * RAM shadow, not the banked VRAM. A matching readback proves the
 * protocol and the shadow. It does not prove the panel. Bank traffic
 * is the driver's fault and switch counters, read separately.
 *
 * Five 8x8 rectangles sit inside five different 64 KiB windows at
 * pitch 640, so a forward shadow copy switches window at most once
 * per rectangle. That stays under the default thrash bound of 32.
 *
 *   y=16  offset 10256  window 0  index 1
 *   y=120 offset 76816  window 1  index 2
 *   y=220 offset 140816 window 2  index 4
 *   y=320 offset 204816 window 3  index 14
 *   y=420 offset 268816 window 4  index 15
 *
 * alarm() ends the process if a reply never comes. Those waits are
 * interruptible. This does not claim to break a kernel mutex sleep.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define NRECT 5

static const struct {
	uint16_t x, y, w, h;
	uint8_t pix;
} rects[NRECT] = {
	{16, 16, 8, 8, 1},
	{16, 120, 8, 8, 2},
	{16, 220, 8, 8, 4},
	{16, 320, 8, 8, 14},
	{16, 420, 8, 8, 15},
};

static void on_alarm(int sig)
{
	(void)sig;
	fprintf(stderr, "wd24x-client: alarm\n");
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

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

/* Xauthority record, big-endian lengths. FamilyLocal is 256.
 * Use a MIT-MAGIC-COOKIE-1 for this display number when one exists.
 * No file means empty authorization, which is this server's local
 * default when it was not started with -auth. */
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

		if (i + 4 > n)
			break;
		i += 2; /* family */
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

static int handshake(int fd, const char *dpy, uint32_t *root, uint32_t *gc,
		     uint16_t *width, uint16_t *height, uint8_t *depth,
		     uint8_t *bpp, uint8_t *scan_pad)
{
	uint8_t cookie[256];
	int clen = 0;
	uint8_t hdr[12];
	uint8_t pref[8];
	uint8_t *body = NULL;
	uint32_t blen, off, vlen, nfmt, i;
	const char *aname = "MIT-MAGIC-COOKIE-1";
	uint16_t anlen = 18;

	load_cookie(dpy, cookie, &clen);
	memset(hdr, 0, sizeof hdr);
	hdr[0] = 'l';
	put16(hdr + 2, 11);
	put16(hdr + 4, 0);
	if (clen) {
		put16(hdr + 6, anlen);
		put16(hdr + 8, (uint16_t)clen);
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
		fprintf(stderr, "wd24x-client: setup refused %u\n", pref[0]);
		return -1;
	}
	blen = get16(pref + 6) * 4u;
	if (blen < 40 || blen > 1048576)
		return -1;
	body = malloc(blen);
	if (!body || rdn(fd, body, blen) < 0) {
		free(body);
		return -1;
	}
	vlen = get16(body + 16);
	nfmt = body[21];
	off = 32 + ((vlen + 3) & ~3u);
	*bpp = 0;
	*scan_pad = 32;
	for (i = 0; i < nfmt; i++) {
		if (off + 8 > blen)
			break;
		if (body[off] == 8) {
			*bpp = body[off + 1];
			*scan_pad = body[off + 4] ? body[off + 4] : 32;
		}
		off += 8;
	}
	if (off + 36 > blen) {
		free(body);
		return -1;
	}
	*root = get32(body + off);
	*width = get16(body + off + 20);
	*height = get16(body + off + 22);
	*depth = body[off + 38];
	*gc = get32(body + 4); /* resource-id base, first client id */
	free(body);
	return 0;
}

static int req(int fd, const uint8_t *p, size_t n)
{
	return wrn(fd, p, n);
}

static int create_gc(int fd, uint32_t gc, uint32_t root, uint32_t pix)
{
	uint8_t b[20];

	memset(b, 0, sizeof b);
	b[0] = 55;
	put16(b + 2, 5);
	put32(b + 4, gc);
	put32(b + 8, root);
	put32(b + 12, 4); /* GCForeground */
	put32(b + 16, pix);
	return req(fd, b, sizeof b);
}

static int change_fg(int fd, uint32_t gc, uint32_t pix)
{
	uint8_t b[16];

	memset(b, 0, sizeof b);
	b[0] = 56;
	put16(b + 2, 4);
	put32(b + 4, gc);
	put32(b + 8, 4);
	put32(b + 12, pix);
	return req(fd, b, sizeof b);
}

static int fill(int fd, uint32_t root, uint32_t gc, uint16_t x, uint16_t y,
		uint16_t w, uint16_t h)
{
	uint8_t b[20];

	memset(b, 0, sizeof b);
	b[0] = 70;
	put16(b + 2, 5);
	put32(b + 4, root);
	put32(b + 8, gc);
	put16(b + 12, x);
	put16(b + 14, y);
	put16(b + 16, w);
	put16(b + 18, h);
	return req(fd, b, sizeof b);
}

/* GetImage is the round trip. The reply bytes are the server pixmap. */
static int get_rect(int fd, uint32_t root, uint16_t seq, uint16_t x, uint16_t y,
		    uint16_t w, uint16_t h, uint8_t *out, size_t outlen,
		    unsigned stride)
{
	uint8_t b[20];
	uint8_t hdr[32];
	uint32_t nbytes;
	uint8_t *data;
	unsigned row;

	memset(b, 0, sizeof b);
	b[0] = 73;
	b[1] = 2; /* ZPixmap */
	put16(b + 2, 5);
	put32(b + 4, root);
	put16(b + 8, x);
	put16(b + 10, y);
	put16(b + 12, w);
	put16(b + 14, h);
	put32(b + 16, 0xffffffffu);
	if (req(fd, b, sizeof b) < 0)
		return -1;
	if (rdn(fd, hdr, sizeof hdr) < 0)
		return -1;
	if (hdr[0] == 0) {
		fprintf(stderr, "wd24x-client: GetImage error %u major %u\n",
			hdr[1], hdr[10]);
		return -1;
	}
	if (hdr[0] != 1 || get16(hdr + 2) != seq)
		return -1;
	nbytes = get32(hdr + 4) * 4u;
	data = malloc(nbytes ? nbytes : 1);
	if (!data || (nbytes && rdn(fd, data, nbytes) < 0)) {
		free(data);
		return -1;
	}
	for (row = 0; row < h; row++) {
		if ((row + 1) * stride > nbytes || row * w > outlen)
			break;
		memcpy(out + row * w, data + row * stride, w);
	}
	free(data);
	return (row == h) ? 0 : -1;
}

int main(int argc, char **argv)
{
	const char *spec = (argc > 1) ? argv[1] : ":1";
	const char *dpy = spec;
	int fd, i, matched = 0;
	uint32_t root = 0, gc = 0;
	uint16_t width = 0, height = 0, seq = 1;
	uint8_t depth = 0, bpp = 0, scan_pad = 32;
	unsigned stride;

	if (dpy[0] == ':')
		dpy++;
	if (!dpy[0] || strlen(dpy) > 8) {
		fprintf(stderr, "wd24x-client: bad display\n");
		return 1;
	}
	signal(SIGALRM, on_alarm);
	alarm(15);
	fd = connect_dpy(dpy);
	if (fd < 0) {
		fprintf(stderr, "wd24x-client: connect :%s: %s\n", dpy,
			strerror(errno));
		printf("WD24X-CLIENT bad connect\n");
		return 1;
	}
	if (handshake(fd, dpy, &root, &gc, &width, &height, &depth, &bpp,
		      &scan_pad) < 0) {
		printf("WD24X-CLIENT bad setup\n");
		close(fd);
		return 1;
	}
	if (width != 640 || height != 480 || depth != 8 || bpp != 8) {
		printf("WD24X-CLIENT bad geom %ux%ux%u bpp %u\n",
		       width, height, depth, bpp);
		close(fd);
		return 1;
	}
	stride = (8u * bpp + scan_pad - 1) / scan_pad * (scan_pad / 8);
	if (stride < 8)
		stride = 8;
	if (create_gc(fd, gc, root, rects[0].pix) < 0) {
		printf("WD24X-CLIENT bad gc\n");
		close(fd);
		return 1;
	}
	seq++;
	for (i = 0; i < NRECT; i++) {
		if (i && change_fg(fd, gc, rects[i].pix) < 0) {
			printf("WD24X-CLIENT bad fg\n");
			close(fd);
			return 1;
		}
		if (i)
			seq++;
		if (fill(fd, root, gc, rects[i].x, rects[i].y, rects[i].w,
			 rects[i].h) < 0) {
			printf("WD24X-CLIENT bad fill\n");
			close(fd);
			return 1;
		}
		seq++;
	}
	for (i = 0; i < NRECT; i++) {
		uint8_t pix[64];
		int x, y, ok = 1;

		memset(pix, 0, sizeof pix);
		if (get_rect(fd, root, seq, rects[i].x, rects[i].y, rects[i].w,
			     rects[i].h, pix, sizeof pix, stride) < 0) {
			printf("WD24X-CLIENT bad read %d\n", i);
			close(fd);
			return 1;
		}
		seq++;
		for (y = 0; y < 8; y++) {
			for (x = 0; x < 8; x++) {
				if (pix[y * 8 + x] != rects[i].pix)
					ok = 0;
			}
		}
		if (ok)
			matched++;
	}
	close(fd);
	alarm(0);
	if (matched != NRECT) {
		printf("WD24X-CLIENT shadow 640x480x8 rects %d match %d\n",
		       NRECT, matched);
		return 1;
	}
	printf("WD24X-CLIENT shadow 640x480x8 rects %d match %d\n",
	       NRECT, matched);
	return 0;
}
