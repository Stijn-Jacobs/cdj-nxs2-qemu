/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * M16C instruction semantics, from the Renesas M16C/60, M16C/20 Series
 * Software Manual: operand access, flags and one case per operation. The
 * decimal arithmetic (DADD and friends) and RMPA are not implemented; the
 * GUI firmware does not use them and m16c_exec reports them undefined.
 */
#include "m16c_priv.h"
#include <stdlib.h>

#define MASK(size) ((size) == 1 ? 0xFFu : (size) == 2 ? 0xFFFFu : 0xFFFFFFFFu)
#define SIGN(size) ((size) == 1 ? 0x80u : (size) == 2 ? 0x8000u : 0x80000000u)

enum { CREG_INTBL = 1, CREG_INTBH, CREG_FLG, CREG_ISP, CREG_SP, CREG_SB,
       CREG_FB };

static uint32_t a0a1(m16c_core *c, unsigned n)
{
    return c->a[m16c_bank(c)][n];
}

/* A 16-bit data address: the sum wraps within 64 KiB. */
static uint32_t ea(m16c_core *c, const m16c_opnd *o)
{
    uint32_t base = 0;

    switch (o->base) {
    case BR_A0: base = a0a1(c, 0); break;
    case BR_A1: base = a0a1(c, 1); break;
    case BR_SB: base = c->sb; break;
    case BR_FB: base = c->fb[m16c_bank(c)]; break;
    case BR_SP: base = *m16c_sp(c); break;
    }
    return (base + o->disp) & 0xFFFF;
}

/* The 20-bit address of LDE and STE. */
static uint32_t ea20(m16c_core *c, const m16c_opnd *o)
{
    uint32_t base = 0;

    switch (o->base) {
    case BR_A0: base = a0a1(c, 0); break;
    case BR_A1A0: base = a0a1(c, 1) << 16 | a0a1(c, 0); break;
    }
    return (base + o->disp) & M16C_ADDR_MASK;
}

static uint32_t get(m16c_core *c, const m16c_opnd *o, unsigned size)
{
    unsigned bank = m16c_bank(c);
    uint16_t w;

    switch (o->kind) {
    case OK_IMM:
        return o->disp & MASK(size);
    case OK_REG8:
        w = c->r[bank][o->reg >> 1];
        return (o->reg & 1) ? w >> 8 : w & 0xFF;
    case OK_REG16:
        return c->r[bank][o->reg];
    case OK_AREG:
        return c->a[bank][o->reg] & MASK(size);
    case OK_REG32:
        return o->reg == 2 ? (uint32_t)c->a[bank][1] << 16 | c->a[bank][0]
                           : (uint32_t)c->r[bank][o->reg + 2] << 16 |
                             c->r[bank][o->reg];
    case OK_MEM:
        return m16c_read(c, ea(c, o), size);
    }
    return 0;
}

static void put(m16c_core *c, const m16c_opnd *o, unsigned size, uint32_t v)
{
    unsigned bank = m16c_bank(c);
    uint16_t *w;

    switch (o->kind) {
    case OK_REG8:
        w = &c->r[bank][o->reg >> 1];
        *w = (o->reg & 1) ? (*w & 0xFF) | (v & 0xFF) << 8
                          : (*w & 0xFF00) | (v & 0xFF);
        break;
    case OK_REG16:
        c->r[bank][o->reg] = v;
        break;
    case OK_AREG:
        c->a[bank][o->reg] = v & MASK(size);
        break;
    case OK_REG32:
        if (o->reg == 2) {
            c->a[bank][0] = v;
            c->a[bank][1] = v >> 16;
        } else {
            c->r[bank][o->reg] = v;
            c->r[bank][o->reg + 2] = v >> 16;
        }
        break;
    case OK_MEM:
        m16c_write(c, ea(c, o), v, size);
        break;
    }
}

static void flag(m16c_core *c, unsigned bit, int on)
{
    c->flg = on ? c->flg | bit : c->flg & ~bit;
}

static void set_sz(m16c_core *c, uint32_t v, unsigned size)
{
    flag(c, M16C_Z, (v & MASK(size)) == 0);
    flag(c, M16C_S, v & SIGN(size));
}

