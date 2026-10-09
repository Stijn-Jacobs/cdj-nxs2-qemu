/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * M16C/60 instruction decoder: bytes to an m16c_insn, after the operation
 * code tables of the M16C/60, M16C/20 Series Software Manual (chapter 4).
 *
 * The first byte selects the instruction or a group. Within each run of
 * eight first bytes, x0/x1 are the :G two-operand form (.B/.W), x2 one of the
 * short single-register forms and x3-x7 the :S form on a 3-bit destination
 * (R0H, R0L, dsp:8[SB], dsp:8[FB], abs16). Add-on bytes follow in operand
 * order, except that the :S forms put the immediate before the destination's
 * add-on bytes and the :G forms after them.
 */
#include "m16c_priv.h"
#include <stdio.h>

typedef struct dec {
    const uint8_t *p;
    unsigned pos;
} dec;

static uint8_t u8(dec *d)
{
    return d->p[d->pos++];
}

static uint16_t u16(dec *d)
{
    uint16_t v = d->p[d->pos] | d->p[d->pos + 1] << 8;

    d->pos += 2;
    return v;
}

static uint32_t u20(dec *d)
{
    uint32_t v = d->p[d->pos] | d->p[d->pos + 1] << 8 | d->p[d->pos + 2] << 16;

    d->pos += 3;
    return v & M16C_ADDR_MASK;
}

static int32_t imm(dec *d, unsigned size)
{
    return size == 1 ? (int8_t)u8(d) : (int16_t)u16(d);
}

static m16c_opnd reg(unsigned kind, unsigned n)
{
    return (m16c_opnd){ .kind = kind, .reg = n };
}

static m16c_opnd mem(unsigned base, int32_t disp)
{
    return (m16c_opnd){ .kind = OK_MEM, .base = base, .disp = disp };
}

/* The 4-bit src/dest field. Size 4 is the 32-bit form of MUL.W, STC PC and
 * JMPI.A: R2R0, R3R1, A1A0 or memory. */
static m16c_opnd ea4(dec *d, unsigned code, unsigned size)
{
    static const uint8_t base[4] = { BR_A0, BR_A1, BR_SB, BR_FB };

    switch (code) {
    case 0: case 1: case 2: case 3:
        if (size == 4) {
            return code < 2 ? reg(OK_REG32, code) : (m16c_opnd){ 0 };
        }
        return reg(size == 1 ? OK_REG8 : OK_REG16, code);
    case 4: case 5:
        if (size == 4) {
            return code == 4 ? reg(OK_REG32, 2) : (m16c_opnd){ 0 };
        }
        return reg(OK_AREG, code - 4);
    case 6: case 7:
        return mem(base[code - 6], 0);
    case 8: case 9: case 10:
        return mem(base[code - 8], u8(d));
    case 11:
        return mem(BR_FB, (int8_t)u8(d));
    case 12: case 13: case 14:
        return mem(base[code - 12], u16(d));
    default:
        return mem(BR_NONE, u16(d));
    }
}

/* The 3-bit dest of the :S forms, 3-7. */
static m16c_opnd ea3(dec *d, unsigned code)
{
    switch (code & 7) {
    case 3: return reg(OK_REG8, 1);
    case 4: return reg(OK_REG8, 0);
    case 5: return mem(BR_SB, u8(d));
    case 6: return mem(BR_FB, (int8_t)u8(d));
    case 7: return mem(BR_NONE, u16(d));
    default: return (m16c_opnd){ 0 };
    }
}

/* The 2-bit src of the short register forms; with 0 it is the R0 half the
 * register field (bit 2) does not name. */
static m16c_opnd ea2(dec *d, unsigned b1)
{
    switch (b1 & 3) {
    case 0: return reg(OK_REG8, (b1 & 4) ? 0 : 1);
    case 1: return mem(BR_SB, u8(d));
    case 2: return mem(BR_FB, (int8_t)u8(d));
    default: return mem(BR_NONE, u16(d));
    }
}

static m16c_opnd r0half(unsigned b1)
{
    return reg(OK_REG8, (b1 >> 2) & 1);
}

/* The bit,base operand of the 0x7E group. An-relative forms count An in
 * bits from base; SB/FB-relative ones count base in bits from the register's
 * byte address. */
