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

/* One tracepoint hit: the core cycle count and R0-R2 when the instruction at
 * pc was about to run. */
typedef struct bfin_trace_rec {
    uint64_t cycles;
    uint32_t pc, r0, r1, r2;
    uint16_t id;
} bfin_trace_rec;

typedef enum bfin_stop {
    BFIN_STOP_BUDGET = 0,   /* ran the whole budget                          */
    BFIN_STOP_IDLE,         /* in IDLE with nothing to wake it               */
    BFIN_STOP_UNDEF,        /* no implementation for the instruction at
                               bfin_trap_pc(); bfin_trap_insn() has its bits */
    BFIN_STOP_BREAK,        /* reached a breakpoint address */
    BFIN_STOP_WATCH,        /* a watchpoint hit; see bfin_watch_hit() */
    BFIN_STOP_STEP,         /* bfin_stepi finished its instruction */
} bfin_stop;

#define BFIN_MAX_BREAK 32
#define BFIN_MAX_WATCH 16
#define BFIN_MAX_TRACE 64
#define BFIN_TRACE_RING 0x100000u   /* default ring size, records */

/* Watchpoint kinds are a mask: an access watchpoint is READ | WRITE. */
enum { BFIN_WATCH_WRITE = 1, BFIN_WATCH_READ = 2, BFIN_WATCH_ACCESS = 3 };

bfin_core *bfin_new(const bfin_bus *bus);
void       bfin_free(bfin_core *c);

/* Host memory the core reads and writes directly (SDRAM, L1 banks). */
void bfin_map_ram(bfin_core *c, uint32_t base, uint32_t size, uint8_t *host);
/* Host memory the core reads directly but writes through the bus (the boot
 * flash): it must not change while the core runs. */
void bfin_map_rom(bfin_core *c, uint32_t base, uint32_t size,
                  const uint8_t *host);

/* Host-only ownership observations. Elapsed model cycles include IDLE and
 * do not depend on architectural CYCLES/CCEN. No snapshot restores qualify. */
typedef struct bfin_mask_timing {
    uint64_t intervals, total_cycles, total_awake_cycles, total_idle_cycles;
    uint64_t max_cycles, max_awake_cycles, max_idle_cycles;
    uint64_t censored_intervals, censored_cycles, censored_idle_cycles;
    int open;
    uint64_t since, idle_since; /* internal current-interval origin */
} bfin_mask_timing;
typedef struct bfin_ownership_timing {
    int enabled, start_configured, start_seen, snapshot_loaded;
    uint32_t start_pc;
    uint64_t start_cycle, end_cycle, overflow;
    struct { bfin_mask_timing imask, ipend_global; } phase[2]; /* boot/runtime */
} bfin_ownership_timing;
int bfin_timing_enable(bfin_core *c, int start_configured, uint32_t start_pc);
void bfin_timing_read(const bfin_core *c, bfin_ownership_timing *out);
int bfin_timing_started(const bfin_core *c); /* observed cold-boot marker only */
int bfin_timing_phase(const bfin_core *c); /* -1 disabled,0 boot,1 runtime */

/* Takes the ROM bank mapped at base away (reads then go through the bus) or
 * maps it again; for a flash that is not in read-array mode. */
void bfin_unmap_rom(bfin_core *c, uint32_t base);

/* Starts recording which 256-byte lines instructions execute from (exactly:
 * it runs without block chains, so it is slower). bfin_exec_lines counts the
 * distinct lines in [lo, hi) executed since. */
void     bfin_exec_track(bfin_core *c);
uint32_t bfin_exec_lines(const bfin_core *c, uint32_t lo, uint32_t hi);

/* The state the boot ROM hands over: supervisor, inside the reset event. */
void      bfin_reset(bfin_core *c, uint32_t pc);
bfin_stop bfin_step(bfin_core *c, uint64_t budget, uint64_t *executed);

/* Makes bfin_step return after the current instruction: a peripheral
 * scheduled an event inside the budget the step was given. */
void bfin_yield(bfin_core *c);

/* Ends an IDLE without taking an event, or the next IDLE if none is running:
 * the PLL wakeup after a relock. */
void bfin_wake(bfin_core *c);
/* Peripheral wake is a level, independent of CEC/SIC interrupt masking.
 * Only with the clock model on (bfin_set_models). */
void bfin_set_wake(bfin_core *c, int level);
/* Two models that change what a stock display boot does, both off unless the
 * board turns them on. clock_model: IDLE stops CYCLES and the core timer,
 * CYCLES counts only with SYSCFG.CCEN, and the SIC wake level (SIC_IWR) ends
 * or prevents IDLE. tint_w0c: TCNTL's sticky TINT clears on a written zero
 * (BF53x HRM 15-46) rather than a written one. */