static uint32_t add(m16c_core *c, uint32_t a, uint32_t b, uint32_t cin,
                    unsigned size)
{
    uint64_t sum = (uint64_t)a + b + cin;
    uint32_t v = sum & MASK(size);

    flag(c, M16C_C, sum > MASK(size));
    flag(c, M16C_O, ~(a ^ b) & (a ^ v) & SIGN(size));
    set_sz(c, v, size);
    return v;
}

/* C is set when the subtraction does not borrow. */
static uint32_t sub(m16c_core *c, uint32_t a, uint32_t b, uint32_t borrow,
                    unsigned size)
{
    uint32_t v = (a - b - borrow) & MASK(size);

    flag(c, M16C_C, (uint64_t)a >= (uint64_t)b + borrow);
    flag(c, M16C_O, (a ^ b) & (a ^ v) & SIGN(size));
    set_sz(c, v, size);
    return v;
}

static int cond(const m16c_core *c, unsigned cnd)
{
    unsigned f = c->flg;
    int cf = f & M16C_C, zf = f & M16C_Z, sf = f & M16C_S, of = f & M16C_O;
    int lt = !sf != !of;

    switch (cnd) {
    case CND_GEU: return cf;
    case CND_GTU: return cf && !zf;
    case CND_EQ:  return zf;
    case CND_N:   return sf;
    case CND_LTU: return !cf;
    case CND_LEU: return !cf || zf;
    case CND_NE:  return !zf;
    case CND_PZ:  return !sf;
    case CND_LE:  return lt || zf;
    case CND_O:   return of;
    case CND_GE:  return !lt;
    case CND_GT:  return !lt && !zf;
    case CND_NO:  return !of;
    case CND_LT:  return lt;
    }
    return 0;
}

static uint32_t creg_get(m16c_core *c, unsigned n)
{
    switch (n) {
    case CREG_INTBL: return c->intb & 0xFFFF;
    case CREG_INTBH: return c->intb >> 16;
    case CREG_FLG:   return c->flg;
    case CREG_ISP:   return c->isp;
    case CREG_SP:    return *m16c_sp(c);
    case CREG_SB:    return c->sb;
    case CREG_FB:    return c->fb[m16c_bank(c)];
    }
    return 0;
}

static void creg_set(m16c_core *c, unsigned n, uint32_t v)
{
    switch (n) {
    case CREG_INTBL: c->intb = (c->intb & 0xF0000) | (v & 0xFFFF); break;
    case CREG_INTBH: c->intb = (c->intb & 0xFFFF) | (v & 0xF) << 16; break;
    case CREG_FLG:   c->flg = v & FLG_VALID; break;
    case CREG_ISP:   c->isp = v; break;
    case CREG_SP:    *m16c_sp(c) = v; break;
    case CREG_SB:    c->sb = v; break;
    case CREG_FB:    c->fb[m16c_bank(c)] = v; break;
    }
}

/* PUSHM and POPM name registers by mask bit: PUSHM bit 0 is FB and bit 7
 * R0, POPM the other way round, and both walk the mask from bit 0 up, so R0
 * is pushed last and lands at the lowest address: the caller of a function
 * taking a far pointer in R2R0 pushes R0 and R2, and the callee reads the
 * pointer back as two words, low word first. Every register is two bytes. */
static uint16_t *pushm_reg(m16c_core *c, unsigned bit)
{
    unsigned bank = m16c_bank(c);

    switch (bit) {
    case 0: return &c->fb[bank];
    case 1: return &c->sb;
    case 2: return &c->a[bank][1];
    case 3: return &c->a[bank][0];
    default: return &c->r[bank][7 - bit];
    }
}

/* A bit operand: a bit of a register, or a byte address and bit number. */
typedef struct bitloc {
    const m16c_opnd *reg;
    uint32_t addr;
    unsigned bit;
} bitloc;