static int bitop(dec *d, unsigned code, m16c_bitop *b)
{
    memset(b, 0, sizeof(*b));
    switch (code) {
    case 0: case 1: case 2: case 3:
        b->reg = reg(OK_REG16, code);
        b->bit = u8(d) & 15;
        break;
    case 4: case 5:
        b->reg = reg(OK_AREG, code - 4);
        b->bit = u8(d) & 15;
        break;
    case 6: case 7:
        b->base = code == 6 ? BR_A0 : BR_A1;
        b->scale = 1;
        break;
    case 8: case 9:
        b->base = code == 8 ? BR_A0 : BR_A1;
        b->scale = 1;
        b->disp = u8(d) * 8;
        break;
    case 10:
        b->base = BR_SB;
        b->scale = 8;
        b->disp = u8(d);
        break;
    case 11:
        b->base = BR_FB;
        b->scale = 8;
        b->disp = (int8_t)u8(d);
        break;
    case 12: case 13:
        b->base = code == 12 ? BR_A0 : BR_A1;
        b->scale = 1;
        b->disp = u16(d) * 8;
        break;
    case 14:
        b->base = BR_SB;
        b->scale = 8;
        b->disp = u16(d);
        break;
    default:
        b->disp = u16(d);
        break;
    }
    return 1;
}

/* The cnd byte of BMcnd on memory or a register. */
static int cnd8(uint8_t v)
{
    static const int8_t lo[7] = {
        CND_GEU, CND_GTU, CND_EQ, CND_N, CND_LE, CND_O, CND_GE,
    };
    static const int8_t hi[7] = {
        CND_LTU, CND_LEU, CND_NE, CND_PZ, CND_GT, CND_NO, CND_LT,
    };

    if (v < 7) {
        return lo[v];
    }
    if (v >= 0xF8 && v < 0xFF) {
        return hi[v - 0xF8];
    }
    return -1;
}

/* A 4-bit shift count: bit 3 the direction (1 right), bits 0-2 the
 * distance less one. */
static int32_t shift4(unsigned n)
{
    int32_t v = (n & 7) + 1;

    return (n & 8) ? -v : v;
}

static void set(m16c_insn *in, unsigned op, unsigned size, char form)
{
    in->op = op;
    in->size = size;
    in->form = form;
}

/* 0x74/0x75: transfers, PUSH/POP, NEG/NOT and the R1H shifts. */
static void group74(dec *d, m16c_insn *in, unsigned size)
{
    uint8_t b2 = u8(d);
    unsigned code = b2 & 15;

    switch (b2 >> 4) {
    case 0x0: case 0x1: case 0x2:
        set(in, OP_STE, size, 0);
        in->src = ea4(d, code, size);
        in->dst = (b2 >> 4) == 0 ? mem(BR_NONE, u20(d))
                : (b2 >> 4) == 1 ? mem(BR_A0, u20(d)) : mem(BR_A1A0, 0);
        break;
    case 0x3:
        set(in, OP_MOV, size, 'G');
        in->src = ea4(d, code, size);
        in->dst = mem(BR_SP, (int8_t)u8(d));
        break;
    case 0x4:
        set(in, OP_PUSH, size, 'G');
        in->src = ea4(d, code, size);
        break;
    case 0x5:
        set(in, OP_NEG, size, 0);
        in->dst = ea4(d, code, size);
        break;
    case 0x6: case 0xE: case 0xF:
        set(in, (b2 >> 4) == 6 ? OP_ROT : (b2 >> 4) == 0xE ? OP_SHL : OP_SHA,
            size, 0);
        in->aux = 1;
        in->dst = ea4(d, code, size);
        break;
    case 0x7:
        set(in, OP_NOT, size, 0);
        in->dst = ea4(d, code, size);
        break;
    case 0x8: case 0x9: case 0xA:
        set(in, OP_LDE, size, 0);
        in->dst = ea4(d, code, size);
        in->src = (b2 >> 4) == 8 ? mem(BR_NONE, u20(d))
                : (b2 >> 4) == 9 ? mem(BR_A0, u20(d)) : mem(BR_A1A0, 0);
        break;
    case 0xB:
        set(in, OP_MOV, size, 'G');
        in->dst = ea4(d, code, size);
        in->src = mem(BR_SP, (int8_t)u8(d));
        break;
    case 0xC:
        set(in, OP_MOV, size, 'G');
        in->dst = ea4(d, code, size);
        in->src = reg(OK_IMM, 0);
        in->src.disp = imm(d, size);
        break;
    case 0xD:
        set(in, OP_POP, size, 'G');
        in->dst = ea4(d, code, size);
        break;
    }
}

