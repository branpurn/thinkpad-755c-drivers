/* Pixel-exact check of the unrolled column and span loops.
 * Same arithmetic as doomgeneric r_draw.c. Not a 755C timedemo. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

enum { W = 320, H = 168, N = 4096 };
static uint8_t tex[128], flat[N], cmap[256];
static uint8_t col_a[H], col_b[H], span_a[W], span_b[W];

static void col_base(int count, int frac, int step)
{
	uint8_t *dest = col_a;
	do {
		*dest = cmap[tex[(frac >> 16) & 127]];
		dest++;
		frac += step;
	} while (count--);
}

static void col_unroll(int count, int frac, int step)
{
	uint8_t *dest = col_b;
	int left = count + 1;

	while (left >= 4) {
		*dest = cmap[tex[(frac >> 16) & 127]];
		dest++; frac += step;
		*dest = cmap[tex[(frac >> 16) & 127]];
		dest++; frac += step;
		*dest = cmap[tex[(frac >> 16) & 127]];
		dest++; frac += step;
		*dest = cmap[tex[(frac >> 16) & 127]];
		dest++; frac += step;
		left -= 4;
	}
	while (left--) {
		*dest = cmap[tex[(frac >> 16) & 127]];
		dest++;
		frac += step;
	}
}

static void span_base(unsigned pos, unsigned step, int count)
{
	uint8_t *dest = span_a;
	do {
		unsigned ytemp = (pos >> 4) & 0x0fc0u;
		unsigned xtemp = pos >> 26;
		*dest++ = cmap[flat[xtemp | ytemp]];
		pos += step;
	} while (count--);
}

static uint8_t span_pix(unsigned pos)
{
	unsigned ytemp = (pos >> 4) & 0x0fc0u;
	unsigned xtemp = pos >> 26;

	return cmap[flat[xtemp | ytemp]];
}

/* Four horizontal pixels in one little-endian store. Same bytes. */
static void span_unroll(unsigned pos, unsigned step, int count)
{
	uint8_t *dest = span_b;
	int left = count + 1;

	while (left && ((uintptr_t)dest & 3u)) {
		*dest++ = span_pix(pos);
		pos += step;
		left--;
	}
	while (left >= 4) {
		unsigned p0 = span_pix(pos); pos += step;
		unsigned p1 = span_pix(pos); pos += step;
		unsigned p2 = span_pix(pos); pos += step;
		unsigned p3 = span_pix(pos); pos += step;

		*(uint32_t *)dest = p0 | (p1 << 8) | (p2 << 16) | (p3 << 24);
		dest += 4;
		left -= 4;
	}
	while (left--) {
		*dest++ = span_pix(pos);
		pos += step;
	}
}

static void low_bytes(uint8_t *d, uint8_t pix)
{
	d[0] = pix;
	d[1] = pix;
}

static void low_word(uint8_t *d, uint8_t pix)
{
	*(uint16_t *)d = (uint16_t)(pix | (pix << 8));
}

int main(void)
{
	int i, fails = 0;

	for (i = 0; i < 128; i++)
		tex[i] = (uint8_t)(i * 3 + 1);
	for (i = 0; i < N; i++)
		flat[i] = (uint8_t)(i * 17);
	for (i = 0; i < 256; i++)
		cmap[i] = (uint8_t)(255 - i);
	for (i = 0; i < 4000; i++) {
		int count = i % H;
		int frac = 0x10000 + i * 13;
		int step = 0x8000 + (i & 31) * 64;
		int n = count + 1;
		unsigned pos = 0x12340000u + (unsigned)i * 0x101u;
		unsigned sstep = 0x00c00040u + (unsigned)(i & 15) * 0x40u;
		int sc = i % 64;
		int sn = sc + 1;

		memset(col_a, 0, sizeof col_a);
		memset(col_b, 0, sizeof col_b);
		col_base(count, frac, step);
		col_unroll(count, frac, step);
		if (memcmp(col_a, col_b, (size_t)n) != 0)
			fails++;
		memset(span_a, 0, sizeof span_a);
		memset(span_b, 0, sizeof span_b);
		span_base(pos, sstep, sc);
		span_unroll(pos, sstep, sc);
		if (memcmp(span_a, span_b, (size_t)sn) != 0)
			fails++;
		{
			uint8_t pair[2], word[2];
			uint8_t pix = (uint8_t)(i * 3);

			low_bytes(pair, pix);
			low_word(word, pix);
			if (pair[0] != word[0] || pair[1] != word[1])
				fails++;
		}
	}
	{
		enum { FT = 50, HH = 64 };
		int fuzz_a = 0, fuzz_b = 0;
		int foff[FT];
		uint8_t map[8 * 256];
		uint8_t fa[W * HH], fb[W * HH];
		uint8_t xlat[256];
		int k;

		for (k = 0; k < FT; k++)
			foff[k] = (k & 1) ? 1 : -1;
		for (k = 0; k < 8 * 256; k++)
			map[k] = (uint8_t)(k * 5);
		for (k = 0; k < 256; k++)
			xlat[k] = (uint8_t)(255 - k);
		for (k = 0; k < W * HH; k++)
			fa[k] = fb[k] = (uint8_t)(k * 9);
		for (i = 0; i < 500; i++) {
			int count = i % 40;
			int n = count + 1;
			int x = 1 + (i % (W - 2));
			int y = i % (HH - n);
			uint8_t *da = fa + y * W + x;
			uint8_t *db = fb + y * W + x;
			int left, frac, step;

			fuzz_a = fuzz_b = i % FT;
			for (k = 0; k < n; k++) {
				da[k * W] = map[6 * 256 + da[k * W + foff[fuzz_a]]];
				if (++fuzz_a == FT)
					fuzz_a = 0;
			}
			left = n;
			while (left >= 4) {
				int t;
				for (t = 0; t < 4; t++) {
					db[0] = map[6 * 256 + db[foff[fuzz_b]]];
					if (++fuzz_b == FT)
						fuzz_b = 0;
					db += W;
				}
				left -= 4;
			}
			while (left--) {
				db[0] = map[6 * 256 + db[foff[fuzz_b]]];
				if (++fuzz_b == FT)
					fuzz_b = 0;
				db += W;
			}
			if (memcmp(fa, fb, sizeof fa) != 0 || fuzz_a != fuzz_b)
				fails++;
			frac = 0x10000 + i;
			step = 0x8000;
			for (k = 0; k < n; k++) {
				fa[k] = map[xlat[tex[(frac >> 16) & 127]]];
				frac += step;
			}
			frac = 0x10000 + i;
			left = n;
			k = 0;
			while (left >= 4) {
				int t;
				for (t = 0; t < 4; t++) {
					fb[k++] = map[xlat[tex[(frac >> 16) & 127]]];
					frac += step;
				}
				left -= 4;
			}
			while (left--) {
				fb[k++] = map[xlat[tex[(frac >> 16) & 127]]];
				frac += step;
			}
			if (memcmp(fa, fb, (size_t)n) != 0)
				fails++;
		}
	}
	if (fails) {
		printf("test_draw_exact: %d mismatches\n", fails);
		return 1;
	}
	printf("test_draw_exact: column span fuzz translated matched\n");
	return 0;
}
