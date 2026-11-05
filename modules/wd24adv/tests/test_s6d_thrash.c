/* host tests: S6d thrash latch */
#include <stdio.h>
#include "../wd24adv_s6d_thrash.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
	struct wd24s6d_thrash t;
	u32 i;

	wd24s6d_thrash_init(&t, 4);
	CHECK(wd24s6d_thrash_note(&t, 0) == 0, "first win");
	CHECK(t.switches == 0 && !t.latched, "quiet after first");
	CHECK(wd24s6d_thrash_note(&t, 0) == 0, "same win resets");
	CHECK(t.switches == 0, "still quiet");

	CHECK(wd24s6d_thrash_note(&t, 0x10) == 0, "switch 1");
	CHECK(t.switches == 1, "sw=1");
	CHECK(wd24s6d_thrash_note(&t, 0) == 0, "switch 2");
	CHECK(wd24s6d_thrash_note(&t, 0x10) == 0, "switch 3");
	CHECK(wd24s6d_thrash_note(&t, 0) == -1, "switch 4 latches");
	CHECK(t.latched && t.trips == 1, "latched");
	CHECK(wd24s6d_thrash_note(&t, 0x10) == -1, "stays latched");

	wd24s6d_thrash_reset(&t);
	CHECK(!t.latched && t.trips == 1, "reset clears latch, keeps trips");
	CHECK(wd24s6d_thrash_note(&t, 0) == 0, "works after reset");

	/* quiet mid-stream clears the counter */
	wd24s6d_thrash_init(&t, 3);
	CHECK(wd24s6d_thrash_note(&t, 0) == 0, "a");
	CHECK(wd24s6d_thrash_note(&t, 8) == 0, "b sw1");
	CHECK(wd24s6d_thrash_note(&t, 8) == 0, "quiet same");
	CHECK(t.switches == 0, "quiet cleared");
	CHECK(wd24s6d_thrash_note(&t, 0) == 0, "sw1 again");
	CHECK(wd24s6d_thrash_note(&t, 8) == 0, "sw2");
	CHECK(wd24s6d_thrash_note(&t, 0) == -1, "sw3 latches");

	/* default bound */
	wd24s6d_thrash_init(&t, 0);
	CHECK(t.bound == WD24S6D_THRASH_BOUND_DEFAULT, "default bound");
	/* i=0 establishes; i=1..bound are switches; latch on switch #bound */
	for (i = 0; i <= WD24S6D_THRASH_BOUND_DEFAULT; i++) {
		u32 win = (i & 1) ? 0x10u : 0u;
		int r = wd24s6d_thrash_note(&t, win);
		if (i < WD24S6D_THRASH_BOUND_DEFAULT)
			CHECK(r == 0, "pre-bound i=%u", i);
		else
			CHECK(r == -1, "bound trip i=%u", i);
	}

	if (fails) {
		printf("test_s6d_thrash: %d/%d FAILED\n", fails, checks);
		return 1;
	}
	printf("test_s6d_thrash: %d checks OK\n", checks);
	return 0;
}
