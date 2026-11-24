/* Host checks for the 13h subset. No hardware. */
#include "../../../tools/wd24-validate/wd24_chip.h"

#include <stdio.h>
#include <string.h>

static int fail;

static void T(int cond, const char *msg)
{
	if (cond)
		return;
	fprintf(stderr, "FAIL %s\n", msg);
	fail++;
}

int main(void)
{
	struct wd24_chip *c = wd24_chip_new();
	unsigned char buf[64], back[64];
	unsigned i;

	T(c != NULL, "alloc");
	T(wd24_chip_program_13h(c) == 0, "program 13h");
	T(wd24_chip_field_millihz(c) == 70086u, "field rate");
	T(wd24_chip_h_dots(c) == 800u, "horizontal dots");
	T(wd24_chip_v_lines(c) == 449u, "vertical total");
	T(wd24_chip_v_disp(c) == 400u, "vertical display");
	T(wd24_chip_unknown(c) == 0 && wd24_chip_unmodeled(c) == 0,
	  "program is modeled");

	T(wd24_chip_outb(c, 0x3ce, 0x09) == 0, "PR0A index");
	T(wd24_chip_outb(c, 0x3cf, 0x10) != 0, "PR0A locked");
	T(wd24_chip_refused(c) == 1, "refuse counted");
	T(wd24_chip_pr0a(c) == 0, "bank unchanged");

	T(wd24_chip_outb(c, 0x3ce, 0x0f) == 0, "PR5 index");
	T(wd24_chip_outb(c, 0x3cf, 0x05) == 0, "PR5 unlock");
	T(wd24_chip_outb(c, 0x3ce, 0x09) == 0, "PR0A index again");
	T(wd24_chip_outb(c, 0x3cf, 0x10) == 0, "PR0A 0x10");
	T(wd24_chip_pr0a(c) == 0x10, "bank 1");

	memset(buf, 0x3c, sizeof(buf));
	T(wd24_chip_mem_write(c, 0, buf, sizeof(buf)) == 0, "write bank 1");
	T(wd24_chip_outb(c, 0x3cf, 0x00) == 0, "back to bank 0");
	memset(back, 0, sizeof(back));
	T(wd24_chip_mem_read(c, 0, back, sizeof(back)) == 0, "read bank 0");
	T(back[0] == 0, "bank 0 is not bank 1");
	T(wd24_chip_outb(c, 0x3cf, 0x10) == 0, "bank 1 again");
	T(wd24_chip_mem_read(c, 0, back, sizeof(back)) == 0, "read bank 1");
	T(back[0] == 0x3c && back[63] == 0x3c, "bytes stuck");

	T(wd24_chip_outb(c, 0x3d4, 0x0c) == 0, "CR0C index");
	T(wd24_chip_outb(c, 0x3d5, 0x40) == 0, "arm CR0C 0x40");
	T(wd24_chip_outb(c, 0x3d4, 0x0d) == 0, "CR0D index");
	T(wd24_chip_outb(c, 0x3d5, 0x00) == 0, "arm CR0D");
	T(wd24_chip_wait_latch(c) == 1, "latch");
	T(wd24_chip_lat_cr0c(c) == 0x40 && wd24_chip_lat_cr0d(c) == 0x00,
	  "latched start");
	T(wd24_chip_latch_to(c) == 0, "no timeout");

	T(wd24_chip_outb(c, 0x3ce, 0x20) == 0, "unknown GR index");
	T(wd24_chip_outb(c, 0x3cf, 0x00) != 0, "unknown GR");
	T(wd24_chip_unmodeled(c) >= 1, "unmodeled counted");
	T(strstr(wd24_chip_log(c), "unmodeled") != NULL, "log names it");

	for (i = 0; i < 256; i++) {
		T(wd24_chip_outb(c, 0x3c8, i) == 0, "dac index");
		T(wd24_chip_outb(c, 0x3c9, 0xff) == 0, "dac r");
		T(wd24_chip_outb(c, 0x3c9, 0x01) == 0, "dac g");
		T(wd24_chip_outb(c, 0x3c9, 0x00) == 0, "dac b");
	}
	T(wd24_chip_outb(c, 0x3c7, 0) == 0, "dac read index");
	T(wd24_chip_inb(c, 0x3c9) == 0x3f, "dac keeps 6 bits");

	wd24_chip_set_stall_ns(c, 0);
	T(wd24_chip_stall_ns(c) == 0, "stall stays off");
	wd24_chip_free(c);
	if (fail) {
		fprintf(stderr, "test_wd24_chip: %d failed\n", fail);
		return 1;
	}
	printf("test_wd24_chip: passed\n");
	return 0;
}