static bitloc bit_locate(m16c_core *c, const m16c_bitop *b)
{
    bitloc loc = { 0 };
    int32_t base = 0, bitaddr;

    if (b->reg.kind != OK_NONE) {
        loc.reg = &b->reg;
        loc.bit = b->bit;
        return loc;
    }
    switch (b->base) {
    case BR_A0: base = a0a1(c, 0); break;
    case BR_A1: base = a0a1(c, 1); break;
    case BR_SB: base = c->sb; break;
    case BR_FB: base = c->fb[m16c_bank(c)]; break;
    }
    bitaddr = base * b->scale + b->disp;
    loc.addr = (bitaddr >> 3) & 0xFFFF;
    loc.bit = bitaddr & 7;
    return loc;
}

static unsigned bit_get(m16c_core *c, const bitloc *loc)
{
    uint32_t v = loc->reg ? get(c, loc->reg, 2) : m16c_read(c, loc->addr, 1);

    return v >> loc->bit & 1;
}

static void bit_put(m16c_core *c, const bitloc *loc, unsigned on)
{
    if (loc->reg) {
        uint32_t v = get(c, loc->reg, 2);

        put(c, loc->reg, 2, on ? v | 1u << loc->bit : v & ~(1u << loc->bit));
    } else {
        uint32_t v = m16c_read(c, loc->addr, 1);

        m16c_write(c, loc->addr, on ? v | 1u << loc->bit : v & ~(1u << loc->bit), 1);
    }
}

static void bit_op(m16c_core *c, const m16c_insn *in)
{
    bitloc loc = bit_locate(c, &in->bit);
    unsigned b = bit_get(c, &loc), cf = c->flg & M16C_C ? 1 : 0;

    switch (in->op) {
    case OP_BTST:  flag(c, M16C_Z, !b); flag(c, M16C_C, b); break;
    case OP_BNTST: flag(c, M16C_Z, b); flag(c, M16C_C, !b); break;
    case OP_BTSTC:
        flag(c, M16C_Z, !b); flag(c, M16C_C, b);
        bit_put(c, &loc, 0);
        break;
    case OP_BTSTS:
        flag(c, M16C_Z, !b); flag(c, M16C_C, b);
        bit_put(c, &loc, 1);
        break;
    case OP_BSET:  bit_put(c, &loc, 1); break;
    case OP_BCLR:  bit_put(c, &loc, 0); break;
    case OP_BNOT:  bit_put(c, &loc, !b); break;
    case OP_BAND:  flag(c, M16C_C, cf & b); break;
    case OP_BNAND: flag(c, M16C_C, cf & !b); break;
    case OP_BOR:   flag(c, M16C_C, cf | b); break;
    case OP_BNOR:  flag(c, M16C_C, cf | !b); break;
    case OP_BXOR:  flag(c, M16C_C, cf ^ b); break;
    case OP_BNXOR: flag(c, M16C_C, cf ^ !b); break;
    case OP_BM:    bit_put(c, &loc, cond(c, in->cnd)); break;
    }
}

/* The count is signed: positive shifts or rotates left. */
static void shift(m16c_core *c, const m16c_insn *in)
{
    unsigned size = in->size, bits = size * 8;
    int32_t count = in->aux ? (int8_t)(get(c, &(m16c_opnd){ .kind = OK_REG8, .reg = 3 }, 1))
                            : in->imm;
    uint64_t v = get(c, &in->dst, size), r, m = MASK(size);
    int64_t sv = (v & SIGN(size)) ? (int64_t)v - (int64_t)(m + 1) : (int64_t)v;
    unsigned n = abs(count), carry;

    if (count == 0) {
        return;
    }
    if (n > bits) {
        n = bits;
    }
    switch (in->op) {
    case OP_SHL:
        if (count > 0) {
            r = v << n;
            carry = n <= bits ? (v >> (bits - n)) & 1 : 0;
        } else {
            r = v >> n;
            carry = (v >> (n - 1)) & 1;
        }
        break;
    case OP_SHA:
        if (count > 0) {
            r = (uint64_t)sv << n;
            carry = (v >> (bits - n)) & 1;
            flag(c, M16C_O, ((r ^ v) & SIGN(size)) != 0);
        } else {
            r = (uint64_t)(sv >> n);
            carry = (uint64_t)(sv >> (n - 1)) & 1;
            flag(c, M16C_O, 0);
        }
        break;
    default:
        n %= bits;
        if (count > 0) {
            r = n ? (v << n | v >> (bits - n)) : v;
            carry = r & 1;
        } else {
            r = n ? (v >> n | v << (bits - n)) : v;
            carry = (r >> (bits - 1)) & 1;
        }
        break;
    }
    flag(c, M16C_C, carry);
    set_sz(c, r, size);
    put(c, &in->dst, size, r & m);
}

