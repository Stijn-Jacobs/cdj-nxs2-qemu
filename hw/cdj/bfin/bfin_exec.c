/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Blackfin instruction semantics: the 16- and 32-bit control, load/store,
 * DAG and loop groups and the parallel bundle. The DSP32 ALU, shift and
 * multiply/accumulate groups are in bfin_dsp.c.
 *
 * Group and field names follow the Programming Reference's encoding tables.
 * A form the display firmware has not needed yet sets c->undef, which stops
 * the step loop at that instruction.
 */
#include "bfin_dsp.h"

static inline int32_t sext(uint32_t v, unsigned bits)
{
    return (int32_t)(v << (32 - bits)) >> (32 - bits);
}

/* Loads inside a bundle write back at its end, after every slot has read its
 * operands. */
static void load_reg(bfin_core *c, unsigned grp, unsigned reg, uint32_t v)
{
    if (c->in_bundle) {
        c->load[c->nload].grp = grp;
        c->load[c->nload].reg = reg;
        c->load[c->nload].val = v;
        c->nload++;
    } else if (grp < 2) {
        (grp ? c->p : c->r)[reg] = v;
    } else {
        bfin_set_reg(c, grp, reg, v);
    }
}

/* DAG post-modify with the circular buffer of I[n]: B[n] base, L[n] length. */
static uint32_t dag_add(bfin_core *c, unsigned n, int32_t m)
{
    uint32_t i = c->i[n], l = c->l[n], b = c->b[n];
    uint32_t r = i + m;

    if (l) {
        if (m >= 0 && r >= b + l && i < b + l) {
            r -= l;
        } else if (m < 0 && r < b && i >= b) {
            r += l;
        }
    }
    return r;
}

static uint32_t brev_add(uint32_t a, uint32_t b)
{
    uint32_t ra = 0, rb = 0, r;

    for (int k = 0; k < 32; k++) {
        ra |= ((a >> k) & 1) << (31 - k);
        rb |= ((b >> k) & 1) << (31 - k);
    }
    r = ra + rb;
    a = 0;
    for (int k = 0; k < 32; k++) {
        a |= ((r >> k) & 1) << (31 - k);
    }
    return a;
}

/* ---- 16-bit groups ------------------------------------------------------ */

static void prog_ctrl(bfin_core *c, uint16_t iw, uint16_t pad)
{
    unsigned op = iw & 0xF;

    switch ((iw >> 4) & 0xF) {
    case 0:
        if (op) {
            c->undef = 1;
        }
        return;
    case 1:
        switch (op) {
        case 0: c->npc = c->rets; return;
        case 1: bfin_return(c, EV_IVHW); return;
        case 2: bfin_return(c, EV_EVX); return;
        case 3: bfin_return(c, EV_NMI); return;
        case 4: bfin_return(c, EV_EMU); return;
        }
        break;
    case 2:
        switch (op) {
        case 0:
            c->idle = !c->wake;
            c->wake = 0;
            return;
        case 3: case 4:                         /* CSYNC, SSYNC */
            return;
        }
        break;
    case 3: bfin_cli(c, op & 7); return;
    case 4: bfin_sti(c, c->r[op & 7]); return;
    case 5: c->npc = c->p[op & 7]; return;
    case 6: c->rets = c->npc; c->npc = c->p[op & 7]; return;
    case 7: c->rets = c->npc; c->npc = c->pc + c->p[op & 7]; return;
    case 8: c->npc = c->pc + c->p[op & 7]; return;
    case 9: bfin_raise(c, op); return;
    case 10: bfin_exception(c, op, c->npc); return;
    case 11: {
        uint32_t a = c->p[op & 7];
        uint8_t v = bfin_load(c, a, 1);

        c->cc = !v;
        bfin_store(c, a, v | 0x80, 1);
        return;
    }
    }
    c->undef = 1;
}

static void push_pop_reg(bfin_core *c, uint16_t iw, uint16_t pad)
{
    unsigned reg = iw & 7, grp = (iw >> 3) & 7;

    if (iw & 0x40) {
        uint32_t v = bfin_reg(c, grp, reg);

        c->p[6] -= 4;
        bfin_store(c, c->p[6], v, 4);
        if (grp == 7 && reg == 3) {
            bfin_reti_pushed(c, 1);
        }
    } else {
        uint32_t v = bfin_load(c, c->p[6], 4);

        c->p[6] += 4;
        bfin_set_reg(c, grp, reg, v);
        if (grp == 7 && reg == 3) {
            bfin_reti_pushed(c, 0);
        }
    }
}

static void push_pop_multiple(bfin_core *c, uint16_t iw, uint16_t pad)
{
    unsigned pr = iw & 7, dr = (iw >> 3) & 7;
    int d = iw & 0x100, p = iw & 0x80;
    uint32_t sp = c->p[6];

    /* The lowest-numbered register is pushed first, so R7 and P5 end up
     * nearest SP. ThreadX's context save stores single registers into the
     * frame its restore pops with (R7:0, P5:0), and only this order fits. */
    if (iw & 0x40) {
        if (d) {
            for (unsigned n = dr; n <= 7; n++) {
                sp -= 4;
                bfin_store(c, sp, c->r[n], 4);
            }
        }
        if (p) {
            for (unsigned n = pr; n <= 5; n++) {
                sp -= 4;
                bfin_store(c, sp, c->p[n], 4);
            }
        }
    } else {
        if (p) {
            for (int n = 5; n >= (int)pr; n--) {
                c->p[n] = bfin_load(c, sp, 4);
                sp += 4;
            }
        }
        if (d) {
            for (int n = 7; n >= (int)dr; n--) {
                c->r[n] = bfin_load(c, sp, 4);
                sp += 4;
            }
        }
    }
    c->p[6] = sp;
}