/* 0x76/0x77: immediate ALU forms and the single-operand arithmetic. */
static void group76(dec *d, m16c_insn *in, unsigned size)
{
    static const uint8_t ops[16] = {
        OP_TST, OP_XOR, OP_AND, OP_OR, OP_ADD, OP_SUB, OP_ADC, OP_SBB,
        OP_CMP, OP_DIVX, OP_ROLC, OP_RORC, OP_DIVU, OP_DIV, OP_ADCF, OP_ABS,
    };
    uint8_t b2 = u8(d);
    unsigned n = b2 >> 4, op = ops[n];
    char form = (op == OP_XOR || op == OP_AND || op == OP_OR || op == OP_ADD ||
                 op == OP_SUB || op == OP_CMP) ? 'G' : 0;

    set(in, op, size, form);
    if (n == 9 || n == 0xC || n == 0xD) {
        in->src = ea4(d, b2 & 15, size);
        return;
    }
    in->dst = ea4(d, b2 & 15, size);
    if (n <= 8) {
        in->src = reg(OK_IMM, 0);
        in->src.disp = imm(d, size);
    }
}

/* Two-operand :G form with the src field in b2's high nibble. */
static void general(dec *d, m16c_insn *in, unsigned op, unsigned size,
                    char form)
{
    uint8_t b2 = u8(d);

    set(in, op, size, form);
    in->src = ea4(d, b2 >> 4, size);
    in->dst = ea4(d, b2 & 15, size);
}

static void group7c(dec *d, m16c_insn *in)
{
    static const uint8_t dec_ops[4] = { OP_DADD, OP_DSUB, OP_DADC, OP_DSBB };
    uint8_t b2 = u8(d);
    unsigned n = b2 >> 4;

    if (n <= 3 || (n >= 8 && n <= 0xB)) {
        set(in, OP_MOVDIR, 1, 0);
        in->cnd = n & 3;
        in->aux = n >= 8;
        in->dst = ea4(d, b2 & 15, 1);
        return;
    }
    switch (n) {
    case 0x4: case 0x5:
        set(in, n == 4 ? OP_MULU : OP_MUL, 1, 0);
        in->dst = ea4(d, b2 & 15, 1);
        in->src = reg(OK_IMM, 0);
        in->src.disp = n == 4 ? u8(d) : (int8_t)u8(d);
        return;
    case 0x6:
        set(in, OP_EXTS, 1, 0);
        in->dst = ea4(d, b2 & 15, 1);
        return;
    case 0xC:
        set(in, OP_STCPC, 4, 0);
        in->dst = ea4(d, b2 & 15, 4);
        return;
    }
    switch (b2) {
    case 0xE0: case 0xE1: case 0xE3:
        set(in, b2 == 0xE0 ? OP_DIVU : b2 == 0xE1 ? OP_DIV : OP_DIVX, 1, 0);
        in->src = reg(OK_IMM, 0);
        in->src.disp = b2 == 0xE0 ? u8(d) : (int8_t)u8(d);
        return;
    case 0xE2:
        set(in, OP_PUSH, 1, 'G');
        in->src = reg(OK_IMM, 0);
        in->src.disp = u8(d);
        return;
    case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        set(in, dec_ops[b2 - 0xE4], 1, 0);
        in->src = reg(OK_REG8, 1);
        in->dst = reg(OK_REG8, 0);
        return;
    case 0xEC: case 0xED: case 0xEE: case 0xEF:
        set(in, dec_ops[b2 - 0xEC], 1, 0);
        in->src = reg(OK_IMM, 0);
        in->src.disp = u8(d);
        in->dst = reg(OK_REG8, 0);
        return;
    case 0xE8: set(in, OP_SMOVF, 1, 0); return;
    case 0xE9: set(in, OP_SMOVB, 1, 0); return;
    case 0xEA: set(in, OP_SSTR, 1, 0); return;
    case 0xEB:
        set(in, OP_ADDSP, 1, 'G');
        in->imm = (int8_t)u8(d);
        return;
    case 0xF1: set(in, OP_RMPA, 1, 0); return;
    case 0xF2:
        set(in, OP_ENTER, 0, 0);
        in->imm = u8(d);
        return;
    case 0xF3:
        set(in, OP_EXTS, 2, 0);
        in->dst = reg(OK_REG16, 0);
        return;
    }
}

