/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C66x instruction decoder, driven directly by GNU binutils' tic6x tables
 * (hw/cdj/c6x/binutils/). Produces a structured form for the interpreter and,
 * on request, the exact text tic6x-elf-objdump prints, so the decoder can be
 * checked against objdump rather than being its own opinion of the ISA.
 */
#ifndef C66X_DECODE_H
#define C66X_DECODE_H

#include <stdbool.h>
#include <stdint.h>

/* Register numbering used everywhere in the core: A0..A31 = 0..31,
 * B0..B31 = 32..63 (C64x+ has 32 registers per side). */
#define C66X_NREGS 64
#define C66X_A(n) (n)
#define C66X_B(n) (32 + (n))

enum c66x_opk {
    C66X_OPK_NONE = 0,
    C66X_OPK_CONST,    /* val                                        */
    C66X_OPK_REG,      /* reg                                        */
    C66X_OPK_PAIR,     /* reg = low (even), reg_hi = high            */
    C66X_OPK_MEM,      /* reg = base, mode, offset const or offreg   */
    C66X_OPK_CTRL,     /* val = index into tic6x_ctrl_table          */
    C66X_OPK_ADDR,     /* val = absolute branch/PC-relative target   */
    C66X_OPK_UNITS,    /* val = spmask unit bits (bit i: "LSDM"[i/2], side i&1) */
    C66X_OPK_IRP,
    C66X_OPK_NRP,
    C66X_OPK_ILC,
    C66X_OPK_FSTG,     /* spkernel stage count, val                  */
    C66X_OPK_FCYC,     /* spkernel cycle count, val                  */
};

typedef struct c66x_operand {
    uint8_t kind;
    uint8_t size;       /* bytes: 1, 2, 4, 5 (40-bit pair) or 8        */
    uint8_t rw;         /* tic6x_rw                                    */
    uint8_t low_first, low_last, high_first, high_last;
    uint8_t reg, reg_hi;
    uint8_t mem_mode;   /* binutils mem_mode: 0 -, 1 +, 8 --x, 9 ++x, 10 x--, 11 x++, |4 = register offset */
    uint8_t mem_offreg;
    uint8_t mem_scale;  /* multiply the offset (const or register) by this */
    uint8_t xpath;      /* read through the 1X/2X cross path (can stall)  */
    int32_t val;
} c66x_operand;

typedef struct c66x_insn {
    uint32_t addr;
    uint32_t opcode;     /* for 16-bit: dsz/br/sat folded in, as binutils */
    int16_t  opc;        /* index into tic6x_opcode_table, -1 = undefined, -2 = header */
    uint8_t  size;       /* 2 or 4 bytes                                   */
    uint8_t  nops;
    int8_t   cond_reg;   /* -1 = unconditional                             */
    uint8_t  cond_z;     /* condition true when register == 0             */
    uint8_t  p;          /* next instruction is in the same execute packet */
    int8_t   unit;       /* 0..7 = L1 L2 S1 S2 D1 D2 M1 M2, -1 = none      */
    uint8_t  side;       /* 1 = A, 2 = B, 0 = no functional unit           */
    uint8_t  prot;       /* fetch packet header PROT                       */
    uint8_t  compact;    /* came from a header-based fetch packet          */
    uint16_t handler;    /* filled in by the interpreter                   */
    c66x_operand op[4];
    /* Interpreter caches; the decoder zeroes them. */
    uint8_t  sub;        /* handler variant (load size, shift, field selects) */
    uint8_t  xread;      /* reads a register through a cross path            */
    uint8_t  rmask;      /* operands read before execution                   */
    uint8_t  regmask;    /* ... of which plain 32-bit registers              */
    uint8_t  constmask;  /* ... of which constants                           */
    uint8_t  xnops;      /* NOP cycles inserted after the packet (NOP n, BNOP, CALLP, PROT load) */
    uint8_t  isbranch;   /* blocks interrupts for the next five packets      */
    uint8_t  fop;        /* nonzero: a plain 32-bit register form the fast loop runs inline */
    uint8_t  pk_n;       /* when this insn starts a packet: its length, 0 = unknown */
    uint8_t  pk_mask;    /* SPMASK units in that packet                       */
    uint8_t  pk_xread;
    uint8_t  pk_special; /* packet has an SPLOOP-family instruction: slow path */
    uint8_t  pk_load;    /* packet has a load: its stores must wait for the cycle end */
    uint8_t  pk_xnops;   /* the NOP cycles the packet inserts (max over its instructions) */
    uint8_t  pk_branched;/* ... has a branch instruction                         */
    uint8_t  pk_allfop;  /* every instruction has a fast form                    */
    uint64_t pk_xmask;   /* registers read through a cross path: stall if written at E1 */
    int32_t  ea_delta;   /* load/store at base register + this, no write-back (mfast) */
    uint8_t  mfast;      /* nonzero: that form; bits 0-2 size, 3 sign-extend, 4 pre-/5 post-modify base, 6 stdw */
    uint32_t pk_next;    /* address after that packet                         */
} c66x_insn;

/* Reads the 32 bytes of a fetch packet; nonzero = not readable. */
typedef int (*c66x_fp_reader)(void *opaque, uint32_t fp_addr, uint8_t fp[32]);

/* Decode the instruction at addr. text (may be NULL) receives objdump's
 * rendering without the leading "|| ", which needs the previous instruction's
 * p-bit and is added by c66x_prev_parallel(). Returns the instruction size in
 * bytes as objdump would advance (1 for a bad offset, 4 for a header). */
int c66x_decode(c66x_fp_reader rd, void *opaque, uint32_t addr,
                c66x_insn *out, char *text, unsigned textlen);

/* Whether the instruction at addr is in parallel with the one before it. */
bool c66x_prev_parallel(c66x_fp_reader rd, void *opaque, uint32_t addr);

const char *c66x_opcode_name(int opc);
unsigned c66x_opcode_count(void);
unsigned c66x_opcode_flags(int opc);     /* TIC6X_FLAG_*              */
unsigned c66x_opcode_pipeline(int opc);  /* tic6x_pipeline_type       */
unsigned c66x_opcode_unit(int opc);      /* tic6x_func_unit_base      */
const char *c66x_ctrl_name(int ctrl);
unsigned c66x_ctrl_crlo(int ctrl);

#endif
