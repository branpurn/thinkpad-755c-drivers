/* SPDX-License-Identifier: GPL-2.0 */
/* Drive the 13h model through the validation scenes and optional video. */
#include "wd24_chip.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "../../modules/wd24adv/wd24adv_ioctl.h"

#define W 640
#define H 480
#define FRAME (W * H * 3)

static int g_fail;
static int g_pass;
static unsigned char rgb[FRAME];
static unsigned char page[64000];

static void val(const char *name, int ok, const char *ev)
{
	printf("VAL %s %s %s\n", name, ok ? "PASS" : "FAIL", ev);
	if (ok)
		g_pass++;
	else
		g_fail++;
}

static void skip(const char *name, const char *ev)
{
	printf("VAL %s SKIP %s\n", name, ev);
}

static const char *DIG[10] = {
	"111101101101111",
	"010010010010111",
	"111001111100111",
	"111001111001111",
	"101101111001001",
	"111100111001111",
	"111100111101111",
	"111001001001001",
	"111101111101111",
	"111101111001111"
};

static void draw_digit(unsigned char *buf, unsigned w, unsigned h,
		       unsigned x, unsigned y, unsigned scale,
		       unsigned digit, unsigned ink)
{
	unsigned row, col, sy, sx;
	const char *g;

	if (digit > 9)
		return;
	g = DIG[digit];
	for (row = 0; row < 5; row++) {
		for (col = 0; col < 3; col++) {
			if (g[row * 3 + col] != '1')
				continue;
			for (sy = 0; sy < scale; sy++) {
				for (sx = 0; sx < scale; sx++) {
					unsigned px = x + col * scale + sx;
					unsigned py = y + row * scale + sy;

					if (px < w && py < h)
						buf[py * w + px] = (unsigned char)ink;
				}
			}
		}
	}
}

static void burn_rgb_digit(unsigned char *p, unsigned x, unsigned y,
			   unsigned scale, unsigned digit)
{
	unsigned row, col, sy, sx;
	const char *g = DIG[digit % 10];

	for (row = 0; row < 5; row++) {
		for (col = 0; col < 3; col++) {
			if (g[row * 3 + col] != '1')
				continue;
			for (sy = 0; sy < scale; sy++) {
				for (sx = 0; sx < scale; sx++) {
					unsigned px = x + col * scale + sx;
					unsigned py = y + row * scale + sy;
					unsigned o;

					if (px >= W || py >= H)
						continue;
					o = (py * W + px) * 3u;
					p[o] = p[o + 1] = p[o + 2] = 255;
				}
			}
		}
	}
}

static void burn_counter(unsigned char *p, unsigned n)
{
	char txt[16];
	unsigned i, x;
	int len;

	len = snprintf(txt, sizeof(txt), "%u", n);
	if (len < 0)
		return;
	for (i = 0, x = 8; i < (unsigned)len; i++, x += 10)
		burn_rgb_digit(p, x, 6, 2, (unsigned)(txt[i] - '0'));
}

