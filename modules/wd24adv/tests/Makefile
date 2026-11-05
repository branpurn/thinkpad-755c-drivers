# Host unit tests for the wd24adv core (no kernel needed)
#  test_core: the shared self-test plus host-only checks
#  test_engine: S0 BITBLT/cursor guard + sequencer vs the 23C0h model
#  test_s1:   S1 read-only 23C0h probe vs the same model
#  xcheck:    wd24adv plans vs wd90c24fb's own register layer (compiled
#             here read-only from ../../wd90c24fb/wd90c24_hw.c)
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=gnu11
CORE = ../wd24adv_core.c ../wd24adv_modes.c ../wd24adv_port.c
HW = ../../wd90c24fb/wd90c24_hw.c

test: test_core xcheck test_vga test_engine test_s1 test_s2 test_s3 test_s4 test_s5 test_s6 test_s6b test_s6d_bank test_dead
	./test_core
	./xcheck
	./test_vga
	./test_engine
	./test_s1
	./test_s2
	./test_s3
	./test_s4
	./test_s5
	./test_s6
	./test_s6b
	./test_s6d_bank
	./test_dead

test_core: test_core.c $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_core.c $(CORE)

xcheck: xcheck.c wdmock.h $(CORE) ../wd24adv_core.h $(HW) ../../wd90c24fb/wd90c24_hw.h
	$(CC) $(CFLAGS) -I. -o $@ xcheck.c $(CORE) $(HW)

test_dead: test_dead.c vgamodel.c vgamodel.h $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_dead.c vgamodel.c $(CORE)

test_vga: test_vga.c vgamodel.c vgamodel.h $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_vga.c vgamodel.c $(CORE)

test_engine: test_engine.c vgamodel.c vgamodel.h ../wd24adv_engine.c ../wd24adv_engine.h $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_engine.c vgamodel.c ../wd24adv_engine.c $(CORE)

test_s1: test_s1.c vgamodel.c vgamodel.h ../wd24adv_s1.c ../wd24adv_s1.h ../wd24adv_engine.c ../wd24adv_engine.h $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_s1.c vgamodel.c ../wd24adv_s1.c ../wd24adv_engine.c $(CORE)

test_s2: test_s2.c ../wd24adv_s2.c ../wd24adv_s2.h ../wd24adv_s1.c ../wd24adv_s1.h ../wd24adv_engine.c ../wd24adv_engine.h $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_s2.c ../wd24adv_s2.c ../wd24adv_s1.c ../wd24adv_engine.c $(CORE)

test_s3: test_s3.c ../wd24adv_s3.c ../wd24adv_s3.h ../wd24adv_s2.c ../wd24adv_s1.c $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_s3.c ../wd24adv_s3.c ../wd24adv_s2.c ../wd24adv_s1.c ../wd24adv_engine.c $(CORE)

test_s5: test_s5.c ../wd24adv_s3.c ../wd24adv_s3.h ../wd24adv_s2.c ../wd24adv_s1.c $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_s5.c ../wd24adv_s3.c ../wd24adv_s2.c ../wd24adv_s1.c ../wd24adv_engine.c $(CORE)

test_s4: test_s4.c ../wd24adv_s3.c ../wd24adv_s3.h ../wd24adv_s2.c ../wd24adv_s1.c $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_s4.c ../wd24adv_s3.c ../wd24adv_s2.c ../wd24adv_s1.c ../wd24adv_engine.c $(CORE)

test_s6: test_s6.c ../wd24adv_s6.c ../wd24adv_s6.h $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_s6.c ../wd24adv_s6.c $(CORE)

test_s6b: test_s6b.c ../wd24adv_s6b.c ../wd24adv_s6b.h ../wd24adv_s6.c ../wd24adv_s6.h $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_s6b.c ../wd24adv_s6b.c ../wd24adv_s6.c $(CORE)

test_s6d_bank: test_s6d_bank.c ../wd24adv_s6d_bank.h ../wd24adv_s6b.h
	$(CC) $(CFLAGS) -o $@ test_s6d_bank.c

clean:
	rm -f test_core xcheck test_vga test_engine test_s1 test_s2 test_s3 test_s4 test_s5 test_s6 test_s6b test_s6d_bank test_dead

.PHONY: test clean