static void rotate_carry(m16c_core *c, const m16c_insn *in)
{
    unsigned size = in->size, cf = c->flg & M16C_C ? 1 : 0;
    uint32_t v = get(c, &in->dst, size), r;

    if (in->op == OP_ROLC) {
        flag(c, M16C_C, v & SIGN(size));
        r = (v << 1 | cf) & MASK(size);
    } else {
        flag(c, M16C_C, v & 1);
        r = v >> 1 | (cf ? SIGN(size) : 0);
    }
    set_sz(c, r, size);
    put(c, &in->dst, size, r);
}

/* The 16 or 32-bit result of MUL and MULU goes to the register pair or
 * memory the destination names: R0 to R2R0, R1 to R3R1, A0 to A1A0 for
 * words; the 16-bit register holding the byte for bytes. */
static int multiply(m16c_core *c, const m16c_insn *in)
{
    unsigned size = in->size, bank = m16c_bank(c);
    int sgn = in->op == OP_MUL;
    uint32_t a = get(c, &in->dst, size), b = get(c, &in->src, size);
    int64_t pa = sgn && (a & SIGN(size)) ? (int64_t)a - (int64_t)(MASK(size) + 1) : a;
    int64_t pb = sgn && (b & SIGN(size)) ? (int64_t)b - (int64_t)(MASK(size) + 1) : b;
    uint32_t p = (uint32_t)(pa * pb);
    const m16c_opnd *d = &in->dst;

    if (size == 1) {
        switch (d->kind) {
        case OK_REG8: c->r[bank][d->reg >> 1] = p; break;
        case OK_AREG: c->a[bank][d->reg] = p; break;
        case OK_MEM: m16c_write(c, ea(c, d), p, 2); break;
        default: return 1;
        }
        return 0;
    }
    switch (d->kind) {
    case OK_REG16:
        if (d->reg > 1) {
            return 1;
        }
        c->r[bank][d->reg] = p;
        c->r[bank][d->reg + 2] = p >> 16;
        break;
    case OK_AREG:
        if (d->reg) {
            return 1;
        }
        c->a[bank][0] = p;
        c->a[bank][1] = p >> 16;
        break;
    case OK_MEM:
        m16c_write(c, ea(c, d), p, 2);
        m16c_write(c, ea(c, d) + 2, p >> 16, 2);
        break;
    default:
        return 1;
    }
    return 0;
}

/* DIVU, DIV and DIVX: a 16-bit dividend in R0 for byte divisors, R2R0 for
 * words. Quotient to R0L or R0, remainder to R0H or R2. A zero divisor or a
 * quotient that does not fit sets O and leaves the registers alone. */
static void divide(m16c_core *c, const m16c_insn *in)
{
    unsigned size = in->size, bank = m16c_bank(c);
    int sgn = in->op != OP_DIVU;
    uint32_t dividend = size == 1 ? c->r[bank][0]
                                  : (uint32_t)c->r[bank][2] << 16 | c->r[bank][0];
    uint32_t d = get(c, &in->src, size);
    int64_t n, m, q, r;

    if (sgn) {
        n = size == 1 ? (int16_t)dividend : (int32_t)dividend;
        m = size == 1 ? (int8_t)d : (int16_t)d;
    } else {
        n = dividend;
        m = d;
    }
    if (m == 0) {
        flag(c, M16C_O, 1);
        return;
    }
    q = n / m;
    r = n % m;
    if (in->op == OP_DIVX && r && ((r < 0) != (m < 0))) {
        q--;
        r += m;
    }
    if (sgn ? (q < -(int64_t)(SIGN(size)) || q >= (int64_t)SIGN(size))
            : q > (int64_t)MASK(size)) {
        flag(c, M16C_O, 1);
        return;
    }
    flag(c, M16C_O, 0);
    if (size == 1) {
        c->r[bank][0] = (q & 0xFF) | (r & 0xFF) << 8;
    } else {
        c->r[bank][0] = q;
        c->r[bank][2] = r;
    }
}

