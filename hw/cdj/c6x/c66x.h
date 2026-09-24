/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C66x core library -- public interface.
 *
 * The contract between the core (c66x_core*, decoder) and everything around it
 * (the C6655 SoC models in soc_*, the offline runner, the QEMU device). It has
 * no QEMU dependency on purpose: the same objects link into the offline test
 * bench and into the machine.
 */
#ifndef C66X_H
#define C66X_H

#include <stdint.h>
#include <stddef.h>

typedef struct c66x_core c66x_core;

/* Anything the core does not own as fast RAM goes through the bus. Sizes are
 * 1, 2 or 4 bytes; the device is little-endian and so are these values. */
typedef struct c66x_bus {
    void     *opaque;
    uint32_t (*read)(void *opaque, uint32_t addr, unsigned size);
    void     (*write)(void *opaque, uint32_t addr, uint32_t val, unsigned size);
} c66x_bus;

/* Why c66x_step returned before its budget ran out. */
typedef enum c66x_stop {
    C66X_STOP_BUDGET = 0,   /* ran the whole budget                          */
    C66X_STOP_IDLE,         /* executed `idle`; resumes on an enabled interrupt */
    C66X_STOP_UNDEF,        /* undefined instruction at c66x_trap_pc()       */
    C66X_STOP_HOOK,         /* a PC hook asked to stop                       */
    C66X_STOP_FAULT,        /* bus fault / unmapped fetch                    */
} c66x_stop;

/* CPU interrupt lines as SPRU732 numbers them: 1 = NMI, 4..15 maskable. */
enum { C66X_INT_NMI = 1, C66X_INT_FIRST = 4, C66X_INT_LAST = 15 };

c66x_core *c66x_new(const c66x_bus *bus);
void       c66x_free(c66x_core *c);

/* Host memory the core reads and writes directly, bypassing the bus. Code
 * executed from here is predecoded; writes into it invalidate the cache. */
void c66x_map_ram(c66x_core *c, uint32_t base, uint32_t size, uint8_t *host);

void      c66x_reset(c66x_core *c, uint32_t pc);
c66x_stop c66x_step(c66x_core *c, uint64_t budget, uint64_t *executed);

/* Level-sensitive CPU interrupt input, driven by the CorePac INTC model. */
void c66x_set_irq(c66x_core *c, int line, int level);

/* Native stand-in for code the image calls but does not load (the missing
 * first stage). Return nonzero to stop the step loop. The hook runs instead of
 * the instruction at pc and must set the PC itself (usually to B3). */
typedef int (*c66x_hook_fn)(c66x_core *c, void *opaque);
void c66x_hook_pc(c66x_core *c, uint32_t pc, c66x_hook_fn fn, void *opaque);

/* State access, for hooks, the runner and a future gdbstub.
 * reg 0..31 = A0..A31, 32..63 = B0..B31. creg is the control register's crlo
 * number from binutils/tic6x-control-registers.h (CSR 1, IER 4, ISTP 5, IRP 6,
 * ILC 0xd, TSR 0x1a, ...); the write-only aliases ICR/ISR/ECR are not readable. */
uint32_t c66x_get_pc(const c66x_core *c);
void     c66x_set_pc(c66x_core *c, uint32_t pc);
uint32_t c66x_get_reg(const c66x_core *c, unsigned reg);
void     c66x_set_reg(c66x_core *c, unsigned reg, uint32_t val);
uint32_t c66x_get_creg(const c66x_core *c, unsigned creg);
void     c66x_set_creg(c66x_core *c, unsigned creg, uint32_t val);
uint32_t c66x_trap_pc(const c66x_core *c);

/* Counters since reset. A cycle is one E1 pipeline advance: an execute packet,
 * a NOP cycle, a stall or an interrupt-entry cycle. */
typedef struct c66x_stats {
    uint64_t cycles;
    uint64_t packets;
    uint64_t insns;
    uint64_t stalls;
    uint64_t interrupts;
    uint64_t sploops;
    uint64_t decodes;
    uint64_t irq_wait_branch;   /* cycles an enabled interrupt waited on branch slots */
    uint64_t irq_wait_sploop;   /* ... on an active loop buffer */
    uint32_t irq_edges[16];     /* rising edges seen per CPU interrupt line */
} c66x_stats;
void c66x_get_stats(const c66x_core *c, c66x_stats *out);

/* The cycle counter (the time-stamp counter's value). */
uint64_t c66x_get_cycle(const c66x_core *c);

