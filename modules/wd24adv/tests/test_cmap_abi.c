/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Host stand-in for the phase 2 and phase 3 clients.
 * The DAC is vgamodel. The copy rules are fb_cmap_to_user and
 * fb_copy_cmap from Linux 6.18.57. This is not the 755C.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/fb.h>
#include "vgamodel.h"
#include "wd24x-cmap-check.h"
#include "wd24x-cmap-os.h"
#include "../wd24adv_dac_entry.h"

static int fails, passes;
#define T(c, w) do { \
	if (c) passes++; \
	else { fails++; fprintf(stderr, "FAIL %s\n", w); } \
} while (0)

static struct vgamodel vm;
static struct wd24_io vio;
static u8 snap[768];
static uint16_t cache_r[256], cache_g[256], cache_b[256];
static unsigned n3c9, n3c6_read, nput, nvs, nget, nclose, nsleep;
static unsigned published, frozen;
static int short_copy, fail_open, put_err;
static int out_fd = -1, saved_fd = -1;

static void wrap_out(void *p, u8 v, u16 port)
{
	if (port == 0x3c9)
		n3c9++;
	vio.outb(p, v, port);
}

static u8 wrap_in(void *p, u16 port)
{
	if (port == 0x3c6)
		n3c6_read++;
	return vio.inb(p, port);
}

static void publish_read(void)
{
	struct wd24_dac_io dio;
	unsigned i;
	unsigned before = n3c9;

	dio.outb = wrap_out;
	dio.inb = wrap_in;
	dio.p = vio.priv;
	dio.outb(dio.p, 0xff, WD24_DAC_PEL);
	dio.outb(dio.p, 0x00, WD24_DAC_RI);
	for (i = 0; i < 768; i++)
		snap[i] = dio.inb(dio.p, WD24_DAC_D);
	T(n3c9 == before, "publish writes no 3C9");
	T(n3c6_read == 0, "publish does not read 3C6");
	wd24_dac_expand_image(snap, cache_r, cache_g, cache_b);
	published = 1;
	frozen = 0;
}

static int copy_user(uint32_t start, uint32_t len,
		     uint16_t *r, uint16_t *g, uint16_t *b)
{
	unsigned int tooff = 0, fromoff = 0;
	size_t n;
	uint32_t from_len = short_copy ? 16 : 256;

	if (start > 0)
		fromoff = start;
	else
		tooff = 0;
	if (fromoff >= from_len || tooff >= len || !from_len || !len)
		return -1;
	n = len - tooff;
	if (from_len - fromoff < n)
		n = from_len - fromoff;
	memcpy(r + tooff, cache_r + fromoff, n * 2);
	memcpy(g + tooff, cache_g + fromoff, n * 2);
	memcpy(b + tooff, cache_b + fromoff, n * 2);
	return 0;
}

static int mock_open(const char *path, int flags)
{
	if (fail_open || !path || strcmp(path, "/dev/fb0") != 0)
		return -1;
	if ((flags & O_RDWR) != O_RDWR)
		return -1;
	publish_read();
	return 3;
}

static int mock_close(int fd)
{
	unsigned at = n3c9;

	(void)fd;
	nclose++;
	frozen = 1;
	T(n3c9 == at, "close writes no 3C9");
	T(published == 1, "close leaves published");
	return 0;
}

static unsigned mock_sleep(unsigned sec)
{
	nsleep++;
	T(sec == 8, "client holds the fd for 8s");
	return 0;
}