/* CCflag, one handler per opcode; the register compares share the operand
 * fetch and the flags. */
static inline void cc_operands(bfin_core *c, uint16_t iw, int sign,
                               uint32_t *a, uint32_t *b)
{
    unsigned x = iw & 7, y = (iw >> 3) & 7;
    int preg = iw & 0x40;

    *a = preg ? c->p[x] : c->r[x];
    *b = iw & 0x400 ? (sign ? (uint32_t)sext(y, 3) : y) : preg ? c->p[y] : c->r[y];
}

static inline void cc_flags(bfin_core *c, uint16_t iw, uint32_t a, uint32_t b)
{
    if (!(iw & 0x40)) {
        bfin_flags_nz(c, a - b);
        bfin_flags_ac0(c, b <= a);
    }
}

static void cc_eq(bfin_core *c, uint16_t iw, uint16_t pad)
{
    uint32_t a, b;

    cc_operands(c, iw, 1, &a, &b);
    c->cc = a == b;
    cc_flags(c, iw, a, b);
}

static void cc_lt(bfin_core *c, uint16_t iw, uint16_t pad)
{
    uint32_t a, b;

    cc_operands(c, iw, 1, &a, &b);
    c->cc = (int32_t)a < (int32_t)b;
    cc_flags(c, iw, a, b);
}

static void cc_le(bfin_core *c, uint16_t iw, uint16_t pad)
{
    uint32_t a, b;

    cc_operands(c, iw, 1, &a, &b);
    c->cc = (int32_t)a <= (int32_t)b;
    cc_flags(c, iw, a, b);
}

static void cc_ltu(bfin_core *c, uint16_t iw, uint16_t pad)
{
    uint32_t a, b;

    cc_operands(c, iw, 0, &a, &b);
    c->cc = a < b;
    cc_flags(c, iw, a, b);
}

static void cc_leu(bfin_core *c, uint16_t iw, uint16_t pad)
{
    uint32_t a, b;

    cc_operands(c, iw, 0, &a, &b);
    c->cc = a <= b;
    cc_flags(c, iw, a, b);
}

static void cc_acc(bfin_core *c, uint16_t iw, uint16_t pad)
{
    unsigned opc = (iw >> 7) & 7;
    int64_t a0 = c->a[0], a1 = c->a[1];

    c->cc = opc == 5 ? a0 == a1 : opc == 6 ? a0 < a1 : a0 <= a1;
}

static bfin_op *const cc_flag[8] = {
    cc_eq, cc_lt, cc_le, cc_ltu, cc_leu, cc_acc, cc_acc, cc_acc,
};

static void cc2stat(bfin_core *c, uint16_t iw, uint16_t pad)
{
    unsigned bit = iw & 0x1F, op = (iw >> 5) & 3;
    uint32_t astat = bfin_astat(c);
    int s = (astat >> bit) & 1, r;

    if (iw & 0x80) {
        r = op == 0 ? c->cc : op == 1 ? s | c->cc : op == 2 ? s & c->cc : s ^ c->cc;
        bfin_set_astat(c, (astat & ~(1u << bit)) | ((uint32_t)r << bit));
    } else {
        c->cc = op == 0 ? s : op == 1 ? c->cc | s : op == 2 ? c->cc & s : c->cc ^ s;
    }
}

static uint32_t shift_reg(bfin_core *c, unsigned kind, uint32_t v, uint32_t n)
{
    uint32_t r;

    switch (kind) {
    case 0:                                     /* >>> */
        r = n > 31 ? (uint32_t)((int32_t)v >> 31) : (uint32_t)((int32_t)v >> n);
        break;
    case 1:                                     /* >> */
        r = n > 31 ? 0 : v >> n;
        break;
    default:                                    /* << */
        r = n > 31 ? 0 : v << n;
        break;
    }
    bfin_flags_nz(c, r);
    bfin_flags_v(c, 0);
    return r;
}

static void alu2op(bfin_core *c, uint16_t iw, uint16_t pad)
{
    unsigned dst = iw & 7, src = (iw >> 3) & 7, opc = (iw >> 6) & 0xF;
    uint32_t *d = &c->r[dst], s = c->r[src];

    switch (opc) {
    case 0: *d = shift_reg(c, 0, *d, s); return;
    case 1: *d = shift_reg(c, 1, *d, s); return;
    case 2: *d = shift_reg(c, 2, *d, s); return;
    case 3: *d *= s; return;
    case 4: *d = (*d + s) << 1; bfin_flags_nz(c, *d); return;
    case 5: *d = (*d + s) << 2; bfin_flags_nz(c, *d); return;
    case 8: bfin_divq(c, dst, src); return;
    case 9: bfin_divs(c, dst, src); return;
    case 10: *d = (uint32_t)(int16_t)s; bfin_flags_nz(c, *d); return;
    case 11: *d = (uint16_t)s; bfin_flags_nz(c, *d); return;
    case 12: *d = (uint32_t)(int8_t)s; bfin_flags_nz(c, *d); return;
    case 13: *d = (uint8_t)s; bfin_flags_nz(c, *d); return;
    case 14: *d = bfin_add32(c, 0, s, 1, 0); return;
    case 15: *d = ~s; bfin_flags_nz(c, *d); return;
    }
    c->undef = 1;
}