static void group7d(dec *d, m16c_insn *in)
{
    static const uint8_t dec_ops[4] = { OP_DADD, OP_DSUB, OP_DADC, OP_DSBB };
    uint8_t b2 = u8(d);
    unsigned n = b2 >> 4;

    switch (n) {
    case 0x0: case 0x1:
        set(in, n == 0 ? OP_JMPI : OP_JSRI, 4, 'A');
        in->src = ea4(d, b2 & 15, 4);
        return;
    case 0x2: case 0x3:
        set(in, n == 2 ? OP_JMPI : OP_JSRI, 2, 'W');
        in->src = ea4(d, b2 & 15, 2);
        return;
    case 0x4: case 0x5:
        set(in, n == 4 ? OP_MULU : OP_MUL, 2, 0);
        in->dst = ea4(d, b2 & 15, 2);
        in->src = reg(OK_IMM, 0);
        in->src.disp = n == 4 ? u16(d) : (int16_t)u16(d);
        return;
    case 0x9:
        if ((b2 & 8) == 0) {
            return;
        }
        set(in, OP_PUSHA, 2, 0);
        in->src = ea4(d, b2 & 15, 2);
        return;
    case 0xA:
        if (b2 & 8) {
            return;
        }
        set(in, OP_LDIPL, 0, 0);
        in->imm = b2 & 7;
        return;
    case 0xB:
        set(in, OP_ADDSP, 1, 'Q');
        in->imm = (int32_t)(b2 << 28) >> 28;
        return;
    case 0xC:
        if ((b2 & 8) == 0 || (b2 & 7) == 3 || (b2 & 7) == 7) {
            return;
        }
        set(in, OP_JCND, 0, 0);
        in->cnd = 8 + (b2 & 7);
        in->target = (in->pc + 2 + (int8_t)u8(d)) & M16C_ADDR_MASK;
        return;
    case 0xD:
        if ((b2 & 15) == 11 || (b2 & 15) == 15) {
            return;
        }
        set(in, OP_BMC, 0, 0);
        in->cnd = b2 & 15;
        return;
    }
    switch (b2) {
    case 0xE0: case 0xE1: case 0xE3:
        set(in, b2 == 0xE0 ? OP_DIVU : b2 == 0xE1 ? OP_DIV : OP_DIVX, 2, 0);
        in->src = reg(OK_IMM, 0);
        in->src.disp = b2 == 0xE0 ? u16(d) : (int16_t)u16(d);
        return;
    case 0xE2:
        set(in, OP_PUSH, 2, 'G');
        in->src = reg(OK_IMM, 0);
        in->src.disp = u16(d);
        return;
    case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        set(in, dec_ops[b2 - 0xE4], 2, 0);
        in->src = reg(OK_REG16, 1);
        in->dst = reg(OK_REG16, 0);
        return;
    case 0xEC: case 0xED: case 0xEE: case 0xEF:
        set(in, dec_ops[b2 - 0xEC], 2, 0);
        in->src = reg(OK_IMM, 0);
        in->src.disp = u16(d);
        in->dst = reg(OK_REG16, 0);
        return;
    case 0xE8: set(in, OP_SMOVF, 2, 0); return;
    case 0xE9: set(in, OP_SMOVB, 2, 0); return;
    case 0xEA: set(in, OP_SSTR, 2, 0); return;
    case 0xEB:
        set(in, OP_ADDSP, 2, 'G');
        in->imm = (int16_t)u16(d);
        return;
    case 0xF1: set(in, OP_RMPA, 2, 0); return;
    case 0xF2: set(in, OP_EXITD, 0, 0); return;
    case 0xF3: set(in, OP_WAIT, 0, 0); return;
    }
}

static void group7e(dec *d, m16c_insn *in)
{
    static const uint8_t ops[16] = {
        OP_BTSTC, OP_BTSTS, OP_BM, OP_BNTST, OP_BAND, OP_BNAND, OP_BOR,
        OP_BNOR, OP_BCLR, OP_BSET, OP_BNOT, OP_BTST, OP_BXOR, OP_BNXOR,
        OP_UNDEF, OP_UNDEF,
    };
    uint8_t b2 = u8(d);
    unsigned op = ops[b2 >> 4];

    if (op == OP_UNDEF) {
        return;
    }
    bitop(d, b2 & 15, &in->bit);
    set(in, op, 0, (op == OP_BCLR || op == OP_BSET || op == OP_BNOT ||
                    op == OP_BTST) ? 'G' : 0);
    if (op == OP_BM) {
        int cnd = cnd8(u8(d));

        if (cnd < 0) {
            in->op = OP_UNDEF;
            return;
        }
        in->cnd = cnd;
    }
}

