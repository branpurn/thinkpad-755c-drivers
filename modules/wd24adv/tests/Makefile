# Host unit test for the wd24adv core (no kernel needed)
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror -std=gnu11
SRC = test_core.c ../wd24adv_core.c ../wd24adv_modes.c

test: test_core
	./test_core

test_core: $(SRC) ../wd24adv_core.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -f test_core

.PHONY: test clean