static void ptr2op(bfin_core *c, uint16_t iw, uint16_t pad)
{
    unsigned dst = iw & 7, src = (iw >> 3) & 7, opc = (iw >> 6) & 7;
    uint32_t *d = &c->p[dst], s = c->p[src];

    switch (opc) {
    case 0: *d -= s; return;
    case 1: *d = s << 2; return;
    case 3: *d = s >> 2; return;
    case 4: *d = s >> 1; return;
    case 5: *d = brev_add(*d, s); return;
    case 6: *d = (*d + s) << 1; return;
    case 7: *d = (*d + s) << 2; return;
    }
    c->undef = 1;
}

static void logi2op(bfin_core *c, uint16_t iw, uint16_t pad)
{
    unsigned dst = iw & 7, n = (iw >> 3) & 0x1F, opc = (iw >> 8) & 7;
    uint32_t *d = &c->r[dst];

    switch (opc) {
    case 0: c->cc = !((*d >> n) & 1); return;
    case 1: c->cc = (*d >> n) & 1; return;
    case 2: *d |= 1u << n; break;
    case 3: *d ^= 1u << n; break;
    case 4: *d &= ~(1u << n); break;
    case 5: *d = shift_reg(c, 0, *d, n); return;
    case 6: *d = shift_reg(c, 1, *d, n); return;
    case 7: *d = shift_reg(c, 2, *d, n); return;
    }
    bfin_flags_nz(c, *d);
    bfin_flags_ac0(c, 0);
    bfin_flags_v(c, 0);
}

/* COMP3op, one handler per opcode: src0 in bits 0-2, src1 in 3-5, dst in 6-8. */
#define S0(iw) ((iw) & 7)
#define S1(iw) (((iw) >> 3) & 7)
#define D3(iw) (((iw) >> 6) & 7)

static void comp3_add(bfin_core *c, uint16_t iw, uint16_t pad)
{
    c->r[D3(iw)] = bfin_add32(c, c->r[S0(iw)], c->r[S1(iw)], 0, 0);
}

static void comp3_sub(bfin_core *c, uint16_t iw, uint16_t pad)
{
    c->r[D3(iw)] = bfin_add32(c, c->r[S0(iw)], c->r[S1(iw)], 1, 0);
}

static void comp3_and(bfin_core *c, uint16_t iw, uint16_t pad)
{
    uint32_t r = c->r[S0(iw)] & c->r[S1(iw)];

    c->r[D3(iw)] = r;
    bfin_flags_logic(c, r);
}

static void comp3_or(bfin_core *c, uint16_t iw, uint16_t pad)
{
    uint32_t r = c->r[S0(iw)] | c->r[S1(iw)];

    c->r[D3(iw)] = r;
    bfin_flags_logic(c, r);
}

static void comp3_xor(bfin_core *c, uint16_t iw, uint16_t pad)
{
    uint32_t r = c->r[S0(iw)] ^ c->r[S1(iw)];

    c->r[D3(iw)] = r;
    bfin_flags_logic(c, r);
}

static void comp3_padd(bfin_core *c, uint16_t iw, uint16_t pad)
{
    c->p[D3(iw)] = c->p[S0(iw)] + c->p[S1(iw)];
}

static void comp3_padd1(bfin_core *c, uint16_t iw, uint16_t pad)
{
    c->p[D3(iw)] = c->p[S0(iw)] + (c->p[S1(iw)] << 1);
}

static void comp3_padd2(bfin_core *c, uint16_t iw, uint16_t pad)
{
    c->p[D3(iw)] = c->p[S0(iw)] + (c->p[S1(iw)] << 2);
}

static bfin_op *const comp3op[8] = {
    comp3_add, comp3_sub, comp3_and, comp3_or,
    comp3_xor, comp3_padd, comp3_padd1, comp3_padd2,
};

static uint32_t ld_ext(uint32_t v, unsigned sz, int x)
{
    if (sz == 1) {
        return x ? (uint32_t)(int16_t)v : (uint16_t)v;
    }
    if (sz == 2) {
        return x ? (uint32_t)(int8_t)v : (uint8_t)v;
    }
    return v;
}

static void ldst(bfin_core *c, uint16_t iw, uint16_t pad)
{
    unsigned reg = iw & 7, ptr = (iw >> 3) & 7, aop = (iw >> 7) & 3;
    unsigned sz = (iw >> 10) & 3, bytes = 4 >> sz;
    int z = iw & 0x40, w = iw & 0x200;
    uint32_t a = c->p[ptr];

    if (aop == 3 || sz == 3 || (w && sz && z)) {
        c->undef = 1;
        return;
    }
    if (w) {
        bfin_store(c, a, z ? c->p[reg] : c->r[reg], bytes);
    } else {
        uint32_t v = ld_ext(bfin_load(c, a, bytes), sz, z);

        if (aop < 2) {
            c->p[ptr] = aop ? a - bytes : a + bytes;
        }
        load_reg(c, !sz && z, reg, v);
        return;
    }
    if (aop < 2) {
        c->p[ptr] = aop ? a - bytes : a + bytes;
    }
}

/* LDSTii, one handler per W and opcode: reg in bits 0-2, ptr in 3-5, the
 * offset in 6-9 scaled by the access size. */
