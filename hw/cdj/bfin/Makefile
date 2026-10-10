# SPDX-License-Identifier: GPL-2.0-or-later
# Out-of-tree build of the offline runner: make -C hw/cdj/bfin O=~/build/bfin
O      ?= $(HOME)/build/bfin
CC     ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS += -std=gnu11 -I.

CORE   = bfin_core.c bfin_exec.c bfin_dsp.c bf531.c
OBJS   = $(patsubst %.c,$(O)/%.o,$(CORE))
HDRS   = $(wildcard *.h)

all: $(O)/bfinrun

$(O):
	mkdir -p $(O)

$(O)/%.o: %.c $(HDRS) | $(O)
	$(CC) $(CFLAGS) -c $< -o $@

$(O)/bfinrun: bfinrun.c $(OBJS) | $(O)
	$(CC) $(CFLAGS) $^ -o $@

$(O)/byteops_test: tests/byteops_test.c $(OBJS) | $(O)
	$(CC) $(CFLAGS) $^ -o $@

check: $(O)/byteops_test
	$(O)/byteops_test

clean:
	rm -rf $(O)

.PHONY: all check clean