static int mock_ioctl(int fd, unsigned long req, void *arg)
{
	struct fb_cmap *c = arg;
	struct wd24_dac_io dio;
	struct wd24_dac_entry ent;
	int err;

	(void)fd;
	if (req == FBIOPUT_VSCREENINFO || req == FBIOGET_VSCREENINFO) {
		nvs++;
		return -1;
	}
	if (req == FBIOGETCMAP) {
		nget++;
		if (!c || c->len != 256 || c->start != 0 || !c->red)
			return -1;
		return copy_user(c->start, c->len, c->red, c->green, c->blue);
	}
	if (req == FBIOPUTCMAP) {
		nput++;
		if (!c || c->start != WD24X_CMAP_SET_INDEX || c->len != 1 ||
		    !c->red || !c->green || !c->blue)
			return -1;
		if (put_err)
			return -1;
		dio.outb = wrap_out;
		dio.inb = wrap_in;
		dio.p = vio.priv;
		err = wd24_dac_entry(c->start, c->red[0], c->green[0],
				     c->blue[0], &ent);
		if (err)
			return -1;
		wd24_dac_mask_ff(&dio);
		err = wd24_dac_put_entry(&dio, ent.index, ent.rgb[0],
					 ent.rgb[1], ent.rgb[2]);
		if (err)
			return -1;
		cache_r[ent.index] = c->red[0];
		cache_g[ent.index] = c->green[0];
		cache_b[ent.index] = c->blue[0];
		return 0;
	}
	nvs++;
	return -1;
}

static const struct wd24x_os os = {
	mock_open, mock_close, mock_ioctl, mock_sleep
};