#define II_REG(iw) ((iw) & 7)
#define II_ADDR(c, iw, size) ((c)->p[((iw) >> 3) & 7] + (((iw) >> 6) & 0xF) * (size))

static void ld_ii_r32(bfin_core *c, uint16_t iw, uint16_t pad)
{
    load_reg(c, 0, II_REG(iw), bfin_load(c, II_ADDR(c, iw, 4), 4));
}

static void ld_ii_r16z(bfin_core *c, uint16_t iw, uint16_t pad)
{
    load_reg(c, 0, II_REG(iw), (uint16_t)bfin_load(c, II_ADDR(c, iw, 2), 2));
}

static void ld_ii_r16x(bfin_core *c, uint16_t iw, uint16_t pad)
{
    load_reg(c, 0, II_REG(iw), (int16_t)bfin_load(c, II_ADDR(c, iw, 2), 2));
}

static void ld_ii_p32(bfin_core *c, uint16_t iw, uint16_t pad)
{
    load_reg(c, 1, II_REG(iw), bfin_load(c, II_ADDR(c, iw, 4), 4));
}

static void st_ii_r32(bfin_core *c, uint16_t iw, uint16_t pad)
{
    bfin_store(c, II_ADDR(c, iw, 4), c->r[II_REG(iw)], 4);
}

static void st_ii_r16(bfin_core *c, uint16_t iw, uint16_t pad)
{
    bfin_store(c, II_ADDR(c, iw, 2), c->r[II_REG(iw)], 2);
}

static void st_ii_p32(bfin_core *c, uint16_t iw, uint16_t pad)
{
    bfin_store(c, II_ADDR(c, iw, 4), c->p[II_REG(iw)], 4);
}

static void undef16(bfin_core *c, uint16_t iw, uint16_t pad);

static bfin_op *const ldst_ii[8] = {
    ld_ii_r32, ld_ii_r16z, ld_ii_r16x, ld_ii_p32,
    st_ii_r32, st_ii_r16, undef16, st_ii_p32,
};

/* LDST without the reserved forms, one handler per W and size. The pointer
 * is post-incremented, post-decremented or left (aop 0-2); Z picks a P
 * register for a word, sign extension for a half or a byte. */
static inline void ldst_sized(bfin_core *c, uint16_t iw, int w, unsigned sz)
{
    unsigned reg = iw & 7, ptr = (iw >> 3) & 7, aop = (iw >> 7) & 3;
    unsigned bytes = 4 >> sz;
    int z = iw & 0x40;
    uint32_t a = c->p[ptr];
    int32_t step = aop == 0 ? (int32_t)bytes : aop == 1 ? -(int32_t)bytes : 0;

    if (w) {
        bfin_store(c, a, z ? c->p[reg] : c->r[reg], bytes);
        c->p[ptr] = a + step;
    } else {
        uint32_t v = ld_ext(bfin_load(c, a, bytes), sz, z);

        c->p[ptr] = a + step;
        load_reg(c, !sz && z, reg, v);
    }
}

static void ld32(bfin_core *c, uint16_t iw, uint16_t pad) { ldst_sized(c, iw, 0, 0); }
static void ld16(bfin_core *c, uint16_t iw, uint16_t pad) { ldst_sized(c, iw, 0, 1); }
static void ld8(bfin_core *c, uint16_t iw, uint16_t pad)  { ldst_sized(c, iw, 0, 2); }
static void st32(bfin_core *c, uint16_t iw, uint16_t pad) { ldst_sized(c, iw, 1, 0); }
static void st16(bfin_core *c, uint16_t iw, uint16_t pad) { ldst_sized(c, iw, 1, 1); }
static void st8(bfin_core *c, uint16_t iw, uint16_t pad)  { ldst_sized(c, iw, 1, 2); }

static bfin_op *ldst_form(uint16_t iw)
{
    static bfin_op *const form[2][3] = { { ld32, ld16, ld8 }, { st32, st16, st8 } };
    unsigned sz = (iw >> 10) & 3;
    int w = !!(iw & 0x200);

    if (((iw >> 7) & 3) == 3 || sz == 3 || (w && sz && (iw & 0x40))) {
        return ldst;
    }
    return form[w][sz];
}

static void ldst_ii_fp(bfin_core *c, uint16_t iw, uint16_t pad)
{
    unsigned reg = iw & 0xF;
    uint32_t a = c->p[7] + (((iw >> 4) & 0x1F) - 32) * 4;

    if (iw & 0x200) {
        bfin_store(c, a, reg < 8 ? c->r[reg] : c->p[reg - 8], 4);
    } else {
        load_reg(c, reg >> 3, reg & 7, bfin_load(c, a, 4));
    }
}

static void ldst_pmod(bfin_core *c, uint16_t iw, uint16_t pad)
{
    unsigned ptr = iw & 7, idx = (iw >> 3) & 7, reg = (iw >> 6) & 7;
    unsigned aop = (iw >> 9) & 3;
    int w = iw & 0x800;
    uint32_t a = c->p[ptr], d = c->r[reg];
    int post = !(aop == 1 || aop == 2) || idx != ptr;

    if (aop == 3) {
        load_reg(c, 0, reg, ld_ext(bfin_load(c, a, 2), 1, w));
    } else if (w) {
        if (aop == 0) {
            bfin_store(c, a, d, 4);
        } else {
            bfin_store(c, a, aop == 1 ? d : d >> 16, 2);
        }
    } else if (aop == 0) {
        load_reg(c, 0, reg, bfin_load(c, a, 4));
    } else {
        uint32_t h = bfin_load(c, a, 2);

        load_reg(c, 0, reg, aop == 1 ? (d & 0xFFFF0000) | h : (d & 0xFFFF) | h << 16);
    }
    if (post) {
        c->p[ptr] = a + c->p[idx];
    }
}

