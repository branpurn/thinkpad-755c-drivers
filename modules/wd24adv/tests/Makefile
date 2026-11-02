# Host unit tests for the wd24adv core (no kernel needed)
#  test_core: the shared self-test plus host-only checks
#  test_engine: S0 BITBLT/cursor guard + sequencer vs the 23C0h model
#  xcheck:    wd24adv plans vs wd90c24fb's own register layer (compiled
#             here read-only from ../../wd90c24fb/wd90c24_hw.c)
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=gnu11
CORE = ../wd24adv_core.c ../wd24adv_modes.c ../wd24adv_port.c
HW = ../../wd90c24fb/wd90c24_hw.c

test: test_core xcheck test_vga test_engine
	./test_core
	./xcheck
	./test_vga
	./test_engine

test_core: test_core.c $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_core.c $(CORE)

xcheck: xcheck.c wdmock.h $(CORE) ../wd24adv_core.h $(HW) ../../wd90c24fb/wd90c24_hw.h
	$(CC) $(CFLAGS) -I. -o $@ xcheck.c $(CORE) $(HW)

test_vga: test_vga.c vgamodel.c vgamodel.h $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_vga.c vgamodel.c $(CORE)

test_engine: test_engine.c vgamodel.c vgamodel.h ../wd24adv_engine.c ../wd24adv_engine.h $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_engine.c vgamodel.c ../wd24adv_engine.c $(CORE)

clean:
	rm -f test_core xcheck test_vga test_engine

.PHONY: test clean
