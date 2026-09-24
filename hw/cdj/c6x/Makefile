# SPDX-License-Identifier: GPL-2.0-or-later
# Out-of-tree build: make -C hw/cdj/c6x O=~/build/c6x
# Builds the core library objects, the offline runner and the decode check tool.
O      ?= $(HOME)/build/c6x
CC     ?= gcc
# -O3 is ~25% faster than -O2 on the interpreter.
CFLAGS ?= -O3 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
# No FP contraction: the DSP's float results must be IEEE-exact per operation.
CFLAGS += -std=gnu11 -I. -ffp-contract=off -frounding-math -pthread
# C66X_JIT_AUTO's defaults: the generator in this tree, the core library it loads
CFLAGS += -DC66X_JIT_TOOLS=\"$(abspath tools)\" -DC66X_JIT_LIBDIR=\"$(O)\"

# The core loads its JIT module through GModule, so the standalone tools need
# glib explicitly (QEMU already pulls it in).
GLIB_CFLAGS := $(shell pkg-config --cflags gmodule-2.0 2>/dev/null)
GLIB_LIBS   := $(shell pkg-config --libs gmodule-2.0 2>/dev/null)
CFLAGS += $(GLIB_CFLAGS)
SRC     = $(abspath .)

CORE    = c66x_decode.c c66x_image.c c66x_core.c
CORE_O  = $(patsubst %.c,$(O)/%.o,$(CORE))
HDRS    = $(wildcard *.h binutils/*.h)

# libc66x.so is the core tools/c14_jitgen.py describes packets with: it must be
# rebuilt with the core, or the generator plans against an older one.
all: $(O)/c6xdis $(O)/c6xrun $(O)/c6xreplay $(O)/libc66x.so

$(O):
	mkdir -p $(O)

$(O)/%.o: %.c $(HDRS) | $(O)
	$(CC) $(CFLAGS) -c $< -o $@

$(O)/c6xdis: tools/c6xdis.c $(O)/c66x_decode.o $(O)/c66x_image.o | $(O)
	$(CC) $(CFLAGS) $^ -o $@

# SoC models, linked when present.
SOC_SRC = $(wildcard soc_*.c)
SOC_O   = $(patsubst %.c,$(O)/%.o,$(SOC_SRC))

# The scripted MAIN peer for corpus replay, when present.
PEER_SRC = $(wildcard peer_*.c)
PEER_O   = $(patsubst %.c,$(O)/%.o,$(PEER_SRC))

$(O)/c6xrun: c6xrun.c $(CORE_O) $(SOC_O) $(PEER_O) | $(O)
	$(CC) $(CFLAGS) $^ -o $@ -lm $(GLIB_LIBS)

# Re-runs a c66x_record_open log with no SoC: the core's benchmark and oracle.
$(O)/c6xreplay: c6xreplay.c $(CORE_O) | $(O)
	$(CC) $(CFLAGS) $^ -o $@ -lm $(GLIB_LIBS)

# The core as a shared library, for tools/c14_jitgen.py (ctypes): the generator
# asks the real decoder for every packet it compiles.
$(O)/libc66x.so: $(CORE) $(HDRS) | $(O)
	$(CC) $(CFLAGS) -fPIC -shared $(CORE) -o $@ -lm $(GLIB_LIBS)

TIC6X ?= $(HOME)/opt/tic6x/bin/tic6x-elf-

CORE_TESTS = pipeline sploop_irq idle brchain splexit mmio mp3 resv rs idleirq xstall wlat memord splmem stall

$(O)/core_%.bin: tests/core_%.s | $(O)
	$(TIC6X)as -mlittle-endian -march=c674x $< -o $(O)/core_$*.o
	$(TIC6X)objcopy -O binary -j .text $(O)/core_$*.o $@

$(O)/core_test: tests/core_test.c $(CORE_O) | $(O)
	$(CC) $(CFLAGS) $^ -o $@ -lm $(GLIB_LIBS)

core-test: $(O)/core_test $(patsubst %,$(O)/core_%.bin,$(CORE_TESTS))
	$(O)/core_test $(O)/core_pipeline.bin $(O)/core_sploop_irq.bin $(O)/core_idle.bin \
	    $$($(TIC6X)nm $(O)/core_idle.o | awk '$$3 == "poll" { print $$1 }') \
	    $(O)/core_brchain.bin $(O)/core_splexit.bin $(O)/core_mmio.bin \
	    $(O)/core_mp3.bin $(O)/core_resv.bin $(O)/core_rs.bin \
	    $(O)/core_idleirq.bin $$($(TIC6X)nm $(O)/core_idleirq.o | awk '$$3 == "poll" { print $$1 }') \
	    $(O)/core_xstall.bin $(O)/core_wlat.bin $(O)/core_memord.bin $(O)/core_splmem.bin $(O)/core_stall.bin

$(O)/ext_test: tests/ext_test.c $(CORE_O) | $(O)
	$(CC) $(CFLAGS) $^ -o $@ -lm $(GLIB_LIBS)

ext-test: $(O)/ext_test
	$(O)/ext_test

m1: $(O)/c6xdis
	python3 tools/c1_m1diff.py $(O)/c6xdis $(firstword $(wildcard ../../../extract/main_unpacked.bin ../../../../extract/main_unpacked.bin))

clean:
	rm -rf $(O)

.PHONY: all m1 core-test ext-test clean
