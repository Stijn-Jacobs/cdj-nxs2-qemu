# SPDX-License-Identifier: GPL-2.0-or-later
# Out-of-tree build of the offline runner: make -C hw/cdj/bfin O=~/build/bfin
# and the host tests: make -C hw/cdj/bfin O=~/build/bfin check
O      ?= $(HOME)/build/bfin
CC     ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS += -std=gnu11 -I.

CORE   = bfin_core.c bfin_exec.c bfin_dsp.c bf531.c bfin_gdb.c
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

$(O)/gdb_test: tests/gdb_test.c tests/gdb_prog.h $(OBJS) | $(O)
	$(CC) $(CFLAGS) -Itests $< $(OBJS) -o $@

$(O)/trace_test: tests/trace_test.c tests/trace_prog.h $(OBJS) | $(O)
	$(CC) $(CFLAGS) -Itests $< $(OBJS) -o $@

$(O)/flash_test: tests/flash_test.c $(OBJS) | $(O)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@

$(O)/sport_dma2d_test: tests/sport_dma2d_test.c $(OBJS) | $(O)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@

$(O)/ppi_delay_test: tests/ppi_delay_test.c $(OBJS) | $(O)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@

$(O)/clock_test: tests/clock_test.c $(OBJS) | $(O)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@

$(O)/watchdog_test: tests/watchdog_test.c $(OBJS) | $(O)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@

$(O)/gdb_server: tests/gdb_server.c tests/gdb_prog.h $(OBJS) | $(O)
	$(CC) $(CFLAGS) -Itests $< $(OBJS) -o $@

check: $(O)/gdb_test $(O)/byteops_test $(O)/trace_test $(O)/flash_test $(O)/sport_dma2d_test $(O)/clock_test $(O)/watchdog_test $(O)/ppi_delay_test
	$(O)/gdb_test
	$(O)/trace_test
	$(O)/flash_test
	$(O)/sport_dma2d_test
	$(O)/clock_test
	$(O)/watchdog_test
	$(O)/ppi_delay_test
	python3 $(CURDIR)/../../../scripts/debug/bfin_trace_test.py
	$(O)/byteops_test

# tests/gdb_prog.h is checked in; this regenerates it from gdb_prog.s.
BFIN_BIN ?= $(HOME)/.local/share/cdj-toolchain/binutils-bfin/bin
tests/gdb_prog.h: tests/gdb_prog.s
	$(BFIN_BIN)/bfin-elf-as -o $(O)/gdb_prog.o $< 
	$(BFIN_BIN)/bfin-elf-objcopy -O binary -j .text $(O)/gdb_prog.o $(O)/gdb_prog.bin
	{ echo '/* SPDX-License-Identifier: GPL-2.0-or-later */'; \
	  echo '/* Generated from gdb_prog.s by make tests/gdb_prog.h. */'; \
	  echo 'static const unsigned char gdb_prog[] = {'; \
	  od -An -v -tx1 $(O)/gdb_prog.bin | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g'; \
	  echo '};'; \
	  $(BFIN_BIN)/bfin-elf-nm $(O)/gdb_prog.o | \
	    awk '$$2 == "T" || $$2 == "t" { printf "#define %s 0x%sU\n", toupper($$3), $$1 }'; \
	} > $@

tests/trace_prog.h: tests/trace_prog.s | $(O)
	$(BFIN_BIN)/bfin-elf-as -o $(O)/trace_prog.o $<
	$(BFIN_BIN)/bfin-elf-objcopy -O binary -j .text $(O)/trace_prog.o $(O)/trace_prog.bin
	{ echo '/* SPDX-License-Identifier: GPL-2.0-or-later */'; \
	  echo '/* Generated from trace_prog.s by make tests/trace_prog.h. */'; \
	  echo 'static const unsigned char trace_prog[] = {'; \
	  od -An -v -tx1 $(O)/trace_prog.bin | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g'; \
	  echo '};'; \
	  $(BFIN_BIN)/bfin-elf-nm $(O)/trace_prog.o | \
	    awk '$$2 == "T" || $$2 == "t" { printf "#define %s 0x%sU\n", toupper($$3), $$1 }'; \
	} > $@

clean:
	rm -rf $(O)

.PHONY: all check clean
