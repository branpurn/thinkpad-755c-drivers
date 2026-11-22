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

static void span_unroll(unsigned pos, unsigned step, int count)
{
	uint8_t *dest = span_b;
	int left = count + 1;

	while (left >= 4) {
		unsigned ytemp, xtemp;
		ytemp = (pos >> 4) & 0x0fc0u; xtemp = pos >> 26;
		*dest++ = cmap[flat[xtemp | ytemp]]; pos += step;
		ytemp = (pos >> 4) & 0x0fc0u; xtemp = pos >> 26;
		*dest++ = cmap[flat[xtemp | ytemp]]; pos += step;
		ytemp = (pos >> 4) & 0x0fc0u; xtemp = pos >> 26;
		*dest++ = cmap[flat[xtemp | ytemp]]; pos += step;
		ytemp = (pos >> 4) & 0x0fc0u; xtemp = pos >> 26;
		*dest++ = cmap[flat[xtemp | ytemp]]; pos += step;
		left -= 4;
	}
	while (left--) {
		unsigned ytemp = (pos >> 4) & 0x0fc0u;
		unsigned xtemp = pos >> 26;
		*dest++ = cmap[flat[xtemp | ytemp]];
		pos += step;
	}
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
	}
	if (fails) {
		printf("test_draw_exact: %d mismatches\n", fails);
		return 1;
	}
	printf("test_draw_exact: 8000 loops matched\n");
	return 0;
}