static void dsp_ldst(bfin_core *c, uint16_t iw, uint16_t pad)
{
    unsigned reg = iw & 7, n = (iw >> 3) & 3, m = (iw >> 5) & 3;
    unsigned aop = (iw >> 7) & 3;
    int w = iw & 0x200;
    uint32_t a = c->i[n], d = c->r[reg];
    unsigned bytes = aop == 3 || m == 0 ? 4 : 2;

    if (aop != 3 && m == 3) {
        c->undef = 1;
        return;
    }
    if (w) {
        bfin_store(c, a, m == 2 && aop != 3 ? d >> 16 : d, bytes);
    } else if (bytes == 4) {
        load_reg(c, 0, reg, bfin_load(c, a, 4));
    } else {
        uint32_t h = bfin_load(c, a, 2);

        load_reg(c, 0, reg, m == 1 ? (d & 0xFFFF0000) | h : (d & 0xFFFF) | h << 16);
    }
    switch (aop) {
    case 0: c->i[n] = dag_add(c, n, bytes); break;
    case 1: c->i[n] = dag_add(c, n, -(int32_t)bytes); break;
    case 3: c->i[n] = dag_add(c, n, c->m[m]); break;
    }
}

/* PREFETCH/FLUSH/FLUSHINV/IFLUSH: no caches, only the post-increment. */
static void cache_ctrl(bfin_core *c, uint16_t iw, uint16_t pad)
{
    if (iw & 0x20) {
        c->p[iw & 7] += 32;
    }
}

static void cc_move(bfin_core *c, uint16_t iw, uint16_t pad)
{
    if (c->cc == !!(iw & 0x100)) {
        uint32_t v = iw & 0x40 ? c->p[iw & 7] : c->r[iw & 7];

        if (iw & 0x80) {
            c->p[(iw >> 3) & 7] = v;
        } else {
            c->r[(iw >> 3) & 7] = v;
        }
    }
}

static void cc_dreg(bfin_core *c, uint16_t iw, uint16_t pad)
{
    switch ((iw >> 3) & 3) {
    case 0: c->r[iw & 7] = c->cc; break;
    case 1: c->cc = c->r[iw & 7] != 0; break;
    case 3: c->cc = !c->cc; break;
    default: c->undef = 1; break;
    }
}

static void branch_cc(bfin_core *c, uint16_t iw, uint16_t pad)
{
    if (c->cc == !!(iw & 0x800)) {
        c->npc = c->pc + sext(iw & 0x3FF, 10) * 2;
    }
}

static void jump_short(bfin_core *c, uint16_t iw, uint16_t pad)
{
    c->npc = c->pc + sext(iw & 0xFFF, 12) * 2;
}

static void reg_move(bfin_core *c, uint16_t iw, uint16_t pad)
{
    bfin_set_reg(c, (iw >> 9) & 7, (iw >> 3) & 7,
                 bfin_reg(c, (iw >> 6) & 7, iw & 7));
}

/* Between data and pointer registers, the common case. */
static void reg_move_dp(bfin_core *c, uint16_t iw, uint16_t pad)
{
    uint32_t v = (iw & 0x40 ? c->p : c->r)[iw & 7];

    (iw & 0x200 ? c->p : c->r)[(iw >> 3) & 7] = v;
}

static void dreg_imm_set(bfin_core *c, uint16_t iw, uint16_t pad)
{
    c->r[iw & 7] = sext((iw >> 3) & 0x7F, 7);
}

static void dreg_imm_add(bfin_core *c, uint16_t iw, uint16_t pad)
{
    c->r[iw & 7] = bfin_add32(c, c->r[iw & 7], sext((iw >> 3) & 0x7F, 7), 0, 0);
}

static void preg_imm(bfin_core *c, uint16_t iw, uint16_t pad)
{
    int32_t imm = sext((iw >> 3) & 0x7F, 7);

    c->p[iw & 7] = iw & 0x400 ? c->p[iw & 7] + imm : (uint32_t)imm;
}

static void dag_modify(bfin_core *c, uint16_t iw, uint16_t pad)
{
    unsigned n = iw & 3, m = (iw >> 2) & 3;

    if (iw & 0x80) {
        c->i[n] = brev_add(c->i[n], c->m[m]);
    } else {
        c->i[n] = dag_add(c, n, iw & 0x10 ? -(int32_t)c->m[m] : (int32_t)c->m[m]);
    }
}

static void dag_step(bfin_core *c, uint16_t iw, uint16_t pad)
{
    static const int8_t step[4] = { 2, -2, 4, -4 };

    c->i[iw & 3] = dag_add(c, iw & 3, step[(iw >> 2) & 3]);
}

static void undef16(bfin_core *c, uint16_t iw, uint16_t pad)
{
    c->undef = 1;
}

static void nop(bfin_core *c, uint16_t iw, uint16_t pad)
{
}