static void grab(void)
{
	fflush(stdout);
	saved_fd = dup(1);
	out_fd = open("/tmp/wd24-cmap-abi.out", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	dup2(out_fd, 1);
}

static int grabbed(const char *want)
{
	char buf[256];
	ssize_t n;

	fflush(stdout);
	dup2(saved_fd, 1);
	close(saved_fd);
	close(out_fd);
	saved_fd = out_fd = -1;
	n = 0;
	{
		int fd = open("/tmp/wd24-cmap-abi.out", O_RDONLY);
		if (fd >= 0) {
			n = read(fd, buf, sizeof buf - 1);
			close(fd);
		}
	}
	if (n < 0)
		n = 0;
	buf[n] = 0;
	return want && strstr(buf, want) != NULL;
}

static void reset_counts(void)
{
	n3c9 = n3c6_read = nput = nvs = nget = nclose = nsleep = 0;
	short_copy = fail_open = put_err = 0;
	published = frozen = 0;
}

static void load_dac(void)
{
	unsigned i;

	vm_init(&vm, VM_WD24, 1);
	for (i = 0; i < 768; i++)
		vm.dac[i] = (u8)((i * 5 + 3) & 0x3f);
	vm_io(&vio, &vm);
}

static void full_restore(void)
{
	struct wd24_dac_io dio;
	u8 back[768];
	unsigned i, bad = 0;
	unsigned writes;

	dio.outb = wrap_out;
	dio.inb = wrap_in;
	dio.p = vio.priv;
	writes = n3c9;
	dio.outb(dio.p, 0xff, WD24_DAC_PEL);
	dio.outb(dio.p, 0x00, WD24_DAC_WI);
	for (i = 0; i < 768; i++)
		dio.outb(dio.p, snap[i], WD24_DAC_D);
	T(n3c9 - writes == 768, "rollback writes 768");
	dio.outb(dio.p, 0xff, WD24_DAC_PEL);
	dio.outb(dio.p, 0x00, WD24_DAC_RI);
	for (i = 0; i < 768; i++)
		back[i] = dio.inb(dio.p, WD24_DAC_D);
	for (i = 0; i < 768; i++)
		bad += ((back[i] ^ snap[i]) & 0x3f) != 0;
	T(bad == 0, "rollback readback matches snapshot");
	wd24_dac_expand_image(snap, cache_r, cache_g, cache_b);
	T(published == 1, "expiry leaves published 1");
	T(cache_r[7] != WD24X_CMAP_SET_VALUE, "index 7 cache restored");
}

int main(void)
{
	unsigned i, changed;
	struct wd24_cmap_fault f;
	struct wd24_cmap_view view;

	load_dac();
	reset_counts();
	T(((7 * 3 * 5 + 3) & 0x3f) != 0x20, "fixture index 7 is not the put");
	grab();
	T(wd24x_cmap_get_run(&os) == 0, "phase 2 run");
	T(grabbed("WD24X-CMAP-GET entries 256 shift10 1"), "phase 2 line");
	T(nput == 0 && nvs == 0, "phase 2 no put and no modeset");
	T(nget == 1 && nclose == 1 && nsleep == 1, "phase 2 one get");
	T(n3c9 == 0, "phase 2 no colour write");
	T(vm.violations == 0, "phase 2 no 3C6 read");
	for (i = 0; i < 256; i++) {
		if (cache_r[i] != (uint16_t)((snap[i * 3] & 0x3f) << 10))
			break;
	}
	T(i == 256, "phase 2 cache is the snapshot");

	load_dac();
	reset_counts();
	short_copy = 1;
	grab();
	T(wd24x_cmap_get_run(&os) != 0, "short copy fails");
	T(grabbed("WD24X-CMAP-GET bad get"), "short copy line");
	T(n3c9 == 0, "short copy writes nothing");

	load_dac();
	reset_counts();
	fail_open = 1;
	grab();
	T(wd24x_cmap_get_run(&os) != 0, "bad open");
	T(grabbed("WD24X-CMAP-GET bad open"), "bad open line");
	T(nclose == 0 && n3c9 == 0, "failed open does not close or write");

	load_dac();
	reset_counts();
	grab();
	T(wd24x_cmap_set_run(&os) == 0, "phase 3 run");
	T(grabbed("WD24X-CMAP-SET index 7 put 1 cache 1"), "phase 3 line");
	T(nput == 1 && nget == 2 && nvs == 0, "phase 3 one put");
	T(n3c9 == 3, "phase 3 writes three 3C9 bytes");
	changed = 0;
	for (i = 0; i < 768; i++) {
		if ((vm.dac[i] & 0x3f) != (snap[i] & 0x3f))
			changed++;
	}
	T(changed == 3, "only index 7 changed in the DAC");
	T(vm.dac[21] == 0x20 && vm.dac[22] == 0x20 && vm.dac[23] == 0x20,
	  "index 7 is 0x20");
	T(cache_r[7] == WD24X_CMAP_SET_VALUE && cache_r[6] != WD24X_CMAP_SET_VALUE,
	  "cache keeps neighbours");
	full_restore();
	T(n3c6_read == 0 && vm.violations == 0, "no 3C6 read on the set path");

	load_dac();
	reset_counts();
	put_err = 1;
	grab();
	T(wd24x_cmap_set_run(&os) != 0, "put error");
	T(grabbed("WD24X-CMAP-SET bad put"), "put error line");
	T(n3c9 == 0, "rejected put writes no 3C9");

	memset(&view, 0, sizeof view);
	view.param = view.live = view.fbcon = view.armed = view.active = 1;
	view.busy = view.have_cmap = view.published = 1;
	T(wd24_cmap_allow(&view) == 0, "allow when published");
	view.published = 0;
	T(wd24_cmap_allow(&view) != 0, "refuse when unpublished");
	view.frozen = 1;
	view.published = 1;
	T(wd24_cmap_allow(&view) != 0, "refuse when frozen");
	T(wd24_cmap_before_read(1, 0, 1, 0, 0) != 0, "no second read when frozen");
	f = wd24_cmap_settle(0, 0, 1);
	T(f.published == 1 && f.err == 0, "successful put stays published");
	f = wd24_cmap_settle(-5, 0, 1);
	T(f.err == -5 && f.rewrite == 1 && f.published == 1,
	  "failed put restores and stays published");
	f = wd24_cmap_read_mismatch();
	T(f.invalidate == 1 && f.published == 0 && f.latch == 1,
	  "publish mismatch does not write");

	printf("test_cmap_abi: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}