static unsigned fnv(const unsigned char *p, unsigned n)
{
	unsigned h = 2166136261u, i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

static unsigned gaps[64];
static unsigned ngaps;

/* Intervals are field periods already measured. Count those above
 * twice the median. An empty list is not a pass.
 */
static unsigned count_over2x(const unsigned *v, unsigned n)
{
	unsigned tmp[64];
	unsigned i, j, med, over = 0;

	if (!n || n > 64)
		return ~0u;
	for (i = 0; i < n; i++)
		tmp[i] = v[i];
	for (i = 1; i < n; i++) {
		unsigned k = tmp[i];

		for (j = i; j > 0 && tmp[j - 1] > k; j--)
			tmp[j] = tmp[j - 1];
		tmp[j] = k;
	}
	med = tmp[(n - 1) / 2];
	for (i = 0; i < n; i++)
		if (v[i] > med * 2u)
			over++;
	return over;
}

static void set_dac(struct wd24_chip *c, unsigned i, unsigned r,
		    unsigned g, unsigned b)
{
	wd24_chip_outb(c, 0x3c8, i);
	wd24_chip_outb(c, 0x3c9, r);
	wd24_chip_outb(c, 0x3c9, g);
	wd24_chip_outb(c, 0x3c9, b);
}

static int bank(struct wd24_chip *c, unsigned pr)
{
	if (wd24_chip_outb(c, 0x3ce, 0x0f) || wd24_chip_outb(c, 0x3cf, 0x05))
		return -1;
	if (wd24_chip_outb(c, 0x3ce, 0x09) || wd24_chip_outb(c, 0x3cf, pr))
		return -1;
	return 0;
}

static int arm_start(struct wd24_chip *c, unsigned cr0c)
{
	if (wd24_chip_outb(c, 0x3d4, 0x0c) || wd24_chip_outb(c, 0x3d5, cr0c))
		return -1;
	if (wd24_chip_outb(c, 0x3d4, 0x0d) || wd24_chip_outb(c, 0x3d5, 0x00))
		return -1;
	return wd24_chip_wait_latch(c) ? 0 : -1;
}

static void sample_add(FILE *sf, unsigned n, unsigned x, unsigned y,
		       int solid, const char *phase)
{
	unsigned o;

	if (!sf)
		return;
	o = (y * W + x) * 3u;
	fprintf(sf, "%u %u %u %u %u %u %d %s\n", n, x, y,
		rgb[o], rgb[o + 1], rgb[o + 2], solid, phase);
}

static int emit(struct wd24_chip *c, FILE *raw, FILE *sf, const char *ppm_dir,
		unsigned n, int solid, const char *phase, unsigned every)
{
	char path[256];
	FILE *fp;

	if (wd24_chip_render(c, rgb))
		return -1;
	if (ngaps < 64) {
		unsigned mhz = wd24_chip_field_millihz(c);

		if (mhz)
			gaps[ngaps++] = (unsigned)(1000000000ull / mhz);
	}
	burn_counter(rgb, n);
	if (raw && fwrite(rgb, 1, FRAME, raw) != FRAME)
		return -1;
	if (every > 1 && (n % every) != 0)
		return 0;
	sample_add(sf, n, 600, 48, solid, phase);
	sample_add(sf, n, 600, 400, solid, phase);
	if (!ppm_dir)
		return 0;
	snprintf(path, sizeof(path), "%s/f%05u.ppm", ppm_dir, n);
	fp = fopen(path, "wb");
	if (!fp)
		return -1;
	fprintf(fp, "P6\n%d %d\n255\n", W, H);
	if (fwrite(rgb, 1, FRAME, fp) != FRAME) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	return 0;
}

static int self_median(void)
{
	unsigned spike[4] = { 10, 10, 10, 40 };
	unsigned flat[4] = { 10, 10, 10, 10 };

	return count_over2x(spike, 4) == 1 && count_over2x(flat, 4) == 0 &&
	       count_over2x(flat, 0) == ~0u;
}

/* 1 = saw 3DA bit 3 low then high. 0 = timed out. -1 = no port. */
static int retrace_edge(void)
{
	static int pfd = -2;
	int i, saw_low = 0;

	if (pfd == -2)
		pfd = open("/dev/port", O_RDONLY);
	if (pfd < 0)
		return -1;
	for (i = 0; i < 2000000; i++) {
		unsigned char b;

		if (pread(pfd, &b, 1, 0x3da) != 1)
			return -1;
		if (b & 0x08) {
			if (saw_low)
				return 1;
		} else {
			saw_low = 1;
		}
	}
	return 0;
}

static unsigned window_bad(const unsigned char *win, const unsigned char *src)
{
	unsigned i, bad = 0;

	for (i = 0; i < 64000; i++)
		if (win[i] != src[i])
			bad++;
	return bad;
}

static int device_run(void)
{
	int fd, page_i, edge, cards_bad = 0, strobe_bad = 0, strobe_skip = 0;
	unsigned char *win;
	unsigned char src[64000];
	struct wd24s6d_flip fr;
	const unsigned ink[3] = { 1, 2, 4 };
	const unsigned cr0c[3] = { 0x00, 0x40, 0x80 };
	unsigned shown = 0, fields = 0, card_rb = 0, strobe_rb = 0;
	char ev[160];

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		val("page0", 0, "no-fb");
		val("page1", 0, "no-fb");
		val("page2", 0, "no-fb");
		val("markers", 0, "no-fb");
		val("strobe", 0, "no-fb");
		return 1;
	}
	win = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (win == MAP_FAILED) {
		val("page0", 0, "mmap");
		close(fd);
		return 1;
	}
	for (page_i = 0; page_i < 3; page_i++) {
		unsigned bad;
		const char *name = page_i == 0 ? "page0" : page_i == 1 ? "page1" : "page2";

		memset(src, (int)ink[page_i], 64000);
		memcpy(win, src, 64000);
		bad = window_bad(win, src);
		memset(&fr, 0, sizeof(fr));
		if (ioctl(fd, WD24S6D_IOC_FLIP, &fr) != 0) {
			snprintf(ev, sizeof(ev), "sum=%u flip=refused", bad);
			val(name, 0, ev);
			continue;
		}
		edge = retrace_edge();
		snprintf(ev, sizeof(ev),
			 "cr0c=%02x cr0d=%02x pr0a=%02x edges=%d sum=%u",
			 fr.cr0c, fr.cr0d, fr.pr0a, edge, bad);
		if (bad || fr.cr0c != cr0c[shown % 3] || fr.cr0d != 0) {
			val(name, 0, ev);
		} else if (edge == 1) {
			val(name, 1, ev);
		} else if (edge < 0) {
			skip(name, ev);
		} else {
			val(name, 0, ev);
		}
		shown++;
	}
	for (page_i = 0; page_i < 5; page_i++) {
		unsigned color = page_i == 4 ? 63u : ink[page_i % 3];
		unsigned bad;

		memset(src, (int)color, 64000);
		draw_digit(src, 320, 200, 148, 80, 8, (unsigned)page_i + 1, 0);
		memcpy(win, src, 64000);
		bad = window_bad(win, src);
		card_rb += bad;
		if (bad)
			cards_bad++;
		memset(&fr, 0, sizeof(fr));
		if (ioctl(fd, WD24S6D_IOC_FLIP, &fr) != 0) {
			val("markers", 0, "flip");
			cards_bad = -1;
			break;
		}
		sleep(2);
	}
	if (cards_bad >= 0) {
		snprintf(ev, sizeof(ev), "cards=5 readback=%u", card_rb);
		val("markers", card_rb == 0, ev);
	}
	edge = retrace_edge();
	if (edge < 0) {
		skip("strobe", "no-3da");
		strobe_skip = 1;
	}
	if (!strobe_skip) {
		for (page_i = 0; page_i < 6; page_i++) {
			unsigned pi = (unsigned)page_i % 3;
			unsigned bad, n;

			memset(src, (int)ink[pi], 64000);
			draw_digit(src, 320, 200, 8, 8, 4, pi, 63);
			memcpy(win, src, 64000);
			bad = window_bad(win, src);
			strobe_rb += bad;
			if (bad)
				strobe_bad++;
			memset(&fr, 0, sizeof(fr));
			if (ioctl(fd, WD24S6D_IOC_FLIP, &fr) != 0 ||
			    fr.cr0c != cr0c[shown % 3]) {
				strobe_bad++;
				break;
			}
			shown++;
			for (n = 0; n < 35; n++) {
				if (retrace_edge() != 1) {
					strobe_bad++;
					break;
				}
				fields++;
			}
		}
		snprintf(ev, sizeof(ev),
			 "holds=6 fields=%u readback=%u", fields, strobe_rb);
		val("strobe", strobe_bad == 0 && fields == 35u * 6u, ev);
	}
	munmap(win, 65536);
	close(fd);
	return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
	struct wd24_chip *c;
	unsigned hold = 2, strobe_cycles = 1, field = 1, every = 1;
	unsigned page_i, cyc, f, src_h, win_h, bad, i;
	int full = 0;
	const char *mp4 = NULL, *ppm = NULL, *samples = NULL;
	char ev[160];
	FILE *raw = NULL, *sf = NULL;
	const unsigned cr0c[3] = { 0x00, 0x40, 0x80 };
	const unsigned pr[3] = { 0x00, 0x10, 0x20 };
	const unsigned ink[3] = { 1, 2, 4 };
	const unsigned card_ink[5] = { 1, 2, 4, 14, 63 };
	unsigned long before;

	for (i = 1; (int)i < argc; i++) {
		if (!strcmp(argv[i], "--full"))
			full = 1;
		else if (!strcmp(argv[i], "--device"))
			return device_run();
		else if (!strcmp(argv[i], "--self")) {
			if (!self_median())
				return 1;
			printf("wd24_run self ok\n");
			return 0;
		} else if (!strcmp(argv[i], "--mp4") && (int)i + 1 < argc)
			mp4 = argv[++i];
		else if (!strcmp(argv[i], "--ppm") && (int)i + 1 < argc)
			ppm = argv[++i];
		else if (!strcmp(argv[i], "--samples") && (int)i + 1 < argc)
			samples = argv[++i];
		else {
			fprintf(stderr, "usage: wd24_run [--full] [--self] [--mp4 f] [--ppm dir] [--samples f]\n");
			return 2;
		}
	}
	if (full) {
		hold = 35;
		strobe_cycles = 2;
		every = 35;
	}
	c = wd24_chip_new();
	if (!c || wd24_chip_program_13h(c)) {
		val("model", 0, "program");
		goto done;
	}
	set_dac(c, 0, 0, 0, 0);
	set_dac(c, 1, 63, 0, 0);
	set_dac(c, 2, 0, 63, 0);
	set_dac(c, 4, 0, 0, 63);
	set_dac(c, 14, 63, 63, 0);
	set_dac(c, 32, 63, 32, 0);
	set_dac(c, 63, 63, 63, 63);

	if (mp4) {
		char cmd[512];

		snprintf(cmd, sizeof(cmd),
			 "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s 640x480 -r 70086/1000 -i - -c:v libx264 -pix_fmt yuv420p '%s'",
			 mp4);
		raw = popen(cmd, "w");
		if (!raw)
			val("record", 0, "ffmpeg");
	}
	if (samples) {
		sf = fopen(samples, "w");
		if (!sf) {
			val("record", 0, "samples");
			goto done;
		}
		fprintf(sf, "STEP %u\n", every);
	}

	{
		unsigned card_rb = 0;

		for (page_i = 0; page_i < 5; page_i++) {
			unsigned char got[64000];

			memset(page, (int)card_ink[page_i], sizeof(page));
			draw_digit(page, 320, 200, 148, 80, 8, page_i + 1, 0);
			if (bank(c, 0) || wd24_chip_mem_write(c, 0, page, 64000) ||
			    wd24_chip_mem_read(c, 0, got, 64000)) {
				val("markers", 0, "card");
				goto done;
			}
			for (i = 0; i < 64000; i++)
				if (got[i] != page[i])
					card_rb++;
			if (arm_start(c, 0)) {
				val("markers", 0, "latch");
				goto done;
			}
			for (f = 0; f < (full ? 140u : hold); f++, field++) {
				if (emit(c, raw, sf, ppm, field, 1, "card", every)) {
					val("markers", 0, "emit");
					goto done;
				}
			}
		}
		snprintf(ev, sizeof(ev), "cards=5 readback=%u", card_rb);
		val("markers", card_rb == 0, ev);
	}

	for (page_i = 0; page_i < 3; page_i++) {
		unsigned mismatch = 0;
		unsigned char got[64000];

		memset(page, (int)ink[page_i], sizeof(page));
		for (i = 0; i < 8; i++)
			page[10 * 320 + page_i * 40 + i] = 63;
		if (bank(c, pr[page_i]) ||
		    wd24_chip_mem_write(c, 0, page, 64000) ||
		    wd24_chip_mem_read(c, 0, got, 64000)) {
			snprintf(ev, sizeof(ev), "bank %u", page_i);
			val(page_i == 0 ? "page0" : page_i == 1 ? "page1" : "page2",
			    0, ev);
			continue;
		}
		for (i = 0; i < 64000; i++)
			if (got[i] != page[i])
				mismatch++;
		before = wd24_chip_latches(c);
		if (arm_start(c, cr0c[page_i])) {
			val(page_i == 0 ? "page0" : page_i == 1 ? "page1" : "page2",
			    0, "latch");
			continue;
		}
		snprintf(ev, sizeof(ev),
			 "cr0c=%02x cr0d=%02x pr0a=%02x edges=%u sum=%u",
			 wd24_chip_lat_cr0c(c), wd24_chip_lat_cr0d(c),
			 wd24_chip_pr0a(c),
			 wd24_chip_latches(c) - (unsigned)before, mismatch);
		val(page_i == 0 ? "page0" : page_i == 1 ? "page1" : "page2",
		    mismatch == 0 && wd24_chip_lat_cr0c(c) == cr0c[page_i] &&
		    wd24_chip_lat_cr0d(c) == 0 &&
		    wd24_chip_pr0a(c) == pr[page_i] &&
		    wd24_chip_latches(c) > (unsigned)before,
		    ev);
		for (f = 0; f < hold; f++, field++) {
			if (emit(c, raw, sf, ppm, field, 1, "page", every)) {
				val("record", 0, "page emit");
				goto done;
			}
		}
	}

	memset(page, 1, sizeof(page));
	for (i = 168u * 320u; i < 64000u; i++)
		page[i] = 32;
	src_h = fnv(page + 168u * 320u, 32u * 320u);
	if (bank(c, 0) || wd24_chip_mem_write(c, 0, page, 64000)) {
		val("hud", 0, "write");
		val("ghost", 0, "write");
	} else {
		unsigned char win[64000];
		unsigned frame_bad = 0, row_bad = 0;

		wd24_chip_mem_read(c, 0, win, 64000);
		win_h = fnv(win + 168u * 320u, 32u * 320u);
		for (i = 0; i < 64000; i++) {
			if (win[i] != page[i])
				frame_bad++;
			if (i >= 168u * 320u && win[i] != page[i])
				row_bad++;
		}
		snprintf(ev, sizeof(ev),
			 "frames=1 mismatch=%u src=0x%08x win=0x%08x",
			 row_bad, src_h, win_h);
		val("hud", row_bad == 0 && src_h == win_h, ev);
		snprintf(ev, sizeof(ev), "frames=1 mismatch=%u", frame_bad);
		val("ghost", frame_bad == 0, ev);
		if (arm_start(c, 0) == 0) {
			for (f = 0; f < hold; f++, field++)
				emit(c, raw, sf, ppm, field, 0, "hud", every);
		}
	}

	{
		unsigned mhz = wd24_chip_field_millihz(c);
		unsigned over = count_over2x(gaps, ngaps);

		snprintf(ev, sizeof(ev),
			 "fps=%u.%03u latch_to=%u intervals=%u over2x=%u source=model_field",
			 mhz / 1000u, mhz % 1000u, wd24_chip_latch_to(c), ngaps, over);
		val("perf", mhz != 0 && wd24_chip_latch_to(c) == 0 && over == 0, ev);
	}

	before = wd24_chip_latch_to(c);
	bad = 0;
	if (arm_start(c, 0) || wd24_chip_lat_cr0c(c) != 0 ||
	    wd24_chip_lat_cr0d(c) != 0)
		bad = 1;
	snprintf(ev, sizeof(ev), "mismatches=%u scope=registers", bad);
	val("restore", bad == 0 && wd24_chip_latch_to(c) == before, ev);
	for (f = 0; f < hold; f++, field++)
		emit(c, raw, sf, ppm, field, 1, "restore", every);

	before = wd24_chip_latch_to(c);
	bad = 0;
	{
		unsigned rb = 0, shown = 0;

		for (cyc = 0; cyc < strobe_cycles; cyc++) {
			for (page_i = 0; page_i < 3; page_i++) {
				unsigned char got[64000];

				memset(page, (int)ink[page_i], sizeof(page));
				draw_digit(page, 320, 200, 8, 8, 4, page_i, 63);
				if (bank(c, pr[page_i]) ||
				    wd24_chip_mem_write(c, 0, page, 64000) ||
				    wd24_chip_mem_read(c, 0, got, 64000) ||
				    arm_start(c, cr0c[page_i]) ||
				    wd24_chip_lat_cr0c(c) != cr0c[page_i])
					bad++;
				else {
					for (i = 0; i < 64000; i++)
						if (got[i] != page[i])
							rb++;
				}
				for (f = 0; f < hold; f++, field++) {
					if (emit(c, raw, sf, ppm, field, 1, "strobe", every))
						bad++;
					else
						shown++;
				}
			}
		}
		snprintf(ev, sizeof(ev),
			 "holds=%u fields=%u fields_each=%u readback=%u",
			 strobe_cycles * 3u, shown, hold, rb);
		val("strobe", bad == 0 && rb == 0 && shown == strobe_cycles * 3u * hold &&
		    wd24_chip_latch_to(c) == before, ev);
	}

done:
	printf("VAL SUMMARY pass=%d fail=%d\n", g_pass, g_fail);
	if (sf)
		fclose(sf);
	if (raw) {
		int st = pclose(raw);

		if (st != 0)
			fprintf(stderr, "ffmpeg status %d\n", st);
	}
	wd24_chip_free(c);
	return g_fail ? 1 : 0;
}
