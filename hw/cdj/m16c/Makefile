# SPDX-License-Identifier: GPL-2.0-or-later
# Out-of-tree build of the tests and the offline runner:
#   make -C hw/cdj/m16c O=~/build/m16c check
O      ?= $(HOME)/build/m16c
CC     ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS += -std=gnu11 -I.

CORE   = m16c_core.c m16c_exec.c m16c_decode.c m16c63.c
OBJS   = $(patsubst %.c,$(O)/%.o,$(CORE))
HDRS   = $(wildcard *.h)

all: $(O)/m16crun $(O)/decode_check $(O)/operand_check $(O)/exec_test $(O)/m16c63_test

$(O):
	mkdir -p $(O)

$(O)/%.o: %.c $(HDRS) | $(O)
	$(CC) $(CFLAGS) -c $< -o $@

$(O)/m16crun: m16crun.c $(OBJS) | $(O)
	$(CC) $(CFLAGS) $^ -o $@

$(O)/decode_check: tests/decode_check.c $(OBJS) | $(O)
	$(CC) $(CFLAGS) $^ -o $@

$(O)/operand_check: tests/operand_check.c $(OBJS) | $(O)
	$(CC) $(CFLAGS) $^ -o $@

$(O)/exec_test: tests/exec_test.c $(OBJS) | $(O)
	$(CC) $(CFLAGS) $^ -o $@

$(O)/m16c63_test: tests/m16c63_test.c $(OBJS) | $(O)
	$(CC) $(CFLAGS) $^ -o $@

check: $(O)/exec_test $(O)/m16c63_test
	$(O)/exec_test
	$(O)/m16c63_test

clean:
	rm -rf $(O)

.PHONY: all check clean
