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

void bfin_flags_nz(bfin_core *c, uint32_t v)
{
    c->astat &= ~(AS_AZ | AS_AN);
    c->astat |= (v ? 0 : AS_AZ) | (v >> 31 ? AS_AN : 0);
}

void bfin_flags_v(bfin_core *c, int v)
{
    c->astat &= ~(AS_V | AS_V_COPY);
    if (v) {
        c->astat |= AS_V | AS_V_COPY | AS_VS;
    }
}

void bfin_flags_ac0(bfin_core *c, int ac0)
{
    c->astat &= ~(AS_AC0 | AS_AC0_COPY);
    if (ac0) {
        c->astat |= AS_AC0 | AS_AC0_COPY;
    }
}

uint32_t bfin_add32(bfin_core *c, uint32_t a, uint32_t b, int sub, int sat)
{
    uint32_t r = sub ? a - b : a + b;
    int v = sub ? ((a ^ b) & (a ^ r)) >> 31 : (~(a ^ b) & (a ^ r)) >> 31;

    if (v && sat) {
        r = (int32_t)a < 0 ? 0x80000000u : 0x7FFFFFFFu;
    }
    bfin_flags_nz(c, r);
    bfin_flags_ac0(c, sub ? b <= a : r < a);
    bfin_flags_v(c, v);
    return r;
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

static void prog_ctrl(bfin_core *c, unsigned fn, unsigned op)
{
    switch (fn) {
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

static void push_pop_reg(bfin_core *c, uint16_t iw)
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

static void push_pop_multiple(bfin_core *c, uint16_t iw)
{
    unsigned pr = iw & 7, dr = (iw >> 3) & 7;
    int d = iw & 0x100, p = iw & 0x80;
    uint32_t sp = c->p[6];

    if (iw & 0x40) {
        if (d) {
            for (int n = 7; n >= (int)dr; n--) {
                sp -= 4;
                bfin_store(c, sp, c->r[n], 4);
            }
        }
        if (p) {
            for (int n = 5; n >= (int)pr; n--) {
                sp -= 4;
                bfin_store(c, sp, c->p[n], 4);
            }
        }
    } else {
        if (p) {
            for (int n = pr; n <= 5; n++) {
                c->p[n] = bfin_load(c, sp, 4);
                sp += 4;
            }
        }
        if (d) {
            for (int n = dr; n <= 7; n++) {
                c->r[n] = bfin_load(c, sp, 4);
                sp += 4;
            }
        }
    }
    c->p[6] = sp;
}

static void cc_flag(bfin_core *c, uint16_t iw)
{
    unsigned x = iw & 7, y = (iw >> 3) & 7, opc = (iw >> 7) & 7;
    int imm = iw & 0x400, preg = iw & 0x40;
    uint32_t a, b;

    if (opc >= 5) {
        int64_t a0 = c->a[0], a1 = c->a[1];

        c->cc = opc == 5 ? a0 == a1 : opc == 6 ? a0 < a1 : a0 <= a1;
        return;
    }
    a = preg ? c->p[x] : c->r[x];
    b = imm ? (opc >= 3 ? y : (uint32_t)sext(y, 3)) : preg ? c->p[y] : c->r[y];
    switch (opc) {
    case 0: c->cc = a == b; break;
    case 1: c->cc = (int32_t)a < (int32_t)b; break;
    case 2: c->cc = (int32_t)a <= (int32_t)b; break;
    case 3: c->cc = a < b; break;
    case 4: c->cc = a <= b; break;
    }
    if (!preg) {
        bfin_flags_nz(c, a - b);
        bfin_flags_ac0(c, b <= a);
    }
}

static void cc2stat(bfin_core *c, uint16_t iw)
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

static void alu2op(bfin_core *c, uint16_t iw)
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

static void ptr2op(bfin_core *c, uint16_t iw)
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

static void logi2op(bfin_core *c, uint16_t iw)
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

static void comp3op(bfin_core *c, uint16_t iw)
{
    unsigned s0 = iw & 7, s1 = (iw >> 3) & 7, dst = (iw >> 6) & 7;
    uint32_t a = c->r[s0], b = c->r[s1], r;

    switch ((iw >> 9) & 7) {
    case 0: c->r[dst] = bfin_add32(c, a, b, 0, 0); return;
    case 1: c->r[dst] = bfin_add32(c, a, b, 1, 0); return;
    case 2: r = a & b; break;
    case 3: r = a | b; break;
    case 4: r = a ^ b; break;
    case 5: c->p[dst] = c->p[s0] + c->p[s1]; return;
    case 6: c->p[dst] = c->p[s0] + (c->p[s1] << 1); return;
    default: c->p[dst] = c->p[s0] + (c->p[s1] << 2); return;
    }
    c->r[dst] = r;
    bfin_flags_nz(c, r);
    bfin_flags_ac0(c, 0);
    bfin_flags_v(c, 0);
}

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

static void ldst(bfin_core *c, uint16_t iw)
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

static void ldst_ii(bfin_core *c, uint16_t iw)
{
    unsigned reg = iw & 7, ptr = (iw >> 3) & 7, off = (iw >> 6) & 0xF;
    unsigned op = (iw >> 10) & 3;
    uint32_t a;

    a = c->p[ptr] + off * (op == 1 || op == 2 ? 2 : 4);
    if (iw & 0x1000) {
        switch (op) {
        case 0: bfin_store(c, a, c->r[reg], 4); return;
        case 1: bfin_store(c, a, c->r[reg], 2); return;
        case 3: bfin_store(c, a, c->p[reg], 4); return;
        }
        c->undef = 1;
        return;
    }
    switch (op) {
    case 0: load_reg(c, 0, reg, bfin_load(c, a, 4)); return;
    case 1: load_reg(c, 0, reg, (uint16_t)bfin_load(c, a, 2)); return;
    case 2: load_reg(c, 0, reg, (int16_t)bfin_load(c, a, 2)); return;
    case 3: load_reg(c, 1, reg, bfin_load(c, a, 4)); return;
    }
}

static void ldst_ii_fp(bfin_core *c, uint16_t iw)
{
    unsigned reg = iw & 0xF;
    uint32_t a = c->p[7] + (((iw >> 4) & 0x1F) - 32) * 4;

    if (iw & 0x200) {
        bfin_store(c, a, reg < 8 ? c->r[reg] : c->p[reg - 8], 4);
    } else {
        load_reg(c, reg >> 3, reg & 7, bfin_load(c, a, 4));
    }
}

static void ldst_pmod(bfin_core *c, uint16_t iw)
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

static void dsp_ldst(bfin_core *c, uint16_t iw)
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

static void exec16(bfin_core *c, uint16_t iw)
{
    if ((iw & 0xFF00) == 0x0000) {
        prog_ctrl(c, (iw >> 4) & 0xF, iw & 0xF);
    } else if ((iw & 0xFFC0) == 0x0240) {
        /* PREFETCH/FLUSH/FLUSHINV/IFLUSH: no caches, only the post-increment. */
        if (iw & 0x20) {
            c->p[iw & 7] += 32;
        }
    } else if ((iw & 0xFF80) == 0x0100) {
        push_pop_reg(c, iw);
    } else if ((iw & 0xFE00) == 0x0400) {
        push_pop_multiple(c, iw);
    } else if ((iw & 0xFE00) == 0x0600) {
        if (c->cc == !!(iw & 0x100)) {
            uint32_t v = iw & 0x40 ? c->p[iw & 7] : c->r[iw & 7];

            if (iw & 0x80) {
                c->p[(iw >> 3) & 7] = v;
            } else {
                c->r[(iw >> 3) & 7] = v;
            }
        }
    } else if ((iw & 0xF800) == 0x0800) {
        cc_flag(c, iw);
    } else if ((iw & 0xFFE0) == 0x0200) {
        switch ((iw >> 3) & 3) {
        case 0: c->r[iw & 7] = c->cc; break;
        case 1: c->cc = c->r[iw & 7] != 0; break;
        case 3: c->cc = !c->cc; break;
        default: c->undef = 1; break;
        }
    } else if ((iw & 0xFF00) == 0x0300) {
        cc2stat(c, iw);
    } else if ((iw & 0xF000) == 0x1000) {
        if (c->cc == !!(iw & 0x800)) {
            c->npc = c->pc + sext(iw & 0x3FF, 10) * 2;
        }
    } else if ((iw & 0xF000) == 0x2000) {
        c->npc = c->pc + sext(iw & 0xFFF, 12) * 2;
    } else if ((iw & 0xF000) == 0x3000) {
        bfin_set_reg(c, (iw >> 9) & 7, (iw >> 3) & 7,
                     bfin_reg(c, (iw >> 6) & 7, iw & 7));
    } else if ((iw & 0xFC00) == 0x4000) {
        alu2op(c, iw);
    } else if ((iw & 0xFE00) == 0x4400) {
        ptr2op(c, iw);
    } else if ((iw & 0xF800) == 0x4800) {
        logi2op(c, iw);
    } else if ((iw & 0xF000) == 0x5000) {
        comp3op(c, iw);
    } else if ((iw & 0xF800) == 0x6000) {
        int32_t imm = sext((iw >> 3) & 0x7F, 7);

        if (iw & 0x400) {
            c->r[iw & 7] = bfin_add32(c, c->r[iw & 7], imm, 0, 0);
        } else {
            c->r[iw & 7] = imm;
        }
    } else if ((iw & 0xF800) == 0x6800) {
        int32_t imm = sext((iw >> 3) & 0x7F, 7);

        c->p[iw & 7] = iw & 0x400 ? c->p[iw & 7] + imm : (uint32_t)imm;
    } else if ((iw & 0xF000) == 0x8000) {
        ldst_pmod(c, iw);
    } else if ((iw & 0xFF60) == 0x9E60) {
        unsigned n = iw & 3, m = (iw >> 2) & 3;

        if (iw & 0x80) {
            c->i[n] = brev_add(c->i[n], c->m[m]);
        } else {
            c->i[n] = dag_add(c, n, iw & 0x10 ? -(int32_t)c->m[m] : (int32_t)c->m[m]);
        }
    } else if ((iw & 0xFFF0) == 0x9F60) {
        static const int8_t step[4] = { 2, -2, 4, -4 };

        c->i[iw & 3] = dag_add(c, iw & 3, step[(iw >> 2) & 3]);
    } else if ((iw & 0xFC00) == 0x9C00) {
        dsp_ldst(c, iw);
    } else if ((iw & 0xF000) == 0x9000) {
        ldst(c, iw);
    } else if ((iw & 0xFC00) == 0xB800) {
        ldst_ii_fp(c, iw);
    } else if ((iw & 0xE000) == 0xA000) {
        ldst_ii(c, iw);
    } else {
        c->undef = 1;
    }
}

/* ---- 32-bit groups ------------------------------------------------------ */

static void loop_setup(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    unsigned n = (iw0 >> 4) & 1, rop = (iw0 >> 5) & 3, reg = iw1 >> 12;

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

static void exec32(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    if ((iw0 & 0xF7FF) == 0xC003 && iw1 == 0x1800) {
        return;                                 /* MNOP */
    }
    if ((iw0 & 0xFF80) == 0xE080 && !(iw1 & 0x0C00)) {
        loop_setup(c, iw0, iw1);
    } else if ((iw0 & 0xFF00) == 0xE100) {
        ldimm_half(c, iw0, iw1);
    } else if ((iw0 & 0xFE00) == 0xE200) {
        int32_t off = sext((uint32_t)(iw0 & 0xFF) << 16 | iw1, 24) * 2;

        if (iw0 & 0x100) {
            c->rets = c->npc;
        }
        c->npc = c->pc + off;
    } else if ((iw0 & 0xFC00) == 0xE400) {
        ldst_idx(c, iw0, iw1);
    } else if ((iw0 & 0xFFFE) == 0xE800) {
        linkage(c, iw0, iw1);
    } else if ((iw0 & 0xF600) == 0xC000) {
        bfin_dsp32mac(c, iw0, iw1, 0);
    } else if ((iw0 & 0xF600) == 0xC200) {
        bfin_dsp32mac(c, iw0, iw1, 1);
    } else if ((iw0 & 0xF600) == 0xC400) {
        bfin_dsp32alu(c, iw0, iw1);
    } else if ((iw0 & 0xF780) == 0xC600) {
        bfin_dsp32shift(c, iw0, iw1);
    } else if ((iw0 & 0xF780) == 0xC680) {
        bfin_dsp32shiftimm(c, iw0, iw1);
    } else {
        c->undef = 1;
    }
}

/* A 64-bit bundle: one 32-bit DSP instruction and two 16-bit loads/stores.
 * All three read their operands before any of them writes: the slots' loads
 * are held back (load_reg) and the 16-bit slots run first, so their stores
 * see the registers the 32-bit slot is about to change. */
static void exec_bundle(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    uint16_t s2 = bfin_fetch16(c, c->pc + 4), s3 = bfin_fetch16(c, c->pc + 6);

    c->in_bundle = 1;
    c->nload = 0;
    exec16(c, s2);
    exec16(c, s3);
    c->in_bundle = 0;
    exec32(c, iw0, iw1);
    for (int n = 0; n < c->nload; n++) {
        bfin_set_reg(c, c->load[n].grp, c->load[n].reg, c->load[n].val);
    }
}

void bfin_exec(bfin_core *c, uint16_t iw0, uint16_t iw1, unsigned len)
{
    if (len == 2) {
        exec16(c, iw0);
    } else if (len == 4) {
        exec32(c, iw0, iw1);
    } else {
        exec_bundle(c, iw0, iw1);
    }
}