static void groupeb(dec *d, m16c_insn *in)
{
    uint8_t b2 = u8(d);
    unsigned hi = (b2 >> 4) & 7;

    if (b2 & 0x80) {
        if (b2 >= 0xC0) {
            set(in, OP_INT, 0, 0);
            in->imm = b2 & 63;
            return;
        }
        set(in, (b2 & 0x20) ? OP_SHA : OP_SHL, 4, 0);
        in->dst = reg(OK_REG32, (b2 >> 4) & 1);
        in->imm = shift4(b2 & 15);
        return;
    }
    switch (b2 & 15) {
    case 0x0:
        if (hi == 0) {
            return;
        }
        set(in, OP_LDC, 2, 0);
        in->cnd = hi;
        in->src = reg(OK_IMM, 0);
        in->src.disp = u16(d);
        return;
    case 0x1:
        if (hi > 3) {
            return;
        }
        set(in, (hi & 2) ? OP_SHA : OP_SHL, 4, 0);
        in->aux = 1;
        in->dst = reg(OK_REG32, hi & 1);
        return;
    case 0x2: case 0x3:
        if (hi == 0) {
            return;
        }
        set(in, (b2 & 15) == 2 ? OP_PUSHC : OP_POPC, 2, 0);
        in->cnd = hi;
        return;
    case 0x4: case 0x5:
        set(in, (b2 & 15) == 4 ? OP_FSET : OP_FCLR, 0, 0);
        in->cnd = hi;
        return;
    }
    if ((b2 & 8) && hi < 6) {
        set(in, OP_MOVA, 2, 0);
        in->src = ea4(d, b2 & 15, 2);
        in->dst = hi < 4 ? reg(OK_REG16, hi) : reg(OK_AREG, hi - 4);
    }
}

/* :S form on a 3-bit destination with an immediate first. */
static void short_imm(dec *d, m16c_insn *in, unsigned op, char form,
                      unsigned b1)
{
    set(in, op, 1, form);
    in->src = reg(OK_IMM, 0);
    in->src.disp = op == OP_MOV || op == OP_AND || op == OP_OR ||
                   op == OP_STZ || op == OP_STNZ || op == OP_STZX
                 ? u8(d) : (int8_t)u8(d);
    in->dst = ea3(d, b1);
}

static void quick(dec *d, m16c_insn *in, unsigned op, unsigned b1)
{
    uint8_t b2 = u8(d);

    set(in, op, (b1 & 1) + 1, 'Q');
    in->src = reg(OK_IMM, 0);
    in->src.disp = (int32_t)(b2 << 24) >> 28;
    in->dst = ea4(d, b2 & 15, in->size);
}

static void shift_imm(dec *d, m16c_insn *in, unsigned op, unsigned b1)
{
    uint8_t b2 = u8(d);

    set(in, op, (b1 & 1) + 1, 0);
    in->imm = shift4(b2 >> 4);
    in->dst = ea4(d, b2 & 15, in->size);
}

