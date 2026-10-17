# Host unit tests for the WD90C24 register layer (no kernel, no QEMU).
CC ?= cc
CFLAGS ?= -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I.
SRC = test_wd90c24.c wdmock.c ../../modules/wd90c24fb/wd90c24_hw.c
OUT ?= ../../build/unit

all: run

$(OUT)/test_wd90c24: $(SRC) wdmock.h ../../modules/wd90c24fb/wd90c24_hw.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $(SRC)

run: $(OUT)/test_wd90c24
	$(OUT)/test_wd90c24

.PHONY: all run
