/* host tests: S6d fault-bank window placement + livelock-span math */
#include <stdio.h>
#include "../wd24adv_s6d_bank.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
	u32 w0, w1;

	wd24s6d_win_for_off(0, &w0, &w1);
	CHECK(w0 == 0 && w1 == 8, "frame start -> w0=0 w1=8 (got %u %u)", w0, w1);
	CHECK(wd24s6d_off_in_win(0, w0, w1), "0 in win");
	CHECK(wd24s6d_off_in_win(0xffff, w0, w1), "64K-1 in win");
	CHECK(!wd24s6d_off_in_win(0x10000, w0, w1), "64K not in first win");

	wd24s6d_win_for_off(0x4afff, &w0, &w1); /* last frame byte */
	CHECK(w0 == 0x40 && w1 == 0x48, "frame end page (got %u %u)", w0, w1);
	CHECK(wd24s6d_off_in_win(0x4afff, w0, w1), "frame end in its win");
	CHECK(wd24s6d_off_in_win(0x40000, w0, w1), "win base in win");

	wd24s6d_win_for_off(0x12345, &w0, &w1);
	CHECK(w0 == 0x10 && w1 == 0x18, "mid (got %u %u)", w0, w1);
	CHECK(wd24s6d_off_in_win(0x12345, w0, w1), "mid in win");

	CHECK(!wd24s6d_livelock_span(0x1000, 0x2000), "same win no span");
	CHECK(!wd24s6d_livelock_span(0x0, 0xffff), "same 64K no span");
	CHECK(wd24s6d_livelock_span(0xffff, 0x10000), "boundary spans");
	CHECK(wd24s6d_livelock_span(0x0, 0x4b000), "frame vs offscreen spans");

	/* pitch 640: ~102.4 rows per 64 KiB — a 200-row blit can span */
	CHECK(wd24s6d_livelock_span(0, 200 * 640),
	      "200-row vertical span livelocks under single-win map");
	CHECK(!wd24s6d_livelock_span(0, 100 * 640),
	      "100-row stays in first win");

	if (fails) {
		printf("test_s6d_bank: %d/%d FAILED\n", fails, checks);
		return 1;
	}
	printf("test_s6d_bank: %d checks OK\n", checks);
	return 0;
}