unsigned m16c_decode(const uint8_t *p, uint32_t pc, m16c_insn *in)
{
    static const uint8_t s2_ops[8] = {
        OP_MOV, OP_MOV, OP_AND, OP_OR, OP_ADD, OP_SUB, OP_MOV, OP_CMP,
    };
    static const uint8_t s3_ops[16] = {
        OP_ADD, OP_SUB, OP_AND, OP_OR, OP_INC, OP_DEC, OP_MOV, OP_NOT,
        OP_MOV, OP_STZ, OP_STNZ, OP_STZX, OP_CMP, OP_UNDEF, OP_UNDEF, OP_UNDEF,
    };
    static const uint8_t g_ops[16] = {
        OP_TST, OP_XOR, OP_AND, OP_OR, OP_ADD, OP_SUB, OP_ADC, OP_SBB,
        OP_CMP, OP_UNDEF, OP_UNDEF, OP_UNDEF, OP_UNDEF, OP_UNDEF, OP_UNDEF,
        OP_UNDEF,
    };
    dec d = { p, 1 };
    uint8_t b1 = p[0];

    memset(in, 0, sizeof(*in));
    in->pc = pc;

    if (b1 < 0x40) {
        unsigned grp = b1 >> 3;

        if (b1 == 0x00) {
            set(in, OP_BRK, 0, 0);
        } else if (b1 == 0x04) {
            set(in, OP_NOP, 0, 0);
        } else if (grp == 0) {
            set(in, OP_MOV, 1, 'S');
            in->src = r0half(b1);
            in->dst = ea2(&d, b1);
        } else {
            set(in, s2_ops[grp], 1, 'S');
            in->src = ea2(&d, b1);
            in->dst = grp == 6 ? reg(OK_AREG, (b1 >> 2) & 1) : r0half(b1);
        }
    } else if (b1 < 0x60) {
        static const uint8_t ops[4] = { OP_BCLR, OP_BSET, OP_BNOT, OP_BTST };

        set(in, ops[(b1 >> 3) & 3], 0, 'S');
        in->bit.base = BR_SB;
        in->bit.scale = 8;
        in->bit.disp = u8(&d) << 3 | (b1 & 7);
    } else if (b1 < 0x68) {
        set(in, OP_JMP, 0, 'S');
        in->target = (pc + 2 + (b1 & 7)) & M16C_ADDR_MASK;
    } else if (b1 < 0x70) {
        set(in, OP_JCND, 0, 0);
        in->cnd = b1 & 7;
        in->target = (pc + 1 + (int8_t)u8(&d)) & M16C_ADDR_MASK;
    } else if (b1 < 0x80) {
        unsigned size = (b1 & 1) + 1;

        switch (b1 & 0xFE) {
        case 0x70: general(&d, in, OP_MULU, size, 0); break;
        case 0x72: general(&d, in, OP_MOV, size, 'G'); break;
        case 0x74: group74(&d, in, size); break;
        case 0x76: group76(&d, in, size); break;
        case 0x78: general(&d, in, OP_MUL, size, 0); break;
        case 0x7A: {
            uint8_t b2 = u8(&d);

            if (b2 & 0x80) {
                set(in, b1 == 0x7A ? OP_LDC : OP_STC, 2, 0);
                in->cnd = (b2 >> 4) & 7;
                if (in->cnd == 0) {
                    in->op = OP_UNDEF;
                    break;
                }
                if (b1 == 0x7A) {
                    in->src = ea4(&d, b2 & 15, 2);
                } else {
                    in->dst = ea4(&d, b2 & 15, 2);
                }
            } else if ((b2 & 0x40) == 0) {
                set(in, OP_XCHG, size, 0);
                in->src = reg(size == 1 ? OK_REG8 : OK_REG16, (b2 >> 4) & 3);
                in->dst = ea4(&d, b2 & 15, size);
            }
            break;
        }
        case 0x7C:
            if (b1 == 0x7C) {
                group7c(&d, in);
            } else {
                group7d(&d, in);
            }
            break;
        case 0x7E:
            if (b1 == 0x7E) {
                group7e(&d, in);
            }
            break;
        }
    } else {
        unsigned grp = (b1 - 0x80) >> 3, low = b1 & 7;

        if (low >= 3) {
            if (s3_ops[grp] != OP_UNDEF) {
                unsigned op = s3_ops[grp];

                if (op == OP_INC || op == OP_DEC || op == OP_NOT) {
                    set(in, op, 1, op == OP_NOT ? 'S' : 0);
                    in->dst = ea3(&d, b1);
                } else if (grp == 6) {
                    set(in, OP_MOV, 1, 'Z');
                    in->src = reg(OK_IMM, 0);
                    in->dst = ea3(&d, b1);
                } else {
                    short_imm(&d, in, op, (op == OP_STZ || op == OP_STNZ ||
                                           op == OP_STZX) ? 0 : 'S', b1);
                    if (op == OP_STZX) {
                        in->imm2 = u8(&d);
                    }
                }
            } else if (b1 == 0xEB) {
                groupeb(&d, in);
            } else if (b1 == 0xEC || b1 == 0xED) {
                set(in, b1 == 0xEC ? OP_PUSHM : OP_POPM, 0, 0);
                in->imm = u8(&d);
            } else if (b1 == 0xEE || b1 == 0xEF) {
                set(in, b1 == 0xEE ? OP_JMPS : OP_JSRS, 0, 0);
                in->imm = u8(&d);
            } else if (b1 == 0xF3) {
                set(in, OP_RTS, 0, 0);
            } else if (b1 == 0xF4 || b1 == 0xF5) {
                set(in, b1 == 0xF4 ? OP_JMP : OP_JSR, 0, 'W');
                in->target = (pc + 1 + (int16_t)u16(&d)) & M16C_ADDR_MASK;
            } else if (b1 == 0xF6) {
                set(in, OP_INTO, 0, 0);
            } else if (b1 == 0xFB) {
                set(in, OP_REIT, 0, 0);
            } else if (b1 == 0xFC || b1 == 0xFD) {
                set(in, b1 == 0xFC ? OP_JMP : OP_JSR, 0, 'A');
                in->target = u20(&d);
            } else if (b1 == 0xFE) {
                set(in, OP_JMP, 0, 'B');
                in->target = (pc + 1 + (int8_t)u8(&d)) & M16C_ADDR_MASK;
            } else if (b1 == 0xFF) {
                set(in, OP_UND, 0, 0);
            }
        } else if (low == 2) {
            unsigned x = (b1 >> 3) & 1;

            switch (b1 & 0xF7) {
            case 0x82:
                set(in, OP_PUSH, 1, 'S');
                in->src = reg(OK_REG8, x);
                break;
            case 0x92:
                set(in, OP_POP, 1, 'S');
                in->dst = reg(OK_REG8, x);
                break;
            case 0xA2:
                set(in, OP_MOV, 2, 'S');
                in->src = reg(OK_IMM, 0);
                in->src.disp = u16(&d);
                in->dst = reg(OK_AREG, x);
                break;
            case 0xB2: case 0xF2:
                set(in, b1 < 0xC0 ? OP_INC : OP_DEC, 2, 0);
                in->dst = reg(OK_AREG, x);
                break;
            case 0xC2:
                set(in, OP_PUSH, 2, 'S');
                in->src = reg(OK_AREG, x);
                break;
            case 0xD2:
                set(in, OP_POP, 2, 'S');
                in->dst = reg(OK_AREG, x);
                break;
            case 0xE2:
                set(in, OP_MOV, 1, 'S');
                in->src = reg(OK_IMM, 0);
                in->src.disp = u8(&d);
                in->dst = reg(OK_AREG, x);
                break;
            }
        } else {
            unsigned size = (b1 & 1) + 1;

            if (grp < 9) {
                unsigned op = g_ops[grp];

                general(&d, in, op, size,
                        (op == OP_TST || op == OP_ADC || op == OP_SBB) ? 0 : 'G');
            } else {
                switch (b1 & 0xFE) {
                case 0xC8: quick(&d, in, OP_ADD, b1); break;
                case 0xD0: quick(&d, in, OP_CMP, b1); break;
                case 0xD8: quick(&d, in, OP_MOV, b1); break;
                case 0xE0: shift_imm(&d, in, OP_ROT, b1); break;
                case 0xE8: shift_imm(&d, in, OP_SHL, b1); break;
                case 0xF0: shift_imm(&d, in, OP_SHA, b1); break;
                case 0xF8: {
                    uint8_t b2 = u8(&d);

                    set(in, OP_ADJNZ, size, 0);
                    in->imm = (int32_t)(b2 << 24) >> 28;
                    in->dst = ea4(&d, b2 & 15, size);
                    in->target = (pc + 2 + (int8_t)u8(&d)) & M16C_ADDR_MASK;
                    break;
                }
                }
            }
        }
    }
    if (in->op == OP_UNDEF) {
        return 0;
    }
    in->len = d.pos;
    return d.pos;
}