static bfin_op *decode16(uint16_t iw)
{
    if (iw == 0x0000) {
        return nop;
    } else if ((iw & 0xFF00) == 0x0000) {
        return prog_ctrl;
    } else if ((iw & 0xFFC0) == 0x0240) {
        return cache_ctrl;
    } else if ((iw & 0xFF80) == 0x0100) {
        return push_pop_reg;
    } else if ((iw & 0xFE00) == 0x0400) {
        return push_pop_multiple;
    } else if ((iw & 0xFE00) == 0x0600) {
        return cc_move;
    } else if ((iw & 0xF800) == 0x0800) {
        return cc_flag[(iw >> 7) & 7];
    } else if ((iw & 0xFFE0) == 0x0200) {
        return cc_dreg;
    } else if ((iw & 0xFF00) == 0x0300) {
        return cc2stat;
    } else if ((iw & 0xF000) == 0x1000) {
        return branch_cc;
    } else if ((iw & 0xF000) == 0x2000) {
        return jump_short;
    } else if ((iw & 0xFD80) == 0x3000) {
        return reg_move_dp;
    } else if ((iw & 0xF000) == 0x3000) {
        return reg_move;
    } else if ((iw & 0xFC00) == 0x4000) {
        return alu2op;
    } else if ((iw & 0xFE00) == 0x4400) {
        return ptr2op;
    } else if ((iw & 0xF800) == 0x4800) {
        return logi2op;
    } else if ((iw & 0xF000) == 0x5000) {
        return comp3op[(iw >> 9) & 7];
    } else if ((iw & 0xF800) == 0x6000) {
        return iw & 0x400 ? dreg_imm_add : dreg_imm_set;
    } else if ((iw & 0xF800) == 0x6800) {
        return preg_imm;
    } else if ((iw & 0xF000) == 0x8000) {
        return ldst_pmod;
    } else if ((iw & 0xFF60) == 0x9E60) {
        return dag_modify;
    } else if ((iw & 0xFFF0) == 0x9F60) {
        return dag_step;
    } else if ((iw & 0xFC00) == 0x9C00) {
        return dsp_ldst;
    } else if ((iw & 0xF000) == 0x9000) {
        return ldst_form(iw);
    } else if ((iw & 0xFC00) == 0xB800) {
        return ldst_ii_fp;
    } else if ((iw & 0xE000) == 0xA000) {
        return ldst_ii[(iw >> 10) & 7];
    }
    return undef16;
}

/* ---- 32-bit groups ------------------------------------------------------ */

static void loop_setup(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    unsigned n = (iw0 >> 4) & 1, rop = (iw0 >> 5) & 3, reg = iw1 >> 12;

    if (iw1 & 0x0C00) {
        c->undef = 1;
        return;
    }
    c->lt[n] = c->pc + (iw0 & 0xF) * 2;
    c->lb[n] = c->pc + (iw1 & 0x3FF) * 2;
    switch (rop) {
    case 0: break;
    case 1: c->lc[n] = c->p[reg & 7]; break;
    case 3: c->lc[n] = c->p[reg & 7] >> 1; break;
    default: c->undef = 1; break;
    }
}

static void ldimm_half(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    unsigned reg = iw0 & 7, grp = (iw0 >> 3) & 3;
    uint32_t v = bfin_reg(c, grp, reg);

    if (iw0 & 0x40) {                           /* H */
        v = (v & 0xFFFF) | (uint32_t)iw1 << 16;
    } else if (iw0 & 0x20) {                    /* S */
        v = (int16_t)iw1;
    } else if (iw0 & 0x80) {                    /* Z */
        v = iw1;
    } else {
        v = (v & 0xFFFF0000) | iw1;
    }
    bfin_set_reg(c, grp, reg, v);
}

/* LDIMMhalf into a data or pointer register, one handler per form. */
static inline uint32_t *ldimm_dp(bfin_core *c, uint16_t iw0)
{
    return (iw0 & 8 ? c->p : c->r) + (iw0 & 7);
}

static void ldimm_dp_h(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    uint32_t *d = ldimm_dp(c, iw0);

    *d = (*d & 0xFFFF) | (uint32_t)iw1 << 16;
}

static void ldimm_dp_s(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    *ldimm_dp(c, iw0) = (int16_t)iw1;
}

static void ldimm_dp_z(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    *ldimm_dp(c, iw0) = iw1;
}

static void ldimm_dp_l(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    uint32_t *d = ldimm_dp(c, iw0);

    *d = (*d & 0xFFFF0000) | iw1;
}

static bfin_op *ldimm_dp_form(uint16_t iw0)
{
    return iw0 & 0x40 ? ldimm_dp_h : iw0 & 0x20 ? ldimm_dp_s :
           iw0 & 0x80 ? ldimm_dp_z : ldimm_dp_l;
}

static void ldst_idx(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    unsigned reg = iw0 & 7, ptr = (iw0 >> 3) & 7, sz = (iw0 >> 6) & 3;
    int z = iw0 & 0x100, w = iw0 & 0x200;
    unsigned bytes = 4 >> sz;
    uint32_t a = c->p[ptr] + (int16_t)iw1 * (int32_t)bytes;

    if (sz == 3) {
        c->undef = 1;
    } else if (w) {
        bfin_store(c, a, !sz && z ? c->p[reg] : c->r[reg], bytes);
    } else {
        load_reg(c, !sz && z, reg, ld_ext(bfin_load(c, a, bytes), sz, z));
    }
}