static void string_op(m16c_core *c, const m16c_insn *in)
{
    unsigned size = in->size, bank = m16c_bank(c);
    uint16_t *r3 = &c->r[bank][3], *a0 = &c->a[bank][0], *a1 = &c->a[bank][1];

    while (*r3) {
        uint32_t src = ((c->r[bank][1] >> 8) & 0xF) << 16 | *a0;

        switch (in->op) {
        case OP_SSTR:
            m16c_write(c, *a1, c->r[bank][0], size);
            break;
        case OP_SMOVF:
            m16c_write(c, *a1, m16c_read(c, src, size), size);
            *a0 += size;
            break;
        default:
            m16c_write(c, *a1, m16c_read(c, src, size), size);
            *a0 -= size;
            break;
        }
        *a1 += in->op == OP_SMOVB ? -size : size;
        (*r3)--;
    }
}

static uint32_t flow_target(m16c_core *c, const m16c_insn *in)
{
    uint32_t v;

    if (in->src.kind == OK_MEM) {
        /* The jump tables in the GUI image follow the instruction and the
         * displacement is their low 16 bits, so the table is read from the
         * 64 KiB page the instruction is in. */
        uint32_t addr = ea(c, &in->src) | (in->pc & 0xF0000);

        v = m16c_read(c, addr, 2);
        if (in->size == 4) {
            v |= (m16c_read(c, addr + 2, 1) & 0xF) << 16;
        }
    } else {
        v = get(c, &in->src, in->size);
    }
    return in->size == 4 ? v & M16C_ADDR_MASK
                         : (in->pc + (int16_t)v) & M16C_ADDR_MASK;
}

static void push_pc(m16c_core *c, uint32_t pc)
{
    m16c_push(c, pc >> 16, 1);
    m16c_push(c, pc & 0xFFFF, 2);
}

static uint32_t pop_pc(m16c_core *c)
{
    uint32_t lo = m16c_pop(c, 2);

    return lo | (m16c_pop(c, 1) & 0xF) << 16;
}

static unsigned extra_cycles(unsigned op)
{
    switch (op) {
    case OP_MUL: case OP_MULU: return 3;
    case OP_DIV: case OP_DIVU: case OP_DIVX: return 16;
    case OP_SSTR: case OP_SMOVF: case OP_SMOVB: return 8;
    }
    return 0;
}