static const char *const cnd_names[16] = {
    "GEU", "GTU", "EQ", "N", "LTU", "LEU", "NE", "PZ",
    "LE", "O", "GE", "?", "GT", "NO", "LT", "?",
};

static const char *const op_names[OP_COUNT] = {
    [OP_MOV] = "MOV", [OP_MOVA] = "MOVA", [OP_LDE] = "LDE", [OP_STE] = "STE",
    [OP_PUSH] = "PUSH", [OP_POP] = "POP", [OP_PUSHA] = "PUSHA",
    [OP_PUSHM] = "PUSHM", [OP_POPM] = "POPM", [OP_PUSHC] = "PUSHC",
    [OP_POPC] = "POPC", [OP_LDC] = "LDC", [OP_STC] = "STC",
    [OP_STCPC] = "STC", [OP_XCHG] = "XCHG", [OP_STZ] = "STZ",
    [OP_STNZ] = "STNZ", [OP_STZX] = "STZX", [OP_LDINTB] = "LDINTB",
    [OP_ADD] = "ADD", [OP_ADC] = "ADC", [OP_ADCF] = "ADCF", [OP_SUB] = "SUB",
    [OP_SBB] = "SBB", [OP_CMP] = "CMP", [OP_NEG] = "NEG", [OP_ABS] = "ABS",
    [OP_INC] = "INC", [OP_DEC] = "DEC", [OP_MUL] = "MUL", [OP_MULU] = "MULU",
    [OP_DIV] = "DIV", [OP_DIVU] = "DIVU", [OP_DIVX] = "DIVX",
    [OP_EXTS] = "EXTS", [OP_DADD] = "DADD", [OP_DADC] = "DADC",
    [OP_DSUB] = "DSUB", [OP_DSBB] = "DSBB", [OP_ADDSP] = "ADD",
    [OP_RMPA] = "RMPA", [OP_AND] = "AND", [OP_OR] = "OR", [OP_XOR] = "XOR",
    [OP_TST] = "TST", [OP_NOT] = "NOT", [OP_SHL] = "SHL", [OP_SHA] = "SHA",
    [OP_ROT] = "ROT", [OP_ROLC] = "ROLC", [OP_RORC] = "RORC",
    [OP_BTST] = "BTST", [OP_BTSTC] = "BTSTC", [OP_BTSTS] = "BTSTS",
    [OP_BSET] = "BSET", [OP_BCLR] = "BCLR", [OP_BNOT] = "BNOT",
    [OP_BAND] = "BAND", [OP_BNAND] = "BNAND", [OP_BOR] = "BOR",
    [OP_BNOR] = "BNOR", [OP_BXOR] = "BXOR", [OP_BNXOR] = "BNXOR",
    [OP_BNTST] = "BNTST", [OP_JMP] = "JMP", [OP_JMPI] = "JMPI",
    [OP_JMPS] = "JMPS", [OP_JSR] = "JSR", [OP_JSRI] = "JSRI",
    [OP_JSRS] = "JSRS", [OP_RTS] = "RTS", [OP_ADJNZ] = "ADJNZ",
    [OP_ENTER] = "ENTER", [OP_EXITD] = "EXITD", [OP_REIT] = "REIT",
    [OP_INT] = "INT", [OP_INTO] = "INTO", [OP_BRK] = "BRK", [OP_UND] = "UND",
    [OP_WAIT] = "WAIT", [OP_NOP] = "NOP", [OP_FSET] = "FSET",
    [OP_FCLR] = "FCLR", [OP_LDIPL] = "LDIPL", [OP_SMOVF] = "SMOVF",
    [OP_SMOVB] = "SMOVB", [OP_SSTR] = "SSTR",
};