/* Address of the execute packet now running (c66x_get_pc is the next fetch). */
uint32_t c66x_get_exec_pc(const c66x_core *c);

/* Called after any store that overlaps [lo, hi], including RAM the core owns. */
typedef void (*c66x_watch_fn)(c66x_core *c, void *opaque, uint32_t addr,
                              uint32_t val, unsigned size);
void c66x_watch_writes(c66x_core *c, uint32_t lo, uint32_t hi,
                       c66x_watch_fn fn, void *opaque);

/* Host code wrote guest memory the core maps (a DMA master, a loader): drop
 * decoded instructions there. Stores made by the core itself need no call. */
void c66x_invalidate(c66x_core *c, uint32_t addr, uint32_t len);

/* Busy-wait skipping. The app has no IDLE instruction: its main loop (head
 * 0x80076F00) polls. Declare the head; when one complete head-to-head
 * iteration stored nothing outside [stack_lo, stack_hi), touched no bus
 * address (MMIO), read no time-stamp register and came back with identical
 * registers, the loop is a fixed point until an interrupt arrives:
 * c66x_step then returns C66X_STOP_IDLE with the PC at the head.
 * The caller may advance its peripherals to their next event (and
 * c66x_skip_cycles the same amount) before stepping again. head_pc 0 disables.
 * Bus reads can be allowed (allow_bus_reads) when every polled register
 * changes only at a peripheral event. */
void c66x_set_idle_loop(c66x_core *c, uint32_t head_pc, uint32_t stack_lo,
                        uint32_t stack_hi, int allow_bus_reads);
/* Advance the time-stamp counter as if n idle cycles ran. */
void c66x_skip_cycles(c66x_core *c, uint64_t n);

typedef struct c66x_idle_info {
    uint64_t iterations;       /* head-to-head iterations seen          */
    uint64_t idle_hits;        /* ... that were fixed points            */
    uint32_t last_iter_cycles; /* length of the last iteration          */
    uint32_t last_reason;      /* 0 idle, 1 store, 2 bus write, 3 bus read,
                                  4 interrupt, 5 registers changed, 6 time read,
                                  7 writes in flight */
    uint32_t last_addr;        /* address behind reasons 1-3            */
    uint32_t last_pc;          /* packet that caused it                 */
    uint64_t resume_idles;     /* STOP_IDLEs where an interrupt handler returned */
    uint64_t isr_store_hits;   /* passes a handler disturbed (reason 4)  */
} c66x_idle_info;
void c66x_get_idle_info(const c66x_core *c, c66x_idle_info *out);

/* An interrupt no longer ends the fixed point by itself. Handler stores are
 * checked against the words the loop reads; when the handler stored none of
 * them and the registers come back unchanged, c66x_step returns
 * C66X_STOP_IDLE right where the handler returned to (resume_idles), not only
 * at the head. */

/* Declare a bus address the idle loop writes on every pass with no effect
 * after the first (a GPIO SET_DATA the loop keeps asserting): such a write is
 * then no side effect. The caller vouches for the peripheral; up to 8. */
void c66x_idle_idempotent_write(c66x_core *c, uint32_t addr);

/* Record everything the core receives from outside, stamped with its cycle, so
 * c6xreplay can re-run the same execution with no SoC: bus reads and writes,
 * interrupt line changes, host stores into mapped RAM (with the bytes), idle
 * skips, RAM maps and resets (with the mapped RAM's non-zero pages), and a
 * register-state hash every 2^22 cycles to locate a divergence. Call right
 * after c66x_new, before c66x_map_ram. Appends, so a restarted chip goes on
 * in the same file. NULL or "" does nothing. Returns 0 on success. */
int  c66x_record_open(c66x_core *c, const char *path);
void c66x_record_close(c66x_core *c);
/* The state hash the recorder writes; exposed for the replayer. */
uint64_t c66x_state_hash(const c66x_core *c);

/* One line on the compiled code (C66X_JIT / C66X_JIT_AUTO): regions, entries,
 * cycles run compiled, verify failures, kernels. Empty when none is loaded. */
void c66x_jit_report(const c66x_core *c, char *buf, size_t len);

/* Optional per-execute-packet trace, for offline debugging. */
typedef void (*c66x_trace_fn)(c66x_core *c, void *opaque, uint32_t pc);
void c66x_set_trace(c66x_core *c, c66x_trace_fn fn, void *opaque);

#endif
