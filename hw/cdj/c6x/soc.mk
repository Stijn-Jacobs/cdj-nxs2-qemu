# SPDX-License-Identifier: GPL-2.0-or-later
# C6655 SoC models: library and offline tests, out of tree.
#   make -C hw/cdj/c6x -f soc.mk O=~/build/c6x-soc test
O      ?= $(HOME)/build/c6x-soc
CC     ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS += -std=gnu11 -I.

SOC_SRC = soc_c6655.c soc_intc.c soc_timer.c soc_upp.c soc_spi.c soc_mcbsp.c soc_edma.c
SOC_O   = $(patsubst %.c,$(O)/%.o,$(SOC_SRC))
HDRS    = c66x.h soc_c6655.h soc_internal.h

all: $(O)/libc6655soc.a $(O)/soc_test

$(O):
	mkdir -p $(O)

$(O)/%.o: %.c $(HDRS) | $(O)
	$(CC) $(CFLAGS) -c $< -o $@

$(O)/libc6655soc.a: $(SOC_O)
	ar rcs $@ $^

$(O)/soc_test: tests/soc_test.c $(O)/libc6655soc.a $(HDRS)
	$(CC) $(CFLAGS) tests/soc_test.c $(O)/libc6655soc.a -o $@

test: $(O)/soc_test
	$(O)/soc_test

clean:
	rm -f $(SOC_O) $(O)/libc6655soc.a $(O)/soc_test

.PHONY: all test clean