static void linkage(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    if (iw0 & 1) {                              /* UNLINK */
        c->p[6] = c->p[7];
        c->p[7] = bfin_load(c, c->p[6], 4);
        c->rets = bfin_load(c, c->p[6] + 4, 4);
        c->p[6] += 8;
    } else {
        bfin_store(c, c->p[6] - 4, c->rets, 4);
        bfin_store(c, c->p[6] - 8, c->p[7], 4);
        c->p[6] -= 8;
        c->p[7] = c->p[6];
        c->p[6] -= iw1 * 4u;
    }
}

static void jump_long(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    int32_t off = sext((uint32_t)(iw0 & 0xFF) << 16 | iw1, 24) * 2;

    if (iw0 & 0x100) {
        c->rets = c->npc;
    }
    c->npc = c->pc + off;
}

static void mac(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    if ((iw0 & 0xF7FF) == 0xC003 && iw1 == 0x1800) {
        return;                                 /* MNOP */
    }
    bfin_dsp32mac(c, iw0, iw1, 0);
}

static void mult(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    bfin_dsp32mac(c, iw0, iw1, 1);
}

static void undef32(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    c->undef = 1;
}

/* Every 32-bit group is told apart by its first word alone. */
static bfin_op *decode32(uint16_t iw0)
{
    if ((iw0 & 0xFF80) == 0xE080) {
        return loop_setup;
    } else if ((iw0 & 0xFF10) == 0xE100) {
        return ldimm_dp_form(iw0);
    } else if ((iw0 & 0xFF00) == 0xE100) {
        return ldimm_half;
    } else if ((iw0 & 0xFE00) == 0xE200) {
        return jump_long;
    } else if ((iw0 & 0xFC00) == 0xE400) {
        return ldst_idx;
    } else if ((iw0 & 0xFFFE) == 0xE800) {
        return linkage;
    } else if ((iw0 & 0xF600) == 0xC000) {
        return mac;
    } else if ((iw0 & 0xF600) == 0xC200) {
        return mult;
    } else if ((iw0 & 0xF600) == 0xC400) {
        return bfin_dsp32alu;
    } else if ((iw0 & 0xF780) == 0xC600) {
        return bfin_dsp32shift;
    } else if ((iw0 & 0xF79F) == 0xC682) {
        return bfin_dsp32shiftimm32;
    } else if ((iw0 & 0xF780) == 0xC680) {
        return bfin_dsp32shiftimm;
    }
    return undef32;
}

/* ---- threaded execution ------------------------------------------------- */

/* Loads inside a bundle write back at its end, after every slot has read its
 * operands: the slots' loads are held back (load_reg) and the 16-bit slots
 * run first, so their stores see the registers the 32-bit slot is about to
 * change. */
static int x_bundle(bfin_core *c, const bfin_insn *i)
{
    BFIN_SYNC(c, i);
    c->in_bundle = 1;
    c->nload = 0;
    if (i->slot[0]) {
        i->slot[0](c, i->sw[0], 0);
    }
    if (i->slot[1]) {
        i->slot[1](c, i->sw[1], 0);
    }
    c->in_bundle = 0;
    i->op(c, i->iw0, i->iw1);
    for (int n = 0; n < c->nload; n++) {
        if (c->load[n].grp < 2) {
            (c->load[n].grp ? c->p : c->r)[c->load[n].reg] = c->load[n].val;
        } else {
            bfin_set_reg(c, c->load[n].grp, c->load[n].reg, c->load[n].val);
        }
    }
    BFIN_NEXT_CHECKED(c, i);
}

/* A bundle whose slots only load whole data registers can run its 32-bit
 * slot first, which touches nothing but the data registers, the
 * accumulators and ASTAT: it reads the registers before the loads change
 * them, and the loads still have the last word. */
static int x_bundle_loads(bfin_core *c, const bfin_insn *i)
{
    BFIN_SYNC(c, i);
    i->op(c, i->iw0, i->iw1);
    if (i->slot[0]) {
        i->slot[0](c, i->sw[0], 0);
    }
    if (i->slot[1]) {
        i->slot[1](c, i->sw[1], 0);
    }
    BFIN_NEXT_CHECKED(c, i);
}

/* Any instruction, given the pc it may read and the npc it may change. */
static int x_any(bfin_core *c, const bfin_insn *i)
{
    uint32_t next = i->pc + i->len;

    BFIN_SYNC(c, i);
    c->pc = i->pc;
    c->npc = next;
    i->op(c, i->iw0, i->iw1);
    if (c->npc == next && !c->stop_flags) {
        BFIN_NEXT(c, i);
    }
    BFIN_MUSTTAIL return bfin_end(c, i);
}

/* The common handlers get a threaded form of their own, so each ends in its
 * own indirect jump and the handler is inlined into it. Straight ones can
 * neither stop the run nor branch; checked ones can stop it (a bus access,
 * an event, an unimplemented form) but not branch; sequenced ones read the
 * pc or set the npc. */
#define FLAT __attribute__((flatten))
#define STRAIGHT(op)                                                    \
    static FLAT int x_##op(bfin_core *c, const bfin_insn *i)            \
    {                                                                   \
        op(c, i->iw0, i->iw1);                                          \
        BFIN_NEXT(c, i);                                                \
    }
#define CHECKED(op)                                                     \
    static FLAT int x_##op(bfin_core *c, const bfin_insn *i)            \
    {                                                                   \
        BFIN_SYNC(c, i);                                                \
        op(c, i->iw0, i->iw1);                                          \
        BFIN_NEXT_CHECKED(c, i);                                        \
    }
