/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Blackfin (BF53x) core library -- public interface.
 *
 * An instruction interpreter for the ADSP-BF531/532/533 core as the
 * CDJ-2000/CDJ-2000NXS display processor uses it, written from the ADI
 * Blackfin Processor Programming Reference. The core owns what the manual
 * puts in the core: registers, the event controller (CEC), the core timer and
 * the core MMR space from 0xFFE00000. Everything from 0xFFC00000 down to the
 * core MMRs is the SoC's (bf531.c) and goes through the bus. No QEMU
 * dependency: the same objects link into the offline runner and the machine.
 */
#ifndef BFIN_H
#define BFIN_H

#include <stdint.h>
#include <stdio.h>

typedef struct bfin_core bfin_core;

/* Anything the core does not own as RAM. Sizes are 1, 2 or 4 bytes. A read
 * must not change the RAM the core maps; a write may (a DMA it starts). */
typedef struct bfin_bus {
    void     *opaque;
    uint32_t (*read)(void *opaque, uint32_t addr, unsigned size);
    void     (*write)(void *opaque, uint32_t addr, uint32_t val, unsigned size);
} bfin_bus;

typedef enum bfin_stop {
    BFIN_STOP_BUDGET = 0,   /* ran the whole budget                          */
    BFIN_STOP_IDLE,         /* in IDLE with nothing to wake it               */
    BFIN_STOP_UNDEF,        /* no implementation for the instruction at
                               bfin_trap_pc(); bfin_trap_insn() has its bits */
    BFIN_STOP_BREAK,        /* reached the address given to bfin_set_break */
} bfin_stop;

bfin_core *bfin_new(const bfin_bus *bus);
void       bfin_free(bfin_core *c);

/* Host memory the core reads and writes directly (SDRAM, L1 banks). */
void bfin_map_ram(bfin_core *c, uint32_t base, uint32_t size, uint8_t *host);
/* Host memory the core reads directly but writes through the bus (the boot
 * flash): it must not change while the core runs. */
void bfin_map_rom(bfin_core *c, uint32_t base, uint32_t size,
                  const uint8_t *host);

/* The state the boot ROM hands over: supervisor, inside the reset event. */
void      bfin_reset(bfin_core *c, uint32_t pc);
bfin_stop bfin_step(bfin_core *c, uint64_t budget, uint64_t *executed);

/* Makes bfin_step return after the current instruction: a peripheral
 * scheduled an event inside the budget the step was given. */
void bfin_yield(bfin_core *c);

/* Ends an IDLE without taking an event, or the next IDLE if none is running:
 * the PLL wakeup after a relock. */
void bfin_wake(bfin_core *c);

/* Level of the SIC's output to core event ivg (7..15). */
void bfin_set_ivg(bfin_core *c, int ivg, int level);

/* Core clock cycles since reset; one per instruction or bundle. IDLE time is
 * added with bfin_skip_cycles by whoever advances the peripherals. */
uint64_t bfin_cycles(const bfin_core *c);
void     bfin_skip_cycles(bfin_core *c, uint64_t n);
/* Cycles until the core timer next expires; UINT64_MAX when it is stopped. */
uint64_t bfin_timer_due(const bfin_core *c);

uint32_t bfin_get_pc(const bfin_core *c);
/* Registers by the instruction set's group/number pair (group 0 R0-R7,
 * 1 P0-P5/SP/FP, 2 I0-I3/M0-M3, 3 B0-B3/L0-L3, 4 A0.X/A0.W/A1.X/A1.W/-/-/
 * ASTAT/RETS, 6 LC0/LT0/LB0/LC1/LT1/LB1/CYCLES/CYCLES2,
 * 7 USP/SEQSTAT/SYSCFG/RETI/RETX/RETN/RETE/EMUDAT). */
uint32_t bfin_get_reg(bfin_core *c, unsigned grp, unsigned reg);
uint32_t bfin_get_ipend(const bfin_core *c);
/* A core MMR (0xFFE00000 up) as the core reads it: EVT, IMASK, ILAT, the
 * core timer. */
uint32_t bfin_get_mmr(bfin_core *c, uint32_t addr);
uint32_t bfin_trap_pc(const bfin_core *c);
uint64_t bfin_trap_insn(const bfin_core *c);

/* Per-instruction trace: one line per executed instruction or bundle (the PC
 * and its 16-bit words, in objdump's order) to f, while the cycle count lies
 * in [from, to). NULL stops it. */
void bfin_set_trace(bfin_core *c, FILE *f, uint64_t from, uint64_t to);

/* Stop before executing the instruction at pc (0 clears). */
void bfin_set_break(bfin_core *c, uint32_t pc);

/* Instruction length at the given first word, as the sequencer decodes it:
 * 2, 4 or 8 bytes. */
unsigned bfin_insn_len(uint16_t iw0);

#endif
