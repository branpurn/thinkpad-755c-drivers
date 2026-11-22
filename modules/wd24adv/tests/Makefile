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

test: test_core xcheck test_vga test_engine test_s1 test_s2 test_s3 test_s4 test_s5 test_s6 test_s6b test_s6d_bank test_s6d_thrash test_s6d_fault test_dead test_cmap_life test_cmap_life_asan test_glyph_bank test_cmap_check test_cmap_abi
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
	./test_s6d_thrash
	./test_s6d_fault
	./test_dead
	./test_cmap_life
	ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:log_path=/tmp/wd24-cmap-asan ./test_cmap_life_asan
	./test_glyph_bank
	dash ./test_glyph_run.sh
	dash ./test_cmap_phase1_run.sh
	./test_cmap_check
	./test_cmap_abi
	python3 ./test_cmap_audit.py
	dash ./test_cmap_p23_run.sh
	dash ./test_tty_order.sh
	$(CC) $(CFLAGS) -I../../../tools/wd24-doom -o /tmp/wd24-test-doom-pick ../../../tools/wd24-doom/test_doom_pick.c
	/tmp/wd24-test-doom-pick
	$(CC) $(CFLAGS) -I../../../tools/wd24-doom -o /tmp/wd24-test-doom-present ../../../tools/wd24-doom/test_doom_present.c
	/tmp/wd24-test-doom-present
	$(CC) $(CFLAGS) -o /tmp/wd24-test-draw-exact ../../../tools/wd24-doom/test_draw_exact.c
	/tmp/wd24-test-draw-exact
	$(CC) $(CFLAGS) -I.. -o /tmp/wd24-test-13h-map test_13h_map.c
	/tmp/wd24-test-13h-map
	$(CC) $(CFLAGS) -I../../../tools/wd24-doom -o /tmp/wd24-test-tty-own ../../../tools/wd24-doom/test_tty_own.c ../../../tools/wd24-doom/wd24x-tty-own.c -lutil
	/tmp/wd24-test-tty-own
	$(CC) $(CFLAGS) -I.. -o /tmp/wd24-test-trial-nomap test_trial_nomap.c
	/tmp/wd24-test-trial-nomap
	$(CC) $(CFLAGS) -I.. -o /tmp/wd24-test-cmap-cache test_cmap_cache.c
	/tmp/wd24-test-cmap-cache
	$(CC) $(CFLAGS) -I. -o /tmp/wd24-test-xproto test_xproto.c
	/tmp/wd24-test-xproto
	$(CC) $(CFLAGS) -I. -o /tmp/wd24-test-xcmp wd24x-xcmp.c
	dash ./test_desktop_run.sh
	dash ./test_doom_run.sh
	dash ./test_browser_pages.sh
	dash ./test_browser_run.sh
	dash ./test_tree_scan.sh
	dash ./test_ko_exports.sh
	dash ./test_fbcon_run.sh
	dash ./test_accel_run.sh
	dash ./test_scroll_run.sh
	$(CC) $(CFLAGS) -o test_cur test_cur.c ../wd24adv_s3.c ../wd24adv_s2.c ../wd24adv_s1.c ../wd24adv_engine.c $(CORE)
	./test_cur
	dash ./test_cursor_run.sh
	$(CC) $(CFLAGS) -I.. -I../../../tools/wd24-doom -o /tmp/wd24-test-present-thrash test_present_thrash.c
	/tmp/wd24-test-present-thrash
	$(CC) $(CFLAGS) -o /tmp/wd24-test-fb-api test_fb_api_61857.c
	/tmp/wd24-test-fb-api

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

test_cur: test_cur.c ../wd24adv_s3.c ../wd24adv_cur.h ../wd24adv_s3.h ../wd24adv_s2.c ../wd24adv_s1.c $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_cur.c ../wd24adv_s3.c ../wd24adv_s2.c ../wd24adv_s1.c ../wd24adv_engine.c $(CORE)

test_s4: test_s4.c ../wd24adv_s3.c ../wd24adv_s3.h ../wd24adv_s2.c ../wd24adv_s1.c $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_s4.c ../wd24adv_s3.c ../wd24adv_s2.c ../wd24adv_s1.c ../wd24adv_engine.c $(CORE)

test_s6: test_s6.c ../wd24adv_s6.c ../wd24adv_s6.h $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_s6.c ../wd24adv_s6.c $(CORE)

test_s6b: test_s6b.c ../wd24adv_s6b.c ../wd24adv_s6b.h ../wd24adv_s6.c ../wd24adv_s6.h $(CORE) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ test_s6b.c ../wd24adv_s6b.c ../wd24adv_s6.c $(CORE)

test_s6d_bank: test_s6d_bank.c ../wd24adv_s6d_bank.h ../wd24adv_s6b.h
	$(CC) $(CFLAGS) -o $@ test_s6d_bank.c

test_s6d_thrash: test_s6d_thrash.c ../wd24adv_s6d_thrash.h
	$(CC) $(CFLAGS) -o $@ test_s6d_thrash.c

test_s6d_fault: test_s6d_fault.c ../wd24adv_s6d_fault.h ../wd24adv_s6d_bank.h ../wd24adv_s6d_thrash.h
	$(CC) $(CFLAGS) -o $@ test_s6d_fault.c

test_cmap_check: test_cmap_check.c wd24x-cmap-check.h
	$(CC) $(CFLAGS) -o $@ test_cmap_check.c

test_cmap_abi: test_cmap_abi.c wd24x-cmap-get.c wd24x-cmap-set.c wd24x-cmap-os.h wd24x-cmap-check.h vgamodel.c vgamodel.h $(CORE) ../wd24adv_core.h ../wd24adv_dac_entry.h
	$(CC) $(CFLAGS) -DWD24X_CMAP_NO_MAIN -o $@ test_cmap_abi.c wd24x-cmap-get.c wd24x-cmap-set.c vgamodel.c $(CORE)

test_glyph_bank: test_glyph_bank.c wd24x-glyph-bits.h ../wd24adv_s6d_bank.h ../wd24adv_s6d_thrash.h
	$(CC) $(CFLAGS) -o $@ test_glyph_bank.c

test_cmap_life: test_cmap_life.c vgamodel.c vgamodel.h $(CORE) ../wd24adv_core.h ../wd24adv_dac_entry.h
	$(CC) $(CFLAGS) -pthread -o $@ test_cmap_life.c vgamodel.c $(CORE)

test_cmap_life_asan: test_cmap_life.c vgamodel.c vgamodel.h $(CORE) ../wd24adv_core.h ../wd24adv_dac_entry.h
	$(CC) -O1 -g -Wall -Wextra -Werror -std=gnu11 -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ test_cmap_life.c vgamodel.c $(CORE)

wd24s6d-mmap: wd24s6d-mmap.c ../wd24adv_s6d_fault.h ../wd24adv_s6d_bank.h ../wd24adv_s6d_thrash.h
	$(CC) $(CFLAGS) -o $@ wd24s6d-mmap.c

# i486 static build is separate. Not part of `make test`.
wd24x-client: wd24x-client.c
	$(CC) $(CFLAGS) -o $@ wd24x-client.c

clean:
	rm -f test_core xcheck test_vga test_engine test_s1 test_s2 test_s3 test_s4 test_s5 test_s6 test_s6b test_s6d_bank test_s6d_thrash test_s6d_fault test_dead test_cmap_life test_cmap_life_asan test_glyph_bank test_cmap_check test_cmap_abi wd24s6d-mmap wd24x-client

.PHONY: test clean