int m16c_exec(m16c_core *c, const m16c_insn *in)
{
    unsigned size = in->size, bank = m16c_bank(c);
    uint32_t next = in->pc + in->len, v, w;
    unsigned n;

    c->pc = next;
    c->cycles += in->len + 1 + extra_cycles(in->op);

    switch (in->op) {
    case OP_MOV:
        v = get(c, &in->src, size);
        put(c, &in->dst, size, v);
        set_sz(c, v, size);
        break;
    case OP_MOVA:
        put(c, &in->dst, 2, ea(c, &in->src));
        break;
    case OP_MOVDIR: {
        m16c_opnd r0l = { .kind = OK_REG8 };
        const m16c_opnd *s = in->aux ? &r0l : &in->dst;
        const m16c_opnd *d = in->aux ? &in->dst : &r0l;
        unsigned sh = in->cnd & 1 ? 4 : 0, dh = in->cnd & 2 ? 4 : 0;

        v = get(c, d, 1) & ~(0xFu << dh);
        put(c, d, 1, v | (get(c, s, 1) >> sh & 0xF) << dh);
        break;
    }
    case OP_LDE:
        v = m16c_read(c, ea20(c, &in->src), size);
        put(c, &in->dst, size, v);
        set_sz(c, v, size);
        break;
    case OP_STE:
        v = get(c, &in->src, size);
        m16c_write(c, ea20(c, &in->dst), v, size);
        set_sz(c, v, size);
        break;
    case OP_PUSH:
        m16c_push(c, get(c, &in->src, size), size);
        break;
    case OP_POP:
        put(c, &in->dst, size, m16c_pop(c, size));
        break;
    case OP_PUSHA:
        m16c_push(c, ea(c, &in->src), 2);
        break;
    case OP_PUSHM:
        for (n = 0; n < 8; n++) {
            if (in->imm & 1u << n) {
                m16c_push(c, *pushm_reg(c, n), 2);
            }
        }
        break;
    case OP_POPM:
        for (n = 0; n < 8; n++) {
            if (in->imm & 1u << n) {
                *pushm_reg(c, 7 - n) = m16c_pop(c, 2);
            }
        }
        break;
    case OP_PUSHC:
        m16c_push(c, creg_get(c, in->cnd), 2);
        break;
    case OP_POPC:
        creg_set(c, in->cnd, m16c_pop(c, 2));
        break;
    case OP_LDC:
        creg_set(c, in->cnd, get(c, &in->src, 2));
        break;
    case OP_STC:
        put(c, &in->dst, 2, creg_get(c, in->cnd));
        break;
    case OP_STCPC:
        if (in->dst.kind == OK_REG32) {
            put(c, &in->dst, 4, next);
        } else {
            m16c_write(c, ea(c, &in->dst), next & 0xFFFF, 2);
            m16c_write(c, ea(c, &in->dst) + 2, next >> 16, 2);
        }
        break;
    case OP_XCHG:
        v = get(c, &in->src, size);
        w = get(c, &in->dst, size);
        put(c, &in->src, size, w);
        put(c, &in->dst, size, v);
        break;
    case OP_STZ:
        if (c->flg & M16C_Z) {
            put(c, &in->dst, 1, in->src.disp);
        }
        break;
    case OP_STNZ:
        if (!(c->flg & M16C_Z)) {
            put(c, &in->dst, 1, in->src.disp);
        }
        break;
    case OP_STZX:
        put(c, &in->dst, 1, (c->flg & M16C_Z) ? in->src.disp : in->imm2);
        break;

    case OP_ADD:
        put(c, &in->dst, size, add(c, get(c, &in->dst, size), get(c, &in->src, size), 0, size));
        break;
    case OP_ADC:
        put(c, &in->dst, size, add(c, get(c, &in->dst, size), get(c, &in->src, size), (c->flg & M16C_C) != 0, size));
        break;
    case OP_ADCF:
        put(c, &in->dst, size, add(c, get(c, &in->dst, size), 0, (c->flg & M16C_C) != 0, size));
        break;
    case OP_SUB:
        put(c, &in->dst, size, sub(c, get(c, &in->dst, size), get(c, &in->src, size), 0, size));
        break;
    case OP_SBB:
        put(c, &in->dst, size, sub(c, get(c, &in->dst, size), get(c, &in->src, size), !(c->flg & M16C_C), size));
        break;
    case OP_CMP:
        sub(c, get(c, &in->dst, size), get(c, &in->src, size), 0, size);
        break;
    case OP_NEG:
        put(c, &in->dst, size, sub(c, 0, get(c, &in->dst, size), 0, size));
        break;
    case OP_ABS:
        v = get(c, &in->dst, size);
        if (v & SIGN(size)) {
            v = sub(c, 0, v, 0, size);
        } else {
            flag(c, M16C_O, 0);
            set_sz(c, v, size);
        }
        put(c, &in->dst, size, v);
        break;
    case OP_INC:
        v = (get(c, &in->dst, size) + 1) & MASK(size);
        put(c, &in->dst, size, v);
        set_sz(c, v, size);
        break;
    case OP_DEC:
        v = (get(c, &in->dst, size) - 1) & MASK(size);
        put(c, &in->dst, size, v);
        set_sz(c, v, size);
        break;
    case OP_MUL: case OP_MULU:
        if (multiply(c, in)) {
            return 1;
        }
        break;
    case OP_DIV: case OP_DIVU: case OP_DIVX:
        divide(c, in);
        break;
    case OP_EXTS:
        if (size == 2) {
            c->r[bank][2] = (int16_t)c->r[bank][0] < 0 ? 0xFFFF : 0;
            set_sz(c, c->r[bank][2] << 16 | c->r[bank][0], 4);
        } else if (in->dst.kind == OK_MEM) {
            v = (int8_t)get(c, &in->dst, 1);
            m16c_write(c, ea(c, &in->dst), v, 2);
            set_sz(c, v, 2);
        } else {
            v = (int8_t)get(c, &in->dst, 1);
            c->r[bank][in->dst.reg >> 1] = v;
            set_sz(c, v, 2);
        }
        break;
    case OP_ADDSP:
        *m16c_sp(c) = add(c, *m16c_sp(c), in->imm & 0xFFFF, 0, 2);
        break;

    case OP_AND:
        v = get(c, &in->dst, size) & get(c, &in->src, size);
        put(c, &in->dst, size, v);
        set_sz(c, v, size);
        break;
    case OP_OR:
        v = get(c, &in->dst, size) | get(c, &in->src, size);
        put(c, &in->dst, size, v);
        set_sz(c, v, size);
        break;
    case OP_XOR:
        v = get(c, &in->dst, size) ^ get(c, &in->src, size);
        put(c, &in->dst, size, v);
        set_sz(c, v, size);
        break;
    case OP_TST:
        set_sz(c, get(c, &in->dst, size) & get(c, &in->src, size), size);
        break;
    case OP_NOT:
        v = ~get(c, &in->dst, size) & MASK(size);
        put(c, &in->dst, size, v);
        set_sz(c, v, size);
        break;
    case OP_SHL: case OP_SHA: case OP_ROT:
        shift(c, in);
        break;
    case OP_ROLC: case OP_RORC:
        rotate_carry(c, in);
        break;

    case OP_BTST: case OP_BTSTC: case OP_BTSTS: case OP_BSET: case OP_BCLR:
    case OP_BNOT: case OP_BAND: case OP_BNAND: case OP_BOR: case OP_BNOR:
    case OP_BXOR: case OP_BNXOR: case OP_BNTST: case OP_BM:
        bit_op(c, in);
        break;
    case OP_BMC:
        flag(c, M16C_C, cond(c, in->cnd));
        break;

    case OP_JCND:
        if (cond(c, in->cnd)) {
            c->pc = in->target;
        }
        break;
    case OP_JMP:
        c->pc = in->target;
        break;
    case OP_JMPI:
        c->pc = flow_target(c, in);
        break;
    case OP_JMPS:
        c->pc = 0xF0000 | m16c_read(c, 0xFFFFE - in->imm * 2, 2);
        break;
    case OP_JSR:
        push_pc(c, next);
        c->pc = in->target;
        break;
    case OP_JSRI:
        push_pc(c, next);
        c->pc = flow_target(c, in);
        break;
    case OP_JSRS:
        push_pc(c, next);
        c->pc = 0xF0000 | m16c_read(c, 0xFFFFE - in->imm * 2, 2);
        break;
    case OP_RTS:
        c->pc = pop_pc(c);
        break;
    case OP_ADJNZ:
        v = (get(c, &in->dst, size) + in->imm) & MASK(size);
        put(c, &in->dst, size, v);
        if (v) {
            c->pc = in->target;
        }
        break;
    case OP_ENTER:
        m16c_push(c, c->fb[bank], 2);
        c->fb[bank] = *m16c_sp(c);
        *m16c_sp(c) -= in->imm;
        break;
    case OP_EXITD:
        *m16c_sp(c) = c->fb[bank];
        c->fb[bank] = m16c_pop(c, 2);
        c->pc = pop_pc(c);
        break;
    case OP_REIT:
        m16c_reit(c);
        break;
    case OP_INT:
        m16c_int(c, in->imm, next);
        break;
    case OP_INTO:
        if (c->flg & M16C_O) {
            m16c_into(c, next);
        }
        break;
    case OP_BRK:
        m16c_brk(c, next);
        break;
    case OP_UND:
        m16c_und(c, next);
        break;
    case OP_WAIT:
        c->waiting = 1;
        break;
    case OP_NOP:
        break;
    case OP_FSET:
        c->flg |= 1u << in->cnd;
        break;
    case OP_FCLR:
        c->flg &= ~(1u << in->cnd);
        break;
    case OP_LDIPL:
        c->flg = (c->flg & ~FLG_IPL) | in->imm << 12;
        break;
    case OP_SMOVF: case OP_SMOVB: case OP_SSTR:
        string_op(c, in);
        break;
    default:
        return 1;
    }
    return 0;
}