#define SEQUENCED(op)                                                   \
    static FLAT int x_##op(bfin_core *c, const bfin_insn *i)            \
    {                                                                   \
        uint32_t next = i->pc + i->len;                                 \
                                                                        \
        c->pc = i->pc;                                                  \
        c->npc = next;                                                  \
        op(c, i->iw0, i->iw1);                                          \
        if (c->npc == next) {                                           \
            BFIN_NEXT(c, i);                                            \
        }                                                               \
        BFIN_MUSTTAIL return bfin_end(c, i);                            \
    }

#define STRAIGHT_OPS(X)                                                 \
    X(nop) X(cache_ctrl) X(cc_move) X(cc_eq) X(cc_lt) X(cc_le) X(cc_ltu)  \
    X(cc_leu) X(cc_acc) X(cc2stat) X(reg_move_dp) X(logi2op)             \
    X(comp3_add) X(comp3_sub) X(comp3_and) X(comp3_or) X(comp3_xor)      \
    X(comp3_padd) X(comp3_padd1) X(comp3_padd2) X(dreg_imm_set)          \
    X(dreg_imm_add) X(preg_imm) X(dag_modify) X(dag_step)                \
    X(ldimm_dp_h) X(ldimm_dp_s) X(ldimm_dp_z) X(ldimm_dp_l)
#define CHECKED_OPS(X)                                                  \
    X(push_pop_multiple) X(cc_dreg) X(alu2op) X(ptr2op) X(ldst)          \
    X(ld_ii_r32) X(ld_ii_r16z) X(ld_ii_r16x) X(ld_ii_p32) X(st_ii_r32)   \
    X(st_ii_r16) X(st_ii_p32) X(ld32) X(ld16) X(ld8) X(st32) X(st16)     \
    X(st8) X(ldst_ii_fp) X(ldst_pmod) X(dsp_ldst) X(ldimm_half)          \
    X(ldst_idx) X(linkage) X(mac) X(mult) X(bfin_dsp32alu)               \
    X(bfin_dsp32shift) X(bfin_dsp32shiftimm) X(bfin_dsp32shiftimm32)     \
    X(undef16) X(undef32)
#define SEQUENCED_OPS(X) X(branch_cc) X(jump_short)

STRAIGHT_OPS(STRAIGHT)
CHECKED_OPS(CHECKED)
SEQUENCED_OPS(SEQUENCED)

#define THREADED(op) { op, x_##op },

static const struct { bfin_op *op; bfin_xop *x; } threaded[] = {
    STRAIGHT_OPS(THREADED) CHECKED_OPS(THREADED) SEQUENCED_OPS(THREADED)
};

/* Decoding is done once per encoding, not once per instruction: the handler
 * of every first word and its threaded form. A bundle's entry is the handler
 * of its 32-bit slot, which is told apart by the same words with the bundle
 * bit ignored. */
static bfin_op *op_table[0x10000];
static bfin_xop *xop_table[0x10000];

static bfin_xop *threaded_form(bfin_op *op)
{
    for (size_t n = 0; n < sizeof(threaded) / sizeof(threaded[0]); n++) {
        if (threaded[n].op == op) {
            return threaded[n].x;
        }
    }
    return x_any;
}

void bfin_decode_init(void)
{
    if (op_table[0]) {
        return;
    }
    for (unsigned iw = 0; iw < 0x10000; iw++) {
        unsigned len = bfin_insn_len(iw);

        op_table[iw] = len == 2 ? decode16(iw) : decode32(iw);
        xop_table[iw] = len == 8 ? x_bundle : threaded_form(op_table[iw]);
    }
}

/* A bundle's 16-bit slot: a load or store, or a NOP left out. */
static bfin_op *slot(uint16_t iw)
{
    return !iw ? NULL : iw >= 0xC000 ? undef16 : op_table[iw];
}

/* Whether a slot is a NOP or a load that writes a whole data register. */
static int loads_dreg(bfin_op *op, uint16_t iw)
{
    return !op || op == ld_ii_r32 || op == ld_ii_r16z || op == ld_ii_r16x ||
           op == ld16 || op == ld8 || (op == ld32 && !(iw & 0x40)) ||
           (op == dsp_ldst && !(iw & 0x200) &&
            (((iw >> 7) & 3) == 3 || !((iw >> 5) & 3)));
}

int bfin_decode(bfin_core *c, bfin_insn *in, uint32_t pc, uint16_t iw0,
                uint16_t iw1)
{
    bfin_op *op = op_table[iw0];

    in->fn = xop_table[iw0];
    in->op = op;
    in->pc = pc;
    in->iw0 = iw0;
    in->iw1 = iw1;
    in->len = bfin_insn_len(iw0);
    if (in->fn == x_bundle) {
        in->sw[0] = bfin_fetch16(c, pc + 4);
        in->sw[1] = bfin_fetch16(c, pc + 6);
        in->slot[0] = slot(in->sw[0]);
        in->slot[1] = slot(in->sw[1]);
        if (loads_dreg(in->slot[0], in->sw[0]) &&
            loads_dreg(in->slot[1], in->sw[1])) {
            in->fn = x_bundle_loads;
        }
    }
    return op == loop_setup ||
           (op == reg_move && ((iw0 >> 9) & 7) == 6) ||
           (op == push_pop_reg && !(iw0 & 0x40) && ((iw0 >> 3) & 7) == 6);
}
