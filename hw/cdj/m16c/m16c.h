/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Renesas M16C/60-series core library -- public interface.
 *
 * An instruction interpreter for the M16C/60 CPU core (as in the M16C/63 that
 * runs the CDJ-900's GUI), written from the Renesas M16C/60, M16C/20 Series
 * Software Manual. The core owns the registers, the instruction set and the
 * interrupt sequence; everything in the 1 MiB address space that is not
 * mapped as host memory, the SFRs first of all, goes through the bus. No QEMU
 * dependency: the same objects link into the offline runner and a machine.
 */
#ifndef M16C_H
#define M16C_H

#include <stdint.h>
#include <stdio.h>

typedef struct m16c_core m16c_core;

/* Anything the core does not map as host memory. Sizes are 1 or 2 bytes.
 * ack is called when the core accepts the maskable interrupt it was given
 * with m16c_set_irq, so the interrupt controller can clear its request. */
typedef struct m16c_bus {
    void     *opaque;
    uint32_t (*read)(void *opaque, uint32_t addr, unsigned size);
    void     (*write)(void *opaque, uint32_t addr, uint32_t val, unsigned size);
    void     (*ack)(void *opaque, unsigned vec);
} m16c_bus;

typedef enum m16c_stop {
    M16C_STOP_BUDGET = 0,   /* ran the whole budget                         */
    M16C_STOP_WAIT,         /* in WAIT with no interrupt to take            */
    M16C_STOP_UNDEF,        /* no decoding for the bytes at m16c_trap_pc()  */
    M16C_STOP_BREAK,        /* reached the address given to m16c_set_break  */
} m16c_stop;

/* Register numbers for m16c_get_reg/m16c_set_reg. R0-R3, A0, A1 and FB are
 * the ones of the bank the B flag selects; SP is the one the U flag
 * selects. */
enum {
    M16C_R0, M16C_R1, M16C_R2, M16C_R3, M16C_A0, M16C_A1, M16C_FB, M16C_SB,
    M16C_USP, M16C_ISP, M16C_SP, M16C_FLG, M16C_INTB, M16C_PC,
};

/* FLG bits. */
enum {
    M16C_C = 1u << 0, M16C_D = 1u << 1, M16C_Z = 1u << 2, M16C_S = 1u << 3,
    M16C_B = 1u << 4, M16C_O = 1u << 5, M16C_I = 1u << 6, M16C_U = 1u << 7,
};

m16c_core *m16c_new(const m16c_bus *bus);
void       m16c_free(m16c_core *c);

/* Host memory the core reads and writes directly. base and size are
 * multiples of 1 KiB. */
void m16c_map_ram(m16c_core *c, uint32_t base, uint32_t size, uint8_t *host);
/* Host memory the core reads directly but writes through the bus (the
 * flash). */
void m16c_map_rom(m16c_core *c, uint32_t base, uint32_t size,
                  const uint8_t *host);

/* Reset: FLG and the registers cleared, PC from the reset vector at
 * 0xFFFFC. */
void      m16c_reset(m16c_core *c);
m16c_stop m16c_step(m16c_core *c, uint64_t budget, uint64_t *executed);

/* Makes m16c_step return after the current instruction. */
void m16c_yield(m16c_core *c);

/* Makes m16c_step return once the cycle count reaches cycles (0: no limit). */
void m16c_set_deadline(m16c_core *c, uint64_t cycles);

/* The highest-priority maskable request (software interrupt number vec,
 * priority level 1-7), or vec < 0 for none. The core takes it when I is set
 * and level is above IPL, then calls bus->ack. */
void m16c_set_irq(m16c_core *c, int vec, unsigned level);

/* CPU clock cycles since reset (an estimate per instruction, see
 * m16c_core.c). Time spent in WAIT is added with m16c_skip_cycles. */
uint64_t m16c_cycles(const m16c_core *c);
void     m16c_skip_cycles(m16c_core *c, uint64_t n);

uint32_t m16c_get_reg(const m16c_core *c, unsigned reg);
void     m16c_set_reg(m16c_core *c, unsigned reg, uint32_t val);
uint32_t m16c_trap_pc(const m16c_core *c);

/* Memory as the core sees it (host memory or the bus). */
uint32_t m16c_read(m16c_core *c, uint32_t addr, unsigned size);
void     m16c_write(m16c_core *c, uint32_t addr, uint32_t val, unsigned size);

/* One line per executed instruction (PC, bytes, mnemonic) to f while the
 * cycle count lies in [from, to). NULL stops it. */
void m16c_set_trace(m16c_core *c, FILE *f, uint64_t from, uint64_t to);

/* Stop before executing the instruction at pc (0 clears). */
void m16c_set_break(m16c_core *c, uint32_t pc);

/* Decodes the instruction in bytes[] (at least 12 valid bytes) at address
 * pc: returns its length, 0 if it has no decoding, and writes its mnemonic
 * in the form Ghidra's M16C/60 listing uses (e.g. "MOV.W:G", "JNE", "BMEQ")
 * to name when name is not NULL. */
unsigned m16c_disas(const uint8_t *bytes, uint32_t pc, char *name, size_t size);

#endif