void m16c_insn_name(const m16c_insn *in, char *name, size_t size)
{
    static const char *const dir[4] = { "LL", "HL", "LH", "HH" };
    static const char sizes[5] = { 0, 'B', 'W', 0, 'L' };

    switch (in->op) {
    case OP_JCND:
        snprintf(name, size, "J%s", cnd_names[in->cnd]);
        return;
    case OP_BM: case OP_BMC:
        snprintf(name, size, "BM%s", cnd_names[in->cnd]);
        return;
    case OP_MOVDIR:
        snprintf(name, size, "MOV%s", dir[in->cnd]);
        return;
    case OP_JMP: case OP_JSR: case OP_JMPI: case OP_JSRI:
        snprintf(name, size, "%s.%c", op_names[in->op], in->form);
        return;
    }
    if (in->size && in->op != OP_MOVA && in->op != OP_LDC &&
        in->op != OP_STC && in->op != OP_STCPC && in->op != OP_PUSHC &&
        in->op != OP_POPC && in->op != OP_PUSHA && in->op != OP_STZ &&
        in->op != OP_STNZ && in->op != OP_STZX) {
        if (in->form) {
            snprintf(name, size, "%s.%c:%c", op_names[in->op], sizes[in->size],
                     in->form);
        } else {
            snprintf(name, size, "%s.%c", op_names[in->op], sizes[in->size]);
        }
    } else if (in->form) {
        snprintf(name, size, "%s:%c", op_names[in->op], in->form);
    } else {
        snprintf(name, size, "%s", op_names[in->op]);
    }
}

unsigned m16c_disas(const uint8_t *bytes, uint32_t pc, char *name, size_t size)
{
    m16c_insn in;
    unsigned len = m16c_decode(bytes, pc, &in);

    if (len && name) {
        m16c_insn_name(&in, name, size);
    }
    return len;
}
