/* SPDX-License-Identifier: GPL-2.0-or-later */
/* M16C core internals, shared by m16c_core.c, m16c_decode.c and m16c_exec.c. */
#ifndef M16C_PRIV_H
#define M16C_PRIV_H

#include "m16c.h"
#include <string.h>

#define M16C_PAGE_BITS 10
#define M16C_PAGES     (1u << (20 - M16C_PAGE_BITS))
#define M16C_ADDR_MASK 0xFFFFFu

/* FLG bits the core keeps: C D Z S B O I U and IPL. */
#define FLG_IPL   0x7000u
#define FLG_VALID 0x70FFu

/* Operand kinds. A byte operation on A0/A1 reads the low byte and writes the
 * register zero-extended. */
enum {
    OK_NONE, OK_REG8, OK_REG16, OK_AREG, OK_REG32, OK_MEM, OK_IMM,
};

/* Base registers of a memory operand. */
enum { BR_NONE, BR_A0, BR_A1, BR_SB, BR_FB, BR_SP, BR_A1A0 };

typedef struct m16c_opnd {
    uint8_t kind;
    uint8_t reg;        /* REG8 R0L R0H R1L R1H; REG16 R0-R3; AREG A0 A1;
                           REG32 R2R0 R3R1 A1A0                           */
    uint8_t base;       /* MEM: BR_*                                      */
    int32_t disp;       /* MEM: displacement or absolute address; IMM     */
} m16c_opnd;

/* Bit operand: bit `bit` of a register, or bit address
 * base_value * scale + disp (byte bitaddr >> 3, bit bitaddr & 7). */
typedef struct m16c_bitop {
    m16c_opnd reg;      /* kind OK_NONE for a memory bit                  */
    uint8_t bit;
    uint8_t base;       /* BR_*                                           */
    uint8_t scale;      /* 1 when base holds a bit address (An), 8 when a
                           byte address (SB, FB), 0 for an absolute one   */
    int32_t disp;
} m16c_bitop;

enum m16c_op {
    OP_UNDEF,
    /* transfer */
    OP_MOV, OP_MOVA, OP_MOVDIR, OP_LDE, OP_STE, OP_PUSH, OP_POP, OP_PUSHA,
    OP_PUSHM, OP_POPM, OP_PUSHC, OP_POPC, OP_LDC, OP_STC, OP_STCPC, OP_XCHG,
    OP_STZ, OP_STNZ, OP_STZX, OP_LDINTB,
    /* arithmetic */
    OP_ADD, OP_ADC, OP_ADCF, OP_SUB, OP_SBB, OP_CMP, OP_NEG, OP_ABS, OP_INC,
    OP_DEC, OP_MUL, OP_MULU, OP_DIV, OP_DIVU, OP_DIVX, OP_EXTS, OP_DADD,
    OP_DADC, OP_DSUB, OP_DSBB, OP_ADDSP, OP_RMPA,
    /* logic and shifts */
    OP_AND, OP_OR, OP_XOR, OP_TST, OP_NOT, OP_SHL, OP_SHA, OP_ROT, OP_ROLC,
    OP_RORC,
    /* bits */
    OP_BTST, OP_BTSTC, OP_BTSTS, OP_BSET, OP_BCLR, OP_BNOT, OP_BAND, OP_BNAND,
    OP_BOR, OP_BNOR, OP_BXOR, OP_BNXOR, OP_BNTST, OP_BM, OP_BMC,
    /* flow */
    OP_JCND, OP_JMP, OP_JMPI, OP_JMPS, OP_JSR, OP_JSRI, OP_JSRS, OP_RTS,
    OP_ADJNZ, OP_ENTER, OP_EXITD, OP_REIT, OP_INT, OP_INTO, OP_BRK, OP_UND,
    OP_WAIT, OP_NOP, OP_FSET, OP_FCLR, OP_LDIPL,
    /* strings */
    OP_SMOVF, OP_SMOVB, OP_SSTR,
    OP_COUNT,
};

/* Condition codes, numbered as the 4-bit field of BMcnd C. */
enum {
    CND_GEU, CND_GTU, CND_EQ, CND_N, CND_LTU, CND_LEU, CND_NE, CND_PZ,
    CND_LE, CND_O, CND_GE, CND_GT = 12, CND_NO, CND_LT,
};

typedef struct m16c_insn {
    uint32_t pc;
    uint8_t len;
    uint8_t op;         /* enum m16c_op                                    */
    uint8_t size;       /* operand size in bytes: 1, 2 (or 4 for .L)       */
    char    form;       /* 'G', 'Q', 'S', 'Z', 'A' (abs), 'W' (word) or 0  */
    uint8_t cnd;        /* CND_*; MOVDIR direction; creg; flag bit         */
    uint8_t aux;        /* MOVDIR: 1 when R0L is the source; shifts: 1 when
                           the count is R1H; JMP/JSR: indirect absolute     */
    m16c_opnd src, dst;
    m16c_bitop bit;
    int32_t imm, imm2;
    uint32_t target;
} m16c_insn;

struct m16c_core {
    uint16_t r[2][4];           /* R0-R3 per bank                         */
    uint16_t a[2][2];           /* A0, A1 per bank                        */
    uint16_t fb[2];
    uint16_t sb, usp, isp, flg;
    uint32_t intb, pc;

    int      irq_vec;
    unsigned irq_level;
    int      waiting;
    int      yield;
    uint32_t brk, trap_pc;
    int      brk_skip;          /* resuming from the break address        */
    uint64_t cycles, deadline;

    FILE    *trace;
    uint64_t trace_from, trace_to;

    const uint8_t *rd[M16C_PAGES];
    uint8_t       *wr[M16C_PAGES];
    m16c_bus bus;
};

/* m16c_decode.c */
unsigned m16c_decode(const uint8_t *p, uint32_t pc, m16c_insn *in);
void     m16c_insn_name(const m16c_insn *in, char *name, size_t size);

/* m16c_exec.c: runs one decoded instruction; nonzero if it has no
 * implementation. Sets c->pc. */
int m16c_exec(m16c_core *c, const m16c_insn *in);

/* m16c_core.c */
void     m16c_push(m16c_core *c, uint32_t val, unsigned size);
uint32_t m16c_pop(m16c_core *c, unsigned size);
void     m16c_interrupt(m16c_core *c, uint32_t vector_addr, int hw_level,
                        int keep_u, uint32_t ret_pc);
void     m16c_reit(m16c_core *c);
void     m16c_int(m16c_core *c, unsigned n, uint32_t ret_pc);
void     m16c_into(m16c_core *c, uint32_t ret_pc);
void     m16c_brk(m16c_core *c, uint32_t ret_pc);
void     m16c_und(m16c_core *c, uint32_t ret_pc);

static inline uint16_t *m16c_sp(m16c_core *c)
{
    return (c->flg & M16C_U) ? &c->usp : &c->isp;
}

static inline unsigned m16c_bank(const m16c_core *c)
{
    return (c->flg & M16C_B) ? 1 : 0;
}

#endif