void bfin_set_models(bfin_core *c, int clock_model, int tint_w0c);

/* Level of the SIC's output to core event ivg (7..15). */
void bfin_set_ivg(bfin_core *c, int ivg, int level);

/* Elapsed simulation time; preserved for runner budgets, peripheral deadlines
 * and traces. With the clock model, architectural CYCLES and the core timer
 * stop during IDLE. */
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

/* Snapshots: the registers, the event controller, the core timer and the
 * core MMRs as one image, taken between two bfin_step calls; decoded code is
 * rebuilt after a load. bfin_snap_save(c, NULL) returns the image size. Only
 * the build that wrote an image can read it. */
size_t bfin_snap_save(const bfin_core *c, uint8_t *buf);
void   bfin_snap_load(bfin_core *c, const uint8_t *buf);

/* Per-instruction trace: one line per executed instruction or bundle (the PC
 * and its 16-bit words, in objdump's order) to f, while the cycle count lies
 * in [from, to). NULL stops it. */
void bfin_set_trace(bfin_core *c, FILE *f, uint64_t from, uint64_t to);

/* Replaces every breakpoint by one at pc (0 clears them all). */
void bfin_set_break(bfin_core *c, uint32_t pc);

/* Breakpoints: bfin_step stops with BFIN_STOP_BREAK before executing the
 * instruction at pc. Resuming from that stop executes it. Adding one that is
 * set is a no-op. Return 0, or -1: table full (BFIN_MAX_BREAK), odd address,
 * or (del) not set. */
int  bfin_break_add(bfin_core *c, uint32_t pc);
int  bfin_break_del(bfin_core *c, uint32_t pc);
void bfin_break_clear(bfin_core *c);

/* Watchpoints on the core's own loads and stores (not instruction fetch, not
 * DMA). kind is BFIN_WATCH_*. A hit stops bfin_step right after the
 * accessing instruction or bundle with BFIN_STOP_WATCH. Return 0, or -1:
 * table full (BFIN_MAX_WATCH), len 0, or (del) not set. */
int  bfin_watch_add(bfin_core *c, uint32_t addr, uint32_t len, int kind);
int  bfin_watch_del(bfin_core *c, uint32_t addr, uint32_t len, int kind);
void bfin_watch_clear(bfin_core *c);
/* After BFIN_STOP_WATCH: the first watchpoint hit (its address and kind as
 * added); returns 1, or 0 when there was none. bfin_watch_extra() is how
 * many further hits the same instruction or bundle made. */
int      bfin_watch_hit(const bfin_core *c, uint32_t *addr, int *kind);
unsigned bfin_watch_extra(const bfin_core *c);

/* Tracepoints: like breakpoints for placement (exact, blocks split there),
 * but a hit appends a bfin_trace_rec to a host ring and the core goes on.
 * Adding one that is set updates its id. Return 0, or -1: table full
 * (BFIN_MAX_TRACE) or odd address. A full ring never overwrites: further
 * hits are counted in bfin_trace_lost() and not recorded.
 * bfin_trace_drain copies up to max records out, oldest first, and returns
 * how many. bfin_trace_ring() sets the capacity (call before the first
 * bfin_trace_add; default BFIN_TRACE_RING); returns 0 or -1. */
int      bfin_trace_add(bfin_core *c, uint32_t pc, uint16_t id);
void     bfin_trace_clear(bfin_core *c);
size_t   bfin_trace_drain(bfin_core *c, bfin_trace_rec *out, size_t max);
uint64_t bfin_trace_lost(const bfin_core *c);
int      bfin_trace_ring(bfin_core *c, uint32_t nrec);

/* Executes exactly one instruction or bundle, then returns BFIN_STOP_STEP
 * (or WATCH, UNDEF). A pending event is taken instead if one is ready: the
 * step ends at the handler's first instruction, before it runs. In IDLE it
 * returns BFIN_STOP_IDLE without advancing. Breakpoints are not tested at
 * the starting pc. */
bfin_stop bfin_stepi(bfin_core *c);

/* Writes a register by the same group/number pair as bfin_get_reg. Returns
 * 0, or -1 for a pair that does not exist. */
int  bfin_set_reg(bfin_core *c, unsigned grp, unsigned reg, uint32_t val);
/* Moves the PC; the instruction there runs even if a breakpoint is set. */
void bfin_set_pc(bfin_core *c, uint32_t pc);

/* Instruction length at the given first word, as the sequencer decodes it:
 * 2, 4 or 8 bytes. */
unsigned bfin_insn_len(uint16_t iw0);

#endif
